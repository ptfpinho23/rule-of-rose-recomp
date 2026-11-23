#version 450
/* GS hardware renderer: fragment stage. All colour arithmetic is done on
 * integers with the GS formulas (texture function, fog, alpha test, optional
 * in-shader blending against a copy of the destination). */
layout(push_constant) uniform PC {
    vec4 rt;
    uint flags; int aref; uint fogcol; uint blend;
    ivec4 region;     /* minu, maxu, minv, maxv */
    vec4 tex;
    vec4 zp;
    uint fbmsk;
} pc;
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(set = 1, binding = 0) uniform sampler2D u_dst;
layout(location = 0) noperspective in vec4 v_col;
layout(location = 1) noperspective in vec3 v_stq;
layout(location = 2) noperspective in vec2 v_zf;
layout(location = 3) flat in vec4 v_spr;
#ifdef MV_PASS
/* Motion-vector pass (FSR 3.1), same texture function and alpha test as the
 * colour pass, then instead of a colour: rg = motion in device pixels of the
 * target, b = reactive value, a = depth for the upscaler (1 at the near
 * plane, 0 at the far plane). pc.zp.w, pc.tex.w: GS depth of the far plane
 * and 1 / (near - far). Pass mode 2 / 3 (failing / passing fragments of
 * translucent geometry): only b is written (colour mask), with the
 * fragment's opacity. */
layout(location = 4) noperspective in vec3 v_mv;
layout(location = 0) out vec4 o_col;
#else
layout(location = 0, index = 0) out vec4 o_col;
layout(location = 0, index = 1) out vec4 o_fac;    /* blend factor (dual source) */
#endif

const uint F_TME = 1u, F_TEXEL = 2u, F_BILIN = 4u, F_TCC = 8u, F_FGE = 64u, F_ATE = 1024u, F_FBA = 8192u,
           F_SPRITE = 16777216u, F_SHBLEND = 16384u, F_DATE = 32768u, F_DATM = 65536u, F_COLCLAMP = 131072u, F_TEX24 = 4194304u, F_AEM = 8388608u;

int wrap(int i, uint mode, int size, int mn, int mx, int sc) {
    if (mode == 0u) return i & (size * sc - 1);
    if (mode == 1u) return clamp(i, 0, size * sc - 1);
    if (mode == 2u) return clamp(i, mn * sc, (mx + 1) * sc - 1);
    return (i & mn) | mx;                         /* region repeat: native textures only */
}
ivec4 texel(ivec2 t) {
    int sc = int(pc.tex.z);
    uint wms = (pc.flags >> 18) & 3u, wmt = (pc.flags >> 20) & 3u;
    t.x = wrap(t.x, wms, int(pc.tex.x), pc.region.x, pc.region.y, sc);
    t.y = wrap(t.y, wmt, int(pc.tex.y), pc.region.z, pc.region.w, sc);
    ivec4 c = ivec4(texelFetch(u_tex, t, 0) * 255.0 + 0.5);
    if ((pc.flags & F_TEX24) != 0u) c.a = ((pc.flags & F_AEM) != 0u && c.rgb == ivec3(0)) ? 0 : int(pc.zp.z);
    return c;
}
int sel(uint m, int s, int d) { return m == 0u ? s : m == 1u ? d : 0; }

