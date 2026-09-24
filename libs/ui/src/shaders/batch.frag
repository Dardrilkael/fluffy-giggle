#version 450
// 2D batch fragment shader. u_textures is the chunk's sampler table; the
// array size must equal kMaxTexturesPerChunk in batch_core.h and the
// descriptor set layout's binding count (all three are comment-tied).

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in flat float v_tex_index;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_textures[32];

void main() {
    const int index = int(v_tex_index + 0.5);  // round: never truncate
    out_color = v_color * texture(u_textures[index], v_uv);
}
