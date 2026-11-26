// Scripted, repeatable voxel benchmark. No world/player ownership:
// the app applies the MoveInput and feeds back per-frame measurements.

#pragma once

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "perf.hpp"
#include "player.hpp"

namespace voxel::bench {

struct ShadowFlags {
    // Preserve the game's current default: directional shadows are opt-in.
    bool cast = false;
    bool receive = false;
};

// "full" renders the map and filters receivers; "no-receive" retains the
// depth-only shadow pass but skips all fragment PCF lookups; "off" does both.
[[nodiscard]] inline constexpr ShadowFlags shadow_flags_from_name(
    std::string_view name) noexcept {
    if (name == "full") return {true, true};
    if (name == "no-receive") return {true, false};
    return {false, false};
}

[[nodiscard]] inline constexpr const char* shadow_mode_name(
    ShadowFlags flags) noexcept {
    if (flags.cast && flags.receive) return "full";
    if (flags.cast && !flags.receive) return "no-receive";
    return "off";
}

struct Config {
    float seconds = 12.0f;
    float warmup_seconds = 3.0f;
    bool fixed_scene = false;
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
        last_report_ = cfg_.fixed_scene ? cfg_.warmup_seconds : 0.0f;
        reset_report_window();
        phase_frames_ = 0;
        phase_times_.clear();
        wall_phase_times_.clear();
        gpu_phase_times_.clear();
        if (cfg_.print_header) {
            std::printf("phase,sec,fps,update_cpu_ms,wall_frame_ms,phys_ms,mesh_cpu_ms,"
                        "upload_cpu_ms,draw_submit_ms,gpu_frame_ms,gpu_samples,"
                        "upload_bytes,mesh_vertices,mesh_indices,mesh_jobs,staging_growths,"
                        "draws,tris,bodies,meshes,chunkvis\n");
            std::fflush(stdout);
        }
    }

    [[nodiscard]] float seconds() const { return cfg_.seconds; }

