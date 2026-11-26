// apps/voxel_game - renderer.cpp: the scene bridge (see renderer.hpp).

#include "renderer.hpp"

#include "frustum.hpp"
#include "texture_atlas.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>

#include <ws/core/log.hpp>

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

void append_outline_bar(ws::ui::r3d::MeshData& out,
                        const ws::ui::r3d::MeshData& unit_cube,
                        float cx, float cy, float cz,
                        float sx, float sy, float sz) {
  const auto base = static_cast<std::uint32_t>(out.vertices.size());
  for (const auto& source : unit_cube.vertices) {
    auto vertex = source;
    vertex.pos[0] = source.pos[0] * sx + cx;
    vertex.pos[1] = source.pos[1] * sy + cy;
    vertex.pos[2] = source.pos[2] * sz + cz;
    out.vertices.push_back(vertex);
  }
  for (const std::uint32_t index : unit_cube.indices) {
    out.indices.push_back(base + index);
  }
}

}  // namespace

ws::core::Result<ws::ui::r3d::Mesh> create_target_outline() {
  // Twelve slim cuboids form a crisp wireframe just outside the voxel.
  // Build once and move the mesh per frame; no per-frame GPU allocations.
  constexpr float edge = 1.025f;
  constexpr float thickness = 0.018f;
  const auto cube = ws::ui::r3d::generate_cube(1.0f);
  ws::ui::r3d::MeshData outline;
  outline.vertices.reserve(cube.vertices.size() * 12);
  outline.indices.reserve(cube.indices.size() * 12);
  for (int sy : {-1, 1}) {
    for (int sz : {-1, 1}) {
      append_outline_bar(outline, cube, 0.0f,
                         static_cast<float>(sy) * 0.5f * edge,
                         static_cast<float>(sz) * 0.5f * edge,
                         edge, thickness, thickness);
    }
  }
  for (int sx : {-1, 1}) {
    for (int sz : {-1, 1}) {
      append_outline_bar(outline, cube,
                         static_cast<float>(sx) * 0.5f * edge, 0.0f,
                         static_cast<float>(sz) * 0.5f * edge,
                         thickness, edge, thickness);
    }
  }
  for (int sx : {-1, 1}) {
    for (int sy : {-1, 1}) {
      append_outline_bar(outline, cube,
                         static_cast<float>(sx) * 0.5f * edge,
                         static_cast<float>(sy) * 0.5f * edge, 0.0f,
                         thickness, thickness, edge);
    }
  }
  return ws::ui::r3d::create_mesh(outline);
}

ws::core::Status destroy_target_outline(ws::ui::r3d::Mesh mesh) {
  return ws::ui::r3d::destroy_mesh(mesh);
}

