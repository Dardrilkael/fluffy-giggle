// The r3d mesh-path suite (Phase 1): the generators' shape contracts
// (counts, unit normals, index ranges), the registry's handle contract
// (generation-counted create/destroy, stale rejection), and the
// recording rules (the 3D billboard gate ladder, the epoch/layer/clip
// carry, the transform/material round-trip). The suite drives the REAL
// Renderer2D CPU half headlessly (device_ never initialized - the same
// shape test_ui pins for the 2D path); no Vulkan call is ever made.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "render/renderer2d.h"
#include "render/renderer3d_math.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using namespace ws::ui::r3d;
using ws::ui::r2d::Renderer2D;

// Every normal in the mesh is unit-length (or the flat zero sentinel
// on a degenerate input, which the generators never emit for valid
// parameters).
void check_unit_normals(const MeshData& mesh) {
    for (const Vertex3D& v : mesh.vertices) {
        const float len = std::sqrt(v.normal[0] * v.normal[0] +
                                    v.normal[1] * v.normal[1] +
                                    v.normal[2] * v.normal[2]);
        CHECK(len == doctest::Approx(1.0f).epsilon(1e-4f));
    }
}

// Every index names a real vertex (the registry's own create-time check
// must never see an out-of-range index from a generator).
void check_index_range(const MeshData& mesh) {
    for (const std::uint32_t index : mesh.indices) {
        CHECK(index < mesh.vertices.size());
    }
}

}  // namespace

// --- generators -----------------------------------------------------------------

TEST_CASE("cube: 24 flat-normal vertices, 36 indices, closed surface") {
    const MeshData cube = generate_cube(2.0f);
    REQUIRE(cube.vertices.size() == 24u);  // 6 faces x 4
    REQUIRE(cube.indices.size() == 36u);   // 6 faces x 2 triangles
    check_unit_normals(cube);
    check_index_range(cube);
    // The six face normals are the axis pair, each exactly four times.
    int counts[6] = {0, 0, 0, 0, 0, 0};  // +x -x +y -y +z -z
    for (const Vertex3D& v : cube.vertices) {
        if (v.normal[0] == 1.0f) ++counts[0];
        else if (v.normal[0] == -1.0f) ++counts[1];
        else if (v.normal[1] == 1.0f) ++counts[2];
        else if (v.normal[1] == -1.0f) ++counts[3];
        else if (v.normal[2] == 1.0f) ++counts[4];
        else if (v.normal[2] == -1.0f) ++counts[5];
    }
    for (const int c : counts) {
        CHECK(c == 4);
    }
    // Every position lands on the half-size hull: |x|+|y|+|z| hit the
    // faces' plane set (each coordinate is +-h exactly once per vertex:
    // a cube corner lies on three planes, a face-interior vertex on one).
    for (const Vertex3D& v : cube.vertices) {
        const bool on_hull = (std::fabs(v.pos[0]) == 1.0f) ||
                             (std::fabs(v.pos[1]) == 1.0f) ||
                             (std::fabs(v.pos[2]) == 1.0f);
        CHECK(on_hull);
    }
    // UVs span the full [0,1] square per face: the min is 0, the max 1.
    float u_min = 1.0f, u_max = 0.0f, v_min = 1.0f, v_max = 0.0f;
    for (const Vertex3D& v : cube.vertices) {
        u_min = std::min(u_min, v.uv[0]);
        u_max = std::max(u_max, v.uv[0]);
        v_min = std::min(v_min, v.uv[1]);
        v_max = std::max(v_max, v.uv[1]);
    }
    CHECK(u_min == 0.0f);
    CHECK(u_max == 1.0f);
    CHECK(v_min == 0.0f);
    CHECK(v_max == 1.0f);
    // A degenerate size answers the empty mesh.
    CHECK(generate_cube(0.0f).vertices.empty());
    CHECK(generate_cube(-1.0f).vertices.empty());
}

