#version 450
/* Upload of native-resolution pixels into a scaled render target: every
 * native pixel becomes an N x N block (texelFetch, no filtering). */
layout(push_constant) uniform PC { vec4 rt; vec4 src; } pc;   /* src: x, y offset of the block in the target (device px), scale */
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_col;
void main() { o_col = texelFetch(u_tex, ivec2((gl_FragCoord.xy - pc.src.xy) / pc.src.z), 0); }
