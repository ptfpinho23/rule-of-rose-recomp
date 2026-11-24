#version 450
/* GS hardware renderer: vertex stage. Positions arrive in native GS pixels
 * (window coordinates after XYOFFSET, 1/16 pixel precision kept as floats).
 * Mapping to the scaled target: gpu = native * N + 0.5, i.e. the GS sample
 * point of native pixel c is the centre of the first sub-pixel of its N x N
 * block. At N = 1 this is exactly the GS rule (sample at integer coordinates,
 * top-left fill); at N > 1 an edge on an integer native coordinate still
 * falls on a block boundary, so screen-aligned quads and sprites cover whole
 * blocks: no seams, no half-pixel shift of 2D. */
layout(push_constant) uniform PC {
    vec4 rt;          /* 2/width, 2/height of the target in device pixels, scale N, uv shift */
    uint flags; int aref; uint fogcol; uint blend;
    ivec4 region;
    vec4 tex;         /* texture width, height (native texels), texel scale, unused */
    vec4 zp;          /* depth scale, depth max, ta0, early-depth margin (0: the rasterizer's depth is not used) */
    uint fbmsk;
} pc;
layout(location = 0) in vec4 a_pos;     /* x, y, z, fog */
layout(location = 1) in vec3 a_stq;
layout(location = 2) in vec4 a_col;     /* 0..255 */
layout(location = 3) in vec4 a_spr;     /* sprites: x0, y0, du/dx, dv/dy (same on all vertices) */
layout(location = 0) noperspective out vec4 v_col;
layout(location = 1) noperspective out vec3 v_stq;
layout(location = 2) noperspective out vec2 v_zf;
layout(location = 3) flat out vec4 v_spr;
#ifdef MV_PASS
/* Motion-vector pass (FSR 3.1): a_spr.xy of a triangle carries its screen
 * motion, previous minus current position in native pixels; interpolated
 * perspective-correctly where the primitive has a Q. */
layout(location = 4) noperspective out vec3 v_mv;
#endif
void main() {
    vec2 p = a_pos.xy * pc.rt.z + 0.5;
    float zr = 0.0;
#ifndef MV_PASS
    /* Early depth test (gs.frag, EARLY_Z): the rasterizer interpolates a depth that is never below the one the
     * fragment shader writes (which is floor(z) of the same vertex values): z plus a margin for the interpolation's
     * rounding. Depth clamping is on for these pipelines, so nothing is clipped where it exceeds 1. */
    if (pc.zp.w > 0.0) zr = (a_pos.z + pc.zp.w) * pc.zp.x;
#endif
    gl_Position = vec4(p * pc.rt.xy - 1.0, zr, 1.0);
    v_col = a_col; v_stq = a_stq; v_zf = a_pos.zw; v_spr = a_spr;
#ifdef MV_PASS
    float q = ((pc.flags & 3u) == 1u && a_stq.z > 0.0) ? a_stq.z : 1.0;      /* textured with STQ */
    v_mv = vec3(a_spr.xy * q, q);
#endif
}