TEST_CASE("sphere: counts pinned, normals = normalized positions") {
    const MeshData sphere = generate_sphere(1.5f, 8, 12);
    // (stacks - 1) * sectors + 2 = 7 * 12 + 2 = 86.
    REQUIRE(sphere.vertices.size() == 86u);
    check_unit_normals(sphere);
    check_index_range(sphere);
    // Triangle count: caps 2 * sectors + body (stacks - 2) * sectors * 2
    // = 24 + 6 * 24 = 168, x3 indices = 504.
    REQUIRE(sphere.indices.size() == 504u);
    // Every normal is its position normalized (the unit sphere's own
    // property) and every position sits at radius 1.5.
    for (const Vertex3D& v : sphere.vertices) {
        const float plen = std::sqrt(v.pos[0] * v.pos[0] +
                                     v.pos[1] * v.pos[1] +
                                     v.pos[2] * v.pos[2]);
        CHECK(plen == doctest::Approx(1.5f).epsilon(1e-4f));
        CHECK(v.pos[0] / plen == doctest::Approx(v.normal[0]).epsilon(1e-4f));
        CHECK(v.pos[1] / plen == doctest::Approx(v.normal[1]).epsilon(1e-4f));
        CHECK(v.pos[2] / plen == doctest::Approx(v.normal[2]).epsilon(1e-4f));
    }
    // The poles ride at +-radius on the y axis.
    CHECK(sphere.vertices.front().pos[1] == 1.5f);
    CHECK(sphere.vertices.back().pos[1] == -1.5f);
    // Degenerate parameters answer empty.
    CHECK(generate_sphere(0.0f, 8, 12).vertices.empty());
    CHECK(generate_sphere(1.0f, 1, 12).vertices.empty());
    CHECK(generate_sphere(1.0f, 8, 2).vertices.empty());
}

TEST_CASE("plane: the subdivided grid, +Y normals, pinned counts") {
    const MeshData plane = generate_plane(5.0f, 4);
    // (subdivisions + 1)^2 = 25.
    REQUIRE(plane.vertices.size() == 25u);
    // subdivisions^2 quads x 2 triangles x 3 = 16 * 6 = 96.
    REQUIRE(plane.indices.size() == 96u);
    check_unit_normals(plane);
    check_index_range(plane);
    // All normals are +Y, all positions sit on y = 0 and inside the
    // extent box.
    for (const Vertex3D& v : plane.vertices) {
        CHECK(v.normal[1] == 1.0f);
        CHECK(v.pos[1] == 0.0f);
        CHECK(std::fabs(v.pos[0]) <= 5.0f);
        CHECK(std::fabs(v.pos[2]) <= 5.0f);
    }
    // The corners are the four extremes.
    bool has_min = false, has_max = false;
    for (const Vertex3D& v : plane.vertices) {
        if (v.pos[0] == -5.0f && v.pos[2] == -5.0f) has_min = true;
        if (v.pos[0] == 5.0f && v.pos[2] == 5.0f) has_max = true;
    }
    CHECK(has_min);
    CHECK(has_max);
    // A degenerate extent answers empty; subdivisions clamp to 1 (a 2x2
    // vertex quad).
    CHECK(generate_plane(0.0f, 4).vertices.empty());
    CHECK(generate_plane(1.0f, 0).vertices.size() == 4u);
}

TEST_CASE("cylinder: wall + two caps, smooth wall normals, flat caps") {
    const MeshData cyl = generate_cylinder(1.0f, 2.0f, 8);
    // Wall: 2 rings x 8 = 16; caps: (1 center + 8 rim) x 2 = 18. Total 34.
    REQUIRE(cyl.vertices.size() == 34u);
    // Wall: 8 quads = 16 tris; caps: 2 x 8 fan tris = 16. Total 32 tris.
    REQUIRE(cyl.indices.size() == 96u);
    check_unit_normals(cyl);
    check_index_range(cyl);
    // The first 16 vertices are the wall: normals are radial (y = 0).
    for (std::size_t i = 0; i < 16u; ++i) {
        CHECK(cyl.vertices[i].normal[1] == 0.0f);
        CHECK(std::fabs(cyl.vertices[i].pos[0]) <= 1.0f + 1e-4f);
    }
    // The wall's top ring rides at +2, the bottom at -2.
    for (int slice = 0; slice < 8; ++slice) {
        CHECK(cyl.vertices[static_cast<std::size_t>(slice)].pos[1] == 2.0f);
        CHECK(cyl.vertices[8u + static_cast<std::size_t>(slice)].pos[1] ==
              -2.0f);
    }
    // The caps' normals are flat: vertex 16 is the top center (+Y), the
    // top rim at 17..24 all +Y; the bottom center (25) -Y, its rim -Y.
    CHECK(cyl.vertices[16u].normal[1] == 1.0f);
    for (std::size_t i = 17u; i <= 24u; ++i) {
        CHECK(cyl.vertices[i].normal[1] == 1.0f);
    }
    CHECK(cyl.vertices[25u].normal[1] == -1.0f);
    for (std::size_t i = 26u; i <= 33u; ++i) {
        CHECK(cyl.vertices[i].normal[1] == -1.0f);
    }
    // Degenerate parameters answer empty; slices clamp to 3.
    CHECK(generate_cylinder(0.0f, 1.0f, 8).vertices.empty());
    CHECK(generate_cylinder(1.0f, 0.0f, 8).vertices.empty());
    CHECK(generate_cylinder(1.0f, 1.0f, 2).vertices.size() ==
          generate_cylinder(1.0f, 1.0f, 3).vertices.size());
}

