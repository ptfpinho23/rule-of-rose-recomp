#version 450
/* Presentation passes (one fragment shader, pc.opt.w selects):
 *  0 simple   target <- render target: nearest or bilinear, colour controls
 *  1 scale    stage A (float) <- render target: nearest, bilinear, FSR 1.0
 *             EASU (upscale) or an area filter (when the internal picture is
 *             larger than the output)
 *  2 sharpen  stage B <- stage A: FSR 1.0 RCAS (optional), then the colour
 *             controls: brightness/contrast/gamma table (u_dst, 256x1) and
 *             saturation, with post.c's integer arithmetic
 *  3 compose  target <- stage B, plus the glow (u_dst) screen-blended
 *  4 glow     glow <- render target: 4x4-cell averages, colour controls,
 *             highlight weight (post.c bloom_prepare)
 *  5/6 blur   box blur of the glow, horizontal / vertical
 *  7 depth    render-target rows <- 16-bit depth memory read as 32-bit
 *             colour (GS local memory seen through another format): u_tex
 *             is a 64x32 table giving, for each pixel of a 32-bit page, the
 *             two depth pixels of a 16-bit page whose words it is made of;
 *             u_dst is the depth image
 * EASU and RCAS follow AMD FidelityFX Super Resolution 1.0 (ffx_fsr1.h,
 * FsrEasuF / FsrRcasF; Copyright (c) 2021 Advanced Micro Devices, Inc., MIT
 * licence, text in ../ffx_fsr1_license.txt), written out for GLSL 450. */
layout(push_constant) uniform PC {
    vec4 src;      /* source rectangle in texels: x, y, w, h */
    vec4 dst;      /* destination rectangle in pixels: x, y, w, h */
    ivec4 opt;     /* x: filter (0 nearest, 1 bilinear, 2 EASU, 3 area) or flag; y: table / glow on; z: saturation * 64 / 100; w: pass */
    vec4 par;      /* 2: RCAS constant (0 = off); 4: bloom strength 0..100; 5/6: radius */
} pc;
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(set = 1, binding = 0) uniform sampler2D u_dst;
layout(location = 0) out vec4 o_col;

