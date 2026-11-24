#pragma once

// Internal: the mesh path's pure-CPU placement math - the part of the
// Phase 2 flush that needs no Vulkan, so the headless suite pins it. This
// header never reaches apps (the public surface is <ws/ui/renderer3d.hpp>).
//
// What lives here:
//   - model_matrix(): r3d::Transform3D -> the 3x4 the vertex shader takes
//   - pack_tint(): 0xRRGGBBAA (r2d's packing) -> the word unpackUnorm4x8
//     reads back as (r, g, b, a)
//   - MeshPush / SceneBlock: the exact byte layouts of the mesh pipeline's
//     push constants and its set-1 scene uniform block, comment-tied to
//     shaders/mesh.vert / mesh.frag and the pipeline layout in
//     renderer2d_device.cpp

#include <cmath>
#include <cstddef>
#include <cstdint>

#include <ws/ui/renderer3d.hpp>

namespace ws::ui::r3d::detail {

// The draw's model matrix, one ROW per entry: world = rows[i] . (p, 1).
// The 3x3 part is  scale * Ry(yaw) * Rx(pitch) * Rz(roll)  - standard
// right-hand-rule matrices, the aircraft "heading, then tilt, then bank"
// reading: yaw turns the object about the world +Y axis, pitch tilts it
// about its own (already yawed) X axis, roll banks it about its own Z
// axis. The translation rides column 3. Uniform scale only, so the same
// 3x3 carries normals correctly up to length (the fragment shader
// renormalizes).
struct Model3x4 {
    float rows[3][4] = {};
};

[[nodiscard]] inline Model3x4 model_matrix(const Transform3D& t) {
    const float cy = std::cos(t.yaw);
    const float sy = std::sin(t.yaw);
    const float cx = std::cos(t.pitch);
    const float sx = std::sin(t.pitch);
    const float cz = std::cos(t.roll);
    const float sz = std::sin(t.roll);
    // Ry * (Rx * Rz), multiplied out (the suite checks it against a
    // generic 3x3 product, so a transcription slip cannot hide).
    const float r[3][3] = {
        {cy * cz + sy * sx * sz, -cy * sz + sy * sx * cz, sy * cx},
        {cx * sz, cx * cz, -sx},
        {-sy * cz + cy * sx * sz, sy * sz + cy * sx * cz, cy * cx},
    };
    Model3x4 m;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            m.rows[row][col] = r[row][col] * t.scale;
        }
        m.rows[row][3] = t.translation[row];
    }
    return m;
}

// r2d's 0xRRGGBBAA -> 0xAABBGGRR: little-endian, the lowest byte is R,
// which is the byte unpackUnorm4x8 returns as .x.
[[nodiscard]] constexpr std::uint32_t pack_tint(std::uint32_t rgba) {
    const std::uint32_t r = (rgba >> 24) & 0xFFu;
    const std::uint32_t g = (rgba >> 16) & 0xFFu;
    const std::uint32_t b = (rgba >> 8) & 0xFFu;
    const std::uint32_t a = rgba & 0xFFu;
    return r | (g << 8) | (b << 16) | (a << 24);
}

// The mesh pipeline's push constants: ONE range over all 120 bytes,
// visible to both stages (the vertex shader reads [0, 112) - vp + the
// three model rows - and the fragment shader [112, 120) - tint, tex_slot).
// One range means one vkCmdPushConstants per draw. 120 of the 128 bytes
// every Vulkan device guarantees.
struct MeshPush {
    float vp[16];         // column-major, batch::Mat4's layout
    float model[3][4];    // Model3x4::rows
    std::uint32_t tint;   // pack_tint()
    std::uint32_t tex_slot;  // bindless: the registry slot; chunked: unused
};
static_assert(sizeof(MeshPush) == 120, "mesh push layout is load-bearing");
inline constexpr std::uint32_t kMeshPushVertexSize = 112;
inline constexpr std::uint32_t kMeshPushFragmentOffset = 112;
inline constexpr std::uint32_t kMeshPushFragmentSize = 8;
static_assert(offsetof(MeshPush, tint) == kMeshPushFragmentOffset);
static_assert(offsetof(MeshPush, model) + sizeof(float) * 12 ==
              kMeshPushVertexSize);

// Which winding the mesh pipeline treats as FRONT (cull mode is back).
// Pinned by the suite against the generators and batch::camera_vp3d.
inline constexpr bool kMeshFrontFaceCcw = true;

// The set-1 scene block (std140: two vec4s). dir is the UNIT direction TO
// the light; color_ambient.rgb the light color, .a the ambient floor.
struct SceneBlock {
    float light_dir[4];
    float light_color_ambient[4];
};
static_assert(sizeof(SceneBlock) == 32);

// The scene block for a light, with the direction normalized (set_light
// stores it as given and guarantees it is non-zero). A negative color
// channel or ambient (nonsense in [0, inf)) clamps to 0 rather than
// darkening below black.
[[nodiscard]] inline SceneBlock make_scene_block(const Light3D& light) {
    SceneBlock block{};
    const float len = std::sqrt(light.dir[0] * light.dir[0] +
                                light.dir[1] * light.dir[1] +
                                light.dir[2] * light.dir[2]);
    const float inv = len > 0.0f ? 1.0f / len : 0.0f;
    for (int i = 0; i < 3; ++i) {
        block.light_dir[i] = light.dir[i] * inv;
        block.light_color_ambient[i] = light.color[i] > 0.0f ? light.color[i] : 0.0f;
    }
    // Ambient is the floor of a [ambient, 1] range (mesh.frag), so it lives
    // in [0, 1]: above 1 the (1 - ambient) fill term would go negative.
    block.light_color_ambient[3] =
        light.ambient < 0.0f ? 0.0f : (light.ambient > 1.0f ? 1.0f : light.ambient);
    return block;
}

}  // namespace ws::ui::r3d::detail