void main() {
    ivec4 c = ivec4(floor(v_col + 0.5));
    if ((pc.flags & F_TME) != 0u) {
        vec2 uv;
        if ((pc.flags & F_SPRITE) != 0u) {
            /* the GS sprite formula, evaluated at the fragment's native position
             * (g + 0.5) / N - 0.5: exact at N = 1, no seam along the diagonal */
            uv = v_stq.xy + (gl_FragCoord.xy / pc.rt.z - 0.5 - v_spr.xy) * v_spr.zw;
        } else {
            if ((pc.flags & F_TEXEL) != 0u) uv = v_stq.xy;
            else uv = v_stq.xy / (v_stq.z == 0.0 ? 1.0 : v_stq.z) * pc.tex.xy;
            /* the fragment stands for the native position (g + 0.5) / N - 0.5, not g / N */
            uv += pc.rt.w * (dFdx(uv) + dFdy(uv));
        }
        uv *= pc.tex.z;
        ivec4 t;
        if ((pc.flags & F_BILIN) == 0u) t = texel(ivec2(floor(uv)));
        else {
            uv -= 0.5;
            vec2 fl = floor(uv);
            ivec2 i = ivec2(fl), f = ivec2((uv - fl) * 256.0);
            ivec4 t00 = texel(i), t10 = texel(i + ivec2(1, 0)), t01 = texel(i + ivec2(0, 1)), t11 = texel(i + ivec2(1, 1));
            ivec4 top = t00 * (256 - f.x) + t10 * f.x, bot = t01 * (256 - f.x) + t11 * f.x;
            t = (top * (256 - f.y) + bot * f.y) >> 16;
        }
        uint tfx = (pc.flags >> 4) & 3u; bool tcc = (pc.flags & F_TCC) != 0u;
        if (tfx == 0u) { c.rgb = min((t.rgb * c.rgb) >> 7, 255); if (tcc) c.a = min((t.a * c.a) >> 7, 255); }
        else if (tfx == 1u) { c.rgb = t.rgb; if (tcc) c.a = t.a; }
        else { c.rgb = min(((t.rgb * c.rgb) >> 7) + c.a, 255); if (tcc) c.a = tfx == 2u ? min(t.a + c.a, 255) : t.a; }
    }
    if ((pc.flags & F_FGE) != 0u) {
        int fog = int(v_zf.y);
        ivec3 fc = ivec3(pc.fogcol & 255u, (pc.fogcol >> 8) & 255u, (pc.fogcol >> 16) & 255u);
        c.rgb = (c.rgb * fog + fc * (255 - fog)) >> 8;
    }
    if ((pc.flags & F_ATE) != 0u) {
        uint atst = (pc.flags >> 7) & 7u; bool pass;
        if (atst == 0u) pass = false; else if (atst == 1u) pass = true;
        else if (atst == 2u) pass = c.a < pc.aref; else if (atst == 3u) pass = c.a <= pc.aref;
        else if (atst == 4u) pass = c.a == pc.aref; else if (atst == 5u) pass = c.a >= pc.aref;
        else if (atst == 6u) pass = c.a > pc.aref; else pass = c.a != pc.aref;
        uint pm = (pc.flags >> 11) & 3u;          /* 0/1: keep passing fragments, 2: keep failing ones */
        if (pm == 2u ? pass : !pass) discard;
    }
#ifdef MV_PASS
    {   float zt = min(floor(v_zf.x), pc.zp.y);
        gl_FragDepth = zt * pc.zp.x;                       /* the colour pass's depth: same visibility */
        if (pc.zp.y > 4.0e9 && zt >= 2147483648.0) zt -= 4294967296.0;      /* 32-bit Z wrapped below zero: beyond the far plane */
        if (((pc.flags >> 11) & 3u) >= 2u) o_col = vec4(0.0, 0.0, min(float(c.a) / 128.0, 1.0) * 0.9, 0.0);
        else o_col = vec4(v_mv.xy / v_mv.z * pc.rt.z, 0.0, clamp((zt - pc.zp.w) * pc.tex.w, 0.0, 1.0));
        return;
    }
#else
    float fac = min(float(c.a) / 128.0, 1.0);
    if ((pc.flags & (F_SHBLEND | F_DATE)) != 0u || pc.fbmsk != 0u) {
        ivec4 d = ivec4(texelFetch(u_dst, ivec2(gl_FragCoord.xy), 0) * 255.0 + 0.5);
        if ((pc.flags & F_DATE) != 0u && ((d.a >> 7) != 0) != ((pc.flags & F_DATM) != 0u)) discard;
        if ((pc.flags & F_SHBLEND) != 0u) {
            uint aa = pc.blend & 3u, ab = (pc.blend >> 2) & 3u, ac = (pc.blend >> 4) & 3u, ad = (pc.blend >> 6) & 3u;
            int cf = ac == 0u ? c.a : ac == 1u ? d.a : int(pc.blend >> 8);
            ivec3 r = ivec3(sel(aa, c.r, d.r) - sel(ab, c.r, d.r), sel(aa, c.g, d.g) - sel(ab, c.g, d.g), sel(aa, c.b, d.b) - sel(ab, c.b, d.b));
            r = ((r * cf) >> 7) + ivec3(sel(ad, c.r, d.r), sel(ad, c.g, d.g), sel(ad, c.b, d.b));
            c.rgb = (pc.flags & F_COLCLAMP) != 0u ? clamp(r, 0, 255) : (r & 255);
        }
        if ((pc.flags & F_FBA) != 0u) c.a |= 128;
        ivec4 m = ivec4(pc.fbmsk & 255u, (pc.fbmsk >> 8) & 255u, (pc.fbmsk >> 16) & 255u, pc.fbmsk >> 24);
        c = (c & ~m) | (d & m);
    } else if ((pc.flags & F_FBA) != 0u) c.a |= 128;
    o_col = vec4(c) / 255.0;
    o_fac = vec4(fac);
    gl_FragDepth = min(floor(v_zf.x), pc.zp.y) * pc.zp.x;
#endif
}
