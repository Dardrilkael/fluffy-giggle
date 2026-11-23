// The CPU half of ws::ui::r3d (Phase 1): the mesh registry, the four
// generators, and draw recording. Pure CPU throughout - no Vulkan, no
// platform, no third-party: the registry is an in-memory table, the
// commands are data. The recording rides Renderer2D's frame plumbing
// (epoch/layer/clip, the accept/kind gates) through a narrow seam, so
// the mesh path sorts and replays beside the quad commands with the
// same invariants the 2D tests pin.
//
// The error family (category "ui", codes append-only per the
// workspace's error rule): 60xx - the r3d block, past every existing
// ui code (font's own 60xx? no - see ui/errors: the ui category's r3d
// codes START at 6000 and nothing else uses them; the constants below
// document their numbers in place).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <ws/core/log.hpp>

#include "render/renderer2d.h"

namespace {

// The r3d error codes (category "ui"). Numbers append-only; new r3d
// failures take the next free one.
constexpr int kErrNoApp = 6000;      // no renderer alive (headless)
constexpr int kErrEmptyMesh = 6001;  // zero vertices
constexpr int kErrBadIndex = 6002;   // index >= vertex count
constexpr int kErrUnknownMesh = 6003;  // destroy: not a live mesh
constexpr int kErrBadDir = 6004;     // set_light: zero direction

// Handle packing - the Texture contract verbatim (renderer2d.h's
// kSlotMask/kGenShift/kGenMask): 20-bit slot, 12-bit generation.
constexpr std::uint32_t kMeshSlotMask = 0xFFFFFu;
constexpr std::uint32_t kMeshGenShift = 20;
constexpr std::uint32_t kMeshGenMask = 0xFFFu;

[[nodiscard]] constexpr std::uint32_t mesh_slot(std::uint32_t id) {
    return id & kMeshSlotMask;
}

[[nodiscard]] constexpr std::uint32_t mesh_generation(std::uint32_t id) {
    return (id >> kMeshGenShift) & kMeshGenMask;
}

}  // namespace

namespace ws::ui::r3d {

}  // namespace ws::ui::r3d

// The shared r3d failure builder (category "ui", the code block's
// numbers): file scope so BOTH namespace halves reach it (r3d's free
// functions and r2d's registry members). Returns the unexpected<Error>
// that converts into any Result<T> - the ws::core::fail spelling, with
// the r3d category baked in.
[[nodiscard]] ws::core::unexpected<ws::core::Error> make_error(
    int code, std::string message) {
    return ws::core::unexpected<ws::core::Error>(ws::core::Error{
        code, "ui", std::move(message)});
}

