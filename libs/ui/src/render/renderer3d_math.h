#pragma once

// Internal: the mesh path's pure-CPU placement math - the part of the
// Phase 2 flush that needs no Vulkan, so the headless suite pins it. This
// header never reaches apps (the public surface is <ws/ui/renderer3d.hpp>).
//
// What lives here:
//   - model_matrix(): r3d::Transform3D -> a 3x4 matrix (the reference form;
//     the flush ships the same rotation as a quaternion, model_quaternion())
//   - pack_tint(): 0xRRGGBBAA (r2d's packing) -> the word unpackUnorm4x8
//     reads back as (r, g, b, a)
//   - MeshPush / SceneBlock: the exact byte layouts of the mesh pipeline's
//     push constants and its set-1 scene uniform block, comment-tied to
//     shaders/mesh.vert / mesh.frag and the pipeline layout in
//     renderer2d_device.cpp

#include <algorithm>
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

// The same rotation as a unit quaternion (x, y, z, w), the form the vertex
// shader takes: q = qy * qx * qz, the quaternion image of
// Ry(yaw) * Rx(pitch) * Rz(roll). Shipping it (plus translation and scale,
// 8 floats) instead of the 3x4 matrix (12) is what leaves push-constant room
// for the eye position specular shading needs. The suite checks that
// rotating by this quaternion equals the matrix product.
struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

[[nodiscard]] inline Quat quat_mul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

[[nodiscard]] inline Quat model_quaternion(const Transform3D& t) {
    const Quat qy{0.0f, std::sin(t.yaw * 0.5f), 0.0f, std::cos(t.yaw * 0.5f)};
    const Quat qx{std::sin(t.pitch * 0.5f), 0.0f, 0.0f, std::cos(t.pitch * 0.5f)};
    const Quat qz{0.0f, 0.0f, std::sin(t.roll * 0.5f), std::cos(t.roll * 0.5f)};
    return quat_mul(qy, quat_mul(qx, qz));
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

// A bounding sphere, in whatever space the caller says (a mesh's local space
// as created, world space once placed).
struct BoundSphere {
    float center[3] = {0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
};

// The bounding sphere of a vertex set: the AABB's center, and the farthest
// vertex from it (not the minimal sphere, but always enclosing and cheap).
// An empty set is the zero sphere.
[[nodiscard]] inline BoundSphere bound_of(const Vertex3D* vertices, std::size_t count) {
    BoundSphere out;
    if (count == 0) {
        return out;
    }
    float lo[3] = {vertices[0].pos[0], vertices[0].pos[1], vertices[0].pos[2]};
    float hi[3] = {lo[0], lo[1], lo[2]};
    for (std::size_t i = 1; i < count; ++i) {
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], vertices[i].pos[k]);
            hi[k] = std::max(hi[k], vertices[i].pos[k]);
        }
    }
    for (int k = 0; k < 3; ++k) {
        out.center[k] = 0.5f * (lo[k] + hi[k]);
    }
    float far_sq = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        float d_sq = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float d = vertices[i].pos[k] - out.center[k];
            d_sq += d * d;
        }
        far_sq = std::max(far_sq, d_sq);
    }
    out.radius = std::sqrt(far_sq);
    return out;
}

// A mesh's local bounding sphere carried to world space by a draw's
// transform: the center is placed like any local point, the radius scales.
[[nodiscard]] inline BoundSphere world_bound(const BoundSphere& local,
                                             const Transform3D& t) {
    const Model3x4 m = model_matrix(t);  // rotation * scale, translation
    BoundSphere out;
    for (int row = 0; row < 3; ++row) {
        out.center[row] = m.rows[row][0] * local.center[0] +
                          m.rows[row][1] * local.center[1] +
                          m.rows[row][2] * local.center[2] + m.rows[row][3];
    }
    out.radius = local.radius * t.scale;
    return out;
}

// The directional shadow map's light-space matrix, fitted to a frame.
//   to_light   the direction TO the light (any length; zero is invalid)
//   casters    world-space spheres of everything that casts a shadow: they
//              set the map's x/y extent (so the resolution goes where the
//              shadows are)
//   receivers  world-space spheres of everything that receives one: with the
//              casters they set the DEPTH range, so a receiver far behind a
//              caster still lands inside the map's depth range
// The matrix is orthographic, column-major (batch::Mat4's layout), mapping
// world space to Vulkan clip space: x/y in [-1, 1], depth in [0, 1] with 0
// nearest the light. `texel_world` is the size of one shadow texel in world
// units (the receiver's normal-offset bias scales with it).
struct ShadowFit {
    bool valid = false;
    float view_proj[16] = {};
    float texel_world = 0.0f;
    float texel_uv = 0.0f;  // one texel in uv units (1 / resolution)
};