// --- the registry -----------------------------------------------------------------

TEST_CASE("create_mesh: the handle contract, the validation gates") {
    Renderer2D r;
    // The empty mesh is a refusal (empty_mesh, 6001).
    const ws::core::Result<Mesh> empty = r.create_mesh(MeshData{});
    REQUIRE(!empty);
    CHECK(empty.error().code == 6001);
    CHECK(empty.error().category == std::string("ui"));

    // An out-of-range index is a refusal (bad_index, 6002).
    MeshData bad;
    bad.vertices.resize(4);
    bad.indices = {0, 1, 5};
    const ws::core::Result<Mesh> bad_index = r.create_mesh(bad);
    REQUIRE(!bad_index);
    CHECK(bad_index.error().code == 6002);

    // A valid mesh hands back a nonzero handle.
    const ws::core::Result<Mesh> cube = r.create_mesh(generate_cube(1.0f));
    REQUIRE(cube);
    CHECK(cube->id != 0u);
    // Slot 1 (the first handed-out row past the sentinel).
    CHECK((cube->id & 0xFFFFFu) == 1u);

    // A trailing partial triangle never rides: the count rounds down.
    MeshData partial;
    partial.vertices.resize(9);
    partial.indices = {0, 1, 2, 3, 4, 5, 6, 7};
    const ws::core::Result<Mesh> trimmed = r.create_mesh(partial);
    REQUIRE(trimmed);
    // Re-read through a second create: the registry keeps only whole
    // triangles - observed by drawing it (the recorded index_count).
    r.frame_begin(100.0f, 100.0f);
    r.begin(ws::ui::r2d::Camera3D{});
    r.draw_mesh(*trimmed, Transform3D{}, MeshMaterial{});
    r.frame_end();
    REQUIRE(r.mesh_commands().size() == 1u);
    CHECK(r.mesh_commands()[0].index_count == 6u);  // 8 -> 6
}

TEST_CASE("destroy_mesh: kill, stale rejection, slot reuse") {
    Renderer2D r;
    const ws::core::Result<Mesh> a = r.create_mesh(generate_cube(1.0f));
    const ws::core::Result<Mesh> b = r.create_mesh(generate_cube(2.0f));
    REQUIRE(a);
    REQUIRE(b);
    CHECK((a->id & 0xFFFFFu) == 1u);
    CHECK((b->id & 0xFFFFFu) == 2u);

    // Destroy a live mesh: success, the handle is dead at once.
    const ws::core::Status gone = r.destroy_mesh(*a);
    REQUIRE(static_cast<bool>(gone));
    CHECK(gone);

    // A stale handle fails the generation check: destroy says unknown,
    // draw records nothing.
    const ws::core::Status stale_destroy = r.destroy_mesh(*a);
    REQUIRE(!stale_destroy);
    CHECK(stale_destroy.error().code == 6003);
    r.frame_begin(100.0f, 100.0f);
    r.begin(ws::ui::r2d::Camera3D{});
    r.draw_mesh(*a, Transform3D{}, MeshMaterial{});
    r.frame_end();
    CHECK(r.mesh_commands().empty());

    // A never-created handle (id 0, or garbage) is the same refusal.
    CHECK(!r.destroy_mesh(Mesh{}));
    CHECK(!r.destroy_mesh(Mesh{0xDEADBEEFu}));

    // The freed slot is reused with a FRESH generation: the new handle
    // names the same slot but never equals the dead one.
    const ws::core::Result<Mesh> c = r.create_mesh(generate_cube(3.0f));
    REQUIRE(c);
    CHECK((c->id & 0xFFFFFu) == 1u);  // the recycled slot
    CHECK(c->id != a->id);            // the bumped generation
    // The stale handle still fails; the new one draws.
    r.frame_begin(100.0f, 100.0f);
    r.begin(ws::ui::r2d::Camera3D{});
    r.draw_mesh(*a, Transform3D{}, MeshMaterial{});  // stale: dropped
    r.draw_mesh(*c, Transform3D{}, MeshMaterial{});  // fresh: recorded
    r.frame_end();
    REQUIRE(r.mesh_commands().size() == 1u);
    CHECK(r.mesh_commands()[0].vertex_count == 24u);  // the cube
}

