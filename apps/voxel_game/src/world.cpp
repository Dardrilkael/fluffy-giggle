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

#include "world.hpp"

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

// --- the mesher ------------------------------------------------------------------
// One (chunk, type) mesh: the chunk's cells of that type, each face
// emitted when the neighbor across it is non-solid (air or water both
// read see-through). Geometry is RENDER space (world.hpp's mirror
// note): game (gx, gy, gz) -> render (gx+0.5, -(gy+0.5), gz+0.5).
//
// Winding: the mesh pipeline culls BACK faces with CCW front
// (renderer3d_math.h's kMeshFrontFaceCcw, pinned by the r3d suite).
// The emitter picks each face's corner order by the right-hand test
// (u x v) . n >= 0 and flips when negative - no per-family spelling to
// get wrong.
[[nodiscard]] ws::ui::r3d::MeshData build_chunk_type_mesh(
    const World& world, int cx, int cz, Block type) {
  ws::ui::r3d::MeshData out;
  const int ox = cx * kChunkSizeX;
  const int oz = cz * kChunkSizeZ;

  // The 6 neighbor offsets with their game-space normals; emission
  // negates y for the render-space normal.
  struct FaceDir {
    int dx, dy, dz;        // the neighbor cell offset (game space)
    float normal[3];       // game-space outward normal
  };
  static constexpr FaceDir kFaces[6] = {
      {0, 1, 0, {0.0f, 1.0f, 0.0f}},    // top
      {0, -1, 0, {0.0f, -1.0f, 0.0f}},  // bottom
      {1, 0, 0, {1.0f, 0.0f, 0.0f}},    // east
      {-1, 0, 0, {-1.0f, 0.0f, 0.0f}},  // west
      {0, 0, 1, {0.0f, 0.0f, 1.0f}},    // south
      {0, 0, -1, {0.0f, 0.0f, -1.0f}},  // north
  };

  // The face plane's tangent basis (GAME space; y is the world's up):
  // for each normal, (u, v) spanning the plane with u x v = n.
  static constexpr float kUV[6][2][3] = {
      // top (+y): u = +z, v = +x -> u x v = +y
      {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},
      // bottom (-y): u = +x, v = +z -> u x v = -y
      {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
      // east (+x): u = +y, v = +z -> u x v = +x
      {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
      // west (-x): u = +z, v = +y -> u x v = -x
      {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
      // south (+z): u = +x, v = +y -> u x v = +z
      {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
      // north (-z): u = +y, v = +x -> u x v = -z
      {{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
  };

  for (int ly = 0; ly < kChunkSizeY; ++ly) {
    for (int lz = 0; lz < kChunkSizeZ; ++lz) {
      for (int lx = 0; lx < kChunkSizeX; ++lx) {
        const int gx = ox + lx;
        const int gz = oz + lz;
        if (world.block_at(gx, ly, gz) != type) {
          continue;
        }
        for (int fi = 0; fi < 6; ++fi) {
          const FaceDir& face = kFaces[fi];
          if (is_solid(world.block_at(gx + face.dx, ly + face.dy,
                                      gz + face.dz))) {
            continue;  // hidden behind the neighbor: never emit
          }
          // The face's center, GAME space: the cell's center pushed
          // half a block along the normal.
          const float gc[3] = {
              static_cast<float>(gx) + 0.5f + face.normal[0] * 0.5f,
              static_cast<float>(ly) + 0.5f + face.normal[1] * 0.5f,
              static_cast<float>(gz) + 0.5f + face.normal[2] * 0.5f};
          const float* u = kUV[fi][0];
          const float* v = kUV[fi][1];
          // The four corners (GAME space): c - u/2 - v/2, c + v/2,
          // c + u/2 + v/2, c + u/2 - the right-hand CCW order for
          // (u, v) with u x v = n.
          float corners[4][3];
          const float signs[4][2] = {
              {-0.5f, -0.5f}, {0.0f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.0f}};
          for (int ci = 0; ci < 4; ++ci) {
            const float su = signs[ci][0];
            const float sv = signs[ci][1];
            corners[ci][0] = gc[0] + su * u[0] + sv * v[0];
            corners[ci][1] = gc[1] + su * u[1] + sv * v[1];
            corners[ci][2] = gc[2] + su * u[2] + sv * v[2];
          }
          // GAME -> RENDER: x/z kept, y negated. A mirrored winding
          // flips handedness, so the CCW order above becomes CW in
          // render space - reverse the quad to keep the pipeline's
          // front face. (The mirror is an improper rotation; the
          // winding fix is the standard companion.)
          const std::uint32_t base =
              static_cast<std::uint32_t>(out.vertices.size());
          // Emit in render space, reversed order: corners[3], [2],
          // [1], [0].
          for (int ci = 3; ci >= 0; --ci) {
            ws::ui::r3d::Vertex3D vertex;
            vertex.pos[0] = corners[ci][0];
            vertex.pos[1] = -corners[ci][1];
            vertex.pos[2] = corners[ci][2];
            vertex.normal[0] = face.normal[0];
            vertex.normal[1] = -face.normal[1];
            vertex.normal[2] = face.normal[2];
            vertex.uv[0] = static_cast<float>(ci & 1);
            vertex.uv[1] = static_cast<float>(ci >> 1);
            out.vertices.push_back(vertex);
          }
          out.indices.push_back(base + 0);
          out.indices.push_back(base + 1);
          out.indices.push_back(base + 2);
          out.indices.push_back(base + 2);
          out.indices.push_back(base + 3);
          out.indices.push_back(base + 0);
        }
      }
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
  // One mesh per solid type present with visible faces.
  for (int type = 1; type < static_cast<int>(Block::count_); ++type) {
    if (type == static_cast<int>(Block::water)) {
      continue;  // water never meshes (a later life)
    }
    const Block block = static_cast<Block>(type);
    ws::ui::r3d::MeshData data = build_chunk_type_mesh(*this, cx, cz, block);
    if (data.vertices.empty()) {
      continue;
    }
    const auto mesh = ws::ui::r3d::create_mesh(data);
    if (!mesh.has_value()) {
      ws::core::log_error(mesh.error());
      continue;  // one failed type leaves the rest of the chunk visible
    }
    chunk_meshes_.push_back(ChunkMesh{cx, cz, block, *mesh});
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

World::RayHit World::raycast(const float origin[3],
                             const float direction[3],
                             float max_distance) const {
  RayHit out;
  float len = std::sqrt(direction[0] * direction[0] +
                        direction[1] * direction[1] +
                        direction[2] * direction[2]);
  if (len < 1e-8f) {
    return out;
  }
  const float inv = 1.0f / len;
  const float dx = direction[0] * inv;
  const float dy = direction[1] * inv;
  const float dz = direction[2] * inv;

  int ix = static_cast<int>(std::floor(origin[0]));
  int iy = static_cast<int>(std::floor(origin[1]));
  int iz = static_cast<int>(std::floor(origin[2]));

  const float stepx = dx > 0.0f ? 1.0f : (dx < 0.0f ? -1.0f : 0.0f);
  const float stepy = dy > 0.0f ? 1.0f : (dy < 0.0f ? -1.0f : 0.0f);
  const float stepz = dz > 0.0f ? 1.0f : (dz < 0.0f ? -1.0f : 0.0f);
  const float boundx = static_cast<float>(dx > 0.0f ? ix + 1 : ix);
  const float boundy = static_cast<float>(dy > 0.0f ? iy + 1 : iy);
  const float boundz = static_cast<float>(dz > 0.0f ? iz + 1 : iz);
  // t at which each boundary is reached; +inf-equivalent (1e30) when the
  // axis never crosses (a 0 component). Mutable: the walk advances them.
  float tx = dx != 0.0f ? (boundx - origin[0]) / dx : 1e30f;
  float ty = dy != 0.0f ? (boundy - origin[1]) / dy : 1e30f;
  float tz = dz != 0.0f ? (boundz - origin[2]) / dz : 1e30f;
  const float dtx = dx != 0.0f ? std::abs(1.0f / dx) : 1e30f;
  const float dty = dy != 0.0f ? std::abs(1.0f / dy) : 1e30f;
  const float dtz = dz != 0.0f ? std::abs(1.0f / dz) : 1e30f;

  int last_axis = -1;
  int last_step = 0;
  float t = 0.0f;
  for (int guard = 0; guard < 1024; ++guard) {
    if (is_solid(block_at(ix, iy, iz))) {
      out.hit = true;
      out.cell[0] = ix;
      out.cell[1] = iy;
      out.cell[2] = iz;
      out.normal[0] = last_axis == 0 ? -last_step : 0;
      out.normal[1] = last_axis == 1 ? -last_step : 0;
      out.normal[2] = last_axis == 2 ? -last_step : 0;
      out.distance = t;
      return out;
    }
    if (tx <= ty && tx <= tz) {
      if (tx > max_distance) {
        break;
      }
      t = tx;
      tx += dtx;
      ix += static_cast<int>(stepx);
      last_axis = 0;
      last_step = static_cast<int>(stepx);
    } else if (ty <= tz) {
      if (ty > max_distance) {
        break;
      }
      t = ty;
      ty += dty;
      iy += static_cast<int>(stepy);
      last_axis = 1;
      last_step = static_cast<int>(stepy);
    } else {
      if (tz > max_distance) {
        break;
      }
      t = tz;
      tz += dtz;
      iz += static_cast<int>(stepz);
      last_axis = 2;
      last_step = static_cast<int>(stepz);
    }
    if (iy < 0 || iy >= kChunkSizeY) {
      break;
    }
  }
  return out;
}

}  // namespace voxel
