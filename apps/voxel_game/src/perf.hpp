// apps/voxel_game - perf.hpp: the performance instrumentation (the
// optimization round's evidence). Pure CPU counters + timers the app's
// frame loop feeds; draw_hud prints the last sample line.
//
// THE BENCHMARK MODE (WS_VOXEL_BENCH=<seconds>): the app runs its own
// loop with scripted phases instead of waiting for a human:
//   phase A  spin the camera slowly (a full 360 sweep),
//   phase B  stand still, look level (the worst-case whole-world view),
//   phase C  sprint-walk forward while turning,
//   phase D  break/place bursts (mesh + shell churn).
// Each phase prints one line per second: fps, phase ms means (cpu
// total, physics, mesh-rebuild), draw calls, triangles, bodies; a
// final summary line at the end. The same counters ALSO print in the
// HUD when no benchmark is running (the user-facing stats line).
//
// Everything here is plain data + tiny inline helpers: no renderer
// types, no physics types - the app tells the counters what happened.

#pragma once

#include <chrono>

#include <ws/core/time.hpp>
#include <cstdint>

namespace voxel::perf {

// One frame's phase timings (milliseconds, steady_clock).
struct FrameTimings {
    float total_ms = 0.0f;      // the whole on_update body
    float physics_ms = 0.0f;    // player tick + phys advance + contacts
    float meshes_ms = 0.0f;     // rebuild_chunk calls this frame (edits)
    float draw_ms = 0.0f;       // draw_world's record pass (draw_mesh calls)
};

// The rolling counters the HUD and the benchmark print.
struct Counters {
    std::uint32_t draw_calls = 0;      // draw_mesh calls this frame
    std::uint32_t triangles = 0;       // indices/3 drawn this frame
    std::uint32_t bodies = 0;          // live physics bodies
    std::uint32_t meshes = 0;          // live chunk meshes
    std::uint32_t chunk_vis = 0;       // distinct (cx, cz) with drawn meshes
};

inline float ms_since(std::chrono::steady_clock::time_point t0,
                     std::chrono::steady_clock::time_point t1) noexcept {
  return ws::core::time::milliseconds_between(t0, t1);
}

inline float ms_since(std::chrono::steady_clock::time_point t0) noexcept {
  return ws::core::time::milliseconds_since(t0);
}

}  // namespace voxel::perf