// The Renderer2D members (the registry's row-facing half) are defined in
// their owning namespace - r2d - because Renderer2D lives there; the
// r3d block below keeps everything else. The r3d names the definitions
// use (r3d::Mesh, the make_error helper) resolve through the enclosing
// ws::ui - the same-named r3d namespace sibling, in scope from r2d.
namespace ws::ui::r2d {

// --- the registry -----------------------------------------------------------------
//
// The registry lives on Renderer2D (like the texture registry), not as
// a second global - one renderer, one mesh table, the device half
// (Phase 2) draws its buffers from the same place. These are the
// registry's table-facing helpers; Renderer2D owns the storage.

bool Renderer2D::mesh_alive(r3d::Mesh mesh) const {
    if (mesh.id == 0) {
        return false;
    }
    const std::uint32_t slot = mesh_slot(mesh.id);
    if (slot >= meshes_.size()) {
        return false;
    }
    return meshes_[slot].generation == mesh_generation(mesh.id);
}

ws::core::Result<r3d::Mesh> Renderer2D::create_mesh(const r3d::MeshData& data) {
    if (data.vertices.empty()) {
        return make_error(
            kErrEmptyMesh, "r3d::create_mesh - no vertices in the mesh");
    }
    for (const std::uint32_t index : data.indices) {
        if (index >= data.vertices.size()) {
            return make_error(
                kErrBadIndex, "r3d::create_mesh - index " +
                                  std::to_string(index) +
                                  " >= vertex count " +
                                  std::to_string(data.vertices.size()));
        }
    }
    // Slot 0 mirrors the texture registry's white row: the reserved
    // "nothing" entry id 0 names, never handed out.
    if (meshes_.empty()) {
        meshes_.push_back({});  // slot 0: the sentinel row
    }
    std::uint32_t slot = 0;
    if (!free_mesh_slots_.empty()) {
        slot = free_mesh_slots_.back();
        free_mesh_slots_.pop_back();
    } else {
        slot = static_cast<std::uint32_t>(meshes_.size());
        meshes_.push_back({});
    }
    MeshEntry& entry = meshes_[slot];
    entry.generation = (entry.generation + 1) & kMeshGenMask;
    if (entry.generation == 0) {
        entry.generation = 1;  // 0 is the "never used" sentinel's own value
    }
    entry.vertices = data.vertices;
    entry.indices = data.indices;
    // A trailing partial triangle never rides: the count rounds down.
    entry.indices.resize(entry.indices.size() / 3 * 3);
    return r3d::Mesh{(entry.generation << kMeshGenShift) | slot};
}

ws::core::Status Renderer2D::destroy_mesh(r3d::Mesh mesh) {
    if (mesh.id == 0 || !mesh_alive(mesh)) {
        return make_error(
            kErrUnknownMesh,
            "r3d::destroy_mesh - the handle names no live mesh");
    }
    const std::uint32_t slot = mesh_slot(mesh.id);
    MeshEntry& entry = meshes_[slot];
    entry.vertices.clear();
    entry.vertices.shrink_to_fit();
    entry.indices.clear();
    entry.indices.shrink_to_fit();
    entry.generation = (entry.generation + 1) & kMeshGenMask;
    free_mesh_slots_.push_back(slot);
    return {};
}

void Renderer2D::record_mesh(const r3d::MeshMaterial& material,
                              const r3d::Transform3D& transform,
                              const MeshEntry& entry) {
    MeshCommand cmd;
    cmd.camera_epoch = current_epoch_;
    cmd.sort_layer = current_layer_;
    const float* clip = active_clip();
    for (int i = 0; i < 4; ++i) {
        cmd.clip[i] = clip[i];
    }
    cmd.tint = material.tint;
    cmd.texture = material.texture;
    cmd.transform = transform;
    cmd.vertex_count = static_cast<std::uint32_t>(entry.vertices.size());
    cmd.index_count = static_cast<std::uint32_t>(entry.indices.size());
    mesh_commands_.push_back(cmd);
}

}  // namespace ws::ui::r2d

namespace ws::ui::r3d {

// --- generators --------------------------------------------------------------------

namespace {

// One vertex pusher, positional (the generators are spelled in local
// space; uv rides [0,1]² by construction below).
void push_vertex(MeshData& out, float px, float py, float pz, float nx,
                 float ny, float nz, float u, float v) {
    out.vertices.push_back(
        Vertex3D{{px, py, pz}, {nx, ny, nz}, {u, v}});
}

void push_tri(MeshData& out, std::uint32_t a, std::uint32_t b,
              std::uint32_t c) {
    out.indices.push_back(a);
    out.indices.push_back(b);
    out.indices.push_back(c);
}

}  // namespace

MeshData generate_cube(float size) {
    MeshData out;
    if (size <= 0.0f) {
        return out;  // degenerate: the empty mesh, create_mesh refuses it
    }
    const float h = size * 0.5f;
    // Six faces, four vertices each (flat normals - a cube's normals
    // are the face normals, so corners never share). The face order
    // walks +X, -X, +Y, -Y, +Z, -Z; per face the quad is spelled
    // CCW seen from OUTSIDE the cube (front-face convention).
    struct FaceDef {
        float normal[3];
        // The quad's four corners in CCW-from-outside order: positions
        // built from (axis u, axis v) spans over the face's tangent
        // pair, [0,1]² uv.
        float corners[4][3];
    };
    const FaceDef faces[6] = {
        // +X: looking from +X toward -X, x fixed at +h; CCW is
        // (top-left, bottom-left, bottom-right, top-right) in the
        // (z, y) plane: (−h,+h), (−h,−h), (+h,−h), (+h,+h) over z/y.
        {{1.0f, 0.0f, 0.0f},
         {{h, h, -h}, {h, -h, -h}, {h, -h, h}, {h, h, h}}},
        // -X: seen from -X, the same (z, y) plane mirrored.
        {{-1.0f, 0.0f, 0.0f},
         {{-h, h, h}, {-h, -h, h}, {-h, -h, -h}, {-h, h, -h}}},
        // +Y: seen from above, the (x, z) plane.
        {{0.0f, 1.0f, 0.0f},
         {{-h, h, -h}, {-h, h, h}, {h, h, h}, {h, h, -h}}},
        // -Y: seen from below.
        {{0.0f, -1.0f, 0.0f},
         {{-h, -h, h}, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}}},
        // +Z: seen from +Z, the (x, y) plane - the "front" face.
        {{0.0f, 0.0f, 1.0f},
         {{-h, h, h}, {-h, -h, h}, {h, -h, h}, {h, h, h}}},
        // -Z: seen from behind.
        {{0.0f, 0.0f, -1.0f},
         {{h, h, -h}, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}}},
    };
    for (const FaceDef& face : faces) {
        const std::uint32_t base =
            static_cast<std::uint32_t>(out.vertices.size());
        // The quad's uv: TL, BL, BR, TR matching the corner order -
        // (0,0), (0,1), (1,1), (1,0). Texture-space y points DOWN the
        // uv (the r2d page convention), so v grows downward here too;
        // a cube face has no privileged orientation, this is the one
        // every face shares.
        push_vertex(out, face.corners[0][0], face.corners[0][1],
                    face.corners[0][2], face.normal[0], face.normal[1],
                    face.normal[2], 0.0f, 0.0f);
        push_vertex(out, face.corners[1][0], face.corners[1][1],
                    face.corners[1][2], face.normal[0], face.normal[1],
                    face.normal[2], 0.0f, 1.0f);
        push_vertex(out, face.corners[2][0], face.corners[2][1],
                    face.corners[2][2], face.normal[0], face.normal[1],
                    face.normal[2], 1.0f, 1.0f);
        push_vertex(out, face.corners[3][0], face.corners[3][1],
                    face.corners[3][2], face.normal[0], face.normal[1],
                    face.normal[2], 1.0f, 0.0f);
        // TL, BL, BR + BR, TR, TL - the quad's two CCW triangles.
        push_tri(out, base + 0, base + 1, base + 2);
        push_tri(out, base + 2, base + 3, base + 0);
    }
    return out;
}

