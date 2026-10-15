#version 450
// 2D batch fragment shader, two tiers from one source (the compile script
// picks with -DWS_BINDLESS; both array sizes are comment-tied to
// batch_core.h and the descriptor set layout's binding count):
// - default (chunked tier): u_textures is the CHUNK's sampler table and
//   the index is the chunk-local slot (kMaxTexturesPerChunk).
// - WS_BINDLESS (bindless tier): u_textures is the GLOBAL texture table
//   and the index is the texture's registry id (kMaxBindlessTextures).
//   The id varies per draw, so the access is nonuniform - the
//   nonuniformEXT qualifier is what descriptor indexing promises it by.

#ifdef WS_BINDLESS
#extension GL_EXT_nonuniform_qualifier : require
#endif

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in flat float v_tex_index;

layout(location = 0) out vec4 out_color;

#ifdef WS_BINDLESS
layout(set = 0, binding = 0) uniform sampler2D u_textures[1024];
#else
layout(set = 0, binding = 0) uniform sampler2D u_textures[32];
#endif

void main() {
    const int index = int(v_tex_index + 0.5);  // round: never truncate
#ifdef WS_BINDLESS
    out_color = v_color * texture(nonuniformEXT(u_textures[index]), v_uv);
#else
    out_color = v_color * texture(u_textures[index], v_uv);
#endif
}