ivec3 colour(ivec3 v) {
    if (pc.opt.y != 0) v = ivec3(int(texelFetch(u_dst, ivec2(v.r, 0), 0).r * 255.0 + 0.5), int(texelFetch(u_dst, ivec2(v.g, 0), 0).r * 255.0 + 0.5),
                                 int(texelFetch(u_dst, ivec2(v.b, 0), 0).r * 255.0 + 0.5));
    if (pc.opt.z != 64) {
        int l = (77 * v.r + 150 * v.g + 29 * v.b + 128) >> 8;
        v = clamp(l + (((v - l) * pc.opt.z + 32) >> 6), 0, 255);
    }
    return v;
}
vec3 src_px(ivec2 p) {          /* texel of the source rectangle, edges replicated */
    ivec2 lo = ivec2(pc.src.xy), hi = ivec2(pc.src.xy + pc.src.zw) - 1;
    return texelFetch(u_tex, clamp(p + lo, lo, hi), 0).rgb;
}
float lo_rcp(float a) { return uintBitsToFloat(0x7ef07ebbu - floatBitsToUint(a)); }
float lo_rsq(float a) { return uintBitsToFloat(0x5f347d74u - (floatBitsToUint(a) >> 1)); }
float med_rcp(float a) { float b = uintBitsToFloat(0x7ef19fffu - floatBitsToUint(a)); return b * (-b * a + 2.0); }
float luma2(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

void easu_set(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {
    float dc = lD - lC, cb = lC - lB;
    float lenX = lo_rcp(max(abs(dc), abs(cb))), dirX = lD - lB;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0); lenX *= lenX;
    float ec = lE - lC, ca = lC - lA;
    float lenY = lo_rcp(max(abs(ec), abs(ca))), dirY = lE - lA;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0); lenY *= lenY;
    dir += vec2(dirX, dirY) * w; len += (lenX + lenY) * w;
}
void easu_tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c) {
    vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * (-dir.y) + off.y * dir.x) * len;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = 2.0 / 5.0 * d2 - 1.0, wA = lob * d2 - 1.0;
    wB *= wB; wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w; aW += w;
}
vec3 easu(vec2 op) {            /* op: position inside the destination rectangle, pixels */
    vec2 pp = (op + 0.5) * pc.src.zw / pc.dst.zw - 0.5, fp = floor(pp);
    pp -= fp;
    ivec2 i = ivec2(fp);
    vec3 b = src_px(i + ivec2(0, -1)), c = src_px(i + ivec2(1, -1));
    vec3 e = src_px(i + ivec2(-1, 0)), f = src_px(i), g = src_px(i + ivec2(1, 0)), h = src_px(i + ivec2(2, 0));
    vec3 ii = src_px(i + ivec2(-1, 1)), j = src_px(i + ivec2(0, 1)), k = src_px(i + ivec2(1, 1)), l = src_px(i + ivec2(2, 1));
    vec3 n = src_px(i + ivec2(0, 2)), o = src_px(i + ivec2(1, 2));
    float bL = luma2(b), cL = luma2(c), eL = luma2(e), fL = luma2(f), gL = luma2(g), hL = luma2(h), iL = luma2(ii), jL = luma2(j),
          kL = luma2(k), lL = luma2(l), nL = luma2(n), oL = luma2(o);
    vec2 dir = vec2(0.0); float len = 0.0;
    easu_set(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    easu_set(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    easu_set(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    easu_set(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
    float dirR = dir.x * dir.x + dir.y * dir.y;
    bool zro = dirR < 1.0 / 32768.0;
    dirR = zro ? 1.0 : lo_rsq(dirR);
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5; len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) * lo_rcp(max(abs(dir.x), abs(dir.y)));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len, clp = lo_rcp(lob);
    vec3 mn = min(min(f, g), min(j, k)), mx = max(max(f, g), max(j, k));
    vec3 aC = vec3(0.0); float aW = 0.0;
    easu_tap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    easu_tap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    easu_tap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, ii);
    easu_tap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    easu_tap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    easu_tap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    easu_tap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    easu_tap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    easu_tap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    easu_tap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    easu_tap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    easu_tap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
    return min(mx, max(mn, aC / aW));
}
vec3 bilinear(vec2 uv) {        /* uv in source texels */
    vec2 lo = pc.src.xy + 0.5, hi = pc.src.xy + pc.src.zw - 0.5;
    return texture(u_tex, clamp(uv, lo, hi) / vec2(textureSize(u_tex, 0))).rgb;
}
/* Filter 0 ("nearest"): every output pixel that lies inside one source texel
 * shows that texel; a pixel that straddles two shows them in proportion. At
 * whole-number scales this is plain nearest; in between it avoids texels of
 * unequal width. u_tex has the bilinear sampler. */
vec3 sharp(vec2 uv) {
    vec2 k = pc.dst.zw / pc.src.zw;
    vec2 fl = floor(uv), d = uv - fl - 0.5, e = 0.5 - abs(d);
    vec2 off = sign(d) * max(vec2(0.0), 0.5 - e * max(k, vec2(1.0)));
    return bilinear(fl + 0.5 + off);
}
vec3 area(vec2 op) {            /* mean of the source texels under the output pixel (bilinear taps, each 2x2 texels) */
    vec2 sc = pc.src.zw / pc.dst.zw, a = pc.src.xy + op * sc;
    ivec2 n = clamp(ivec2(ceil(sc * 0.5)), ivec2(1), ivec2(8));
    vec3 s = vec3(0.0);
    for (int y = 0; y < n.y; y++) for (int x = 0; x < n.x; x++) s += bilinear(a + (vec2(x, y) + 0.5) / vec2(n) * sc);
    return s / float(n.x * n.y);
}
vec3 rcas(ivec2 p) {            /* u_tex: stage A, same size as the destination rectangle */
    ivec2 hi = ivec2(pc.dst.zw) - 1;
    vec3 b = texelFetch(u_tex, clamp(p + ivec2(0, -1), ivec2(0), hi), 0).rgb, d = texelFetch(u_tex, clamp(p + ivec2(-1, 0), ivec2(0), hi), 0).rgb,
         e = texelFetch(u_tex, p, 0).rgb, f = texelFetch(u_tex, clamp(p + ivec2(1, 0), ivec2(0), hi), 0).rgb,
         h = texelFetch(u_tex, clamp(p + ivec2(0, 1), ivec2(0), hi), 0).rgb;
    if (pc.par.x <= 0.0) return e;
    vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));
    vec3 hitMin = min(mn4, e) / (4.0 * mx4), hitMax = (1.0 - max(mx4, e)) / (4.0 * mn4 - 4.0);
    vec3 lobeC = max(-hitMin, hitMax);
    /* a 0/0 where the neighbourhood is pure black or white: take the other operand, as post.c does */
    lobeC = mix(lobeC, -hitMin, isnan(hitMax)); lobeC = mix(lobeC, hitMax, isnan(hitMin));
    float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeC.r, max(lobeC.g, lobeC.b)), 0.0)) * pc.par.x;
    float rcpL = med_rcp(4.0 * lobe + 1.0);
    return (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;
}
void main() {
    vec2 op = gl_FragCoord.xy - pc.dst.xy;       /* position inside the destination rectangle */
    int pass = pc.opt.w;
    if (pass == 0) {
        vec2 uv = pc.src.xy + op / pc.dst.zw * pc.src.zw;
        vec3 c = pc.opt.x != 0 ? bilinear(uv) : sharp(uv);
        o_col = vec4(vec3(colour(ivec3(c * 255.0 + 0.5))) / 255.0, 1.0);
    } else if (pass == 1) {
        vec2 uv = pc.src.xy + op / pc.dst.zw * pc.src.zw;
        vec3 c = pc.opt.x == 2 ? easu(floor(op)) : pc.opt.x == 3 ? area(floor(op)) : pc.opt.x == 1 ? bilinear(uv) : sharp(uv);
        o_col = vec4(c, 1.0);
    } else if (pass == 2) {
        vec3 c = clamp(rcas(ivec2(op)), 0.0, 1.0);
        o_col = vec4(vec3(colour(ivec3(c * 255.0 + 0.5))) / 255.0, 1.0);
    } else if (pass == 3) {
        ivec3 c = ivec3(texelFetch(u_tex, ivec2(op), 0).rgb * 255.0 + 0.5);
        if (pc.opt.y != 0) {                     /* screen: c + g * (255 - c) / 255, glow sampled bilinearly at pixel centres */
            ivec3 g = ivec3(texture(u_dst, (op / pc.dst.zw)).rgb * 255.0 + 0.5);
            ivec3 t = g * (255 - c) + 128;
            c += (t + (t >> 8)) >> 8;
        }
        o_col = vec4(vec3(c) / 255.0, 1.0);
    } else if (pass == 4) {
        /* glow cell = 4x4 native pixels of the picture: 16 taps at their centres */
        vec2 cell = pc.src.zw / pc.dst.zw, a = pc.src.xy + floor(op) * cell;
        vec3 s = vec3(0.0);
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) s += bilinear(a + (vec2(x, y) + 0.5) * cell * 0.25);
        ivec3 c = colour(ivec3(s / 16.0 * 255.0 + 0.5));
        int l = ((77 * c.r + 150 * c.g + 29 * c.b + 128) >> 8) - 40;
        l = min(l, 110);
        int k = l <= 0 ? 0 : l * int(pc.par.x) * 256 / (110 * 100);
        o_col = vec4(vec3((c * k + 128) >> 8) / 255.0, 1.0);
    } else if (pass == 7) {
        /* opt.x: scale N, opt.y: page distance from the depth buffer's first page, opt.z: buffer width in pages; dst.xy: device origin of the rows */
        int N = pc.opt.x;
        ivec2 q = ivec2(op) / N;                               /* native pixel inside the rows */
        int page = pc.opt.y + (q.y >> 5) * pc.opt.z + (q.x >> 6);
        ivec4 t = ivec4(texelFetch(u_tex, ivec2(q.x & 63, q.y & 31), 0) * 255.0 + 0.5);
        ivec2 zo = ivec2((page % pc.opt.z) * 64, (page / pc.opt.z) * 64);
        int z0 = int(texelFetch(u_dst, (zo + t.xy) * N, 0).r * 65536.0 + 0.5), z1 = int(texelFetch(u_dst, (zo + t.zw) * N, 0).r * 65536.0 + 0.5);
        o_col = vec4(float(z0 & 255), float((z0 >> 8) & 255), float(z1 & 255), float((z1 >> 8) & 255)) / 255.0;
    } else {
        ivec2 p = ivec2(op), hi = ivec2(pc.dst.zw) - 1, step = pass == 5 ? ivec2(1, 0) : ivec2(0, 1);
        int r = int(pc.par.x); vec3 s = vec3(0.0);
        for (int k = -r; k <= r; k++) s += texelFetch(u_tex, clamp(p + step * k, ivec2(0), hi), 0).rgb;
        o_col = vec4(s / float(2 * r + 1), 1.0);
    }
}