// --- the recording rules ----------------------------------------------------------

TEST_CASE("draw_mesh: only inside a begin(Camera3D) block") {
    Renderer2D r;
    const ws::core::Result<Mesh> cube = r.create_mesh(generate_cube(1.0f));
    REQUIRE(cube);

    // Outside any frame: nothing records.
    r.draw_mesh(*cube, Transform3D{}, MeshMaterial{});

    // Inside a frame, outside any block: nothing records.
    r.frame_begin(100.0f, 100.0f);
    r.draw_mesh(*cube, Transform3D{}, MeshMaterial{});
    r.frame_end();
    CHECK(r.mesh_commands().empty());

    // Inside a 2D block: the wrong kind, nothing records (the billboard
    // rule's exact shape).
    r.frame_begin(100.0f, 100.0f);
    r.begin(ws::ui::r2d::Camera2D{});
    r.draw_mesh(*cube, Transform3D{}, MeshMaterial{});
    r.frame_end();
    CHECK(r.mesh_commands().empty());

    // Outside the block again (an ended 3D block): nothing records.
    r.frame_begin(100.0f, 100.0f);
    r.begin(ws::ui::r2d::Camera3D{});
    r.end();
    r.draw_mesh(*cube, Transform3D{}, MeshMaterial{});
    r.frame_end();
    CHECK(r.mesh_commands().empty());

    // Inside a 3D block: ONE command, the mesh's counts, the epoch.
    r.frame_begin(100.0f, 100.0f);
    r.begin(ws::ui::r2d::Camera3D{});
    r.draw_mesh(*cube, Transform3D{}, MeshMaterial{});
    r.frame_end();
    REQUIRE(r.mesh_commands().size() == 1u);
    CHECK(r.mesh_commands()[0].camera_epoch == 1u);
    CHECK(r.mesh_commands()[0].vertex_count == 24u);
    CHECK(r.mesh_commands()[0].index_count == 36u);

    // A degenerate transform (scale <= 0) records nothing.
    r.frame_begin(100.0f, 100.0f);
    r.begin(ws::ui::r2d::Camera3D{});
    Transform3D broken;
    broken.scale = 0.0f;
    r.draw_mesh(*cube, broken, MeshMaterial{});
    r.frame_end();
    CHECK(r.mesh_commands().empty());
}

TEST_CASE("draw_mesh: layer, clip and material ride; multi-block epochs") {
    Renderer2D r;
    const ws::core::Result<Mesh> cube = r.create_mesh(generate_cube(1.0f));
    REQUIRE(cube);

    Transform3D transform;
    transform.translation[0] = 5.0f;
    transform.scale = 2.0f;
    transform.yaw = 0.5f;
    MeshMaterial material;
    material.tint = ws::ui::r2d::rgba(200, 100, 50, 255);

    r.frame_begin(100.0f, 100.0f);
    r.begin(ws::ui::r2d::Camera3D{});  // epoch 1
    r.set_layer(7u);
    r.push_clip(10.0f, 10.0f, 30.0f, 30.0f);
    r.draw_mesh(*cube, transform, material);
    r.pop_clip();
    r.begin(ws::ui::r2d::Camera3D{});  // epoch 2
    r.set_layer(3u);
    r.draw_mesh(*cube, Transform3D{}, MeshMaterial{});
    r.frame_end();

    // Sorted: layer 3 (epoch 2) before layer 7 (epoch 1).
    REQUIRE(r.mesh_commands().size() == 2u);
    CHECK(r.mesh_commands()[0].sort_layer == 3u);
    CHECK(r.mesh_commands()[0].camera_epoch == 2u);
    CHECK(r.mesh_commands()[1].sort_layer == 7u);
    CHECK(r.mesh_commands()[1].camera_epoch == 1u);
    // The recorded clip is the ACTIVE one at record time (the 30x30
    // scope), the material/transform round-trip byte-for-byte.
    const auto& cmd = r.mesh_commands()[1];
    CHECK(cmd.clip[0] == 10.0f);
    CHECK(cmd.clip[2] == 30.0f);
    CHECK(cmd.tint == material.tint);
    CHECK(cmd.transform.translation[0] == 5.0f);
    CHECK(cmd.transform.scale == 2.0f);
    CHECK(cmd.transform.yaw == 0.5f);
    // Commands never leak into the next frame.
    r.frame_begin(100.0f, 100.0f);
    r.frame_end();
    CHECK(r.mesh_commands().empty());
}

