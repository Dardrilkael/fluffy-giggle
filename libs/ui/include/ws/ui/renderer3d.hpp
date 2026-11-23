#pragma once

// ws::ui::r3d - the mesh-based 3D draw path (Phase 1: the CPU half).
//
// A THIRD pipeline beside r2d's two: the depth-off 2D painter and the
// depth-on billboard/ribbon one (both replay through batch.vert/frag).
// The mesh path draws INDEXED triangles with per-vertex normals - a
// lit, transformable surface family the quad uber-shader cannot spell.
// The 2D uber-shader (batch.vert/.frag) and batch_core's 40-byte
// Vertex are deliberately untouched; the mesh pipeline gets its own
// vertex layout (Vertex3D below) and, in Phase 2, its own pipeline
// object in renderer2d_device.cpp.
//
// Phase 1 scope - the CPU, headless-testable half:
//   - the mesh registry (create/destroy, generation-counted handles)
//   - four generators (cube, uv-sphere, plane, cylinder)
//   - command recording against the frame's Camera3D epoch
//   - the light state
// The device half (buffers, the pipeline_3d_mesh_, the flush) is Phase
// 2; until it lands a recorded mesh command simply does not draw, the
// same "empty contract" the shell documents for other staged work.
//
// Purity: this header is the workspace rule in miniature - a consumer
// compiles it with nothing but ws::core installed. Mesh handles mirror
// Texture handles (see renderer2d.hpp): id 0 is the "nothing" sentinel,
// every other id packs (generation << 20) | slot, and a stale handle
// fails the generation check at record time (warned once per frame,
// nothing recorded).

#include <cstddef>
#include <cstdint>
#include <vector>

#include <ws/core/result.hpp>  // Result/Status: the error currency of the
                               // workspace (root CLAUDE.md, "Error handling")
#include <ws/ui/renderer2d.hpp>  // Texture (mesh materials sample it),
                                 // Camera3D (the block kind draw_mesh
                                 // requires), rgba()

