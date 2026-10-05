// apps/bench_physics - headless ws::physics throughput bench.
//
// Steps a "rain" of dynamic boxes onto a static floor and reports
// wall-clock cost per tick: 2D (Box2D) once, 3D (Jolt) single-threaded and
// with the machine's default worker pool. Two phases are timed per config -
// while the boxes are falling/settling (every island awake, contacts
// solving) and after they have had time to go to sleep - because the
// steady-state cost of a resting world is half the point of the sleep
// pipeline.
//
// Pass a body count as argv[1] (default 1000). Numbers are machine- and
// build-dependent on purpose (debug builds run Jolt with its internal
// asserts armed); that is why this is an app and not a suite case.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <ws/core/log.hpp>
#include <ws/math/vecmath.hpp>
#include <ws/physics/2d/body.hpp>
#include <ws/physics/2d/shape.hpp>
#include <ws/physics/2d/world.hpp>
#include <ws/physics/3d/body.hpp>
#include <ws/physics/3d/shape.hpp>
#include <ws/physics/3d/world.hpp>

namespace {

using ws::math::Quat;
using ws::math::Vec2;
using ws::math::Vec3;

// 1/60 s ticks. ~3 s of active falling/settling, then ~3 s of quiet for
// both engines to put every island down, then the sleeping measurement.
constexpr int kActiveTicks = 180;
constexpr int kSettleTicks = 180;
constexpr int kSleepTicks = 180;

// Boxes are 1x1 (2D) / 1x1x1 (3D) with a gap so nothing spawns overlapping;
// the grid is square-ish per dimension.
constexpr float kHalf = 0.5f;
constexpr float kPitch = 1.2f;

struct PhaseResult {
  double ms_per_step;
  std::uint64_t events;
};

[[nodiscard]] int clamp_count(int bodies) {
  return bodies > 0 ? (bodies < 100000 ? bodies : 100000) : 1000;
}

[[nodiscard]] std::uint32_t grid_width(std::uint32_t bodies) {
  const auto root = static_cast<std::uint32_t>(
      std::ceil(std::sqrt(static_cast<double>(bodies))));
  return root < 1u ? 1u : root;
}

// Steps + times n ticks, draining the per-tick surface exactly like a real
// app would (events read every frame).
[[nodiscard]] PhaseResult measure_2d(ws::physics2d::World2D& world,
                                    int ticks) {
  std::uint64_t events = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < ticks; ++i) {
    world.step();
    events += world.contact_events().size();
  }
  const auto t1 = std::chrono::steady_clock::now();
  return {std::chrono::duration<double, std::milli>(t1 - t0).count() / ticks,
          events};
}

[[nodiscard]] PhaseResult measure_3d(ws::physics3d::World3D& world,
                                    int ticks) {
  std::uint64_t events = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < ticks; ++i) {
    world.step();
    events += world.contact_events().size();
  }
  const auto t1 = std::chrono::steady_clock::now();
  return {std::chrono::duration<double, std::milli>(t1 - t0).count() / ticks,
          events};
}

void settle_2d(ws::physics2d::World2D& world, int ticks) {
  for (int i = 0; i < ticks; ++i) {
    world.step();
  }
}

void settle_3d(ws::physics3d::World3D& world, int ticks) {
  for (int i = 0; i < ticks; ++i) {
    world.step();
  }
}