TEST_CASE("set_light: the scene state, the zero-dir refusal") {
    Renderer2D r;
    // The default: the top-down key plus the ambient floor.
    CHECK(r.light().dir[1] == -1.0f);
    CHECK(r.light().ambient == 0.25f);

    // A live light stores as given (no normalization at set time - the
    // shade normalizes; the getter returns the stored value).
    Light3D custom;
    custom.dir[0] = 0.5f;
    custom.dir[1] = -2.0f;
    custom.ambient = 0.5f;
    CHECK(r.set_light(custom));
    CHECK(r.light().dir[0] == 0.5f);
    CHECK(r.light().dir[1] == -2.0f);
    CHECK(r.light().ambient == 0.5f);

    // A zero direction is refused AND KEEPS the previous light.
    Light3D zero;
    zero.dir[0] = 0.0f;
    zero.dir[1] = 0.0f;
    zero.dir[2] = 0.0f;
    const ws::core::Status refused = r.set_light(zero);
    REQUIRE(!refused);
    CHECK(refused.error().code == 6004);
    CHECK(r.light().dir[0] == 0.5f);  // unchanged
}

// --- winding: the mesh pipeline culls back faces -----------------------------------
//
// Two contracts meet at the cull mode. (1) The generators wind every
// triangle counter-clockwise seen from OUTSIDE (right-handed: cross(b-a,
// c-a) points along the surface normal). (2) batch::camera_vp3d, whose NDC
// +y points down, maps a world-CCW triangle that faces the eye to a
// triangle with POSITIVE Vulkan polygon area, a = -1/2 * sum(x_i*y_{i+1} -
// x_{i+1}*y_i) - i.e. COUNTER_CLOCKWISE in Vulkan's own terms, which is
// what the pipeline's frontFace (kMeshFrontFaceCcw) says is the front.

namespace {

struct Ndc {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
};

Ndc project(const ws::ui::r2d::batch::Mat4& vp, const float p[3]) {
    // Column-major: clip = vp * (p, 1).
    float clip[4];
    for (int row = 0; row < 4; ++row) {
        clip[row] = vp.m[0 + row] * p[0] + vp.m[4 + row] * p[1] +
                    vp.m[8 + row] * p[2] + vp.m[12 + row];
    }
    return {clip[0] / clip[3], clip[1] / clip[3], clip[3]};
}

// cross(b - a, c - a) of one triangle's positions.
void geometric_normal(const MeshData& m, std::size_t t, float out[3]) {
    const Vertex3D& a = m.vertices[m.indices[t]];
    const Vertex3D& b = m.vertices[m.indices[t + 1]];
    const Vertex3D& c = m.vertices[m.indices[t + 2]];
    const float e1[3] = {b.pos[0] - a.pos[0], b.pos[1] - a.pos[1],
                         b.pos[2] - a.pos[2]};
    const float e2[3] = {c.pos[0] - a.pos[0], c.pos[1] - a.pos[1],
                         c.pos[2] - a.pos[2]};
    out[0] = e1[1] * e2[2] - e1[2] * e2[1];
    out[1] = e1[2] * e2[0] - e1[0] * e2[2];
    out[2] = e1[0] * e2[1] - e1[1] * e2[0];
}

const MeshData& wound_meshes(int index) {
    static const MeshData meshes[] = {
        generate_cube(100.0f),
        generate_sphere(60.0f, 12, 16),
        generate_plane(50.0f, 4),
        generate_cylinder(40.0f, 50.0f, 16),
        generate_sphere(10.0f, 2, 3),   // the smallest legal sphere
        generate_cylinder(5.0f, 5.0f, 3),
    };
    return meshes[index];
}
constexpr int kWoundMeshCount = 6;

}  // namespace

TEST_CASE("winding: every generator triangle is CCW seen from outside") {
    for (int m = 0; m < kWoundMeshCount; ++m) {
        const MeshData& mesh = wound_meshes(m);
        REQUIRE_FALSE(mesh.indices.empty());
        for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            float g[3];
            geometric_normal(mesh, t, g);
            float n[3] = {0.0f, 0.0f, 0.0f};
            for (int k = 0; k < 3; ++k) {
                for (int axis = 0; axis < 3; ++axis) {
                    n[axis] += mesh.vertices[mesh.indices[t + k]].normal[axis];
                }
            }
            const float facing = g[0] * n[0] + g[1] * n[1] + g[2] * n[2];
            // Strictly positive: a zero-area or edge-on triangle would
            // make the check vacuous, and no generator emits one.
            CHECK_MESSAGE(facing > 0.0f, "mesh " << m << " triangle " << t / 3);
        }
    }
}