namespace ws::ui::r3d {

// --- handles -----------------------------------------------------------------------

// Opaque mesh handle, the Texture contract verbatim: id 0 = nothing
// (a destroyed or never-created mesh), every other id is
// (generation << 20) | slot. destroy_mesh kills it the moment it
// returns - further draws warn once per frame and record nothing -
// and the freed slot is what a later create reuses, so a stale handle
// kept past that fails the generation check. The 12-bit generation
// wraps after 4096 reuses of one slot (the documented Texture limit's
// own limit - one mesh slot wraps as rarely as a texture slot does).
struct Mesh {
    std::uint32_t id = 0;
};

// --- mesh data ---------------------------------------------------------------------

// One vertex of the mesh layout. pos/normal in the mesh's OWN local
// space (the transform carries it to world; the lighting math in the
// Phase 2 shader will rotate the normal by the same transform's
// rotation part). uv over [0,1]². 44 bytes, packed, no padding - the
// Phase 2 pipeline's vertex input state pins these offsets.
struct Vertex3D {
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float normal[3] = {0.0f, 0.0f, 1.0f};
    float uv[2] = {0.0f, 0.0f};
};
static_assert(sizeof(Vertex3D) == 32, "mesh vertex layout is load-bearing (the Phase 2 pipeline's input state)");

// A whole mesh's CPU payload: the generators' answer, create_mesh's
// input. indices name vertices by array position (triangle list,
// three per triangle). An empty mesh is a valid no-op draw (nothing
// records); an out-of-range index fails create_mesh with
// MeshError::bad_index (corrupt data, not a draw-time concern - the
// registry never accepts it, so the GPU never sees it).
struct MeshData {
    std::vector<Vertex3D> vertices;
    std::vector<std::uint32_t> indices;  // triangle list, 3k entries
};

// The material a mesh draw carries. tint is 0xRRGGBBAA (rgba()'s
// packing, the r2d convention); texture {} (id 0) means untextured -
// the draw shades the tint alone (the pipeline's white-texture
// fallback, the same contract every untextured r2d draw keeps).
struct MeshMaterial {
    std::uint32_t tint = r2d::rgba(255, 255, 255);
    r2d::Texture texture{};
};

// The placement of one draw: translation, UNIFORM scale, and a
// rotation as a yaw/pitch/roll Euler triple (radians, applied in that
// order: yaw about +Y, then pitch about +X, then roll about +Z - the
// intuitive "heading, then tilt, then bank" reading). Eulers, not a
// matrix or quaternion: the public API stays ws-plain (no math header
// in this surface), and the Phase 2 push constants bake the matrix
// from these four numbers at record time.
struct Transform3D {
    float translation[3] = {0.0f, 0.0f, 0.0f};
    float scale = 1.0f;  // uniform; <= 0 refuses the draw with a warn
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
};

// The scene light draw_mesh shades against: one directional light plus
// an ambient term (the classic minimal lighting model). dir does not
// need to be unit-length - set_light normalizes it; a zero vector
// refuses with a Status (no direction to light along) and keeps the
// previous light. Color is 0xRRGGBBAA with alpha unused (the ambient
// intensity rides the color's own brightness, not alpha).
struct Light3D {
    float dir[3] = {0.0f, -1.0f, 0.0f};  // TO the light (the shading's
                                         // negated dot)
    float color[3] = {1.0f, 1.0f, 1.0f};  // linear rgb, [0, inf)
    float ambient = 0.25f;  // the flat floor every surface keeps
};

// --- the registry ------------------------------------------------------------------

// Uploads one mesh (CPU payload in, generation-counted handle out).
// Valid from on_setup() onward, like load_texture; synchronous.
// Failures - the r3d half of the ui error family (category "ui",
// codes append-only):
//   - no_app        : no renderer is alive (the headless contract)
//   - empty_mesh    : no vertices at all (an empty indices vector is
//                     legal - nothing draws)
//   - bad_index     : an index >= vertices.size() (corrupt data; the
//                     registry refuses it whole)
// A failed create changes nothing; the caller's handle stays whatever
// it was. Indices beyond 3 that do not form whole triangles are NOT an
// error - the trailing partial triangle is dropped (a generator bug
// would otherwise take the whole draw down).
[[nodiscard]] ws::core::Result<Mesh> create_mesh(const MeshData& data);

// Kills a mesh the moment it returns: further draw_mesh calls with the
// handle warn once per frame and record nothing. Mesh{} (id 0) and
// unknown ids are a warned no-op returning the failure Status (an app
// double-destroying is a bug worth naming), a valid live handle
// returns success. The CPU registry frees at once; the Phase 2 GPU
// buffers will retire through the same frames-in-flight queue the
// texture path uses (documented here so the Phase 2 change adds no new
// contract).
[[nodiscard]] ws::core::Status destroy_mesh(Mesh mesh);

// --- recording ---------------------------------------------------------------------

// Records one mesh draw against the CURRENT begin(Camera3D) block:
// valid only inside one (the 3D billboard rule verbatim - inside a 2D
// block or outside any block it warns once per frame and records
// nothing). Carries the caller's current layer and clip, the same
// (layer, camera epoch) sort keys the quad commands ride, so mesh and
// quad draws interleave in one stable painter's order per block. A
// stale/destroyed mesh, or a degenerate transform (scale <= 0), warns
// once per frame and records nothing - the draw is data until the
// frame ends, so refusing it here is the whole cost. Returns nothing
// (drop-safe, the r2d drawing-call convention: creates and destroys
// return Results, draws never did).
void draw_mesh(Mesh mesh, const Transform3D& transform,
               const MeshMaterial& material);

// The scene light for every mesh draw of the frame that follows. Not
// block-scoped: the one light serves the whole frame (the ambient
// floor plus one directional term is the entire model). A zero dir
// returns the bad_dir Status and keeps the previous light; alpha in
// the color rides unused. Valid anywhere - it is scene state, not a
// draw (the same standing set_layer() keeps).
[[nodiscard]] ws::core::Status set_light(const Light3D& light);

// --- generators --------------------------------------------------------------------

// The generator family - pure CPU, no renderer state, headless
// testable by construction. Every generator answers a closed,
// two-manifold-or-bounded mesh with OUTWARD unit normals and
// triangle-list indices; counts are pinned in the test suite.

// Axis-aligned cube, size across each axis (x/y/z extents, centered
// on the origin): 24 vertices (4 per face, flat normals), 36 indices.
// UVs map each face to the full [0,1]² (the texture repeats per face -
// the box unwrap's own convention, no per-face atlas offsets).
[[nodiscard]] MeshData generate_cube(float size);

// UV sphere, radius, stacks/sectors (rings of quads capped with
// triangles). rows = stacks + 1 latitude rings; sector_count around.
// Vertex count: (stacks - 1) * sectors + 2 poles. Normals are the
// normalized positions (the sphere's own). degenerate inputs (radius
// <= 0, stacks < 2, sectors < 3) return the empty MeshData (the
// caller's create_mesh then fails empty_mesh - one loud path, not
// two).
[[nodiscard]] MeshData generate_sphere(float radius, int stacks,
                                       int sectors);

// XZ plane (y = 0, normal +Y), half-extent `extent` each side
// (side = 2*extent), subdivided `subdivisions` per axis (>= 1; a
// smaller value clamps to 1). (subdivisions+1)² vertices, the tessell
// grid's own indices. The checker/ground-plate family.
[[nodiscard]] MeshData generate_plane(float extent, int subdivisions);

// Y-axis cylinder, radius, half-height (height = 2*half_height),
// radial `slices` (>= 3; smaller clamps to 3 - a cylinder needs three
// sides to enclose anything): side wall quads plus one center-fan cap
// per end, flat normals on the caps, smooth (shared-vertex) normals
// on the wall. UV: u = angle around, v = height up the wall; caps map
// the fan to a disc (polar u/v - radius to v, angle to u). radius
// <= 0 or half_height <= 0 returns the empty MeshData.
[[nodiscard]] MeshData generate_cylinder(float radius, float half_height,
                                         int slices);

}  // namespace ws::ui::r3d
