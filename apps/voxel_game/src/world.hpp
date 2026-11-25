// apps/voxel_game - world.hpp: the voxel world's data model.
//
// THE DESIGN (pure CPU except the mesh registry handles):
//   - Blocks: a 7-entry palette (air, grass, dirt, stone, wood, sand,
//     water), one tint per type - per-face shading (top bright, sides
//     darker) comes from the r3d pipeline's own directional light, not
//     baked colors, so one tint per type is enough.
//   - Storage: 8x8 chunks of 16x64x16 blocks, one flat vector each,
//     linear index x + z*16 + y*256 (y the slow axis: a column's stack
//     is contiguous - the walk the mesher and the height probe make).
//   - Meshing: per (chunk, block type) merged mesh with face culling -
//     a face between two solid blocks never emits (the neighbor rule).
//     Each mesh is one r3d::create_mesh upload; a rebuild is destroy +
//     create (the registry's whole-draw model). At most 6 solid types
//     per chunk -> 6 meshes per chunk, ~64 draws... per type-grouped
//     chunk, ~300 total draws for the 8x8 world - the r3d ring's scale.
//   - Interaction: the Amanatides & Woo DDA voxel raycast (game space)
//     for place/remove.
//
// COORDINATE FRAMES (the one real subtlety):
//   - GAME space: right-handed, +Y UP, +X east, +Z south, one block =
//     one meter. The physics world, the player, the raycast, the
//     terrain and the block table all live here.
//   - RENDER space: r3d's world has +Y DOWN (the 2D convention the
//     mesh path inherited). The mesher itself emits render-space
//     geometry directly: game (gx, gy, gz) -> render (gx+0.5,
//     -(gy+0.5), -(gz+0.5)) - the 180-degree turn about the x axis
//     that swaps up/north, a PROPER rotation (det +1), so CCW winding
//     survives it. (The first build negated only y - a mirror, det
//     -1 - which mirrored the world and inverted the view's
//     horizontal handedness: the "mouse x is flipped" field report.
//     The rotation is the fix; the camera rides the same map, so only
//     the mesher and the camera builder ever spell it.)

#pragma once

#include <cstdint>
#include <vector>

#include <ws/ui/renderer3d.hpp>  // MeshData, Mesh (the registry handle)

namespace voxel {

// --- blocks -----------------------------------------------------------------------

enum class Block : std::uint8_t {
  air = 0,
  grass,
  dirt,
  stone,
  wood,
  sand,
  water,
  count_,
};

// The tint the type's chunk meshes are drawn with (r2d's 0xRRGGBBAA).
[[nodiscard]] std::uint32_t tint_of(Block block);

// Solid for meshing/collision/raycast. Water is not solid (see-through,
// swimmable in a later life).
[[nodiscard]] bool is_solid(Block block);

// The GAME -> RENDER map, the one spell the mesher, the camera and the
// light all share: the 180-degree turn about the x axis (x, -y, -z) -
// a PROPER rotation (det +1), unlike the first build's y-only mirror
// whose det -1 mirrored the world (the "mouse x is flipped" field
// report: east landed on screen-left, so turning east read as turning
// left). Exposed for the frame's regression test.
[[nodiscard]] inline constexpr float game_to_render(
    float value, int axis /*0 = x, 1 = y, 2 = z*/) {
  return axis == 1 ? -value : (axis == 2 ? -value : value);
}

// --- chunks ------------------------------------------------------------------------

inline constexpr int kChunkSizeX = 16;
inline constexpr int kChunkSizeZ = 16;
inline constexpr int kChunkSizeY = 64;
inline constexpr int kBlocksPerChunk =
    kChunkSizeX * kChunkSizeZ * kChunkSizeY;
// The fixed world: 8x8 chunks = 128 x 128 blocks, 64 tall. Every mesh
// exists up front - no streaming in this first build.
inline constexpr int kWorldChunksX = 8;
inline constexpr int kWorldChunksZ = 8;
inline constexpr int kWorldSizeX = kWorldChunksX * kChunkSizeX;
inline constexpr int kWorldSizeZ = kWorldChunksZ * kChunkSizeZ;

// In-chunk linear index (see the storage note above).
[[nodiscard]] inline constexpr int block_index(int x, int y, int z) {
  return x + z * kChunkSizeX + y * kChunkSizeX * kChunkSizeZ;
}

// --- the world ---------------------------------------------------------------------

class World {
 public:
  // Generates the seeded terrain into the block table (deterministic
  // per seed - value-noise fbm over a hashed lattice, no third party).
  void generate(std::uint32_t seed);

  // The block at game-space integer coordinates; outside the region
  // reads air (the world floats in a void).
  [[nodiscard]] Block block_at(int x, int y, int z) const;

  // Sets one in-bounds block; true when the table changed. The caller
  // decides the mesh rebuild (the game rebuilds the touched chunk, and
  // a chunk-boundary block its neighbor's too).
  bool set_block(int x, int y, int z, Block block);

  // Builds (or rebuilds) one chunk's whole mesh set: one mesh per solid
  // block type present, each face-culled against the 6-neighbor rule
  // and uploaded through r3d::create_mesh. A type with no visible faces
  // has no mesh. Destroys the chunk's previous meshes first (an edit
  // rebuilds the chunk this way). Needs the renderer alive (call from
  // on_setup/on_update, not before run()).
  void rebuild_chunk(int cx, int cz);

  // Rebuilds every chunk's meshes (the startup build: 8x8 chunks).
  void rebuild_all();

  // Every chunk's meshes (the renderer walks this: one draw_mesh per
  // entry, at the entry's tint).
  struct ChunkMesh {
    int cx = 0;
    int cz = 0;
    Block type = Block::air;
    ws::ui::r3d::Mesh mesh{};
  };
  [[nodiscard]] const std::vector<ChunkMesh>& chunk_meshes() const {
    return chunk_meshes_;
  }

  // The highest solid block's y at a column, -1 when none (the spawn
  // probe).
  [[nodiscard]] int surface_height(int x, int z) const;

  // The COLLISION SHELL: true when the cell is solid and has at least
  // one non-solid 6-neighbor - the skin of the terrain, the trunks,
  // every placed block's face. Buried cells (solid on all six sides)
  // read false: nothing can ever touch them, so the physics grid keeps
  // exactly the exposed cells and re-derives membership around every
  // edit (a dig exposes the wall behind it, a place buries what it
  // covers). The void outside the world reads SOLID for this test -
  // the border columns' outboard faces are not "exposed" (nothing can
  // reach them without leaving the world, and there is no floor out
  // there anyway).
  [[nodiscard]] bool is_exposed(int x, int y, int z) const;

  // --- the DDA raycast ----------------------------------------------------------
  // Amanatides & Woo grid traversal in GAME space, from `origin` along
  // `direction` (normalized internally) up to max_distance. The first
  // SOLID block hit: its cell, the entry face's normal (the axis the
  // ray stepped through, negated - the face's outward direction), and
  // the distance. A miss reads hit = false. Place position = cell +
  // normal (the empty cell the hit face looks at).
  struct RayHit {
    bool hit = false;
    int cell[3] = {0, 0, 0};
    int normal[3] = {0, 0, 0};
    float distance = 0.0f;
  };
  [[nodiscard]] RayHit raycast(const float origin[3],
                               const float direction[3],
                               float max_distance) const;

 private:
  // Chunk-major: chunks_[cz * kWorldChunksX + cx], each kBlocksPerChunk.
  std::vector<std::vector<Block>> chunks_;
  std::vector<ChunkMesh> chunk_meshes_;
};

}  // namespace voxel
