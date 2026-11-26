// apps/voxel_game - renderer.cpp: the scene bridge (see renderer.hpp).

#include "renderer.hpp"

#include "frustum.hpp"
#include "texture_atlas.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

namespace voxel {

std::array<ws::ui::r2d::Texture, atlas::kTileCount>
load_tile_textures() {
  // One GPU texture per tile: the generated header's per-tile PNG bytes
  // through the engine's own memory decoder (no filesystem seam - the
  // same bytes on every host, Android included). The white fallback on
  // a dead decode is the load's own contract; the app checks the id
  // and warns (that tile shades its flat tint alone).
  std::array<ws::ui::r2d::Texture, atlas::kTileCount> out{};
  for (int i = 0; i < atlas::kTileCount; ++i) {
    std::size_t bytes = 0;
    const std::uint8_t* png = atlas::tile_png(i, bytes);
    out[i] = ws::ui::r2d::load_texture_from_memory(png, bytes);
  }
  return out;
}


ws::ui::r3d::Light3D scene_light() {
  // The direction TO the light (r3d::set_light normalizes): from the
  // east and overhead in GAME space, mapped to render space by the
  // same proper rotation the mesher uses (x, -y, -z - the 180-degree
  // turn about the x axis).
  ws::ui::r3d::Light3D light;
  light.dir[0] = 0.55f;   // east
  light.dir[1] = -0.82f;  // GAME up -> render -y (the rotation)
  light.dir[2] = -0.18f;  // GAME slightly-north -> render -z
  light.color[0] = light.color[1] = light.color[2] = 1.0f;
  light.ambient = 0.35f;  // the shadow-side floor: dark, not black
  return light;
}

ws::ui::r2d::Camera3D begin_scene(const Player& player) {
  // The player's GAME-space eye and look, mapped to render space by
  // world.hpp's game_to_render - the SAME map the mesher baked the
  // geometry with, so the camera and the world can never disagree
  // about the frame (the first build's mirror-vs-rotation drift is
  // exactly the bug this centralization closes).
  const ws::math::Vec3 eye = player.eye_position();
  const ws::math::Vec3 look = player.look_direction();

  ws::ui::r2d::Camera3D camera;
  camera.eye[0] = game_to_render(eye.x, 0);
  camera.eye[1] = game_to_render(eye.y, 1);
  camera.eye[2] = game_to_render(eye.z, 2);
  camera.target[0] = game_to_render(eye.x + look.x, 0);
  camera.target[1] = game_to_render(eye.y + look.y, 1);
  camera.target[2] = game_to_render(eye.z + look.z, 2);
  // up (0, -1, 0) is the default already: render +y is DOWN, so
  // "up" on screen is -y in render space = +y in game space.
  camera.up[0] = 0.0f;
  camera.up[1] = -1.0f;
  camera.up[2] = 0.0f;
  camera.fov_y = 1.2217f;  // 70 degrees, the FPS default
  // A tighter near plane reduces visible clipping when the FPS camera
  // approaches voxel faces; keep the far plane unchanged for world visibility.
  camera.near_z = 0.03f;
  camera.far_z = 260.0f;   // the whole 128-block world is in view
  ws::ui::r2d::begin(camera);
  (void)ws::ui::r3d::set_light(scene_light());
  return camera;
}

void draw_world(const World& world,
                const std::array<ws::ui::r2d::Texture, atlas::kTileCount>&
                    tile_textures,
                const ws::ui::r2d::Camera3D& camera, float aspect,
                perf::Counters* counters) {
  // Leaves have binary transparent pixels. Draw opaque geometry first,
  // then foliage with depth testing enabled but depth writes disabled:
  // transparent texels blend as zero and reveal the already-rendered world.
  const int leaves_tile =
      atlas::kTileFor[static_cast<std::size_t>(Block::leaves)][0];
  const culling::Frustum fr = culling::extract_frustum(
      camera.eye, camera.target, camera.up, camera.fov_y, aspect,
      camera.near_z, camera.far_z);
  // Coordinate-indexed deduplication: chunk_vis is independent of
  // chunk_meshes() iteration order and counts each non-empty chunk once.
  std::array<bool, kWorldChunksX * kWorldChunksZ> visible_chunks{};

  if (counters != nullptr) {
    counters->draw_calls = 0;
    counters->triangles = 0;
    counters->chunk_vis = 0;
    counters->meshes = static_cast<std::uint32_t>(
        world.chunk_meshes().size());
  }

  const auto chunk_visible = [&](const World::ChunkMesh& chunk) {
    const float x0 = static_cast<float>(chunk.cx * kChunkSizeX);
    const float z0 = static_cast<float>(chunk.cz * kChunkSizeZ);
    const float rmin[3] = {x0, -static_cast<float>(kChunkSizeY),
                           -(z0 + static_cast<float>(kChunkSizeZ))};
    const float rmax[3] = {x0 + static_cast<float>(kChunkSizeX), 0.0f, -z0};
    return culling::aabb_visible(fr, rmin, rmax);
  };

  const auto draw_chunk = [&](const World::ChunkMesh& chunk, bool foliage) {
    if (chunk.index_count == 0) {
      return;
    }
    if (counters != nullptr) {
      ++counters->draw_calls;
      counters->triangles += chunk.index_count / 3;
      const std::size_t index =
          static_cast<std::size_t>(chunk.cz * kWorldChunksX + chunk.cx);
      if (!visible_chunks[index]) {
        visible_chunks[index] = true;
        ++counters->chunk_vis;
      }
    }

    ws::ui::r3d::Transform3D identity{};
    ws::ui::r3d::MeshMaterial material;
    material.texture =
        chunk.tile >= 0 && chunk.tile < atlas::kTileCount
            ? tile_textures[static_cast<std::size_t>(chunk.tile)]
            : ws::ui::r2d::Texture{};
    material.tint = ws::ui::r2d::rgba(255, 255, 255);
    if (foliage) {
      material.depth_write = false;
      // The shadow pass has no alpha-tested fragment stage; don't cast
      // solid rectangular leaf-card shadows.
      material.casts_shadow = false;
    }
    ws::ui::r3d::draw_mesh(chunk.mesh, identity, material);
  };

  // Pass 1: all opaque terrain, in its normal chunk order.
  for (const World::ChunkMesh& chunk : world.chunk_meshes()) {
    if (chunk.index_count != 0 && chunk.tile != leaves_tile &&
        chunk_visible(chunk)) {
      draw_chunk(chunk, false);
    }
  }

  // Pass 2: foliage, far-to-near by chunk center. Back-to-front order
  // reduces overlap artifacts between transparent leaf meshes while
  // the opaque depth buffer still prevents foliage showing through walls.
  std::array<const World::ChunkMesh*, kWorldChunksX * kWorldChunksZ>
      foliage_meshes{};
  std::size_t foliage_count = 0;
  for (const World::ChunkMesh& chunk : world.chunk_meshes()) {
    if (chunk.index_count != 0 && chunk.tile == leaves_tile &&
        chunk_visible(chunk) && foliage_count < foliage_meshes.size()) {
      foliage_meshes[foliage_count++] = &chunk;
    }
  }
  const auto distance_sq = [&](const World::ChunkMesh* chunk) {
    const float dx = static_cast<float>(chunk->cx * kChunkSizeX) +
                     0.5f * static_cast<float>(kChunkSizeX) - camera.eye[0];
    const float dy = -0.5f * static_cast<float>(kChunkSizeY) - camera.eye[1];
    const float dz = -static_cast<float>(chunk->cz * kChunkSizeZ) -
                     0.5f * static_cast<float>(kChunkSizeZ) - camera.eye[2];
    return dx * dx + dy * dy + dz * dz;
  };
  std::sort(foliage_meshes.begin(),
            foliage_meshes.begin() + static_cast<std::ptrdiff_t>(foliage_count),
            [&](const World::ChunkMesh* a, const World::ChunkMesh* b) {
              return distance_sq(a) > distance_sq(b);
            });
  for (std::size_t i = 0; i < foliage_count; ++i) {
    draw_chunk(*foliage_meshes[i], true);
  }
}

}  // namespace voxel