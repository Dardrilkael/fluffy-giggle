// apps/voxel_game - bench.hpp: the scripted benchmark driver (perf.hpp's
// WS_VOXEL_BENCH mode). One header, engine-free: the app's on_update feeds
// it the frame's dt and the perf counters, and it answers the frame's
// synthetic MoveInput (the phases below). It prints one CSV line per
// second to stdout and a summary at the end - the before/after evidence.
//
// Phases (each ~3s at 60Hz; the whole run is the sum):
//   A spin    - yaw sweeps a full 360 (frustum + culling stress),
//   B still   - level look, no motion (the whole-world-in-view worst case),
//   C move    - sprint forward with a slow turn (streaming view + physics),
//   D edit    - break/place bursts against the cell under the reticle
//               (mesh rebuild + collision churn).
//
// The driver owns no world/player state: the app applies the input it
// hands back (the same MoveInput path the real keyboard fills).

#pragma once

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>

#include "perf.hpp"
#include "player.hpp"

namespace voxel::bench {

struct Config {
    float seconds = 12.0f;   // total run (4 phases, ~3s each)
    bool print_header = true;
};

class Driver {
 public:
    void start(const Config& cfg) {
        cfg_ = cfg;
        elapsed_ = 0.0f;
        phase_ = 0;
        frames_ = 0;
        acc_ms_ = 0.0f;
        acc_phys_ = 0.0f;
        acc_mesh_ = 0.0f;
        acc_draw_ = 0.0f;
        last_report_ = 0.0f;
        if (cfg_.print_header) {
            std::printf("phase,sec,fps,cpu_ms,phys_ms,mesh_ms,draw_ms,"
                        "draws,tris,bodies,meshes,chunkvis\n");
            std::fflush(stdout);
        }
    }

    [[nodiscard]] float seconds() const { return cfg_.seconds; }

    // The frame's synthetic input (phase-shaped).
    [[nodiscard]] Player::MoveInput input(float dt) {
        Player::MoveInput in{};
        const float phase_t = phase_time();
        switch (phase_) {
            case 0:  // spin: constant yaw rate, full circle in ~3s
                in.look_dx = 6.28318531f / 3.0f * dt / 0.0025f;
                in.forward = phase_t > 2.5f;
                break;
            case 1:  // still: nothing at all
                break;
            case 2:  // move: sprint forward + slow turn
                in.forward = true;
                in.sprint = true;
                in.look_dx = 0.6f * dt / 0.0025f;
                break;
            case 3:  // edit: stand still, look DOWN at the ground ahead.
                // look_dy is in MOUSE PIXELS (sensitivity 0.0025 rad/px):
                // ~300 px pitches the eye ~43 deg down onto the ground a
                // few blocks ahead - the reticle's actual dig target.
                in.look_dy = (phase_t < 0.5f ? 300.0f : 0.0f);
                break;
            default:
                break;
        }
        return in;
    }

    // Whether this frame should break/place (phase D fires pairs).
    [[nodiscard]] bool wants_edit() const { return phase_ == 3; }

    // True while the driver is inside phase D's "look down" preamble:
    // the app should hold its edit hand until the reticle is on ground.
    [[nodiscard]] bool edit_ready() const { return phase_time() >= 0.5f; }

    // Feeds the frame's counters; prints one line per second.
    void frame(float dt, const perf::FrameTimings& t,
               const perf::Counters& c) {
        elapsed_ += dt;
        ++frames_;
        acc_ms_ += t.total_ms;
        acc_phys_ += t.physics_ms;
        acc_mesh_ += t.meshes_ms;
        acc_draw_ += t.draw_ms;
        if (elapsed_ - last_report_ >= 1.0f) {
            const float n = static_cast<float>(frames_);
            std::printf("%s,%.1f,%.1f,%.2f,%.2f,%.2f,%.2f,%u,%u,%u,%u,%u\n",
                        phase_name(), last_report_ + 1.0f,
                        static_cast<float>(frames_), acc_ms_ / n,
                        acc_phys_ / n, acc_mesh_ / n, acc_draw_ / n,
                        c.draw_calls, c.triangles, c.bodies, c.meshes,
                        c.chunk_vis);
            std::fflush(stdout);
            frames_ = 0;
            acc_ms_ = acc_phys_ = acc_mesh_ = acc_draw_ = 0.0f;
            last_report_ += 1.0f;
        }
    }

    // Advances the phase clock; true when the whole run is over.
    bool advance_phase(float dt) {
        (void)dt;
        if (phase_time() >= 3.0f && phase_ < 3) {
            ++phase_;
            phase_start_ = elapsed_;
        }
        return elapsed_ >= cfg_.seconds;
    }

    [[nodiscard]] bool running() const { return elapsed_ < cfg_.seconds; }
    [[nodiscard]] const char* phase_name() const {
        static constexpr const char* kNames[5] = {"spin", "still", "move",
                                                 "edit", "done"};
        return phase_ < 4 ? kNames[phase_] : kNames[4];
    }

 private:
    [[nodiscard]] float phase_time() const { return elapsed_ - phase_start_; }

    Config cfg_{};
    float elapsed_ = 0.0f;
    float phase_start_ = 0.0f;
    int phase_ = 0;
    std::uint32_t frames_ = 0;
    float acc_ms_ = 0.0f, acc_phys_ = 0.0f, acc_mesh_ = 0.0f, acc_draw_ = 0.0f;
    float last_report_ = 0.0f;
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
    if (raw == nullptr) {
        return 0.0f;
    }
    const float v = static_cast<float>(std::atof(raw));
    return v > 0.5f ? v : 0.0f;
}

}  // namespace voxel::bench
