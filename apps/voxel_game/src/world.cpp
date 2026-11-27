// apps/voxel_game - world.cpp: terrain generation, chunk meshing, the
// DDA raycast. See world.hpp for the design (the two coordinate
// frames, the per-(chunk, type) mesh set, the neighbor culling rule).
//
// Terrain: deterministic multi-field FBM climate and height generation.
// Temperature/moisture select desert, plains and forest; ridged height
// fields create mountain regions; warped low-frequency noise carves
// rivers. 3D coherent density carves caves and clustered 3D noise places
// ore veins. All fields are seed-stable and continuous across chunks.

#include "texture_atlas.hpp"
#include "world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>

#include <ws/math/noise.hpp>

namespace voxel {

// The generated atlas is indexed directly by Block's underlying value.
// A stale atlas is a hard compile error rather than an out-of-bounds read
// that can scramble face materials across the terrain.
static_assert(sizeof(atlas::kTileFor) / sizeof(atlas::kTileFor[0]) ==
              static_cast<std::size_t>(Block::count_),
              "texture_atlas.hpp is stale: run scripts/create_atlas.py");

// --- the palette ------------------------------------------------------------------

namespace {

constexpr std::uint32_t pack_rgba(std::uint8_t r, std::uint8_t g,
                                   std::uint8_t b, std::uint8_t a) noexcept {
  return (static_cast<std::uint32_t>(r) << 24) |
         (static_cast<std::uint32_t>(g) << 16) |
         (static_cast<std::uint32_t>(b) << 8) |
         static_cast<std::uint32_t>(a);
}
const std::uint32_t kTints[static_cast<std::size_t>(Block::count_)] = {
    0u, pack_rgba(106, 170, 64, 255), pack_rgba(134, 96, 67, 255),
    pack_rgba(125, 125, 130, 255), pack_rgba(106, 82, 51, 255),
    pack_rgba(218, 210, 158, 255), pack_rgba(50, 104, 170, 255),
    pack_rgba(105, 105, 110, 255), pack_rgba(164, 126, 83, 255),
    pack_rgba(174, 76, 61, 255), pack_rgba(160, 210, 220, 190),
    pack_rgba(65, 125, 55, 255), pack_rgba(45, 45, 48, 255),
    pack_rgba(170, 125, 95, 255),
};

// --- deterministic terrain fields ------------------------------------------------

using ws::math::noise::fbm2d;
using ws::math::noise::hash01;
using ws::math::noise::smooth01;
using ws::math::noise::value_noise2d;
using ws::math::noise::value_noise3d;

[[nodiscard]] bool tree_at(int x, int z, std::uint32_t seed,
                           float density) {
  return hash01(static_cast<std::uint32_t>(x) * 3u,
                static_cast<std::uint32_t>(z) * 3u,
                seed ^ 0x1F1F1F1Fu) > 1.0f - density;
}

constexpr int kWaterLevel = 12;
constexpr int kWorldFloor = 1;

// --- the mesher (GREEDY) ----------------------------------------------------------
// The mesher has two stages: one chunk traversal classifies all exposed
// faces and their atlas tile IDs; direction/plane sweeps then greedily merge
// cached faces by tile. Equal face textures can merge across block IDs.
//
// Face existence preserves the neighbor rule: solid blocks hide solid
// neighbors; water emits only its top against non-water/non-solid cells.
// Geometry is RENDER space
// (world.hpp's frame note), winding CCW by the right-hand (u x v) . n
// test, normals per face direction - all unchanged.
//
// UVs: a merged W x H quad carries (u, v) in TILE UNITS [0, W] x [0, H];
// mesh.frag's wrap_tiled repeats the tile per unit (the tile's own
// standalone texture - one per draw - samples its full [0,1]).
namespace {

// The 6 face directions: cell offset + game-space outward normal.
struct FaceDir {
  int dx, dy, dz;
  float normal[3];
};
constexpr FaceDir kFaces[6] = {
    {0, 1, 0, {0.0f, 1.0f, 0.0f}},    // top
    {0, -1, 0, {0.0f, -1.0f, 0.0f}},  // bottom
    {1, 0, 0, {1.0f, 0.0f, 0.0f}},    // east
    {-1, 0, 0, {-1.0f, 0.0f, 0.0f}},  // west
    {0, 0, 1, {0.0f, 0.0f, 1.0f}},    // south
    {0, 0, -1, {0.0f, 0.0f, -1.0f}},  // north
};

// Each face's tangent basis (GAME space, y up): (u, v) with u x v = n.
// Side faces deliberately keep U horizontal and V vertical so the top
// edge of grass_block_side stays at the TOP of every visible block face.
// The old east/north bases swapped those axes, rotating the grass fringe
// onto a vertical edge on two sides of each block.
constexpr float kUVB[6][2][3] = {
    {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},  // top: u=+x (east) v=-z (north)
    {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},   // bottom: u=+x v=+z
    {{0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}}, // east: u=-z v=+y
    {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},  // west: u=+z v=+y
    {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},  // south: u=+x v=+y
    {{-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}, // north: u=-x v=+y
};

// Emits one merged quad for the greedy rectangle (slice sa, [pu, pu+w)
// x [pv, pv+h) on face fi's plane lattice. The lattice walk mirrors
// the mask build exactly - the two can never disagree where a cell is.
void emit_quad(CpuMeshData& out, int fi, int ox,
                int oz, int sa, int pu, int pv, int w, int h,
                int u_len, int v_len) {
  const FaceDir& face = kFaces[fi];
  const int ux = static_cast<int>(kUVB[fi][0][0]);
  const int uy = static_cast<int>(kUVB[fi][0][1]);
  const int uz = static_cast<int>(kUVB[fi][0][2]);
  const int vx = static_cast<int>(kUVB[fi][1][0]);
  const int vy = static_cast<int>(kUVB[fi][1][1]);
  const int vz = static_cast<int>(kUVB[fi][1][2]);

  // Chunk-local cell coords of one lattice point (the mask's walk).
  const auto cell_of = [&](int s, int pU, int pV) {
    int lx = 0, ly = 0, lz = 0;
    lx += ux != 0 ? (ux > 0 ? pU : u_len - 1 - pU) : 0;
    ly += uy != 0 ? (uy > 0 ? pU : u_len - 1 - pU) : 0;
    lz += uz != 0 ? (uz > 0 ? pU : u_len - 1 - pU) : 0;
    lx += vx != 0 ? (vx > 0 ? pV : v_len - 1 - pV) : 0;
    ly += vy != 0 ? (vy > 0 ? pV : v_len - 1 - pV) : 0;
    lz += vz != 0 ? (vz > 0 ? pV : v_len - 1 - pV) : 0;
    if (face.dx != 0) lx = s;
    if (face.dy != 0) ly = s;
    if (face.dz != 0) lz = s;
    return std::array<int, 3>{ox + lx, ly, oz + lz};
  };

  // The covered rectangle's four corner CELLS; the quad rides the
  // boundary plane between the cells and their neighbors across the
  // normal. Compute the min/max cell corners, then the quad's corners.
  const auto c00 = cell_of(sa, pu, pv);
  const auto cW0 = cell_of(sa, pu + w - 1, pv);
  const auto c0H = cell_of(sa, pu, pv + h - 1);
  const auto cWH = cell_of(sa, pu + w - 1, pv + h - 1);
  // Cell corners: the face covers each cell's full [0,1]x[0,1] square
  // on the plane; the quad spans min(corner0) .. max(cornerW) in the
  // plane's axes, pushed half a block outward along the normal.
  float pmin[3] = {1e9f, 1e9f, 1e9f};
  float pmax[3] = {-1e9f, -1e9f, -1e9f};
  for (const auto& c : {c00, cW0, c0H, cWH}) {
    for (int a = 0; a < 3; ++a) {
      pmin[a] = std::min(pmin[a], static_cast<float>(c[a]));
      pmax[a] = std::max(pmax[a], static_cast<float>(c[a] + 1));
    }
  }
  const float nrm[3] = {face.normal[0], face.normal[1], face.normal[2]};
  // The plane offset: the boundary between the cell and its neighbor -
  // for +faces, the cell's +1 face; for -faces, the cell's -0 face.
  // Both are min/max already in pmin/pmax along the normal's axis when
  // w or h spans the normal axis? No - the normal axis is neither u
  // nor v; the plane sits at the cell's boundary along the normal:
  // +dir faces at pmax along the axis, -dir faces at pmin.
  const int na = nrm[0] != 0.0f ? 0 : (nrm[1] != 0.0f ? 1 : 2);
  const float plane = nrm[na] > 0.0f ? pmax[na] : pmin[na];
  // Build the quad's 4 corners in the plane basis (u, v GAME vectors).
  const float* ub = kUVB[fi][0];
  const float* vb = kUVB[fi][1];
  // Plane anchor: the min corner of the rectangle projected on the
  // plane axes; walk (u, v) from 0 to (extent_u, extent_v).
  // The rectangle's extent along u/v: the bounding box's span along
  // the u and v basis directions (they are axis-aligned unit steps).
  float anchor[3];
  for (int a = 0; a < 3; ++a) {
    // The anchor must be the corner from which the positive (u, v)
    // lattice walk starts. For a negative tangent axis, that is the
    // maximum coordinate, not pmin: starting at pmin and then adding
    // a negative basis vector shifts the whole greedy quad outside the
    // cells it is meant to cover (notably every horizontal face's -Z
    // tangent). Each non-normal axis belongs to exactly one tangent.
    const bool negative_u = ub[a] < 0.0f;
    const bool negative_v = vb[a] < 0.0f;
    anchor[a] = (negative_u || negative_v) ? pmax[a] : pmin[a];
  }
  anchor[na] = plane;
  // Extents: the covered cells span w cells along u and h along v
  // (the lattice's own units; u/v steps are unit-length).
  const float eu = static_cast<float>(w);
  const float ev = static_cast<float>(h);
  // The rectangle's corners: anchor, +u, +v, +u+v.
  const float signs[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f},
                             {1.0f, 1.0f}, {0.0f, 1.0f}};
  float corners[4][3];
  for (int ci = 0; ci < 4; ++ci) {
    const float su = signs[ci][0] * eu;
    const float sv = signs[ci][1] * ev;
    corners[ci][0] = anchor[0] + su * ub[0] + sv * vb[0];
    corners[ci][1] = anchor[1] + su * ub[1] + sv * vb[1];
    corners[ci][2] = anchor[2] + su * ub[2] + sv * vb[2];
  }
  // Winding: CCW seen from the normal's side = (u x v) . n >= 0 - the
  // basis guarantees it; emit 0,1,2 / 2,3,0 (the per-face mesher's
  // own order, preserved by the proper game_to_render rotation).
  const std::uint32_t base =
      static_cast<std::uint32_t>(out.vertices.size());
  for (int ci = 0; ci < 4; ++ci) {
    CpuMeshVertex vertex;
    vertex.pos[0] = game_to_render(corners[ci][0], 0);
    vertex.pos[1] = game_to_render(corners[ci][1], 1);
    vertex.pos[2] = game_to_render(corners[ci][2], 2);
    // Normals are VECTORS: rotate by (x, -y, -z) only - the point
    // transform's +0.5/-0.5/-0.5 translation contaminated every
    // normal (a top face read (0.5, -1.5, -0.5) instead of (0, -1, 0)).
    vertex.normal[0] = nrm[0];
    vertex.normal[1] = -nrm[1];
    vertex.normal[2] = -nrm[2];
    // UVs in TILE UNITS: [0, w] x [0, h], v flipped inside each unit
    // (the per-cell mesher's own orientation rule, generalized).
    vertex.uv[0] = signs[ci][0] * eu;
    vertex.uv[1] = (1.0f - signs[ci][1]) * ev;
    out.vertices.push_back(vertex);
  }
  out.indices.push_back(base + 0);
  out.indices.push_back(base + 1);
  out.indices.push_back(base + 2);
  out.indices.push_back(base + 2);
  out.indices.push_back(base + 3);
  out.indices.push_back(base + 0);
}

// Cache the texture tile for every exposed face in one chunk traversal.
// -1 means that the face is hidden. Tile identity is the render-material
// boundary, so equal textures can merge even when the block IDs differ.
using FaceTileCache =
    std::array<std::int8_t, 6u * static_cast<std::size_t>(kBlocksPerChunk)>;

[[nodiscard]] FaceTileCache classify_chunk_faces(const ChunkMeshSnapshot& snapshot) {
  FaceTileCache face_tiles{};
  face_tiles.fill(static_cast<std::int8_t>(-1));

  // x is the fastest storage axis. Neighbor classification for all six
  // directions happens while each block is visited exactly once.
  for (int y = 0; y < kChunkSizeY; ++y) {
    for (int z = 0; z < kChunkSizeZ; ++z) {
      for (int x = 0; x < kChunkSizeX; ++x) {
        const std::size_t cell =
            static_cast<std::size_t>(block_index(x, y, z));
        const Block type = snapshot.at(x, y, z);
        if (type == Block::air) continue;

        const std::size_t type_index = static_cast<std::size_t>(type);
        if (type_index >= static_cast<std::size_t>(Block::count_)) continue;

        for (int fi = 0; fi < 6; ++fi) {
          const int tile = atlas::kTileFor[type_index][fi];
          if (tile < 0 || tile >= atlas::kTileCount) continue;

          const FaceDir& face = kFaces[fi];
          const int nx = x + face.dx;
          const int ny = y + face.dy;
          const int nz = z + face.dz;
          const Block neighbor = snapshot.at(nx, ny, nz);

          // Water is non-solid and only its top face is textured. It does
          // not emit against another water cell or a solid block.
          if (type == Block::water) {
            if (neighbor == Block::water || is_solid(neighbor)) continue;
          } else if (is_solid(neighbor)) {
            continue;
          }

          face_tiles[static_cast<std::size_t>(fi) *
                  static_cast<std::size_t>(kBlocksPerChunk) + cell] =
              static_cast<std::int8_t>(tile);
        }
      }
    }
  }
  return face_tiles;
}

// Merge one face direction from the cached tile IDs. Per-plane masks are
// grouped by tile, and consumed cells are cleared in-place (no second used
// bitset). This stage never reads voxel data or checks neighbors.
void greedy_faces(const FaceTileCache& face_tiles, int fi, int cx, int cz,
                  std::vector<CpuMeshData>& outputs) {
  const FaceDir& face = kFaces[fi];
  const int ox = cx * kChunkSizeX;
  const int oz = cz * kChunkSizeZ;
  const int ux = static_cast<int>(kUVB[fi][0][0]);
  const int uy = static_cast<int>(kUVB[fi][0][1]);
  const int uz = static_cast<int>(kUVB[fi][0][2]);
  const int vx = static_cast<int>(kUVB[fi][1][0]);
  const int vy = static_cast<int>(kUVB[fi][1][1]);
  const int vz = static_cast<int>(kUVB[fi][1][2]);
  const int u_len = (ux != 0) ? kChunkSizeX : (uy != 0) ? kChunkSizeY
                                                        : kChunkSizeZ;
  const int v_len = (vx != 0) ? kChunkSizeX : (vy != 0) ? kChunkSizeY
                                                        : kChunkSizeZ;
  const int a_len = (face.dy != 0) ? kChunkSizeY
                    : (face.dx != 0) ? kChunkSizeX
                                     : kChunkSizeZ;

  const auto cell_of = [&](int s, int pU, int pV) {
    int lx = 0, ly = 0, lz = 0;
    lx += ux != 0 ? (ux > 0 ? pU : u_len - 1 - pU) : 0;
    ly += uy != 0 ? (uy > 0 ? pU : u_len - 1 - pU) : 0;
    lz += uz != 0 ? (uz > 0 ? pU : u_len - 1 - pU) : 0;
    lx += vx != 0 ? (vx > 0 ? pV : v_len - 1 - pV) : 0;
    ly += vy != 0 ? (vy > 0 ? pV : v_len - 1 - pV) : 0;
    lz += vz != 0 ? (vz > 0 ? pV : v_len - 1 - pV) : 0;
    if (face.dx != 0) lx = s;
    if (face.dy != 0) ly = s;
    if (face.dz != 0) lz = s;
    return std::array<int, 3>{lx, ly, lz};
  };

  constexpr std::size_t kMaxPlaneArea =
      static_cast<std::size_t>(kChunkSizeY) * kChunkSizeX;
  std::array<std::array<std::uint8_t, kMaxPlaneArea>,
             atlas::kTileCount> masks{};
  const std::size_t face_offset =
      static_cast<std::size_t>(fi) *
          static_cast<std::size_t>(kBlocksPerChunk);

  for (int sa = 0; sa < a_len; ++sa) {
    std::array<bool, atlas::kTileCount> active_tiles{};
    // Populate every tile's mask in one plane traversal.
    for (int pu = 0; pu < u_len; ++pu) {
      for (int pv = 0; pv < v_len; ++pv) {
        const auto cell = cell_of(sa, pu, pv);
        const std::size_t source_index = face_offset +
            static_cast<std::size_t>(block_index(cell[0], cell[1], cell[2]));
        const std::int8_t tile_id = face_tiles[source_index];
        if (tile_id < 0) continue;

        const std::size_t tile = static_cast<std::size_t>(tile_id);
        const std::size_t mask_index =
            static_cast<std::size_t>(pu) * v_len + pv;
        masks[tile][mask_index] = 1;
        active_tiles[tile] = true;
      }
    }

    // No voxel reads in the greedy stage. Absent material masks are skipped.
    for (int tile = 0; tile < atlas::kTileCount; ++tile) {
      if (!active_tiles[static_cast<std::size_t>(tile)]) continue;
      auto& mask = masks[static_cast<std::size_t>(tile)];

      for (int pu = 0; pu < u_len; ++pu) {
        for (int pv = 0; pv < v_len; ++pv) {
          const std::size_t idx =
              static_cast<std::size_t>(pu) * v_len + pv;
          if (mask[idx] == 0) continue;

          int w = 1;
          while (pu + w < u_len &&
                 mask[idx + static_cast<std::size_t>(w) * v_len] != 0) {
            ++w;
          }
          int h = 1;
          bool grew = true;
          while (grew && pv + h < v_len) {
            for (int k = 0; k < w; ++k) {
              const std::size_t probe =
                  static_cast<std::size_t>(pu + k) * v_len +
                  static_cast<std::size_t>(pv + h);
              if (mask[probe] == 0) {
                grew = false;
                break;
              }
            }
            if (grew) ++h;
          }

          for (int dw = 0; dw < w; ++dw) {
            for (int dh = 0; dh < h; ++dh) {
              mask[static_cast<std::size_t>(pu + dw) * v_len +
                   static_cast<std::size_t>(pv + dh)] = 0;
            }
          }
          emit_quad(outputs[static_cast<std::size_t>(tile)], fi, ox, oz,
                    sa, pu, pv, w, h, u_len, v_len);
        }
      }
    }
  }
}

}  // namespace


}  // namespace

// --- the API ----------------------------------------------------------------------

std::uint32_t tint_of(Block block) {
  return kTints[static_cast<std::size_t>(block)];
}

bool is_solid(Block block) {
  return block != Block::air && block != Block::water;
}

void World::reset_empty() {
  blocks_.reset(Block::air);
  column_metadata_.fill(ColumnMetadata{});
}

bool World::set_column_metadata(
    int x, int z, Biome biome, std::uint8_t feature_flags) noexcept {
  if (x < 0 || x >= kWorldSizeX || z < 0 || z >= kWorldSizeZ ||
      static_cast<std::size_t>(biome) >=
          static_cast<std::size_t>(Biome::count_)) {
    return false;
  }
  const auto index = static_cast<std::size_t>(x) +
                     static_cast<std::size_t>(z) * kWorldSizeX;
  column_metadata_[index] = ColumnMetadata{biome, feature_flags};
  return true;
}

ColumnMetadata World::column_metadata(int x, int z) const noexcept {
  if (x < 0 || x >= kWorldSizeX || z < 0 || z >= kWorldSizeZ) {
    return {};
  }
  const auto index = static_cast<std::size_t>(x) +
                     static_cast<std::size_t>(z) * kWorldSizeX;
  return column_metadata_[index];
}

void World::generate(std::uint32_t seed) {
  reset_empty();

  // Generate chunk-by-chunk for contiguous writes. Every noise field
  // still uses WORLD coordinates, so adjacent chunks share continuous
  // terrain; all calculations for a column are independent of traversal order.
  for (int cz = 0; cz < kWorldChunksZ; ++cz) {
    for (int cx = 0; cx < kWorldChunksX; ++cx) {
      auto chunk = blocks_.chunk(cx, cz);
      const int chunk_origin_x = cx * kChunkSizeX;
      const int chunk_origin_z = cz * kChunkSizeZ;
      for (int lz = 0; lz < kChunkSizeZ; ++lz) {
        const int wz = chunk_origin_z + lz;
        for (int lx = 0; lx < kChunkSizeX; ++lx) {
          const int wx = chunk_origin_x + lx;
          const float continental = fbm2d(wx * 0.010f, wz * 0.010f,
                                         seed ^ 0x31415926u);
          const float hills = fbm2d(wx * 0.027f, wz * 0.027f,
                                  seed ^ 0x27182818u);
          const float ridge_noise = value_noise2d(wx * 0.018f, wz * 0.018f,
                                            seed ^ 0xA341316Cu);
          const float ridge = 1.0f - std::abs(ridge_noise * 2.0f - 1.0f);
          const float mountain_mask = smooth01((continental - 0.48f) * 3.2f);
          const float temperature = fbm2d(wx * 0.009f, wz * 0.009f,
                                        seed ^ 0xB5297A4Du);
          const float moisture = fbm2d(wx * 0.013f, wz * 0.013f,
                                     seed ^ 0x68E31DA4u);
          const bool desert = temperature > 0.53f && moisture < 0.43f;
          const bool forest = moisture > 0.54f && temperature > 0.30f;
          const bool mountains = mountain_mask > 0.48f && ridge > 0.38f;
    
          float terrain = 13.0f + (continental - 0.5f) * 19.0f +
                          (hills - 0.5f) * 11.0f +
                          mountain_mask * ridge * 23.0f;
          terrain = std::clamp(terrain, 3.0f, 46.0f);
    
          // A low-frequency warped noise field creates winding river corridors.
          // The channel floor approaches the water plane; nearby banks become sand.
          const float river_field = value_noise2d(wx * 0.016f, wz * 0.016f,
                                            seed ^ 0xC0FFEE11u);
          const float river_distance = std::abs(river_field - 0.5f);
          const bool river = river_distance < 0.035f && !mountains;
          const bool river_bank = river_distance < 0.065f && !mountains;
          if (river) terrain = std::min(terrain, 10.0f + river_distance * 35.0f);
    
          const int height = std::clamp(static_cast<int>(terrain + 0.5f),
                                        kWorldFloor + 1, kChunkSizeY - 9);
          const bool shoreline = height <= kWaterLevel + 1;
          const Block surface_block =
              (desert || river_bank || shoreline) ? Block::sand
              : (mountains ? Block::stone : Block::grass);
          const float tree_density = forest ? 0.075f : 0.012f;
          const bool has_tree = !desert && !mountains && !river_bank &&
                                height > kWaterLevel + 1 &&
                                tree_at(wx, wz, seed, tree_density);
    
    
          for (int y = kWorldFloor; y <= height; ++y) {
            Block block = Block::stone;
            if (y == height) {
              block = surface_block;
            } else if (surface_block == Block::stone) {
              block = Block::stone;  // exposed mountain rock continues below cap
            } else if (y >= height - (surface_block == Block::sand ? 2 : 4)) {
              block = surface_block == Block::sand ? Block::sand : Block::dirt;
            }
    
            // 3D coherent density carves connected cave pockets below the
            // surface. Keep a solid roof and floor so caves do not open
            // randomly through the surface or world bottom.
            if (y > kWorldFloor + 2 && y < height - 4) {
              const float cave_a = value_noise3d(wx * 0.075f, y * 0.075f,
                                           wz * 0.075f, seed ^ 0xDEADBEEFu);
              const float cave_b = value_noise3d(wx * 0.145f, y * 0.145f,
                                           wz * 0.145f, seed ^ 0x8BADF00Du);
              if (cave_a > 0.64f && cave_b > 0.48f) block = Block::air;
            }
    
            // Ore is placed in clustered 3D veins rather than isolated rolls.
            if (block == Block::stone && y >= 4 && y <= 22) {
              const float vein = value_noise3d(wx * 0.19f, y * 0.22f, wz * 0.19f,
                                         seed ^ 0x51ED270Bu);
              const float pockets = value_noise3d(wx * 0.065f, y * 0.08f,
                                            wz * 0.065f, seed ^ 0xBADC0DEu);
              if (vein > 0.69f && pockets > 0.48f) {
                block = y < 11 ? Block::coal_ore : Block::iron_ore;
              }
            }
            chunk[static_cast<std::size_t>(block_index(lx, y, lz))] = block;
          }
    
          // Water fills low ground, including the carved river channels.
          for (int y = height + 1; y <= kWaterLevel; ++y) {
            chunk[static_cast<std::size_t>(block_index(lx, y, lz))] = Block::water;
          }
    
          if (has_tree) {
            for (int y = height + 1; y <= height + 4; ++y) {
              chunk[static_cast<std::size_t>(block_index(lx, y, lz))] = Block::wood;
            }
            for (int dy = 3; dy <= 5; ++dy) {
              const int radius = dy == 5 ? 1 : 2;
              for (int dx = -radius; dx <= radius; ++dx) {
                for (int dz = -radius; dz <= radius; ++dz) {
                  const int ax = lx + dx;
                  const int az = lz + dz;
                  const int ay = height + dy;
                  if (ax < 0 || ax >= kChunkSizeX || az < 0 ||
                      az >= kChunkSizeZ || ay >= kChunkSizeY ||
                      (dx == 0 && dz == 0 && dy <= 4)) continue;
                  auto& leaf = chunk[static_cast<std::size_t>(block_index(ax, ay, az))];
                  if (leaf == Block::air) leaf = Block::leaves;
                }
              }
            }
          }
        }
      }
    }
  }
}
Block World::block_at(int x, int y, int z) const {
  const Block* block = blocks_.try_at(x, y, z);
  return block != nullptr ? *block : Block::air;
}

bool World::set_block(int x, int y, int z, Block block) {
  Block* slot = blocks_.try_at(x, y, z);
  if (slot == nullptr || *slot == block) {
    return false;
  }
  *slot = block;
  return true;
}

std::vector<ChunkMeshData> World::build_chunk_meshes(
    int cx, int cz) const {
  std::vector<CpuMeshData> tile_meshes;
  build_chunk_meshes(cx, cz, tile_meshes);
  std::vector<ChunkMeshData> out;
  out.reserve(tile_meshes.size());
  for (std::size_t tile = 0; tile < tile_meshes.size(); ++tile) {
    auto& geometry = tile_meshes[tile];
    if (!geometry.vertices.empty()) {
      out.push_back(ChunkMeshData{static_cast<int>(tile),
                                  std::move(geometry)});
    }
  }
  return out;
}

ChunkMeshSnapshot World::snapshot_chunk(int cx, int cz, std::uint64_t revision) const {
  ChunkMeshSnapshot s{}; s.cx = cx; s.cz = cz; s.revision = revision;
  if (cx < 0 || cx >= kWorldChunksX || cz < 0 || cz >= kWorldChunksZ) return s;
  const auto chunk = chunk_data(cx, cz);
  if (chunk.size() != static_cast<std::size_t>(kBlocksPerChunk)) return s;
  const int ox = cx * kChunkSizeX, oz = cz * kChunkSizeZ;
  for (int y = 0; y < kChunkSizeY; ++y)
    for (int z = -1; z <= kChunkSizeZ; ++z)
      for (int x = -1; x <= kChunkSizeX; ++x) {
        const Block b = (x >= 0 && x < kChunkSizeX && z >= 0 && z < kChunkSizeZ)
          ? chunk[static_cast<std::size_t>(block_index(x,y,z))]
          : block_at(ox+x, y, oz+z);
        const auto i = (static_cast<std::size_t>(y)*ChunkMeshSnapshot::kHaloSizeZ + static_cast<std::size_t>(z+1))*ChunkMeshSnapshot::kHaloSizeX + static_cast<std::size_t>(x+1);
        s.blocks[i] = b;
      }
  return s;
}

void mesh_chunk_snapshot(const ChunkMeshSnapshot& snapshot, std::vector<CpuMeshData>& tile_meshes) {
  tile_meshes.resize(atlas::kTileCount);
  for (auto& g : tile_meshes) { g.vertices.clear(); g.indices.clear(); }
  if (snapshot.cx < 0 || snapshot.cx >= kWorldChunksX || snapshot.cz < 0 || snapshot.cz >= kWorldChunksZ) return;
  const FaceTileCache cache = classify_chunk_faces(snapshot);
  for (int fi = 0; fi < 6; ++fi) greedy_faces(cache, fi, snapshot.cx, snapshot.cz, tile_meshes);
}

void World::build_chunk_meshes(int cx, int cz, std::vector<CpuMeshData>& tile_meshes) const {
  mesh_chunk_snapshot(snapshot_chunk(cx, cz, 0), tile_meshes);
}

int World::surface_height(int x, int z) const {
  for (int y = kChunkSizeY - 1; y >= 0; --y) {
    if (is_solid(block_at(x, y, z))) {
      return y;
    }
  }
  return -1;
}

bool World::is_exposed(int x, int y, int z) const {
  // The cell itself must be solid (air and water never collide).
  if (!is_solid(block_at(x, y, z))) {
    return false;
  }
  // An exposed cell has at least one non-solid in-bounds neighbor. The
  // out-of-world side reads SOLID (world.hpp's note): the border's
  // outboard faces are not reachable, and y's outside reads solid too
  // (a pillar's cap and the y=0 floor are interior questions, not
  // shell questions). In-bounds neighbors consult the table.
  static constexpr int kDirs[6][3] = {{0, 1, 0},  {0, -1, 0}, {1, 0, 0},
                                       {-1, 0, 0}, {0, 0, 1},  {0, 0, -1}};
  for (const auto& d : kDirs) {
    const int nx = x + d[0];
    const int ny = y + d[1];
    const int nz = z + d[2];
    if (nx < 0 || nx >= kWorldSizeX || nz < 0 || nz >= kWorldSizeZ ||
        ny < 0 || ny >= kChunkSizeY) {
      continue;  // the void: reads solid for this test
    }
    if (!is_solid(block_at(nx, ny, nz))) {
      return true;  // a face open to air or water
    }
  }
  return false;  // buried on all six sides
}

// --- the DDA raycast --------------------------------------------------------------

bool World::solid_query(const void* context, int x, int y,
                         int z) noexcept {
  const auto* world = static_cast<const World*>(context);
  return world != nullptr && is_solid(world->block_at(x, y, z));
}

ws::physics3d::VoxelGrid3D World::collision_grid() const noexcept {
  return ws::physics3d::VoxelGrid3D{
      ws::physics3d::VoxelGrid3D::Desc{
          ws::math::IVec3{0, 0, 0},
          ws::math::IVec3{kWorldSizeX, kChunkSizeY, kWorldSizeZ},
          this, &World::solid_query}};
}

World::RayHit World::raycast(const float origin[3],
                             const float direction[3],
                             float max_distance) const {
  return collision_grid().raycast(
      ws::math::Vec3{origin[0], origin[1], origin[2]},
      ws::math::Vec3{direction[0], direction[1], direction[2]},
      max_distance);
}

}  // namespace voxel