TEST_CASE("winding: camera_vp3d maps eye-facing world-CCW to Vulkan-CCW") {
    // The pipeline's frontFace choice, pinned.
    REQUIRE(ws::ui::r3d::detail::kMeshFrontFaceCcw);

    ws::ui::r2d::Camera3D cameras[4]{};
    cameras[0].eye[0] = 30.0f;  cameras[0].eye[1] = -40.0f; cameras[0].eye[2] = -200.0f;
    cameras[1].eye[0] = 200.0f; cameras[1].eye[1] = -60.0f; cameras[1].eye[2] = 0.0f;
    cameras[2].eye[0] = 0.0f;   cameras[2].eye[1] = -250.0f; cameras[2].eye[2] = -40.0f;
    cameras[3].eye[0] = -150.0f; cameras[3].eye[1] = 90.0f; cameras[3].eye[2] = 120.0f;
    int checked = 0;
    for (const ws::ui::r2d::Camera3D& camera : cameras) {
        const ws::ui::r2d::batch::Mat4 vp =
            ws::ui::r2d::batch::camera_vp3d(camera, 800.0f, 600.0f);
        for (int m = 0; m < kWoundMeshCount; ++m) {
            const MeshData& mesh = wound_meshes(m);
            for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
                const Vertex3D& a = mesh.vertices[mesh.indices[t]];
                const Vertex3D& b = mesh.vertices[mesh.indices[t + 1]];
                const Vertex3D& c = mesh.vertices[mesh.indices[t + 2]];
                float g[3];
                geometric_normal(mesh, t, g);
                // Facing the eye by the GEOMETRIC normal (the vertex
                // normals can disagree with it at a silhouette).
                const float to_eye[3] = {camera.eye[0] - a.pos[0],
                                         camera.eye[1] - a.pos[1],
                                         camera.eye[2] - a.pos[2]};
                const float facing =
                    g[0] * to_eye[0] + g[1] * to_eye[1] + g[2] * to_eye[2];
                const Ndc pa = project(vp, a.pos);
                const Ndc pb = project(vp, b.pos);
                const Ndc pc = project(vp, c.pos);
                if (pa.w <= 0.0f || pb.w <= 0.0f || pc.w <= 0.0f) {
                    continue;  // behind the camera
                }
                const float area =
                    -0.5f * ((pa.x * pb.y - pb.x * pa.y) +
                             (pb.x * pc.y - pc.x * pb.y) +
                             (pc.x * pa.y - pa.x * pc.y));
                if (std::fabs(area) < 1e-9f || std::fabs(facing) < 1e-3f) {
                    continue;  // a sliver / edge-on: the sign is noise
                }
                ++checked;
                CHECK_MESSAGE((facing > 0.0f) == (area > 0.0f),
                              "mesh " << m << " triangle " << t / 3);
            }
        }
    }
    CHECK(checked > 500);  // the loop above really ran
}

// --- the placement math ------------------------------------------------------------

namespace {

using Mat3 = std::array<std::array<float, 3>, 3>;

Mat3 mul(const Mat3& l, const Mat3& r) {
    Mat3 out{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            for (int k = 0; k < 3; ++k) {
                out[i][j] += l[i][k] * r[k][j];
            }
        }
    }
    return out;
}

// Standard right-hand-rule rotations, spelled independently of
// model_matrix's multiplied-out form.
Mat3 rot_x(float a) {
    return {{{1, 0, 0}, {0, std::cos(a), -std::sin(a)}, {0, std::sin(a), std::cos(a)}}};
}
Mat3 rot_y(float a) {
    return {{{std::cos(a), 0, std::sin(a)}, {0, 1, 0}, {-std::sin(a), 0, std::cos(a)}}};
}
Mat3 rot_z(float a) {
    return {{{std::cos(a), -std::sin(a), 0}, {std::sin(a), std::cos(a), 0}, {0, 0, 1}}};
}

}  // namespace