[[nodiscard]] inline ShadowFit fit_shadow_map(const float to_light[3],
                                              const BoundSphere* casters,
                                              std::size_t caster_count,
                                              const BoundSphere* receivers,
                                              std::size_t receiver_count,
                                              std::uint32_t resolution) {
    ShadowFit fit;
    const float len = std::sqrt(to_light[0] * to_light[0] + to_light[1] * to_light[1] +
                                to_light[2] * to_light[2]);
    if (caster_count == 0 || len < 1e-6f || resolution == 0) {
        return fit;
    }
    // The light looks along f (away from the light, into the scene).
    const float f[3] = {-to_light[0] / len, -to_light[1] / len, -to_light[2] / len};
    // Any stable basis perpendicular to f: pick the hint axis least aligned.
    const float hint[3] = {std::fabs(f[1]) < 0.99f ? 0.0f : 1.0f,
                           std::fabs(f[1]) < 0.99f ? 1.0f : 0.0f, 0.0f};
    float right[3] = {hint[1] * f[2] - hint[2] * f[1], hint[2] * f[0] - hint[0] * f[2],
                      hint[0] * f[1] - hint[1] * f[0]};
    const float rlen = std::sqrt(right[0] * right[0] + right[1] * right[1] +
                                 right[2] * right[2]);
    for (float& c : right) {
        c /= rlen;
    }
    const float up[3] = {f[1] * right[2] - f[2] * right[1],
                         f[2] * right[0] - f[0] * right[2],
                         f[0] * right[1] - f[1] * right[0]};
    const auto dot = [](const float a[3], const float b[3]) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };

    float min_x = INFINITY, max_x = -INFINITY, min_y = INFINITY, max_y = -INFINITY;
    float min_z = INFINITY, max_z = -INFINITY;
    for (std::size_t i = 0; i < caster_count; ++i) {
        const BoundSphere& s = casters[i];
        const float x = dot(s.center, right);
        const float y = dot(s.center, up);
        const float z = dot(s.center, f);
        min_x = std::min(min_x, x - s.radius);
        max_x = std::max(max_x, x + s.radius);
        min_y = std::min(min_y, y - s.radius);
        max_y = std::max(max_y, y + s.radius);
        min_z = std::min(min_z, z - s.radius);
        max_z = std::max(max_z, z + s.radius);
    }
    for (std::size_t i = 0; i < receiver_count; ++i) {
        const BoundSphere& s = receivers[i];
        const float z = dot(s.center, f);
        min_z = std::min(min_z, z - s.radius);
        max_z = std::max(max_z, z + s.radius);
    }
    // A degenerate extent (a point-sized caster) still needs a real volume.
    constexpr float kMinExtent = 1e-3f;
    const float pad = 0.01f * std::max({max_x - min_x, max_y - min_y, kMinExtent});
    min_x -= pad; max_x += pad; min_y -= pad; max_y += pad;
    min_z -= pad; max_z += pad;
    const float ex = std::max(max_x - min_x, kMinExtent);
    const float ey = std::max(max_y - min_y, kMinExtent);
    const float ez = std::max(max_z - min_z, kMinExtent);
    const float sx = 2.0f / ex;
    const float sy = 2.0f / ey;
    const float sz = 1.0f / ez;
    // Column-major: m[col * 4 + row]. Rows: x', y', z' from (right, up, f).
    float* m = fit.view_proj;
    m[0] = sx * right[0]; m[1] = sy * up[0]; m[2] = sz * f[0]; m[3] = 0.0f;
    m[4] = sx * right[1]; m[5] = sy * up[1]; m[6] = sz * f[1]; m[7] = 0.0f;
    m[8] = sx * right[2]; m[9] = sy * up[2]; m[10] = sz * f[2]; m[11] = 0.0f;
    m[12] = -sx * min_x - 1.0f;
    m[13] = -sy * min_y - 1.0f;
    m[14] = -sz * min_z;
    m[15] = 1.0f;
    fit.texel_world = std::max(ex, ey) / static_cast<float>(resolution);
    fit.texel_uv = 1.0f / static_cast<float>(resolution);
    fit.valid = true;
    return fit;
}

