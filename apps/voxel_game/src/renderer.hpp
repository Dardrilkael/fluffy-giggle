// apps/voxel_game - renderer.hpp: the game-space -> r3d draw bridge.
//
// One job: given the player's GAME-space pose and the world's chunk
// meshes, set the r3d scene (camera + light) and draw every mesh. The
// coordinate map lives here and in the mesher (world.cpp's frame note):
// game (x, y, z) -> render (x, -y, -z) - the 180-degree turn about the
// x axis, a proper rotation, so the camera's eye/target ride the same
// map the baked geometry did, with the camera's up (0, -1, 0): render
// +y is DOWN, so render-up is -y... spelled: the r2d::Camera3D
// defaults have up (0, -1, 0) which IS "game +y up" after the map.
//
// The camera's look direction: the player's look_direction() is GAME
// space; mapped it is (dx, -dy, -dz). The camera's eye looks at
// target = eye + mapped direction.

#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <ws/math/vecmath.hpp>
#include <ws/ui/renderer2d.hpp>  // Camera3D
#include <ws/ui/renderer3d.hpp>  // draw_mesh, set_light

#include "perf.hpp"
#include "player.hpp"
#include "texture_atlas.hpp"
#include "world.hpp"

namespace voxel {

// The scene's fixed light: mid-afternoon, a bit overhead and from the
// east - the terrain's top faces catch the full term, north/south
// faces stay dimmer (the classic voxel read).
[[nodiscard]] ws::ui::r3d::Light3D scene_light();

// One reusable twelve-edge wireframe cube. Create/destroy with the app;
// draw it after the world using the GAME-space voxel cell from the DDA.
[[nodiscard]] ws::core::Result<ws::ui::r3d::Mesh> create_target_outline();
[[nodiscard]] ws::core::Status destroy_target_outline(ws::ui::r3d::Mesh mesh);
void draw_target_outline(ws::ui::r3d::Mesh mesh,
                         const World::RayHit& target);

// Sets the r3d scene for the frame: the camera from the player's pose
// (mirrored), the scene light. Call inside on_update, before the
// chunk draws.
// Builds the frame's scene (camera from the player's pose, the scene
// light), begins the r3d block, and hands the camera back - the game
// threads it into draw_world for the chunk culling.
[[nodiscard]] ws::ui::r2d::Camera3D begin_scene(const Player& player, float fov_degrees = 70.0f,
                                                   float render_distance = 260.0f);

// The per-tile block textures (the generated texture_atlas.hpp's
// atlas::kTileCount PNGs through the engine's memory decoder - one per tile:
// greedy-merged quads carry tile-unit UVs and the engine shader wraps
// them, so each draw samples its own tile's full [0,1]). Call from
// on_setup onward; a dead decode answers the white-fallback contract
// (id 0 - that tile's draw then shades the flat tint alone).
[[nodiscard]] std::array<ws::ui::r2d::Texture, atlas::kTileCount>
load_tile_textures();

class VoxelRenderer {
 public:
  struct ChunkMesh {
    int cx = 0;
    int cz = 0;
    int tile = -1;
    ws::ui::r3d::Mesh mesh{};
    std::uint32_t index_count = 0;
  };

  VoxelRenderer();
  ~VoxelRenderer();
  VoxelRenderer(const VoxelRenderer&) = delete;
  VoxelRenderer& operator=(const VoxelRenderer&) = delete;
  void rebuild_chunk(const World& world, int cx, int cz);
  void request_rebuild_chunk(const World& world, int cx, int cz);
  void process_completed(std::size_t max_jobs = 2);
  void rebuild_all(const World& world);
  void destroy_all();

  [[nodiscard]] const std::vector<ChunkMesh>& chunk_meshes() const noexcept {
    return chunk_meshes_;
  }

 private:
  struct MeshJob { ChunkMeshSnapshot snapshot{}; };
  struct MeshResult { bool ready = false; std::uint64_t revision = 0; std::vector<CpuMeshData> tiles; };
  static constexpr std::size_t kChunkCount = static_cast<std::size_t>(kWorldChunksX*kWorldChunksZ);
  static constexpr std::size_t kQueueCapacity = kChunkCount;
  void worker_loop();
  void upload_result(int cx, int cz, const std::vector<CpuMeshData>& tiles);
  [[nodiscard]] static constexpr std::size_t chunk_slot(int cx,int cz) { return static_cast<std::size_t>(cz*kWorldChunksX+cx); }
  std::vector<ChunkMesh> chunk_meshes_;
  // Tile-indexed CPU geometry scratch. clear() preserves capacity, avoiding
  // repeated vertex/index allocations when the player edits the same chunks.
  std::vector<CpuMeshData> cpu_mesh_scratch_;
  std::array<std::uint64_t,kChunkCount> revisions_{};
  std::array<MeshResult,kChunkCount> completed_{};
  std::array<MeshJob,kQueueCapacity> jobs_{};
  std::size_t queue_head_=0, queue_count_=0;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  bool stopping_=false;
  std::thread worker_;
};

void draw_world(const VoxelRenderer& renderer,
                const std::array<ws::ui::r2d::Texture, atlas::kTileCount>&
                    tile_textures,
                const ws::ui::r2d::Camera3D& camera, float aspect,
                perf::Counters* counters = nullptr);

}  // namespace voxel