TEST_CASE("model_matrix: scale * Ry(yaw) * Rx(pitch) * Rz(roll), translation in col 3") {
    const float angles[][3] = {{0, 0, 0},       {0.5f, 0, 0},      {0, 0.7f, 0},
                               {0, 0, -1.1f},   {0.3f, -0.8f, 1.9f}, {3.0f, 1.2f, -2.5f}};
    for (const auto& a : angles) {
        Transform3D t;
        t.yaw = a[0];
        t.pitch = a[1];
        t.roll = a[2];
        t.scale = 2.5f;
        t.translation[0] = 4.0f;
        t.translation[1] = -5.0f;
        t.translation[2] = 6.0f;
        const auto m = ws::ui::r3d::detail::model_matrix(t);
        const Mat3 r = mul(rot_y(a[0]), mul(rot_x(a[1]), rot_z(a[2])));
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                CHECK(m.rows[i][j] == doctest::Approx(r[i][j] * 2.5f).epsilon(1e-5f));
            }
            CHECK(m.rows[i][3] == t.translation[i]);
        }
    }
    // The headline meaning: yaw a quarter turn about +Y carries +Z to +X.
    Transform3D quarter;
    quarter.yaw = 1.57079632679f;
    const auto m = ws::ui::r3d::detail::model_matrix(quarter);
    CHECK(m.rows[0][2] == doctest::Approx(1.0f).epsilon(1e-5f));  // x of local +Z
    CHECK(m.rows[2][2] == doctest::Approx(0.0f).epsilon(1e-5f));  // z of local +Z
}

TEST_CASE("pack_tint / scene block / push layouts") {
    using namespace ws::ui::r3d::detail;
    // 0xRRGGBBAA -> the word unpackUnorm4x8 reads back as (r, g, b, a).
    CHECK(pack_tint(0x11223344u) == 0x44332211u);
    CHECK(pack_tint(0xFFFFFFFFu) == 0xFFFFFFFFu);
    CHECK(pack_tint(0xFF000080u) == 0x800000FFu);

    Light3D light;
    light.dir[0] = 0.0f;
    light.dir[1] = -3.0f;
    light.dir[2] = 4.0f;  // length 5
    light.color[0] = 2.0f;
    light.color[1] = -1.0f;  // nonsense: clamps to 0
    light.color[2] = 0.5f;
    light.ambient = 1.7f;  // clamps to 1
    const SceneBlock block = make_scene_block(light);
    CHECK(block.light_dir[0] == doctest::Approx(0.0f));
    CHECK(block.light_dir[1] == doctest::Approx(-0.6f));
    CHECK(block.light_dir[2] == doctest::Approx(0.8f));
    CHECK(block.light_color_ambient[0] == doctest::Approx(2.0f));
    CHECK(block.light_color_ambient[1] == 0.0f);
    CHECK(block.light_color_ambient[2] == doctest::Approx(0.5f));
    CHECK(block.light_color_ambient[3] == 1.0f);
    CHECK(make_scene_block(Light3D{}).light_color_ambient[3] == doctest::Approx(0.25f));

    CHECK(sizeof(MeshPush) == 120u);
    CHECK(sizeof(SceneBlock) == 32u);
}

// --- the weave: mesh draws between quad chunks --------------------------------------

namespace {

constexpr std::uint32_t kWhite = 0xFFFFFFFFu;

// One billboard in the open 3D block. (The z form: five floats + color.)
void billboard(Renderer2D& r) {
    r.draw_quad(0.0f, 0.0f, 0.0f, 10.0f, 10.0f, kWhite);
}

}  // namespace

TEST_CASE("weave: a frame of only meshes keeps them, with mark 0 and stats") {
    Renderer2D r;
    const auto cube = r.create_mesh(generate_cube(1.0f));
    REQUIRE(cube);
    r.frame_begin(100.0f, 100.0f);
    r.begin(ws::ui::r2d::Camera3D{});
    r.draw_mesh(*cube, Transform3D{}, MeshMaterial{});
    r.draw_mesh(*cube, Transform3D{}, MeshMaterial{});
    r.frame_end();
    REQUIRE(r.mesh_commands().size() == 2u);
    REQUIRE(r.mesh_chunk_marks().size() == 2u);
    CHECK(r.mesh_chunk_marks()[0] == 0u);
    CHECK(r.mesh_chunk_marks()[1] == 0u);
    CHECK(r.batch().chunk_count() == 0u);
    const auto stats = r.stats();
    CHECK(stats.mesh_draws == 2u);
    CHECK(stats.mesh_triangles == 24u);  // 2 cubes x 12
    CHECK(stats.quads == 0u);
}

