#version 450
/* Passes around the temporal upscaler (--filter fsr3; gs_hw.c, "FSR 3.1").
 * pc.opt.w selects:
 *  0 extract  target <- texel of u_tex at (fragment + src.xy): opt.x 0 all
 *             channels, 1 alpha (depth), 2 blue (reactive mask)
 *  1 compose  target <- upscaled picture (u_tex, 1:1) * overlay "on white"
 *  2 compose  target += (1 - upscaled picture) * overlay "on black"
 *             (u_dst: the 2D layer drawn over black / over white at its own
 *             scale, rectangle src; together: picture * (W - B) + B, which
 *             is the 2D layer's blending applied to the upscaled picture)
 *  3 copy     target <- u_tex 1:1
 *  4 view     target <- u_tex rectangle src, bilinear (debug views): opt.x 0
 *             colour, 1 motion (rg, scaled by par.x, around grey), 2 alpha,
 *             3 blue */
layout(push_constant) uniform PC { vec4 src; vec4 dst; ivec4 opt; vec4 par; } pc;
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(set = 1, binding = 0) uniform sampler2D u_dst;
layout(location = 0) out vec4 o_col;
void main() {
    ivec2 g = ivec2(gl_FragCoord.xy);
    if (pc.opt.w == 0) {
        vec4 t = texelFetch(u_tex, g + ivec2(pc.src.xy), 0);
        o_col = pc.opt.x == 1 ? vec4(t.a) : pc.opt.x == 2 ? vec4(t.b) : t;
    } else if (pc.opt.w == 4) {
        vec2 uv = pc.src.xy + gl_FragCoord.xy * pc.src.zw / pc.dst.zw;
        uv = clamp(uv, pc.src.xy + 0.5, pc.src.xy + pc.src.zw - 0.5);
        vec4 t = texture(u_tex, uv / vec2(textureSize(u_tex, 0)));
        o_col = pc.opt.x == 1 ? vec4(0.5 + t.rg * pc.par.x, 0.5, 1.0) : pc.opt.x == 2 ? vec4(vec3(pow(t.a, 0.25)), 1.0) : pc.opt.x == 3 ? vec4(vec3(t.b), 1.0) : vec4(t.rgb, 1.0);
    } else {
        vec3 f = clamp(texelFetch(u_tex, g, 0).rgb, 0.0, 1.0);
        if (pc.opt.w == 3) { o_col = vec4(f, 1.0); return; }
        vec2 uv = pc.src.xy + gl_FragCoord.xy * pc.src.zw / pc.dst.zw;
        uv = clamp(uv, pc.src.xy + 0.5, pc.src.xy + pc.src.zw - 0.5);
        vec3 o = texture(u_dst, uv / vec2(textureSize(u_dst, 0))).rgb;
        o_col = vec4(pc.opt.w == 1 ? f * o : (1.0 - f) * o, 1.0);
    }
}