[[nodiscard]] bool spawn_floor_and_rain_2d(ws::physics2d::World2D& world,
                                          std::uint32_t bodies) {
  namespace p2d = ws::physics2d;
  const std::uint32_t width = grid_width(bodies);
  const float span = static_cast<float>(width) * kPitch;

  p2d::BodyDesc floor{};
  floor.kind = ws::physics::BodyKind::static_;
  floor.shape = p2d::Box{Vec2{span, 0.5f}};
  floor.transform = p2d::Transform{Vec2{span * 0.5f, -0.5f}, 0.0f};
  if (!world.create_body(floor).has_value()) {
    return false;
  }

  for (std::uint32_t i = 0; i < bodies; ++i) {
    const std::uint32_t col = i % width;
    const std::uint32_t row = i / width;
    p2d::BodyDesc d{};
    d.shape = p2d::Box{Vec2{kHalf, kHalf}};
    d.transform = p2d::Transform{
        Vec2{(0.5f + static_cast<float>(col)) * kPitch,
             1.0f + static_cast<float>(row) * kPitch},
        0.0f};
    if (!world.create_body(d).has_value()) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool spawn_floor_and_rain_3d(ws::physics3d::World3D& world,
                                          std::uint32_t bodies) {
  namespace p3d = ws::physics3d;
  const std::uint32_t width = grid_width(bodies);
  const std::uint32_t per_layer = width * width;
  const float span = static_cast<float>(width) * kPitch;

  p3d::BodyDesc floor{};
  floor.kind = ws::physics::BodyKind::static_;
  floor.shape = p3d::Box{Vec3{span, 0.5f, span}};
  floor.transform =
      p3d::Transform{Vec3{span * 0.5f, -0.5f, span * 0.5f}, Quat{}};
  if (!world.create_body(floor).has_value()) {
    return false;
  }

  for (std::uint32_t i = 0; i < bodies; ++i) {
    const std::uint32_t layer = i / per_layer;
    const std::uint32_t rest = i % per_layer;
    const std::uint32_t col = rest % width;
    const std::uint32_t depth = rest / width;
    p3d::BodyDesc d{};
    d.shape = p3d::Box{Vec3{kHalf, kHalf, kHalf}};
    d.transform = p3d::Transform{
        Vec3{(0.5f + static_cast<float>(col)) * kPitch,
             1.0f + static_cast<float>(layer) * kPitch,
             (0.5f + static_cast<float>(depth)) * kPitch},
        Quat{}};
    if (!world.create_body(d).has_value()) {
      return false;
    }
  }
  return true;
}

void report(const char* config, std::uint32_t bodies, int threads,
            const PhaseResult& active, const PhaseResult& sleeping) {
  std::printf("%-14s %6u bodies %2d thr | active %8.3f ms/step %8llu events"
              " | sleeping %7.3f ms/step\n",
              config, bodies, threads, active.ms_per_step,
              static_cast<unsigned long long>(active.events),
              sleeping.ms_per_step);
}

}  // namespace

int main(int argc, char** argv) {
  const int bodies = clamp_count(argc > 1 ? std::atoi(argv[1]) : 1000);
  const auto total = static_cast<std::uint32_t>(bodies);
  ws::core::info(
      "bench_physics: {} boxes per dimension, active {} + settle {} + sleep"
      " {} ticks per config",
      bodies, kActiveTicks, kSettleTicks, kSleepTicks);

  std::printf("%-14s %17s | %29s | %s\n", "config", "", "active phase",
              "sleeping phase");
  std::printf("--------------------------------------------------------------"
              "------------------\n");

  // --- 2D: Box2D ----------------------------------------------------------
  {
    namespace p2d = ws::physics2d;
    p2d::World2D::Desc desc{};
    desc.max_bodies = total + 1;  // floor + the rain
    desc.max_contact_events = 8192;  // 1000 boxes land in one loud tick
    auto world = p2d::World2D::create(desc);
    if (!world.has_value()) {
      ws::core::log_error(world.error());
      return 1;
    }
    if (!spawn_floor_and_rain_2d(*world, total)) {
      ws::core::error("bench_physics: 2D spawn failed (world full?)");
      return 1;
    }
    const PhaseResult active = measure_2d(*world, kActiveTicks);
    settle_2d(*world, kSettleTicks);
    const PhaseResult sleeping = measure_2d(*world, kSleepTicks);
    report("2D Box2D", total, 0, active, sleeping);
  }

  // --- 3D: Jolt, single-threaded and default workers ----------------------
  const int thread_configs[] = {0, static_cast<int>(
                                       ws::physics::default_thread_count())};
  for (int threads : thread_configs) {
    namespace p3d = ws::physics3d;
    p3d::World3D::Desc desc{};
    desc.max_bodies = total + 1;
    desc.max_contact_events = 8192;
    desc.thread_count = static_cast<std::uint32_t>(threads);
    auto world = p3d::World3D::create(desc);
    if (!world.has_value()) {
      ws::core::log_error(world.error());
      return 1;
    }
    if (!spawn_floor_and_rain_3d(*world, total)) {
      ws::core::error("bench_physics: 3D spawn failed (world full?)");
      return 1;
    }
    const PhaseResult active = measure_3d(*world, kActiveTicks);
    settle_3d(*world, kSettleTicks);
    const PhaseResult sleeping = measure_3d(*world, kSleepTicks);
    char label[32];
    std::snprintf(label, sizeof(label), "3D Jolt %dw", threads);
    report(label, total, threads, active, sleeping);
  }

  ws::core::info("bench_physics: done");
  return 0;
}
