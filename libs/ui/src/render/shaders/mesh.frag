#version 450
// Mesh (r3d) fragment shader: one directional light + up to 8 point lights
// + an ambient floor, Lambert diffuse and a Blinn-Phong highlight, shading
// a tinted (optionally textured) surface. Two tiers from one source, the
// batch.frag pattern (the compile script picks with -DWS_BINDLESS; the
// array sizes are comment-tied to batch_core.h and the descriptor set
// layout's binding count):
// - default (chunked tier): set 0 is a 32-slot table the flush filled for
//   THIS draw; the draw's texture is always entry 0 (white when the draw
//   is untextured), so the index is the constant 0.
// - WS_BINDLESS: set 0 is the GLOBAL 1024-slot table and pc.tex_slot is
//   the texture's registry slot (0 = the built-in white). The slot is
//   uniform across one draw but the nonuniformEXT qualifier rides the
//   INDEX anyway (batch.frag's rule: it decorates the access chain).
//
// Push block, fragment half (bytes 96..127; the vertex half is 0..95):
//   tint      0xAABBGGRR packed so unpackUnorm4x8 yields (r, g, b, a)
//   tex_slot  see above
//   surface   bits 0-7 the highlight strength (unorm), bits 8-15 the
//             shininess exponent (renderer3d_math.h pack_surface)
//   flags     bit 0: this draw RECEIVES shadows
//   eye       the block's camera position in world space (for the view
//             vector the highlight needs)
// Set 1 is the frame slot's scene: binding 0 the block (the lights and the
// shadow matrix), binding 1 the slot's directional shadow map (a depth image
// behind a comparison sampler: a lookup returns the lit fraction).

#ifdef WS_BINDLESS
#extension GL_EXT_nonuniform_qualifier : require
#endif

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec3 v_world;

layout(location = 0) out vec4 out_color;

#ifdef WS_BINDLESS
layout(set = 0, binding = 0) uniform sampler2D u_textures[1024];
#else
layout(set = 0, binding = 0) uniform sampler2D u_textures[32];
#endif

layout(push_constant) uniform PushConstants {
    layout(offset = 96) uint tint;
    layout(offset = 100) uint tex_slot;
    layout(offset = 104) uint surface;
    layout(offset = 108) uint flags;
    layout(offset = 112) vec4 eye;
} pc;

// std140, comment-tied to renderer3d_math.h's SceneBlock (kMaxPointLights).
struct PointLight {
    vec4 position_radius;  // xyz world position, w radius
    vec4 color;            // rgb
};
layout(set = 1, binding = 0, std140) uniform Scene {
    vec4 light_dir;            // xyz: the UNIT direction TO the light
    vec4 light_color_ambient;  // rgb light color, a the ambient floor
    ivec4 point_count;         // x: live entries of points[]
    PointLight points[8];
    mat4 light_view_proj;      // world -> the shadow map's clip space
    vec4 shadow_params;        // x: 1 = a shadow map is live, y: one texel in
                               // uv units (1 / resolution), w: normal offset
} scene;
layout(set = 1, binding = 1) uniform sampler2DShadow u_shadow;

// The fraction of the directional light that reaches this fragment: 1 = fully
// lit, 0 = fully shadowed. A 3x3 grid of comparison taps, each already a
// bilinear 2x2 compare in hardware - soft, cheap edges. Anything outside the
// map (or beyond its depth range) is treated as lit. textureLod, not
// texture(): the early returns make this non-uniform control flow, where an
// implicit derivative would be undefined (the map has one level anyway). The
// tap offsets are computed in uv space (textureLodOffset would need
// compile-time-constant offsets, which a loop cannot give).
float shadow_factor(vec3 world, vec3 n) {
    if (scene.shadow_params.x < 0.5 || (pc.flags & 1u) == 0u) {
        return 1.0;
    }
    // Move the lookup off the surface along its normal: kills acne on
    // surfaces that graze the light.
    const vec4 clip = scene.light_view_proj * vec4(world + n * scene.shadow_params.w, 1.0);
    const vec2 uv = clip.xy * 0.5 + 0.5;
    const float depth = clip.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || depth > 1.0) {
        return 1.0;
    }
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const vec2 tap = uv + vec2(float(x), float(y)) * scene.shadow_params.y;
            lit += textureLod(u_shadow, vec3(tap, depth), 0.0);
        }
    }
    return lit / 9.0;
}