void draw_target_outline(ws::ui::r3d::Mesh mesh,
                         const World::RayHit& target) {
  if (!target.hit || mesh.id == 0) return;
  ws::ui::r3d::Transform3D transform{};
  transform.translation[0] = static_cast<float>(target.cell.x) + 0.5f;
  transform.translation[1] = -static_cast<float>(target.cell.y) - 0.5f;
  transform.translation[2] = -static_cast<float>(target.cell.z) - 0.5f;
  ws::ui::r3d::MeshMaterial material;
  material.tint = ws::ui::r2d::rgba(255, 222, 118, 255);
  material.casts_shadow = false;
  material.receives_shadow = false;
  ws::ui::r3d::draw_mesh(mesh, transform, material);
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

ws::ui::r2d::Camera3D begin_scene(const Player& player, float fov_degrees,
                                          float render_distance) {
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
  camera.fov_y = fov_degrees * 0.01745329251994329577f;
  // A tighter near plane reduces visible clipping when the FPS camera
  // approaches voxel faces; keep the far plane unchanged for world visibility.
  camera.near_z = 0.03f;
  camera.far_z = render_distance;
  ws::ui::r2d::begin(camera);
  (void)ws::ui::r3d::set_light(scene_light());
  return camera;
}

VoxelRenderer::VoxelRenderer() : worker_(&VoxelRenderer::worker_loop,this) {}
VoxelRenderer::~VoxelRenderer() {
  { std::lock_guard<std::mutex> lock(queue_mutex_); stopping_=true; }
  queue_cv_.notify_all(); if(worker_.joinable()) worker_.join();
}
void VoxelRenderer::worker_loop() {
  for (;;) {
    MeshJob job;
    { std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock,[this]{return stopping_ || queue_count_!=0;});
      if(stopping_ && queue_count_==0) return;
      job=std::move(jobs_[queue_head_]); queue_head_=(queue_head_+1)%kQueueCapacity; --queue_count_;
    }
    MeshResult result; result.revision=job.snapshot.revision;
    mesh_chunk_snapshot(job.snapshot,result.tiles);
    const auto slot=chunk_slot(job.snapshot.cx,job.snapshot.cz);
    { std::lock_guard<std::mutex> lock(queue_mutex_);
      if(result.revision==revisions_[slot]) { result.ready=true; completed_[slot]=std::move(result); }
    }
  }
}
void VoxelRenderer::request_rebuild_chunk(const World& world,int cx,int cz) {
  if(cx<0||cx>=kWorldChunksX||cz<0||cz>=kWorldChunksZ) return;
  std::lock_guard<std::mutex> lock(queue_mutex_);
  const auto slot=chunk_slot(cx,cz); const auto rev=++revisions_[slot]; completed_[slot].ready=false;
  MeshJob job; job.snapshot=world.snapshot_chunk(cx,cz,rev);
  for(std::size_t i=0;i<queue_count_;++i) {
    const auto q=(queue_head_+i)%kQueueCapacity;
    if(jobs_[q].snapshot.cx==cx && jobs_[q].snapshot.cz==cz) { jobs_[q]=std::move(job); queue_cv_.notify_one(); return; }
  }
  // Capacity equals the total chunk count; after coalescing, a full queue
  // already contains every chunk, so this is only a defensive fallback.
  if(queue_count_==kQueueCapacity) { queue_head_=(queue_head_+1)%kQueueCapacity; --queue_count_; }
  const auto tail=(queue_head_+queue_count_)%kQueueCapacity; jobs_[tail]=std::move(job); ++queue_count_; queue_cv_.notify_one();
}
void VoxelRenderer::upload_result(int cx,int cz,const std::vector<CpuMeshData>& tiles) {
  for(auto it=chunk_meshes_.begin();it!=chunk_meshes_.end();) {
    if(it->cx==cx&&it->cz==cz) { if(it->mesh.id) (void)ws::ui::r3d::destroy_mesh(it->mesh); it=chunk_meshes_.erase(it); } else ++it;
  }
  for(std::size_t tile=0;tile<tiles.size();++tile) {
    const auto& cpu=tiles[tile]; if(cpu.vertices.empty()) continue;
    ws::ui::r3d::MeshData upload; upload.vertices.reserve(cpu.vertices.size());
    for(const auto& source:cpu.vertices) { ws::ui::r3d::Vertex3D v{}; for(int a=0;a<3;++a){v.pos[a]=source.pos[a];v.normal[a]=source.normal[a];} v.uv[0]=source.uv[0];v.uv[1]=source.uv[1];upload.vertices.push_back(v); }
    upload.indices=cpu.indices; const auto mesh=ws::ui::r3d::create_mesh(upload);
    if(!mesh.has_value()){ws::core::log_error(mesh.error());continue;}
    chunk_meshes_.push_back(ChunkMesh{cx,cz,static_cast<int>(tile),*mesh,static_cast<std::uint32_t>(upload.indices.size())});
  }
}
void VoxelRenderer::process_completed(std::size_t max_jobs) {
  std::size_t count=0;
  for(std::size_t offset=0;offset<kChunkCount&&count<max_jobs;++offset) {
    const std::size_t slot=(upload_cursor_+offset)%kChunkCount;
    MeshResult result;
    { std::lock_guard<std::mutex> lock(queue_mutex_); if(!completed_[slot].ready) continue;
      if(completed_[slot].revision!=revisions_[slot]){completed_[slot].ready=false;continue;}
      result=std::move(completed_[slot]);completed_[slot].ready=false;
    }
    upload_result(static_cast<int>(slot%kWorldChunksX),static_cast<int>(slot/kWorldChunksX),result.tiles);
    ++count; upload_cursor_=(slot+1)%kChunkCount;
  }
}
void VoxelRenderer::rebuild_chunk(const World& world, int cx, int cz) {
  for (auto it = chunk_meshes_.begin(); it != chunk_meshes_.end();) {
    if (it->cx == cx && it->cz == cz) {
      if (it->mesh.id != 0u) (void)ws::ui::r3d::destroy_mesh(it->mesh);
      it = chunk_meshes_.erase(it);
    } else {
      ++it;
    }
  }
  world.build_chunk_meshes(cx, cz, cpu_mesh_scratch_);
  for (std::size_t tile = 0; tile < cpu_mesh_scratch_.size(); ++tile) {
    const CpuMeshData& cpu = cpu_mesh_scratch_[tile];
    if (cpu.vertices.empty()) continue;
    ws::ui::r3d::MeshData upload;
    upload.vertices.reserve(cpu.vertices.size());
    for (const CpuMeshVertex& source : cpu.vertices) {
      ws::ui::r3d::Vertex3D vertex{};
      for (int axis = 0; axis < 3; ++axis) {
        vertex.pos[axis] = source.pos[axis];
        vertex.normal[axis] = source.normal[axis];
      }
      vertex.uv[0] = source.uv[0];
      vertex.uv[1] = source.uv[1];
      upload.vertices.push_back(vertex);
    }
    upload.indices = cpu.indices;
    const auto mesh = ws::ui::r3d::create_mesh(upload);
    if (!mesh.has_value()) {
      ws::core::log_error(mesh.error());
      continue;
    }
    chunk_meshes_.push_back(ChunkMesh{
        cx, cz, static_cast<int>(tile), *mesh,
        static_cast<std::uint32_t>(upload.indices.size())});
  }
}
void VoxelRenderer::rebuild_all(const World& world) {
  destroy_all();
  for (int cz = 0; cz < kWorldChunksZ; ++cz)
    for (int cx = 0; cx < kWorldChunksX; ++cx)
      rebuild_chunk(world, cx, cz);
}
void VoxelRenderer::destroy_all() {
  for (const ChunkMesh& chunk : chunk_meshes_)
    if (chunk.mesh.id != 0u) (void)ws::ui::r3d::destroy_mesh(chunk.mesh);
  chunk_meshes_.clear();
  // Keep CPU scratch capacity: destroy_all is also used before full rebuilds.
}