    [[nodiscard]] Player::MoveInput input(float dt) {
        Player::MoveInput in{};
        if (!started_ || finished_ || cfg_.fixed_scene) return in;
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
        // Let the camera/player settle, then measure only the stationary
        // scene. This keeps each render-distance/resolution run comparable.
        if (cfg_.fixed_scene && elapsed_ < cfg_.warmup_seconds) return;
        ++frames_;
        ++phase_frames_;
        phase_times_.add(t.total_ms);
        wall_phase_times_.add(t.wall_frame_ms);
        acc_frame_ms_ += t.total_ms;
        acc_wall_frame_ms_ += t.wall_frame_ms;
        acc_phys_ms_ += t.physics_ms;
        acc_mesh_ms_ += t.cpu_mesh_ms;
        acc_upload_ms_ += t.upload_ms;
        acc_upload_bytes_ += t.upload_bytes;
        acc_mesh_vertices_ += t.mesh_vertices;
        acc_mesh_indices_ += t.mesh_indices;
        acc_mesh_jobs_ += t.mesh_jobs;
        acc_growths_ += t.upload_capacity_growths;
        acc_draw_ms_ += t.draw_ms;
        if (t.gpu_frame_valid) {
            acc_gpu_ms_ += t.gpu_frame_ms;
            ++acc_gpu_samples_;
            gpu_phase_times_.add(t.gpu_frame_ms);
        }

        if (elapsed_ - last_report_ >= 1.0f) {
            const float n = static_cast<float>(frames_);
            const float report_seconds = cfg_.fixed_scene
                ? last_report_ + 1.0f - cfg_.warmup_seconds
                : last_report_ + 1.0f;
            std::printf("%s,%.1f,%.1f,"
                        "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
                        "%u,%llu,%llu,%llu,%u,%u,%u,%u,%u,%u,%u\n",
                        phase_name(), report_seconds,
                        static_cast<float>(frames_), acc_frame_ms_ / n,
                        acc_wall_frame_ms_ / n, acc_phys_ms_ / n,
                        acc_mesh_ms_ / n, acc_upload_ms_ / n,
                        acc_draw_ms_ / n,
                        acc_gpu_samples_ != 0
                            ? acc_gpu_ms_ / static_cast<float>(acc_gpu_samples_)
                            : 0.0f,
                        acc_gpu_samples_,
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
        if (cfg_.fixed_scene) return elapsed_ >= cfg_.seconds;
        if (phase_time() >= 3.0f && phase_ < 3) {
            print_phase_percentiles();
            ++phase_;
            phase_start_ = elapsed_;
            phase_frames_ = 0;
            phase_times_.clear();
            wall_phase_times_.clear();
            gpu_phase_times_.clear();
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
        if (cfg_.fixed_scene) return "scale";
        static constexpr const char* kNames[5] = {
            "spin", "still", "move", "edit", "done"};
        return phase_ < 4 ? kNames[phase_] : kNames[4];
    }

 private:
    [[nodiscard]] float phase_time() const { return elapsed_ - phase_start_; }

    void reset_report_window() {
        frames_ = 0;
        acc_frame_ms_ = 0.0f;
        acc_wall_frame_ms_ = 0.0f;
        acc_phys_ms_ = 0.0f;
        acc_mesh_ms_ = 0.0f;
        acc_upload_ms_ = 0.0f;
        acc_upload_bytes_ = 0;
        acc_mesh_vertices_ = 0;
        acc_mesh_indices_ = 0;
        acc_mesh_jobs_ = 0;
        acc_growths_ = 0;
        acc_draw_ms_ = 0.0f;
        acc_gpu_ms_ = 0.0f;
        acc_gpu_samples_ = 0;
    }

    void print_phase_percentiles() const {
        const auto p = phase_times_.percentiles();
        const auto wall = wall_phase_times_.percentiles();
        const auto gpu = gpu_phase_times_.percentiles();
        std::printf("bench:percentiles,%s,n=%u,window=%zu,"
                    "update_p50_ms=%.3f,update_p95_ms=%.3f,update_p99_ms=%.3f,"
                    "wall_n=%zu,wall_p50_ms=%.3f,wall_p95_ms=%.3f,wall_p99_ms=%.3f,"
                    "gpu_n=%zu,gpu_p50_ms=%.3f,gpu_p95_ms=%.3f,"
                    "gpu_p99_ms=%.3f\n",
                    phase_name(), phase_frames_, phase_times_.sample_count(),
                    p.p50_ms, p.p95_ms, p.p99_ms,
                    wall_phase_times_.sample_count(),
                    wall.p50_ms, wall.p95_ms, wall.p99_ms,
                    gpu_phase_times_.sample_count(),
                    gpu.p50_ms, gpu.p95_ms, gpu.p99_ms);
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
    float acc_wall_frame_ms_ = 0.0f;
    float acc_phys_ms_ = 0.0f;
    float acc_mesh_ms_ = 0.0f;
    float acc_upload_ms_ = 0.0f;
    float acc_draw_ms_ = 0.0f;
    float acc_gpu_ms_ = 0.0f;
    std::uint32_t acc_gpu_samples_ = 0;
    std::uint64_t acc_upload_bytes_ = 0;
    std::uint64_t acc_mesh_vertices_ = 0;
    std::uint64_t acc_mesh_indices_ = 0;
    std::uint32_t acc_mesh_jobs_ = 0;
    std::uint32_t acc_growths_ = 0;
    float last_report_ = 0.0f;
    perf::FrameTimeWindow phase_times_{};
    perf::FrameTimeWindow wall_phase_times_{};
    perf::FrameTimeWindow gpu_phase_times_{};
};

// Keep getenv behind one MSVC warning boundary; these values are optional
// benchmark-only overrides and never affect a normal launch unless set.
inline const char* env_value(const char* name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
    const char* value = std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    return value;
}

// WS_VOXEL_BENCH ("12" -> 12 s); 0 when unset/invalid.
inline float bench_seconds_from_env() {
    const char* raw = env_value("WS_VOXEL_BENCH");
    if (raw == nullptr) return 0.0f;
    const float v = static_cast<float>(std::atof(raw));
    return std::isfinite(v) && v > 0.5f ? v : 0.0f;
}

// WS_VOXEL_BENCH_MODE=scale selects a fixed-camera stationary-scene run.
inline bool bench_fixed_scene_from_env() {
    const char* raw = env_value("WS_VOXEL_BENCH_MODE");
    return raw != nullptr && std::strcmp(raw, "scale") == 0;
}

// Zero means no override. The app clamps a valid distance to its supported range.
inline float bench_render_distance_from_env() {
    const char* raw = env_value("WS_VOXEL_BENCH_DISTANCE");
    if (raw == nullptr) return 0.0f;
    const float value = static_cast<float>(std::atof(raw));
    return std::isfinite(value) && value >= 16.0f ? value : 0.0f;
}

inline int bench_dimension_from_env(const char* name, int fallback,
                                    int minimum, int maximum) {
    const char* raw = env_value(name);
    if (raw == nullptr) return fallback;
    const int value = std::atoi(raw);
    return value >= minimum && value <= maximum ? value : fallback;
}

inline ShadowFlags bench_shadow_flags_from_env() {
    const char* raw = env_value("WS_VOXEL_BENCH_SHADOW_MODE");
    return raw == nullptr ? ShadowFlags{} : shadow_flags_from_name(raw);
}

inline bool bench_vsync_from_env(bool fallback) {
    const char* raw = env_value("WS_VOXEL_BENCH_VSYNC");
    if (raw == nullptr) return fallback;
    if (std::strcmp(raw, "0") == 0 || std::strcmp(raw, "off") == 0 ||
        std::strcmp(raw, "false") == 0) return false;
    if (std::strcmp(raw, "1") == 0 || std::strcmp(raw, "on") == 0 ||
        std::strcmp(raw, "true") == 0) return true;
    return fallback;
}

}  // namespace voxel::bench