// One light's contribution: Lambert diffuse into `diffuse`, the Blinn-Phong
// highlight into `highlight` (only lit-side surfaces get one).
void shade(vec3 to_light, vec3 color, float atten, vec3 n, vec3 view,
           float shininess, bool want_highlight,
           inout vec3 diffuse, inout vec3 highlight) {
    const float ndl = max(dot(n, to_light), 0.0);
    diffuse += color * (ndl * atten);
    if (want_highlight && ndl > 0.0) {
        const vec3 h = normalize(to_light + view);
        highlight += color * (pow(max(dot(n, h), 0.0), shininess) * atten);
    }
}

// Tiled UV support (greedy-merged quads): a UV component ABOVE 1.0
// wraps fract-style so one texture repeats across an N-wide quad.
// In-range values - including exactly 1.0 - pass through untouched,
// so every existing mesh (all UVs in [0,1]) renders bit-identically.
// The sampler is CLAMP_TO_EDGE; the wrap happens here, in-shader.
// Requires GLSL derivatives: the dFdx/dFdy below already sit behind
// no extension pragma because #version 450 core covers them.
vec2 wrap_tiled(vec2 uv) {
    vec2 r = uv;
    if (r.x > 1.0) { r.x = fract(r.x); }
    if (r.y > 1.0) { r.y = fract(r.y); }
    return r;
}

void main() {
    const vec3 n = normalize(v_normal);
    const vec3 view = normalize(pc.eye.xyz - v_world);
    const float strength = float(pc.surface & 0xFFu) / 255.0;
    const float shininess = float((pc.surface >> 8) & 0xFFu);
    const bool want_highlight = strength > 0.0;

    vec3 diffuse = vec3(0.0);
    vec3 highlight = vec3(0.0);
    // The directional light is the one that casts shadows: its diffuse AND
    // highlight scale by how much of it arrives.
    shade(scene.light_dir.xyz, scene.light_color_ambient.rgb, shadow_factor(v_world, n),
          n, view, shininess, want_highlight, diffuse, highlight);
    const int count = min(scene.point_count.x, 8);
    for (int i = 0; i < count; ++i) {
        const vec3 offset = scene.points[i].position_radius.xyz - v_world;
        const float dist = length(offset);
        const float radius = scene.points[i].position_radius.w;
        // Smooth squared falloff to exactly 0 at the radius.
        float atten = clamp(1.0 - dist / radius, 0.0, 1.0);
        atten *= atten;
        if (atten > 0.0) {
            shade(offset / max(dist, 1e-4), scene.points[i].color.rgb, atten,
                  n, view, shininess, want_highlight, diffuse, highlight);
        }
    }

    // The lit range with one white light is exactly [ambient, 1]: ambient is
    // the floor, the lights fill the rest (several lights, or a color above
    // 1, overdrive; the attachment clamps).
    const float ambient = scene.light_color_ambient.a;
    const vec3 light = vec3(ambient) + (1.0 - ambient) * diffuse;

    // textureGrad with the UNWRAPPED uv's derivatives: the LOD comes
    // from the smooth pre-wrap coordinate (a REPEAT sampler's own
    // math), so a repeat seam never spikes the mip selection - plain
    // texture() on the fract'd coordinate would flash a coarse-mip
    // line at every integer boundary. (Taken before any divergent
    // branch below the lights: derivatives need uniform control flow.)
    const vec2 wuv = wrap_tiled(v_uv);
    const vec2 grad_x = dFdx(v_uv);
    const vec2 grad_y = dFdy(v_uv);
#ifdef WS_BINDLESS
    const vec4 texel = textureGrad(
        u_textures[nonuniformEXT(int(pc.tex_slot))], wuv, grad_x, grad_y);
#else
    const vec4 texel = textureGrad(u_textures[0], wuv, grad_x, grad_y);
#endif
    const vec4 albedo = unpackUnorm4x8(pc.tint) * texel;
    // The highlight is the lights' own color, added on top of the albedo.
    out_color = vec4(albedo.rgb * light + highlight * strength, albedo.a);
}
