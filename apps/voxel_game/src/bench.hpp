// Scripted, repeatable voxel benchmark. No world/player ownership:
// the app applies the MoveInput and feeds back per-frame measurements.

#pragma once

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>

#include "perf.hpp"
#include "player.hpp"

namespace voxel::bench {

struct Config {
    float seconds = 12.0f;
    bool print_header = true;
};

class Driver {
 public:
    void start(const Config& cfg) {
        started_ = true;
        finished_ = false;
        cfg_ = cfg;
        elapsed_ = 0.0f;
        phase_start_ = 0.0f;
        phase_ = 0;
        reset_report_window();
        phase_frames_ = 0;
        phase_times_.clear();
        if (cfg_.print_header) {
            std::printf("phase,sec,fps,frame_ms,phys_ms,mesh_cpu_ms,"
                        "upload_cpu_ms,draw_submit_ms,upload_bytes,"
                        "mesh_vertices,mesh_indices,mesh_jobs,staging_growths,"
                        "draws,tris,bodies,meshes,chunkvis\n");
            std::fflush(stdout);
        }
    }

    [[nodiscard]] float seconds() const { return cfg_.seconds; }

    [[nodiscard]] Player::MoveInput input(float dt) {
        Player::MoveInput in{};
        if (!started_ || finished_) return in;
        const float phase_t = phase_time();
        switch (phase_) {
            case 0:
                in.look_dx = 6.28318531f / 3.0f * dt / 0.0025f;
                in.forward = phase_t > 2.5f;
                break;
            case 1:
                break;
            case 2:
                in.forward = true;
                in.sprint = true;
                in.look_dx = 0.6f * dt / 0.0025f;
                break;
            case 3:
                in.look_dy = (phase_t < 0.5f ? 300.0f : 0.0f);
                break;
            default:
                break;
        }
        return in;
    }

    [[nodiscard]] bool wants_edit() const { return phase_ == 3; }
    [[nodiscard]] bool edit_ready() const { return phase_time() >= 0.5f; }

    void frame(float dt, const perf::FrameTimings& t,
               const perf::Counters& c) {
        if (!running()) return;
        elapsed_ += dt;
        ++frames_;
        ++phase_frames_;
        phase_times_.add(t.total_ms);
        acc_frame_ms_ += t.total_ms;
        acc_phys_ms_ += t.physics_ms;
        acc_mesh_ms_ += t.cpu_mesh_ms;
        acc_upload_ms_ += t.upload_ms;
        acc_upload_bytes_ += t.upload_bytes;
        acc_mesh_vertices_ += t.mesh_vertices;
        acc_mesh_indices_ += t.mesh_indices;
        acc_mesh_jobs_ += t.mesh_jobs;
        acc_growths_ += t.upload_capacity_growths;
        acc_draw_ms_ += t.draw_ms;

        if (elapsed_ - last_report_ >= 1.0f) {
            const float n = static_cast<float>(frames_);
            std::printf("%s,%.1f,%.1f,%.3f,%.3f,%.3f,%.3f,%.3f,"
                        "%llu,%llu,%llu,%u,%u,%u,%u,%u,%u,%u,%u\n",
                        phase_name(), last_report_ + 1.0f,
                        static_cast<float>(frames_), acc_frame_ms_ / n,
                        acc_phys_ms_ / n, acc_mesh_ms_ / n,
                        acc_upload_ms_ / n, acc_draw_ms_ / n,
                        static_cast<unsigned long long>(acc_upload_bytes_),
                        static_cast<unsigned long long>(acc_mesh_vertices_),
                        static_cast<unsigned long long>(acc_mesh_indices_),
                        acc_mesh_jobs_, acc_growths_, c.draw_calls,
                        c.triangles, c.bodies, c.meshes, c.chunk_vis);
            std::fflush(stdout);
            reset_report_window();
            last_report_ += 1.0f;
        }
    }

    bool advance_phase(float dt) {
        (void)dt;
        if (!running()) return true;
        if (phase_time() >= 3.0f && phase_ < 3) {
            print_phase_percentiles();
            ++phase_;
            phase_start_ = elapsed_;
            phase_frames_ = 0;
            phase_times_.clear();
        }
        return elapsed_ >= cfg_.seconds;
    }

    void finish() {
        if (!started_ || finished_) return;
        print_phase_percentiles();
        finished_ = true;
        std::printf("bench: done (%.1f s)\n", elapsed_);
        std::fflush(stdout);
    }

    [[nodiscard]] bool running() const {
        return started_ && !finished_ && elapsed_ < cfg_.seconds;
    }

    [[nodiscard]] const char* phase_name() const {
        static constexpr const char* kNames[5] = {
            "spin", "still", "move", "edit", "done"};
        return phase_ < 4 ? kNames[phase_] : kNames[4];
    }

 private:
    [[nodiscard]] float phase_time() const { return elapsed_ - phase_start_; }

    void reset_report_window() {
        frames_ = 0;
        acc_frame_ms_ = 0.0f;
        acc_phys_ms_ = 0.0f;
        acc_mesh_ms_ = 0.0f;
        acc_upload_ms_ = 0.0f;
        acc_upload_bytes_ = 0;
        acc_mesh_vertices_ = 0;
        acc_mesh_indices_ = 0;
        acc_mesh_jobs_ = 0;
        acc_growths_ = 0;
        acc_draw_ms_ = 0.0f;
    }

    void print_phase_percentiles() const {
        const auto p = phase_times_.percentiles();
        std::printf("bench:percentiles,%s,n=%u,window=%zu,"
                    "p50_ms=%.3f,p95_ms=%.3f,p99_ms=%.3f\n",
                    phase_name(), phase_frames_, phase_times_.sample_count(),
                    p.p50_ms, p.p95_ms, p.p99_ms);
        std::fflush(stdout);
    }

    Config cfg_{};
    bool started_ = false;
    bool finished_ = false;
    float elapsed_ = 0.0f;
    float phase_start_ = 0.0f;
    int phase_ = 0;
    std::uint32_t frames_ = 0;
    std::uint32_t phase_frames_ = 0;
    float acc_frame_ms_ = 0.0f;
    float acc_phys_ms_ = 0.0f;
    float acc_mesh_ms_ = 0.0f;
    float acc_upload_ms_ = 0.0f;
    float acc_draw_ms_ = 0.0f;
    std::uint64_t acc_upload_bytes_ = 0;
    std::uint64_t acc_mesh_vertices_ = 0;
    std::uint64_t acc_mesh_indices_ = 0;
    std::uint32_t acc_mesh_jobs_ = 0;
    std::uint32_t acc_growths_ = 0;
    float last_report_ = 0.0f;
    perf::FrameTimeWindow phase_times_{};
};

// Reads WS_VOXEL_BENCH ("12" -> 12 s); 0 when unset/invalid.
inline float bench_seconds_from_env() {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
    const char* raw = std::getenv("WS_VOXEL_BENCH");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (raw == nullptr) return 0.0f;
    const float v = static_cast<float>(std::atof(raw));
    return v > 0.5f ? v : 0.0f;
}

}  // namespace voxel::bench
