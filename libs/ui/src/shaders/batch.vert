#version 450
// 2D batch vertex shader. One draw call per chunk; u_vp is the chunk's
// camera view-projection (push constant, 64 bytes). The vertex layout must
// match batch_core.h's Vertex exactly: pos, color, uv, tex_index. a_pos is
// vec3 - 2D draws land on z = 0, the 3D camera path owns z.

layout(push_constant) uniform PushConstants {
    mat4 u_vp;
} pc;

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in float a_tex_index;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out flat float v_tex_index;

void main() {
    gl_Position = pc.u_vp * vec4(a_pos, 1.0);
    v_color = a_color;
    v_uv = a_uv;
    v_tex_index = a_tex_index;
}
