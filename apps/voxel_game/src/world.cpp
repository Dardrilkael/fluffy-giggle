// apps/voxel_game - world.cpp: terrain generation, chunk meshing, the
// DDA raycast. See world.hpp for the design (the two coordinate
// frames, the per-(chunk, type) mesh set, the neighbor culling rule).
//
// Terrain: value-noise fbm (two octaves of smoothstep-interpolated
// hashed lattice values - deterministic per seed, no third party).
// The column spell: sand near water level, grass above, 3 dirt under
// the top, stone beneath; scattered 4-high wood trunks on grass
// columns a hash dice fires on; water fills every cell at or below
// its level the terrain does not.

#include "texture_atlas.hpp"
#include "world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include <ws/core/log.hpp>
#include <ws/ui/renderer2d.hpp>  // rgba()

namespace voxel {

// --- the palette ------------------------------------------------------------------

namespace {

const std::uint32_t kTints[static_cast<std::size_t>(Block::count_)] = {
    0u,                                   // air (never drawn)
    ws::ui::r2d::rgba(106, 170, 64, 255),  // grass - a mid green
    ws::ui::r2d::rgba(134, 96, 67, 255),  // dirt
    ws::ui::r2d::rgba(125, 125, 130, 255),  // stone
    ws::ui::r2d::rgba(106, 82, 51, 255),  // wood
    ws::ui::r2d::rgba(218, 210, 158, 255),  // sand
    ws::ui::r2d::rgba(50, 104, 170, 255),  // water
};

// --- the noise -------------------------------------------------------------------

// Integer hash -> [0, 1). The 32-bit mix is Murmur-style; the same
// (x, y, seed) always answers the same value.
[[nodiscard]] float hash01(std::uint32_t x, std::uint32_t y,
                           std::uint32_t seed) {
  std::uint32_t h = seed + 0x9E3779B9u;
  h ^= x * 0x85EBCA6Bu;
  h = (h << 13) | (h >> 19);
  h *= 0xC2B2AE35u;
  h ^= y * 0x27D4EB2Fu;
  h = (h << 13) | (h >> 19);
  h *= 0x165667B1u;
  h ^= h >> 15;
  return static_cast<float>(h & 0xFFFFFFu) / 16777216.0f;
}

// Smooth value noise at (x, z): bilinear over the 4 hashed corners,
// smoothstep-weighted, [0, 1].
[[nodiscard]] float noise2d(float x, float z, std::uint32_t seed) {
  const int xi = static_cast<int>(std::floor(x));
  const int zi = static_cast<int>(std::floor(z));
  const float fx = x - static_cast<float>(xi);
  const float fz = z - static_cast<float>(zi);
  const float sx = fx * fx * (3.0f - 2.0f * fx);
  const float sz = fz * fz * (3.0f - 2.0f * fz);
  const float c00 = hash01(static_cast<std::uint32_t>(xi),
                           static_cast<std::uint32_t>(zi), seed);
  const float c10 = hash01(static_cast<std::uint32_t>(xi + 1),
                           static_cast<std::uint32_t>(zi), seed);
  const float c01 = hash01(static_cast<std::uint32_t>(xi),
                           static_cast<std::uint32_t>(zi + 1), seed);
  const float c11 = hash01(static_cast<std::uint32_t>(xi + 1),
                           static_cast<std::uint32_t>(zi + 1), seed);
  const float a = c00 + (c10 - c00) * sx;
  const float b = c01 + (c11 - c01) * sx;
  return a + (b - a) * sz;
}

// Two-octave fbm, [0, 1]: broad hills plus one detail pass.
[[nodiscard]] float fbm(float x, float z, std::uint32_t seed) {
  const float hills = noise2d(x, z, seed);
  const float detail =
      noise2d(x * 4.13f, z * 4.13f, seed ^ 0xA5A5A5A5u) * 0.25f;
  return (hills + detail) / 1.25f;
}

// A column gets a trunk when its hash fires (rare) - the tree dice.
[[nodiscard]] bool tree_at(int x, int z, std::uint32_t seed) {
  return hash01(static_cast<std::uint32_t>(x) * 3u,
                static_cast<std::uint32_t>(z) * 3u, seed ^ 0x1F1F1F1Fu) >
         0.985f;
}

constexpr int kWaterLevel = 12;  // game-space y water fills to
constexpr int kHeightMin = 2;    // the terrain's lowest column (a lake bed)
constexpr int kHeightSpan = 28; // the range above kHeightMin (hills to 30)

// --- the mesher (GREEDY) ----------------------------------------------------------
// One (chunk, TILE) mesh: the chunk's exposed faces whose (block, face)
// pair selects THIS atlas tile (kTileFor), merged into large quads by the
// classic greedy-meshing sweep - same-direction faces that sit coplanar
// and adjacent merge into one quad. A hillside of 100 grass blocks is a
// handful of quads instead of 100; the triangles drop several-fold.
//
// Face existence is the SAME rule the per-face mesher used: a face
// emits when the neighbor across it is non-solid; greedy only changes
// HOW faces group, never WHICH exist. Geometry is RENDER space
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
constexpr float kUVB[6][2][3] = {
    {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},  // top: u=+x (east) v=-z (north)
    {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},   // bottom: u=+x v=+z
    {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},   // east: u=+y v=+z
    {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},   // west: u=+z v=+y
    {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},   // south: u=+x v=+y
    {{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},   // north: u=+y v=+x
};

// Emits one merged quad for the greedy rectangle (slice sa, [pu, pu+w)
// x [pv, pv+h) on face fi's plane lattice. The lattice walk mirrors
// the mask build exactly - the two can never disagree where a cell is.
void emit_quad(ws::ui::r3d::MeshData& out, Block /*type*/, int fi, int ox,
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
    ws::ui::r3d::Vertex3D vertex;
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

// The greedy sweep for one (chunk, tile, face direction): builds the
// exposed-face mask on the face's plane lattice, then carves greedy
// rectangles out of it.
void greedy_faces(const World& world, Block type, int fi, int cx, int cz,
                  ws::ui::r3d::MeshData& out) {
  const FaceDir& face = kFaces[fi];
  const int ox = cx * kChunkSizeX;
  const int oz = cz * kChunkSizeZ;
  // The plane lattice axes (chunk-local): u/v are the face basis'
  // integer directions; the slice axis is the face's normal axis.
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
  // One lattice point -> its chunk-local cell (the emit_quad walk).
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
  // The mask + used grid, per slice (the sweep only merges within one
  // slice: two cells at different slice positions are NOT coplanar).
  std::vector<bool> mask(static_cast<std::size_t>(u_len) * v_len, false);
  std::vector<bool> used(static_cast<std::size_t>(u_len) * v_len, false);
  for (int sa = 0; sa < a_len; ++sa) {
    std::fill(mask.begin(), mask.end(), false);
    std::fill(used.begin(), used.end(), false);
    bool any = false;
    for (int pu = 0; pu < u_len; ++pu) {
      for (int pv = 0; pv < v_len; ++pv) {
        const auto c = cell_of(sa, pu, pv);
        const int gx = ox + c[0];
        const int gy = c[1];
        const int gz = oz + c[2];
        if (world.block_at(gx, gy, gz) != type) {
          continue;
        }
        if (is_solid(world.block_at(gx + face.dx, gy + face.dy,
                                    gz + face.dz))) {
          continue;  // hidden behind the neighbor: never emit
        }
        mask[static_cast<std::size_t>(pu) * v_len + pv] = true;
        any = true;
      }
    }
    if (!any) {
      continue;
    }
    // Carve greedy rectangles.
    for (int pu = 0; pu < u_len; ++pu) {
      for (int pv = 0; pv < v_len; ++pv) {
        const std::size_t idx =
            static_cast<std::size_t>(pu) * v_len + pv;
        if (!mask[idx] || used[idx]) {
          continue;
        }
        int w = 1;
        while (pu + w < u_len && mask[idx + static_cast<std::size_t>(w) * v_len]
               && !used[idx + static_cast<std::size_t>(w) * v_len]) {
          ++w;
        }
        int h = 1;
        bool grew = true;
        while (grew && pv + h < v_len) {
          for (int k = 0; k < w; ++k) {
            const std::size_t probe = static_cast<std::size_t>(pu + k) *
                v_len + static_cast<std::size_t>(pv + h);
            if (!mask[probe] || used[probe]) {
              grew = false;
              break;
            }
          }
          if (grew) {
            ++h;
          }
        }
        for (int dw = 0; dw < w; ++dw) {
          for (int dh = 0; dh < h; ++dh) {
            used[static_cast<std::size_t>(pu + dw) * v_len +
                 static_cast<std::size_t>(pv + dh)] = true;
          }
        }
        emit_quad(out, type, fi, ox, oz, sa, pu, pv, w, h, u_len, v_len);
      }
    }
  }
}

}  // namespace

// One (chunk, tile) mesh: every face of every block whose kTileFor
// entry selects this tile, merged greedily per face direction.
[[nodiscard]] ws::ui::r3d::MeshData build_chunk_tile_mesh(
    const World& world, int cx, int cz, int tile) {
  ws::ui::r3d::MeshData out;
  for (int type = 1; type < static_cast<int>(Block::count_); ++type) {
    if (type == static_cast<int>(Block::water)) {
      continue;  // water never meshes
    }
    for (int fi = 0; fi < 6; ++fi) {
      if (atlas::kTileFor[static_cast<std::size_t>(type)][fi] != tile) {
        continue;
      }
      greedy_faces(world, static_cast<Block>(type), fi, cx, cz, out);
    }
  }
  return out;
}
}  // namespace

// --- the API ----------------------------------------------------------------------

std::uint32_t tint_of(Block block) {
  return kTints[static_cast<std::size_t>(block)];
}

bool is_solid(Block block) {
  return block != Block::air && block != Block::water;
}

void World::generate(std::uint32_t seed) {
  chunks_.assign(static_cast<std::size_t>(kWorldChunksX) * kWorldChunksZ, {});
  for (int cz = 0; cz < kWorldChunksZ; ++cz) {
    for (int cx = 0; cx < kWorldChunksX; ++cx) {
      auto& chunk =
          chunks_[static_cast<std::size_t>(cz * kWorldChunksX + cx)];
      chunk.assign(kBlocksPerChunk, Block::air);
      for (int lz = 0; lz < kChunkSizeZ; ++lz) {
        for (int lx = 0; lx < kChunkSizeX; ++lx) {
          const int wx = cx * kChunkSizeX + lx;
          const int wz = cz * kChunkSizeZ + lz;
          const float h01 = fbm(static_cast<float>(wx) * 0.037f,
                                static_cast<float>(wz) * 0.037f, seed);
          // The range is chosen wide on purpose: fbm rarely visits its
          // extremes, so kHeightMin must sit well under the water level
          // for lakes to actually appear, and the span high enough for
          // real hills (the observed range for this seed: ~6..~30).
          const int height =
              kHeightMin + static_cast<int>(h01 * kHeightSpan + 0.5f);
          for (int y = 0; y <= height; ++y) {
            Block block = Block::stone;
            if (y == height) {
              block = height <= kWaterLevel + 1 ? Block::sand : Block::grass;
            } else if (y >= height - 3) {
              block = Block::dirt;
            }
            chunk[static_cast<std::size_t>(block_index(lx, y, lz))] = block;
          }
          for (int y = height + 1; y <= kWaterLevel; ++y) {
            chunk[static_cast<std::size_t>(block_index(lx, y, lz))] =
                Block::water;
          }
          if (height > kWaterLevel + 1 && tree_at(wx, wz, seed)) {
            for (int y = height + 1; y <= height + 4; ++y) {
              chunk[static_cast<std::size_t>(block_index(lx, y, lz))] =
                  Block::wood;
            }
          }
        }
      }
    }
  }
}

Block World::block_at(int x, int y, int z) const {
  if (x < 0 || x >= kWorldSizeX || z < 0 || z >= kWorldSizeZ || y < 0 ||
      y >= kChunkSizeY) {
    return Block::air;
  }
  const int cx = x / kChunkSizeX;
  const int cz = z / kChunkSizeZ;
  const auto& chunk =
      chunks_[static_cast<std::size_t>(cz * kWorldChunksX + cx)];
  return chunk[static_cast<std::size_t>(block_index(x % kChunkSizeX, y,
                                                    z % kChunkSizeZ))];
}

bool World::set_block(int x, int y, int z, Block block) {
  if (x < 0 || x >= kWorldSizeX || z < 0 || z >= kWorldSizeZ || y < 0 ||
      y >= kChunkSizeY) {
    return false;
  }
  const int cx = x / kChunkSizeX;
  const int cz = z / kChunkSizeZ;
  auto& chunk = chunks_[static_cast<std::size_t>(cz * kWorldChunksX + cx)];
  auto& slot = chunk[static_cast<std::size_t>(block_index(x % kChunkSizeX, y,
                                                          z % kChunkSizeZ))];
  if (slot == block) {
    return false;
  }
  slot = block;
  return true;
}

void World::rebuild_chunk(int cx, int cz) {
  // Kill the chunk's previous meshes (an edit rebuilds the chunk).
  for (auto it = chunk_meshes_.begin(); it != chunk_meshes_.end();) {
    if (it->cx == cx && it->cz == cz) {
      if (it->mesh.id != 0u) {
        (void)ws::ui::r3d::destroy_mesh(it->mesh);
      }
      it = chunk_meshes_.erase(it);
    } else {
      ++it;
    }
  }
  // One mesh per TILE present with visible faces (greedy-merged).
  for (int tile = 0; tile < atlas::kTileCount; ++tile) {
    ws::ui::r3d::MeshData data = build_chunk_tile_mesh(*this, cx, cz, tile);
    if (data.vertices.empty()) {
      continue;
    }
    const auto mesh = ws::ui::r3d::create_mesh(data);
    if (!mesh.has_value()) {
      ws::core::log_error(mesh.error());
      continue;  // one failed tile leaves the rest of the chunk visible
    }
    chunk_meshes_.push_back(
        ChunkMesh{cx, cz, tile, *mesh,
                  static_cast<std::uint32_t>(data.indices.size())});
  }
}

void World::rebuild_all() {
  for (int cz = 0; cz < kWorldChunksZ; ++cz) {
    for (int cx = 0; cx < kWorldChunksX; ++cx) {
      rebuild_chunk(cx, cz);
    }
  }
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
  const auto hit = collision_grid().raycast(
      ws::math::Vec3{origin[0], origin[1], origin[2]},
      ws::math::Vec3{direction[0], direction[1], direction[2]},
      max_distance);
  RayHit out{};
  out.hit = hit.hit;
  out.cell[0] = hit.cell.x;
  out.cell[1] = hit.cell.y;
  out.cell[2] = hit.cell.z;
  out.normal[0] = hit.normal.x;
  out.normal[1] = hit.normal.y;
  out.normal[2] = hit.normal.z;
  out.distance = hit.distance;
  return out;
}

}  // namespace voxel
