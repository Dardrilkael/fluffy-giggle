// Low-overhead voxel-game performance measurements.
// CPU-side timings are deliberately named as such: draw_ms is command
// submission time, and upload_ms is CPU staging/create_mesh time. Neither
// is a measurement of work completed by the GPU.

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <ws/core/time.hpp>

namespace voxel::perf {

// One frame's timings. Worker meshing time is the time spent by the CPU
// worker, not time charged to the main-thread frame. Upload time measures
// CPU conversion/staging plus create_mesh submission, not GPU completion.
struct FrameTimings {
    float total_ms = 0.0f;
    float physics_ms = 0.0f;
    float cpu_mesh_ms = 0.0f;
    float upload_ms = 0.0f;
    float draw_ms = 0.0f;  // CPU draw-command recording/submission only
    std::uint64_t upload_bytes = 0;
    std::uint64_t mesh_vertices = 0;
    std::uint64_t mesh_indices = 0;
    std::uint32_t mesh_jobs = 0;
    std::uint32_t upload_capacity_growths = 0;
};

// Rolling, fixed-storage frame-time sample window. It has no per-frame
// allocation and computes nearest-rank p50/p95/p99 only when requested.
class FrameTimeWindow {
 public:
    static constexpr std::size_t kCapacity = 8192;

    struct Percentiles {
        float p50_ms = 0.0f;
        float p95_ms = 0.0f;
        float p99_ms = 0.0f;
    };

    void add(float milliseconds) noexcept {
        if (!std::isfinite(milliseconds) || milliseconds < 0.0f) return;
        samples_[next_] = milliseconds;
        next_ = (next_ + 1) % kCapacity;
        if (count_ < kCapacity) ++count_;
    }

    void clear() noexcept {
        count_ = 0;
        next_ = 0;
    }

    [[nodiscard]] std::size_t sample_count() const noexcept {
        return count_;
    }

    [[nodiscard]] Percentiles percentiles() const noexcept {
        Percentiles out{};
        if (count_ == 0) return out;

        // Sorting a bounded stack copy avoids mutating the live rolling
        // window. add() remains O(1) and performs no allocation.
        std::array<float, kCapacity> ordered{};
        for (std::size_t i = 0; i < count_; ++i) {
            ordered[i] = samples_[i];
        }
        std::sort(ordered.begin(),
                  ordered.begin() + static_cast<std::ptrdiff_t>(count_));

        const auto nearest_rank = [this](double percentile) {
            const double rank = std::ceil(percentile *
                                          static_cast<double>(count_));
            return static_cast<std::size_t>(rank > 0.0 ? rank - 1.0 : 0.0);
        };
        out.p50_ms = ordered[nearest_rank(0.50)];
        out.p95_ms = ordered[nearest_rank(0.95)];
        out.p99_ms = ordered[nearest_rank(0.99)];
        return out;
    }

 private:
    std::array<float, kCapacity> samples_{};
    std::size_t count_ = 0;
    std::size_t next_ = 0;
};

struct Counters {
    std::uint32_t draw_calls = 0;
    std::uint32_t triangles = 0;
    std::uint32_t bodies = 0;
    std::uint32_t meshes = 0;
    std::uint32_t chunk_vis = 0;
};

inline float ms_since(std::chrono::steady_clock::time_point t0,
                     std::chrono::steady_clock::time_point t1) noexcept {
  return ws::core::time::milliseconds_between(t0, t1);
}

inline float ms_since(std::chrono::steady_clock::time_point t0) noexcept {
  return ws::core::time::milliseconds_since(t0);
}

}  // namespace voxel::perf