void draw_world(const VoxelRenderer& renderer,
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
  // Compute frustum visibility once per chunk, not once per material mesh.
  // A chunk may own several tile meshes, all sharing the same AABB test.
  std::array<bool, kWorldChunksX * kWorldChunksZ> chunk_in_frustum{};
  for (int cz = 0; cz < kWorldChunksZ; ++cz) {
    for (int cx = 0; cx < kWorldChunksX; ++cx) {
      const float x0 = static_cast<float>(cx * kChunkSizeX);
      const float z0 = static_cast<float>(cz * kChunkSizeZ);
      const float rmin[3] = {x0, -static_cast<float>(kChunkSizeY),
                             -(z0 + static_cast<float>(kChunkSizeZ))};
      const float rmax[3] = {x0 + static_cast<float>(kChunkSizeX), 0.0f, -z0};
      const std::size_t index =
          static_cast<std::size_t>(cz * kWorldChunksX + cx);
      chunk_in_frustum[index] = culling::aabb_visible(fr, rmin, rmax);
    }
  }

  // Counts chunks that actually submit geometry, distinct from the cached
  // visibility result (an entirely occluded/empty chunk submits no draw).
  std::array<bool, kWorldChunksX * kWorldChunksZ> visible_chunks{};

  if (counters != nullptr) {
    counters->draw_calls = 0;
    counters->triangles = 0;
    counters->chunk_vis = 0;
    counters->meshes = static_cast<std::uint32_t>(
        renderer.chunk_meshes().size());
  }

  const auto chunk_visible = [&](const VoxelRenderer::ChunkMesh& chunk) {
    const std::size_t index =
        static_cast<std::size_t>(chunk.cz * kWorldChunksX + chunk.cx);
    return chunk_in_frustum[index];
  };

  const auto draw_chunk = [&](const VoxelRenderer::ChunkMesh& chunk, bool foliage) {
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
  for (const VoxelRenderer::ChunkMesh& chunk : renderer.chunk_meshes()) {
    if (chunk.index_count != 0 && chunk.tile != leaves_tile &&
        chunk_visible(chunk)) {
      draw_chunk(chunk, false);
    }
  }

  // Pass 2: foliage, far-to-near by chunk center. Back-to-front order
  // reduces overlap artifacts between transparent leaf meshes while
  // the opaque depth buffer still prevents foliage showing through walls.
  std::array<const VoxelRenderer::ChunkMesh*, kWorldChunksX * kWorldChunksZ>
      foliage_meshes{};
  std::size_t foliage_count = 0;
  for (const VoxelRenderer::ChunkMesh& chunk : renderer.chunk_meshes()) {
    if (chunk.index_count != 0 && chunk.tile == leaves_tile &&
        chunk_visible(chunk) && foliage_count < foliage_meshes.size()) {
      foliage_meshes[foliage_count++] = &chunk;
    }
  }
  const auto distance_sq = [&](const VoxelRenderer::ChunkMesh* chunk) {
    const float dx = static_cast<float>(chunk->cx * kChunkSizeX) +
                     0.5f * static_cast<float>(kChunkSizeX) - camera.eye[0];
    const float dy = -0.5f * static_cast<float>(kChunkSizeY) - camera.eye[1];
    const float dz = -static_cast<float>(chunk->cz * kChunkSizeZ) -
                     0.5f * static_cast<float>(kChunkSizeZ) - camera.eye[2];
    return dx * dx + dy * dy + dz * dz;
  };
  std::sort(foliage_meshes.begin(),
            foliage_meshes.begin() + static_cast<std::ptrdiff_t>(foliage_count),
            [&](const VoxelRenderer::ChunkMesh* a, const VoxelRenderer::ChunkMesh* b) {
              return distance_sq(a) > distance_sq(b);
            });
  for (std::size_t i = 0; i < foliage_count; ++i) {
    draw_chunk(*foliage_meshes[i], true);
  }
}

}  // namespace voxel