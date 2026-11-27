// apps/voxel_game - world.hpp: the voxel world's data model.
//
// THE DESIGN (CPU-only world data and geometry):
//   - Blocks: compact IDs for air, terrain, vegetation and ores. Rendering
//     policy and GPU mesh ownership live outside this data model.
//   - Storage: 8x8 chunks of 16x64x16 blocks in one contiguous,
//     chunk-major allocation. Within each chunk x is fastest, then z,
//     then y: x + z*16 + y*256. Chunk spans are direct contiguous views.
//   - Meshing: CPU-only per-chunk/tile snapshots with greedy face merging.
//     A face between two occluding solid blocks never emits. The result
//     contains ordinary vertex/index arrays; VoxelRenderer owns GPU upload,
//     mesh replacement and draw handles.
//   - Interaction: the Amanatides & Woo DDA voxel raycast (game space)
//     for place/remove.
//
// COORDINATE FRAMES (the one real subtlety):
//   - GAME space: right-handed, +Y UP, +X east, +Z south, one block =
//     one meter. The voxel-grid collision queries, player, raycast,
//     terrain and block table all live here.
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

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "voxel_mesh.hpp"

#include <ws/gameplay/chunked_grid3d.hpp>
#include <ws/physics/3d/voxel_grid.hpp>

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
  cobblestone,
  planks,
  bricks,
  glass,
  leaves,
  coal_ore,
  iron_ore,
  count_,
};

enum class Biome : std::uint8_t {
  plains = 0,
  forest,
  desert,
  mountain,
  count_,
};

struct ColumnMetadata {
  Biome biome = Biome::plains;
  std::uint8_t feature_flags = 0;
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
// The fixed world: 16x16 chunks = 256 x 256 blocks, 64 tall. This
// matches the RGBA terrain data-map's native 256x256 spatial resolution;
// no resampling or loss of source columns is needed. Every mesh exists up
// front - no streaming in this first build.
inline constexpr int kWorldChunksX = 16;
inline constexpr int kWorldChunksZ = 16;
inline constexpr int kWorldSizeX = kWorldChunksX * kChunkSizeX;
inline constexpr int kWorldSizeZ = kWorldChunksZ * kChunkSizeZ;

using BlockStorage = ws::gameplay::ChunkedGrid3D<
    Block, kWorldChunksX, kWorldChunksZ,
    kChunkSizeX, kChunkSizeY, kChunkSizeZ>;

// In-chunk linear index, forwarded to the shared storage contract.
[[nodiscard]] inline constexpr int block_index(int x, int y, int z) {
  return static_cast<int>(BlockStorage::cell_index(x, y, z));
}

// Immutable worker input: one chunk plus a one-cell X/Z halo.
struct ChunkMeshSnapshot {
  static constexpr int kHaloSizeX = kChunkSizeX + 2;
  static constexpr int kHaloSizeZ = kChunkSizeZ + 2;
  std::array<Block, static_cast<std::size_t>(kHaloSizeX) * kChunkSizeY * kHaloSizeZ> blocks{};
  int cx = 0, cz = 0;
  std::uint64_t revision = 0;
  [[nodiscard]] Block at(int x, int y, int z) const noexcept {
    if (x < -1 || x > kChunkSizeX || y < 0 || y >= kChunkSizeY || z < -1 || z > kChunkSizeZ) return Block::air;
    const auto i = (static_cast<std::size_t>(y) * kHaloSizeZ + static_cast<std::size_t>(z + 1)) * kHaloSizeX + static_cast<std::size_t>(x + 1);
    return blocks[i];
  }
};
void mesh_chunk_snapshot(const ChunkMeshSnapshot& snapshot, std::vector<CpuMeshData>& tile_meshes);

// --- the world ---------------------------------------------------------------------

class World {
 public:
  // Generates seeded biome terrain: temperature/moisture climate fields,
  // plains/desert/forest/mountain surfaces, winding rivers, 3D cave
  // pockets and clustered ore veins. All fields are deterministic per
  // seed and continuous across chunk boundaries.
  void generate(std::uint32_t seed);

  // Clears blocks and per-column metadata without regenerating terrain.
  void reset_empty();

  [[nodiscard]] bool set_column_metadata(
      int x, int z, Biome biome, std::uint8_t feature_flags) noexcept;
  [[nodiscard]] ColumnMetadata column_metadata(int x, int z) const noexcept;


  // The block at game-space integer coordinates; outside the region
  // reads air (the world floats in a void).
  [[nodiscard]] Block block_at(int x, int y, int z) const;

  // Sets one in-bounds block; true when the table changed. The caller
  // decides the mesh rebuild (the game rebuilds the touched chunk, and
  // a chunk-boundary block its neighbor's too).
  bool set_block(int x, int y, int z, Block block);

  // Build a renderer-independent CPU mesh snapshot for one chunk.
  [[nodiscard]] std::vector<ChunkMeshData> build_chunk_meshes(
      int cx, int cz) const;

  [[nodiscard]] ChunkMeshSnapshot snapshot_chunk(int cx, int cz, std::uint64_t revision) const;
  // Reusable tile-indexed output for synchronous builds and worker scratch.
  void build_chunk_meshes(int cx, int cz, std::vector<CpuMeshData>& tile_meshes) const;

  // Direct read-only access to contiguous chunk storage. Chunk-local
  // algorithms use this for interior cells and block_at() only for the halo.
  [[nodiscard]] std::span<const Block> chunk_data(
      int cx, int cz) const noexcept { return blocks_.chunk(cx, cz); }

  // A zero-copy, read-only collision view over the world's chunk data.
  // The view is cheap to copy and remains valid while this World lives.
  [[nodiscard]] ws::physics3d::VoxelGrid3D collision_grid() const noexcept;

  // The highest solid block's y at a column, -1 when none (the spawn
  // probe).
  [[nodiscard]] int surface_height(int x, int z) const;

  // Surface-membership helper: true when a solid cell has at least one
  // non-solid 6-neighbor. Kept for terrain diagnostics/tests; runtime
  // collision queries use collision_grid() and the full solid occupancy.
  [[nodiscard]] bool is_exposed(int x, int y, int z) const;

  // --- the DDA raycast ----------------------------------------------------------
  // Amanatides & Woo grid traversal in GAME space, from `origin` along
  // `direction` (normalized internally) up to max_distance. The first
  // SOLID block hit: its cell, the entry face's normal (the axis the
  // ray stepped through, negated - the face's outward direction), and
  // the distance. A miss reads hit = false. Place position = cell +
  // normal (the empty cell the hit face looks at).
  using RayHit = ws::physics3d::VoxelGrid3D::RayHit;
  [[nodiscard]] RayHit raycast(const float origin[3],
                               const float direction[3],
                               float max_distance) const;

 private:
  static bool solid_query(const void* context, int x, int y,
                          int z) noexcept;

  // One contiguous chunk-major block volume; each chunk span contains
  // kBlocksPerChunk cells with x fastest and y slowest.
  BlockStorage blocks_;
  std::array<ColumnMetadata,
             static_cast<std::size_t>(kWorldSizeX) * kWorldSizeZ>
      column_metadata_{};
};

}  // namespace voxel
