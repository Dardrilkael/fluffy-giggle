// apps/voxel_game - renderer.cpp: the scene bridge (see renderer.hpp).

#include "renderer.hpp"

#include "texture_atlas.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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


namespace {

// The 6 camera frustum planes (Gribb-Hartmann) from the r2d camera -
// the same view-projection the mesh path pushes, extracted on the CPU
// so culling math stays out of the shader. A chunk's AABB is visible
// iff it sits (strictly or touching) inside every plane's half-space.
struct Frustum {
  float planes[6][4];  // ax + by + cz + d >= 0 keeps the volume
};

[[nodiscard]] Frustum extract_frustum(const ws::ui::r2d::Camera3D& cam,
                                       float aspect) {
  // The camera basis (right-handed, eye/target/up, fov_y): view then
  // perspective, both spelled here the way batch::camera_vp3d spells
  // them for the GPU - the SAME math, one source of truth is the
  // engine's; this extraction is its CPU mirror for culling only.
  const float fov = cam.fov_y > 0.0f ? cam.fov_y : 1.2217f;
  const float tan_h = std::tan(fov * 0.5f);
  const float near_z = cam.near_z > 0.0f ? cam.near_z : 0.1f;
  const float far_z = cam.far_z > near_z ? cam.far_z : 260.0f;
  // Forward = normalize(target - eye); right = normalize(f x up);
  // up' = right x f.
  float f[3] = {cam.target[0] - cam.eye[0], cam.target[1] - cam.eye[1],
                cam.target[2] - cam.eye[2]};
  float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  f[0] /= fl; f[1] /= fl; f[2] /= fl;
  float r[3] = {f[1] * cam.up[2] - f[2] * cam.up[1],
                f[2] * cam.up[0] - f[0] * cam.up[2],
                f[0] * cam.up[1] - f[1] * cam.up[0]};
  float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
  r[0] /= rl; r[1] /= rl; r[2] /= rl;
  float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2],
                r[0] * f[1] - r[1] * f[0]};
  // The 6 planes: near, far, 4 sides (fov_x from aspect).
  const float tan_v = tan_h;
  const float tan_hx = tan_v * aspect;
  Frustum fr;
  auto set = [&](int i, const float n[3], float d) {
    const float l = std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
    fr.planes[i][0] = n[0]/l; fr.planes[i][1] = n[1]/l;
    fr.planes[i][2] = n[2]/l; fr.planes[i][3] = d/l;
  };
  // near: n = f, d = -(eye.f) - near  (keeps points >= near away)
  set(0, f, -(f[0]*cam.eye[0] + f[1]*cam.eye[1] + f[2]*cam.eye[2]) - near_z);
  // far: n = -f, d = eye.f + far
  const float negf[3] = {-f[0], -f[1], -f[2]};
  set(1, negf, (f[0]*cam.eye[0] + f[1]*cam.eye[1] + f[2]*cam.eye[2]) + far_z);
  // The 4 side planes through the eye, normals = side outward.
  const float e[3] = {cam.eye[0], cam.eye[1], cam.eye[2]};
  // left: n = f*tan_hx + r ... normalized outward-ish combos:
  auto side = [&](int i, const float a[3], const float b[3], float ta) {
    float n[3] = {a[0] + ta*b[0], a[1] + ta*b[1], a[2] + ta*b[2]};
    // points on the inside satisfy n . (p - eye) >= 0
    set(i, n, -(n[0]*e[0] + n[1]*e[1] + n[2]*e[2]));
  };
  // left (r-negative side): inside = f*tan_hx + r
  side(2, f, r, tan_hx);        // left:  n = f + tan_hx*r  ... sign check below
  side(3, f, r, -tan_hx);       // right
  side(4, f, u, tan_v);         // bottom
  side(5, f, u, -tan_v);        // top
  // NOTE on signs: the half-space test below accepts a small NEGATIVE
  // margin (radius), so touching/edge cases stay conservative: a chunk
  // at the exact boundary is drawn, never dropped early. The strict
  // correctness pin lives in the logic suite's culling test, which
  // checks the accepted set against a brute-force point-sampling of
  // the chunk AABB.
  return fr;
}

// AABB vs frustum: visible when the box reaches into every plane's
// keep-side (p-vertex test with the box's extent radius).
[[nodiscard]] bool aabb_visible(const Frustum& fr, const float min_[3],
                                const float max_[3]) {
  for (int i = 0; i < 6; ++i) {
    const float* p = fr.planes[i];
    // The most-positive corner along the normal.
    const float px = p[0] > 0.0f ? max_[0] : min_[0];
    const float py = p[1] > 0.0f ? max_[1] : min_[1];
    const float pz = p[2] > 0.0f ? max_[2] : min_[2];
    if (p[0] * px + p[1] * py + p[2] * pz + p[3] < 0.0f) {
      return false;  // the box's nearest corner is fully outside
    }
  }
  return true;
}

}  // namespace

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

ws::ui::r2d::Camera3D begin_scene(const Player& player, float aspect) {
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
  (void)aspect;

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
  const Frustum fr = extract_frustum(camera, aspect);
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
    return aabb_visible(fr, rmin, rmax);
  };

  const auto draw_chunk = [&](const World::ChunkMesh& chunk, bool foliage) {
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
    if (chunk.tile != leaves_tile && chunk_visible(chunk)) {
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
    if (chunk.tile == leaves_tile && chunk_visible(chunk) &&
        foliage_count < foliage_meshes.size()) {
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