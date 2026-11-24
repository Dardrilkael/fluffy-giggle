#version 450
// Mesh (r3d) fragment shader: one directional light + an ambient floor,
// shading a tinted (optionally textured) surface. Two tiers from one
// source, the batch.frag pattern (the compile script picks with
// -DWS_BINDLESS; the array sizes are comment-tied to batch_core.h and the
// descriptor set layout's binding count):
// - default (chunked tier): set 0 is a 32-slot table the flush filled for
//   THIS draw; the draw's texture is always entry 0 (white when the draw
//   is untextured), so the index is the constant 0.
// - WS_BINDLESS: set 0 is the GLOBAL 1024-slot table and pc.tex_slot is
//   the texture's registry slot (0 = the built-in white). The slot is
//   uniform across one draw but the nonuniformEXT qualifier rides the
//   INDEX anyway (batch.frag's rule: it decorates the access chain).
//
// Push block, fragment half (bytes 112..119; the vertex half is 0..111):
//   tint      0xAABBGGRR packed so unpackUnorm4x8 yields (r, g, b, a) -
//             the CPU swaps r2d's 0xRRGGBBAA at record->flush time
//   tex_slot  see above
// Set 1 binding 0 is the frame slot's scene block (the one light).

#ifdef WS_BINDLESS
#extension GL_EXT_nonuniform_qualifier : require
#endif

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 out_color;

#ifdef WS_BINDLESS
layout(set = 0, binding = 0) uniform sampler2D u_textures[1024];
#else
layout(set = 0, binding = 0) uniform sampler2D u_textures[32];
#endif

layout(push_constant) uniform PushConstants {
    layout(offset = 112) uint tint;
    layout(offset = 116) uint tex_slot;
} pc;

// std140: two vec4s. light_dir.xyz is the UNIT direction TO the light
// (the CPU normalizes at flush); light_color.rgb is the light's linear
// color/intensity [0, inf); ambient is the flat floor every surface keeps.
layout(set = 1, binding = 0, std140) uniform Scene {
    vec4 light_dir;
    vec4 light_color_ambient;  // rgb = color, a = ambient
} scene;

void main() {
    const vec3 n = normalize(v_normal);
    const float ndl = max(dot(n, scene.light_dir.xyz), 0.0);
    const float ambient = scene.light_color_ambient.a;
    // The lit range at color 1 is exactly [ambient, 1]: ambient is the
    // floor, the directional term fills the rest. A color above 1
    // overdrives (the output clamps in the attachment).
    const vec3 light =
        vec3(ambient) + (1.0 - ambient) * scene.light_color_ambient.rgb * ndl;

#ifdef WS_BINDLESS
    const vec4 texel = texture(u_textures[nonuniformEXT(int(pc.tex_slot))], v_uv);
#else
    const vec4 texel = texture(u_textures[0], v_uv);
#endif
    const vec4 albedo = unpackUnorm4x8(pc.tint) * texel;
    out_color = vec4(albedo.rgb * light, albedo.a);
}