MeshData generate_sphere(float radius, int stacks, int sectors) {
    MeshData out;
    if (radius <= 0.0f || stacks < 2 || sectors < 3) {
        return out;
    }
    const float pi = 3.14159265358979323846f;
    const float two_pi = 2.0f * pi;
    // The top pole (all sectors' fan apex; uv.x at the seam's start).
    push_vertex(out, 0.0f, radius, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
    // Latitude rings, stack 1..stacks-1 (the poles are the caps).
    for (int stack = 1; stack < stacks; ++stack) {
        const float phi = pi * static_cast<float>(stack) /
                          static_cast<float>(stacks);  // 0..pi, top to bottom
        const float sin_phi = std::sin(phi);
        const float cos_phi = std::cos(phi);
        // v runs down the texture with phi (uv.y down = toward the
        // south pole, the cube-face convention again).
        const float v = static_cast<float>(stack) /
                        static_cast<float>(stacks);
        for (int sector = 0; sector < sectors; ++sector) {
            const float theta =
                two_pi * static_cast<float>(sector) /
                static_cast<float>(sectors);
            const float sin_theta = std::sin(theta);
            const float cos_theta = std::cos(theta);
            const float nx = sin_phi * cos_theta;
            const float ny = cos_phi;
            const float nz = sin_phi * sin_theta;
            // The unit sphere: position = radius * normal, so the
            // normal IS the normalized position by construction.
            push_vertex(out, radius * nx, radius * ny, radius * nz, nx, ny,
                        nz, static_cast<float>(sector) /
                                static_cast<float>(sectors),
                        v);
        }
    }
    // The bottom pole.
    push_vertex(out, 0.0f, -radius, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f);

    const std::uint32_t top = 0;
    const std::uint32_t bottom =
        static_cast<std::uint32_t>(out.vertices.size()) - 1;
    // Ring r (stack r to r+1) starts at vertex 1 + (r-1)*sectors.
    const auto ring_vertex = [sectors](int ring, int sector)
        -> std::uint32_t {
        // ring 1..stacks-1; sector wraps mod sectors.
        const int s = sector % sectors;
        return static_cast<std::uint32_t>(1 + (ring - 1) * sectors + s);
    };
    // Top cap: the pole fan over ring 1.
    for (int sector = 0; sector < sectors; ++sector) {
        const std::uint32_t a = ring_vertex(1, sector);
        const std::uint32_t b = ring_vertex(1, sector + 1);
        // CCW seen from outside: pole, then the ring's CCW pair.
        push_tri(out, top, b, a);
    }
    // The body quads between rings 1..stacks-1.
    for (int ring = 1; ring + 1 < stacks; ++ring) {
        for (int sector = 0; sector < sectors; ++sector) {
            const std::uint32_t tl = ring_vertex(ring, sector);
            const std::uint32_t tr = ring_vertex(ring, sector + 1);
            const std::uint32_t bl = ring_vertex(ring + 1, sector);
            const std::uint32_t br = ring_vertex(ring + 1, sector + 1);
            // CCW seen from outside: (tl, bl, br) + (br, tr, tl).
            push_tri(out, tl, bl, br);
            push_tri(out, br, tr, tl);
        }
    }
    // Bottom cap: the fan over the last ring, seen from below.
    const int last_ring = stacks - 1;
    for (int sector = 0; sector < sectors; ++sector) {
        const std::uint32_t a = ring_vertex(last_ring, sector);
        const std::uint32_t b = ring_vertex(last_ring, sector + 1);
        push_tri(out, bottom, a, b);
    }
    return out;
}

MeshData generate_plane(float extent, int subdivisions) {
    MeshData out;
    if (extent <= 0.0f) {
        return out;
    }
    if (subdivisions < 1) {
        subdivisions = 1;
    }
    const int n = subdivisions;
    const float step = 2.0f * extent / static_cast<float>(n);
    // The (row, col) grid, row 0 at -extent z (uv.y 0), col 0 at
    // -extent x (uv.x 0); y = 0, normal +Y everywhere.
    for (int row = 0; row <= n; ++row) {
        for (int col = 0; col <= n; ++col) {
            const float x = -extent + step * static_cast<float>(col);
            const float z = -extent + step * static_cast<float>(row);
            push_vertex(out, x, 0.0f, z, 0.0f, 1.0f, 0.0f,
                        static_cast<float>(col) / static_cast<float>(n),
                        static_cast<float>(row) / static_cast<float>(n));
        }
    }
    const auto grid_vertex = [n](int row, int col) -> std::uint32_t {
        return static_cast<std::uint32_t>(row * (n + 1) + col);
    };
    for (int row = 0; row < n; ++row) {
        for (int col = 0; col < n; ++col) {
            // CCW seen from +Y (above): the quad's (tl, bl, br) +
            // (br, tr, tl) with tl at (row, col).
            const std::uint32_t tl = grid_vertex(row, col);
            const std::uint32_t tr = grid_vertex(row, col + 1);
            const std::uint32_t bl = grid_vertex(row + 1, col);
            const std::uint32_t br = grid_vertex(row + 1, col + 1);
            push_tri(out, tl, bl, br);
            push_tri(out, br, tr, tl);
        }
    }
    return out;
}

MeshData generate_cylinder(float radius, float half_height, int slices) {
    MeshData out;
    if (radius <= 0.0f || half_height <= 0.0f) {
        return out;
    }
    if (slices < 3) {
        slices = 3;
    }
    const float pi = 3.14159265358979323846f;
    const float two_pi = 2.0f * pi;
    // The smooth wall: one ring of vertices at each height (the
    // shared vertices give the wall its smooth normals - each wall
    // vertex's normal is its radial direction, top and bottom alike).
    const float hy = half_height;
    for (int slice = 0; slice < slices; ++slice) {
        const float theta =
            two_pi * static_cast<float>(slice) / static_cast<float>(slices);
        const float cx = std::cos(theta);
        const float cz = std::sin(theta);
        // u = angle around, v = height: v 0 at the TOP (uv.y down =
        // toward the base, the shared convention).
        push_vertex(out, radius * cx, hy, radius * cz, cx, 0.0f, cz,
                    static_cast<float>(slice) / static_cast<float>(slices),
                    0.0f);
    }
    for (int slice = 0; slice < slices; ++slice) {
        const float theta =
            two_pi * static_cast<float>(slice) / static_cast<float>(slices);
        const float cx = std::cos(theta);
        const float cz = std::sin(theta);
        push_vertex(out, radius * cx, -hy, radius * cz, cx, 0.0f, cz,
                    static_cast<float>(slice) / static_cast<float>(slices),
                    1.0f);
    }
    const auto wall_vertex = [slices](int ring, int slice)
        -> std::uint32_t {
        // ring 0 = top, ring 1 = bottom; slice wraps mod slices.
        const int s = slice % slices;
        return static_cast<std::uint32_t>(ring * slices + s);
    };
    for (int slice = 0; slice < slices; ++slice) {
        const std::uint32_t tl = wall_vertex(0, slice);
        const std::uint32_t tr = wall_vertex(0, slice + 1);
        const std::uint32_t bl = wall_vertex(1, slice);
        const std::uint32_t br = wall_vertex(1, slice + 1);
        // CCW seen from outside the wall.
        push_tri(out, tl, bl, br);
        push_tri(out, br, tr, tl);
    }
    // The caps: a center vertex each + the rim reused with FLAT cap
    // normals. The rim vertices cannot be shared with the wall (their
    // normals differ - smooth vs flat), so the caps carry their own
    // rim rings.
    // Top cap: center + rim, normal +Y.
    const std::uint32_t top_center =
        static_cast<std::uint32_t>(out.vertices.size());
    push_vertex(out, 0.0f, hy, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 0.5f);
    for (int slice = 0; slice < slices; ++slice) {
        const float theta =
            two_pi * static_cast<float>(slice) / static_cast<float>(slices);
        const float cx = std::cos(theta);
        const float cz = std::sin(theta);
        // Polar uv: the disc mapped by radius/angle - v the radius,
        // u the angle fraction.
        push_vertex(out, radius * cx, hy, radius * cz, 0.0f, 1.0f, 0.0f,
                    0.5f + 0.5f * (cx * 0.5f + 0.5f),
                    0.5f - 0.5f * (cz * 0.5f + 0.5f));
    }
    for (int slice = 0; slice < slices; ++slice) {
        const std::uint32_t a = top_center + 1 + slice;
        const std::uint32_t b = top_center + 1 + (slice + 1) % slices;
        // CCW seen from +Y (above).
        push_tri(out, top_center, b, a);
    }
    // Bottom cap: normal -Y, winding flipped.
    const std::uint32_t bottom_center =
        static_cast<std::uint32_t>(out.vertices.size());
    push_vertex(out, 0.0f, -hy, 0.0f, 0.0f, -1.0f, 0.0f, 0.5f, 0.5f);
    for (int slice = 0; slice < slices; ++slice) {
        const float theta =
            two_pi * static_cast<float>(slice) / static_cast<float>(slices);
        const float cx = std::cos(theta);
        const float cz = std::sin(theta);
        push_vertex(out, radius * cx, -hy, radius * cz, 0.0f, -1.0f, 0.0f,
                    0.5f + 0.5f * (cx * 0.5f + 0.5f),
                    0.5f + 0.5f * (cz * 0.5f + 0.5f));
    }
    for (int slice = 0; slice < slices; ++slice) {
        const std::uint32_t a = bottom_center + 1 + slice;
        const std::uint32_t b = bottom_center + 1 + (slice + 1) % slices;
        push_tri(out, bottom_center, a, b);
    }
    return out;
}

// --- the free-function layer ---------------------------------------------------------

void draw_mesh(Mesh mesh, const Transform3D& transform,
               const MeshMaterial& material) {
    // The r2d free-function convention: g_active routes, no app warns
    // once per process. The block/kind gates live on the Renderer2D
    // method (the same accept/warn_drop/warn_kind ladder every 3D
    // overload walks).
    if (r2d::Renderer2D* active = r2d::active_renderer()) {
        active->draw_mesh(mesh, transform, material);
    } else {
        r2d::warn_no_app_r3d("draw_mesh");
    }
}

ws::core::Status set_light(const Light3D& light) {
    if (r2d::Renderer2D* active = r2d::active_renderer()) {
        return active->set_light(light);
    }
    return make_error(
        kErrNoApp, "r3d::set_light - no app is running (the light is "
                   "renderer state; headless there is none to set)");
}

ws::core::Result<Mesh> create_mesh(const MeshData& data) {
    if (r2d::Renderer2D* active = r2d::active_renderer()) {
        return active->create_mesh(data);
    }
    return make_error(
        kErrNoApp,
        "r3d::create_mesh - no app is running (headless: no renderer, "
        "no mesh registry)");
}

ws::core::Status destroy_mesh(Mesh mesh) {
    if (r2d::Renderer2D* active = r2d::active_renderer()) {
        return active->destroy_mesh(mesh);
    }
    return make_error(
        kErrNoApp,
        "r3d::destroy_mesh - no app is running (headless: no renderer, "
        "no mesh registry)");
}

}  // namespace ws::ui::r3d