// A material's highlight in one word: bits 0-7 the strength (0..1 as an
// 8-bit unorm), bits 8-15 the shininess exponent (1..255, stored as is).
// NaN and out-of-range inputs clamp (a NaN strength is matte).
[[nodiscard]] inline std::uint32_t pack_surface(float specular, float shininess) {
    const float s = specular > 0.0f ? (specular < 1.0f ? specular : 1.0f) : 0.0f;
    const float p = shininess > 1.0f ? (shininess < 255.0f ? shininess : 255.0f)
                                     : 1.0f;
    const auto strength = static_cast<std::uint32_t>(s * 255.0f + 0.5f);
    const auto exponent = static_cast<std::uint32_t>(p + 0.5f);
    return strength | (exponent << 8);
}

// The mesh pipeline's push constants: ONE range over all 128 bytes (the
// minimum every Vulkan device guarantees), visible to both stages. The
// vertex shader reads [0, 96); the fragment shader [96, 128).
struct MeshPush {
    float vp[16];                // 0   column-major, batch::Mat4's layout
    float rotation[4];           // 64  unit quaternion x, y, z, w
    float translation_scale[4];  // 80  translation xyz, uniform scale w
    std::uint32_t tint;          // 96  pack_tint()
    std::uint32_t tex_slot;      // 100 bindless: the registry slot
    std::uint32_t surface;       // 104 pack_surface()
    std::uint32_t flags;         // 108 kMeshFlagReceivesShadow
    float eye[4];                // 112 the block's camera position, world
};
static_assert(sizeof(MeshPush) == 128, "mesh push layout is load-bearing");
inline constexpr std::uint32_t kMeshFlagReceivesShadow = 1u;

// The shadow pass's push constants (vertex stage only): the light-space
// matrix + the same placement the mesh vertex shader takes.
struct ShadowPush {
    float light_vp[16];          // 0
    float rotation[4];           // 64
    float translation_scale[4];  // 80
};
static_assert(sizeof(ShadowPush) == 96, "shadow push layout is load-bearing");
static_assert(offsetof(MeshPush, tint) == 96);
static_assert(offsetof(MeshPush, eye) == 112);

// Which winding the mesh pipeline treats as FRONT (cull mode is back).
// Pinned by the suite against the generators and batch::camera_vp3d.
inline constexpr bool kMeshFrontFaceCcw = true;

// The set-1 scene block (std140): the directional light, the ambient floor,
// and the frame's point lights. 16-byte-aligned vec4 rows throughout, so the
// C++ layout is the std140 layout.
struct ScenePoint {
    float position_radius[4];  // xyz world position, w radius
    float color[4];            // rgb (a unused)
};
static_assert(sizeof(ScenePoint) == 32);

struct SceneBlock {
    float light_dir[4];            // xyz: UNIT direction TO the light
    float light_color_ambient[4];  // rgb light color, a the ambient floor
    std::int32_t point_count[4];   // x: how many of points[] are live
    ScenePoint points[kMaxPointLights];
    float light_view_proj[16];  // the shadow map's matrix (column-major)
    float shadow_params[4];     // x: 1 = a shadow map is live this frame,
                                // y: one texel in uv units, z: unused,
                                // w: the receiver's normal offset, world units
};
static_assert(sizeof(SceneBlock) == 48 + 32 * kMaxPointLights + 64 + 16);

// The scene block for a light (+ the frame's point lights), with the
// direction normalized (set_light stores it as given and guarantees it is
// non-zero). A negative color channel clamps to 0 rather than darkening
// below black; ambient clamps to [0, 1]. Point lights past the shader's
// array are ignored (add_point_light already refuses them).
[[nodiscard]] inline SceneBlock make_scene_block(
    const Light3D& light, const PointLight3D* points = nullptr,
    std::size_t point_count = 0, const ShadowFit* shadow = nullptr) {
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
    const std::size_t live =
        point_count < static_cast<std::size_t>(kMaxPointLights)
            ? point_count
            : static_cast<std::size_t>(kMaxPointLights);
    for (std::size_t i = 0; i < live; ++i) {
        for (int k = 0; k < 3; ++k) {
            block.points[i].position_radius[k] = points[i].position[k];
            block.points[i].color[k] =
                points[i].color[k] > 0.0f ? points[i].color[k] : 0.0f;
        }
        block.points[i].position_radius[3] = points[i].radius;
    }
    block.point_count[0] = static_cast<std::int32_t>(live);
    if (shadow != nullptr && shadow->valid) {
        std::copy(shadow->view_proj, shadow->view_proj + 16, block.light_view_proj);
        block.shadow_params[0] = 1.0f;
        block.shadow_params[1] = shadow->texel_uv;
        // Push the lookup off the surface by ~1.5 texels along the normal:
        // the standard cure for acne on faces that graze the light.
        block.shadow_params[3] = 1.5f * shadow->texel_world;
    }
    return block;
}

}  // namespace ws::ui::r3d::detail