TEST_CASE("weave: a mesh between two quad runs of one camera splits the chunk") {
    Renderer2D r;
    const auto cube = r.create_mesh(generate_cube(1.0f));
    REQUIRE(cube);

    // Control: the same two billboards (layers 0 and 2, one block) with no
    // mesh between them merge into ONE chunk.
    r.frame_begin(100.0f, 100.0f);
    r.begin(ws::ui::r2d::Camera3D{});
    r.set_layer(0u);
    billboard(r);
    r.set_layer(2u);
    billboard(r);
    r.frame_end();
    CHECK(r.batch().chunk_count() == 1u);

    // With a layer-1 mesh between them: two chunks, the mesh after chunk 0.
    r.frame_begin(100.0f, 100.0f);
    r.begin(ws::ui::r2d::Camera3D{});
    r.set_layer(0u);
    billboard(r);
    r.set_layer(1u);
    r.draw_mesh(*cube, Transform3D{}, MeshMaterial{});
    r.set_layer(2u);
    billboard(r);
    r.frame_end();
    REQUIRE(r.batch().chunk_count() == 2u);
    CHECK(r.batch().chunk(0).quad_count == 1u);
    CHECK(r.batch().chunk(1).quad_count == 1u);
    REQUIRE(r.mesh_chunk_marks().size() == 1u);
    CHECK(r.mesh_chunk_marks()[0] == 1u);
    CHECK(r.batch().chunk(0).depth_test);  // the 3D block's chunks
}

TEST_CASE("weave: ties draw the mesh first; epochs and layers order the rest") {
    Renderer2D r;
    const auto cube = r.create_mesh(generate_cube(1.0f));
    REQUIRE(cube);

    // Same layer, same epoch: the mesh is woven BEFORE the billboards (they
    // blend over opaque geometry), so its mark is 0 and the quads share one
    // chunk.
    r.frame_begin(100.0f, 100.0f);
    r.begin(ws::ui::r2d::Camera3D{});
    billboard(r);
    r.draw_mesh(*cube, Transform3D{}, MeshMaterial{});
    billboard(r);
    r.frame_end();
    REQUIRE(r.mesh_chunk_marks().size() == 1u);
    CHECK(r.mesh_chunk_marks()[0] == 0u);
    CHECK(r.batch().chunk_count() == 1u);
    CHECK(r.batch().chunk(0).quad_count == 2u);

    // A mesh in epoch 2 sorts after a layer-0 quad of epoch 1 and before a
    // layer-1 quad of epoch 1: the marks follow the sorted order.
    r.frame_begin(100.0f, 100.0f);
    r.begin(ws::ui::r2d::Camera3D{});  // epoch 1
    billboard(r);                      // layer 0
    r.set_layer(1u);
    billboard(r);                      // layer 1
    r.begin(ws::ui::r2d::Camera3D{});  // epoch 2
    r.set_layer(0u);
    r.draw_mesh(*cube, Transform3D{}, MeshMaterial{});  // (0, 2)
    r.set_layer(1u);
    r.draw_mesh(*cube, Transform3D{}, MeshMaterial{});  // (1, 2)
    r.frame_end();
    // Sorted by (layer, epoch): quad(0,1), mesh(0,2), quad(1,1), mesh(1,2).
    REQUIRE(r.mesh_chunk_marks().size() == 2u);
    CHECK(r.mesh_chunk_marks()[0] == 1u);  // after the first quad's chunk
    CHECK(r.mesh_chunk_marks()[1] == 2u);  // after the second quad's chunk
    CHECK(r.batch().chunk_count() == 2u);
    CHECK(r.mesh_chunk_marks()[0] <= r.mesh_chunk_marks()[1]);
}

TEST_CASE("weave: meshes after the last quad mark the chunk count; drops reset") {
    Renderer2D r;
    const auto cube = r.create_mesh(generate_cube(1.0f));
    REQUIRE(cube);
    r.frame_begin(100.0f, 100.0f);
    r.begin(ws::ui::r2d::Camera3D{});
    billboard(r);
    r.set_layer(5u);
    r.draw_mesh(*cube, Transform3D{}, MeshMaterial{});
    r.frame_end();
    REQUIRE(r.mesh_chunk_marks().size() == 1u);
    CHECK(r.mesh_chunk_marks()[0] == r.batch().chunk_count());
    CHECK(r.batch().chunk_count() == 1u);

    // An abandoned frame keeps nothing: no stale mesh to draw next time.
    r.frame_begin(100.0f, 100.0f);
    r.begin(ws::ui::r2d::Camera3D{});
    r.draw_mesh(*cube, Transform3D{}, MeshMaterial{});
    r.frame_drop();
    CHECK(r.mesh_commands().empty());
    CHECK(r.mesh_chunk_marks().empty());
}
