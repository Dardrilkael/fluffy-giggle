// CPU-only asynchronous chunk meshing. No renderer or GPU dependency.
#pragma once

#include <array>
#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "world.hpp"

namespace voxel {

class AsyncChunkMesher {
 public:
  static constexpr std::size_t kChunkCount =
      static_cast<std::size_t>(kWorldChunksX * kWorldChunksZ);
  static constexpr std::size_t kQueueCapacity = kChunkCount;

  struct Stats {
    std::size_t queued = 0;
    std::size_t completed = 0;
    std::uint64_t coalesced = 0;
    std::uint64_t stale_discarded = 0;
  };

  AsyncChunkMesher()
      : jobs_(std::make_unique<Job[]>(kQueueCapacity)),
        worker_(&AsyncChunkMesher::worker_loop, this) {}

  ~AsyncChunkMesher() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

  AsyncChunkMesher(const AsyncChunkMesher&) = delete;
  AsyncChunkMesher& operator=(const AsyncChunkMesher&) = delete;

  void set_priority_origin(float x, float z) {
    std::lock_guard<std::mutex> lock(mutex_);
    origin_x_ = x;
    origin_z_ = z;
  }

  void request(const World& world, int cx, int cz) {
    if (cx < 0 || cx >= kWorldChunksX || cz < 0 || cz >= kWorldChunksZ)
      return;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t slot = slot_of(cx, cz);
    const std::uint64_t revision = ++revisions_[slot];
    completed_[slot].ready = false;
    Job job{world.snapshot_chunk(cx, cz, revision)};

    for (std::size_t i = 0; i < queue_count_; ++i) {
      if (jobs_[i].snapshot.cx == cx && jobs_[i].snapshot.cz == cz) {
        jobs_[i] = std::move(job);
        ++coalesced_;
        cv_.notify_one();
        return;
      }
    }

    // Capacity equals the total chunk count. Coalescing means a full queue
    // already contains every chunk; retain the nearest jobs defensively.
    if (queue_count_ == kQueueCapacity) {
      std::size_t farthest = 0;
      float farthest_d2 = -1.0f;
      for (std::size_t i = 0; i < queue_count_; ++i) {
        const float d2 = distance_sq(jobs_[i].snapshot.cx,
                                     jobs_[i].snapshot.cz);
        if (d2 > farthest_d2) { farthest_d2 = d2; farthest = i; }
      }
      if (distance_sq(cx, cz) >= farthest_d2) return;
      jobs_[farthest] = std::move(job);
      cv_.notify_one();
      return;
    }
    jobs_[queue_count_++] = std::move(job);
    cv_.notify_one();
  }

  // Transfers the latest completed CPU result to the caller. This is the
  // only consumer operation; callers decide when/how to upload it.
  bool take_completed(int cx, int cz, std::vector<CpuMeshData>& out,
                      float& mesh_ms, std::uint32_t& vertex_count,
                      std::uint32_t& index_count) {
    if (cx < 0 || cx >= kWorldChunksX || cz < 0 || cz >= kWorldChunksZ)
      return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t slot = slot_of(cx, cz);
    Result& result = completed_[slot];
    if (!result.ready) return false;
    if (result.revision != revisions_[slot]) {
      result.ready = false;
      ++stale_discarded_;
      return false;
    }
    out.swap(result.tiles);
    mesh_ms = result.mesh_ms;
    vertex_count = result.vertex_count;
    index_count = result.index_count;
    result.ready = false;
    return true;
  }

  [[nodiscard]] Stats stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats out;
    out.queued = queue_count_;
    for (const Result& result : completed_) out.completed += result.ready;
    out.coalesced = coalesced_;
    out.stale_discarded = stale_discarded_;
    return out;
  }

 private:
  struct Job { ChunkMeshSnapshot snapshot{}; };
  struct Result {
    bool ready = false;
    std::uint64_t revision = 0;
    float mesh_ms = 0.0f;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count = 0;
    std::vector<CpuMeshData> tiles;
  };

  [[nodiscard]] static constexpr std::size_t slot_of(int cx, int cz) {
    return static_cast<std::size_t>(cz * kWorldChunksX + cx);
  }
  [[nodiscard]] float distance_sq(int cx, int cz) const {
    const float dx = (static_cast<float>(cx) + 0.5f) * kChunkSizeX - origin_x_;
    const float dz = (static_cast<float>(cz) + 0.5f) * kChunkSizeZ - origin_z_;
    return dx * dx + dz * dz;
  }

  void worker_loop() {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stopping_ || queue_count_ != 0; });
        if (stopping_ && queue_count_ == 0) return;
        std::size_t best = 0;
        float best_d2 = std::numeric_limits<float>::max();
        for (std::size_t i = 0; i < queue_count_; ++i) {
          const float d2 = distance_sq(jobs_[i].snapshot.cx,
                                        jobs_[i].snapshot.cz);
          if (d2 < best_d2) { best_d2 = d2; best = i; }
        }
        job = std::move(jobs_[best]);
        --queue_count_;
        if (best != queue_count_) jobs_[best] = std::move(jobs_[queue_count_]);
      }

      Result result;
      result.revision = job.snapshot.revision;
      const std::size_t slot = slot_of(job.snapshot.cx, job.snapshot.cz);
      // The consumer swaps its previous scratch buffers back into this
      // result slot. Reclaim them before meshing so each chunk reuses its
      // own tile-vector capacities instead of allocating on every edit.
      {
        std::lock_guard<std::mutex> lock(mutex_);
        result.tiles.swap(completed_[slot].tiles);
      }
      const auto mesh_start = std::chrono::steady_clock::now();
      mesh_chunk_snapshot(job.snapshot, result.tiles);
      result.mesh_ms = std::chrono::duration<float, std::milli>(
          std::chrono::steady_clock::now() - mesh_start).count();
      for (const CpuMeshData& tile : result.tiles) {
        result.vertex_count += static_cast<std::uint32_t>(tile.vertices.size());
        result.index_count += static_cast<std::uint32_t>(tile.indices.size());
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        Result& published = completed_[slot];
        if (result.revision == revisions_[slot]) {
          published.ready = true;
          published.revision = result.revision;
          published.mesh_ms = result.mesh_ms;
          published.vertex_count = result.vertex_count;
          published.index_count = result.index_count;
          published.tiles.swap(result.tiles);
        } else {
          // A newer request owns the next build for this slot. Return the
          // completed stale buffers to the slot so that build can reuse them.
          if (!published.ready && published.tiles.empty())
            published.tiles.swap(result.tiles);
          ++stale_discarded_;
        }
      }
    }
  }

  std::unique_ptr<Job[]> jobs_;
  std::size_t queue_count_ = 0;
  float origin_x_ = 0.0f;
  float origin_z_ = 0.0f;
  std::array<std::uint64_t, kChunkCount> revisions_{};
  std::array<Result, kChunkCount> completed_{};
  std::uint64_t coalesced_ = 0;
  std::uint64_t stale_discarded_ = 0;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace voxel
