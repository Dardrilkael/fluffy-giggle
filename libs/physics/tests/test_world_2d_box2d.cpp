// No DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here - test_common.cpp owns doctest's
// main (one binary, several TUs).
//
// The Box2D CONTRACT suite: engine BEHAVIOR through the public API. Gravity
// and drift, bounce, stacking + sleep, per-tick force clearing (Box2D
// integrates an applied force across a step's internal substeps and clears
// it once per step - verified against src/solver.c), contact events (pair
// order, normal direction, the per-CALL union across an advance()'s
// substeps, hit events), joints (weld, distance hold, revolute motor +
// limits, the body-destroy cascade), raycast/overlap results,
// moved_bodies()/prev_transforms() semantics, the replay determinism
// contract, and the zero-allocation steady state. The backend-independent
// machinery is pinned in test_world_2d.cpp.
//
// The bottom of this file replaces global operator new/delete for the
// whole test binary: the zero-allocation case counts every C++
// allocation in the process. That measures THIS wrapper - Box2D's own C
// heap (its arena/block allocator) is engine-internal by design.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

#include <new>

#include "check.hpp"

#include <ws/math/vecmath.hpp>
#include <ws/physics/2d/joint.hpp>
#include <ws/physics/2d/world.hpp>

using ws::physics::PhysicsError;
using ws::physics2d::Aabb;
using ws::physics2d::BodyDesc;
using ws::physics2d::BodyId;
using ws::physics2d::BodyMove;
using ws::physics2d::Box;
using ws::physics2d::Capsule;
using ws::physics2d::Circle;
using ws::physics2d::ContactPhase;
using ws::physics2d::DistanceJoint;
using ws::physics2d::FixedJoint;
using ws::physics2d::RayHit;
using ws::physics2d::RevoluteJoint;
using ws::physics2d::Transform;
using ws::physics2d::World2D;
using wsphys::test::has_code;

namespace {

constexpr float kDt = 1.0f / 60.0f;

[[nodiscard]] World2D make_world(World2D::Desc desc = {}) {
  auto world = World2D::create(desc);
  REQUIRE(world.has_value());
  return std::move(*world);
}

// A 1 m^2 dynamic box (density 1 -> mass 1 kg, I = 1/6 kg*m^2): the exact
// numbers the force/torque case below depends on.
[[nodiscard]] BodyDesc dynamic_box(ws::math::Vec2 position) {
  BodyDesc d{};
  d.shape = Box{ws::math::Vec2{0.5f, 0.5f}};
  d.transform = Transform{position, 0.0f};
  return d;
}

[[nodiscard]] BodyDesc static_box(ws::math::Vec2 position,
                                  ws::math::Vec2 half) {
  BodyDesc d{};
  d.kind = ws::physics::BodyKind::static_;
  d.shape = Box{half};
  d.transform = Transform{position, 0.0f};
  return d;
}

[[nodiscard]] BodyDesc ball_at(ws::math::Vec2 position, float restitution) {
  BodyDesc d{};
  d.shape = Circle{0.5f};
  d.transform = Transform{position, 0.0f};
  d.restitution = restitution;
  return d;
}

void step_n(World2D& world, int n) {
  for (int i = 0; i < n; ++i) {
    REQUIRE(world.step().has_value());
  }
}

}  // namespace

TEST_CASE("gravity accelerates straight down; x stays put") {
  auto world = make_world();  // the default pool: real threaded stepping
  auto id = world.create_body(dynamic_box({0.0f, 10.0f}));
  REQUIRE(id.has_value());

  step_n(world, 60);  // one second of free fall

  auto state = world.body_state(*id);
  REQUIRE(state.has_value());
  CHECK(std::abs(state->position.x) < 0.01f);
  CHECK(state->position.y < 8.0f);   // fell
  CHECK(state->position.y > 3.0f);   // ~5.1 m expected after 1 s from rest
  CHECK(state->linear_velocity.y == doctest::Approx(-9.81f).epsilon(0.02));
  CHECK(std::abs(state->linear_velocity.x) < 0.01f);
}

TEST_CASE("a zero-gravity world preserves inertial drift exactly") {
  auto world = make_world(World2D::Desc{.gravity = {0.0f, 0.0f},
                                         .thread_count = 0});
  auto id = world.create_body(dynamic_box({0.0f, 0.0f}));
  REQUIRE(id.has_value());
  REQUIRE(world.set_velocity(*id, {2.0f, 1.0f}, 0.0f).has_value());

  step_n(world, 60);

  auto state = world.body_state(*id);
  REQUIRE(state.has_value());
  CHECK(state->position.x == doctest::Approx(2.0f).epsilon(0.001));
  CHECK(state->position.y == doctest::Approx(1.0f).epsilon(0.001));
  CHECK(state->linear_velocity.x == doctest::Approx(2.0f).epsilon(0.001));
  CHECK(state->linear_velocity.y == doctest::Approx(1.0f).epsilon(0.001));
}

TEST_CASE("restitution 1 bounces: the impact flips the velocity sign") {
  auto world = make_world(World2D::Desc{.thread_count = 0});
  auto floor = world.create_body(static_box({0.0f, -1.0f}, {50.0f, 1.0f}));
  REQUIRE(floor.has_value());
  auto ball = world.create_body(ball_at({0.0f, 5.0f}, 1.0f));
  REQUIRE(ball.has_value());

  bool bounced = false;
  bool began = false;
  for (int i = 0; i < 240; ++i) {  // 4 s
    REQUIRE(world.step().has_value());
    for (const auto& e : world.contact_events()) {
      CHECK(e.a.index < e.b.index);  // the pair-order invariant, every event
      began = true;
    }
    auto state = world.body_state(*ball);
    REQUIRE(state.has_value());
    if (state->linear_velocity.y > 0.5f) {
      bounced = true;  // moving up after a downward drop: it bounced
    }
  }
  CHECK(began);
  CHECK(bounced);
}

TEST_CASE("a stack settles, sleeps and leaves moved_bodies() empty") {
  auto world = make_world(World2D::Desc{.thread_count = 0});
  auto floor = world.create_body(static_box({0.0f, -0.5f}, {50.0f, 0.5f}));
  REQUIRE(floor.has_value());
  std::vector<BodyId> boxes;
  for (int i = 0; i < 3; ++i) {
    // Deliberate gaps: the stack falls together, slams, then settles.
    auto id = world.create_body(dynamic_box({0.0f, 0.5f + 1.5f * i}));
    REQUIRE(id.has_value());
    boxes.push_back(*id);
  }

  // Falling: all three report as moved, and prev holds the earlier pose.
  step_n(world, 1);
  REQUIRE(world.moved_bodies().size() == 3);
  for (const auto& m : world.moved_bodies()) {
    const float cur = world.transforms()[m.id.index].position.y;
    const float prev = world.prev_transforms()[m.id.index].position.y;
    CHECK(cur < prev);
  }
  CHECK(world.prev_transforms().size() == world.transforms().size());

  step_n(world, 300);  // five seconds: slam, wobble, settle, sleep

  for (std::size_t i = 0; i < boxes.size(); ++i) {
    auto state = world.body_state(boxes[i]);
    REQUIRE(state.has_value());
    CHECK(std::abs(state->linear_velocity.x) < 0.05f);
    CHECK(std::abs(state->linear_velocity.y) < 0.05f);
    CHECK(std::abs(state->angular_velocity) < 0.05f);
    // Rest heights preserved: 0.5 / 1.5 / 2.5 (+- small solver overlap).
    CHECK(std::abs(state->position.y - (0.5f + 1.0f * i)) < 0.02f);
  }

  // Asleep: nothing moved on the last call, so the sleep sync cost is
  // zero and the prev mirror equals the current one.
  CHECK(world.moved_bodies().empty());
  for (const auto& id : boxes) {
    const auto& cur = world.transforms()[id.index];
    const auto& prev = world.prev_transforms()[id.index];
    CHECK(cur.position.x == doctest::Approx(prev.position.x));
    CHECK(cur.position.y == doctest::Approx(prev.position.y));
  }
}

TEST_CASE("forces accumulate within a tick and clear at its end") {
  auto world = make_world(World2D::Desc{.gravity = {0.0f, 0.0f},
                                         .thread_count = 0});
  auto id = world.create_body(dynamic_box({0.0f, 0.0f}));
  REQUIRE(id.has_value());

  // One tick of F = 10 N on 1 kg: dv = F/m * dt = 1/6 m/s.
  REQUIRE(world.apply_force(*id, {10.0f, 0.0f}).has_value());
  step_n(world, 1);
  auto state = world.body_state(*id);
  REQUIRE(state.has_value());
  CHECK(std::abs(state->linear_velocity.x - 10.0f * kDt) < 1e-3f);

  // No force this tick: the velocity coasts unchanged - it did not persist.
  step_n(world, 1);
  state = world.body_state(*id);
  REQUIRE(state.has_value());
  CHECK(std::abs(state->linear_velocity.x - 10.0f * kDt) < 1e-3f);

  // Two applies before one tick accumulate: dv doubles on top of that.
  REQUIRE(world.apply_force(*id, {10.0f, 0.0f}).has_value());
  REQUIRE(world.apply_force(*id, {10.0f, 0.0f}).has_value());
  step_n(world, 1);
  state = world.body_state(*id);
  REQUIRE(state.has_value());
  CHECK(std::abs(state->linear_velocity.x - 3.0f * 10.0f * kDt) < 1e-3f);

  // Torque behaves identically: dw = tau/I * dt = 6 * dt rad/s.
  REQUIRE(world.apply_torque(*id, 1.0f).has_value());
  step_n(world, 1);
  state = world.body_state(*id);
  REQUIRE(state.has_value());
  CHECK(std::abs(state->angular_velocity - 6.0f * kDt) < 1e-3f);
  step_n(world, 1);
  state = world.body_state(*id);
  REQUIRE(state.has_value());
  CHECK(std::abs(state->angular_velocity - 6.0f * kDt) < 1e-3f);
}

TEST_CASE("contact events: pair order fixes the normal to point a -> b") {
  const auto run = [](bool floor_first) {
    auto world = make_world(World2D::Desc{.thread_count = 0});
    const BodyDesc floor_desc = static_box({0.0f, -0.5f}, {50.0f, 0.5f});
    const BodyDesc ball_desc = ball_at({0.0f, 5.0f}, 0.0f);
    BodyId floor{};
    BodyId ball{};
    if (floor_first) {
      floor = *world.create_body(floor_desc);
      ball = *world.create_body(ball_desc);
    } else {
      ball = *world.create_body(ball_desc);
      floor = *world.create_body(floor_desc);
    }
    REQUIRE(floor.index != ball.index);

    for (int i = 0; i < 90; ++i) {
      REQUIRE(world.step().has_value());
      if (!world.contact_events().empty()) {
        break;
      }
    }
    REQUIRE_FALSE(world.contact_events().empty());
    const auto& e = world.contact_events().front();
    CHECK(e.phase == ContactPhase::began);
    CHECK(e.a.index < e.b.index);
    // The interface sits at y=0; Box2D's speculative margin can begin the
    // contact slightly BEFORE geometric touch, hence the loose bound.
    CHECK(std::abs(e.point.y) < 0.1f);
    if (floor_first) {
      CHECK(e.a == floor);  // lower slot index wins the "a" role
      CHECK(e.b == ball);
      CHECK(e.normal.y == doctest::Approx(1.0f).epsilon(0.01));  // floor -> ball
      CHECK(std::abs(e.normal.x) < 0.01f);
    } else {
      CHECK(e.a == ball);
      CHECK(e.b == floor);
      CHECK(e.normal.y == doctest::Approx(-1.0f).epsilon(0.01));  // ball -> floor
      CHECK(std::abs(e.normal.x) < 0.01f);
    }
  };
  run(true);   // floor created first: a = floor
  run(false);  // ball created first: a = ball, normal re-faced
}

TEST_CASE("advance() unions contact events across its substeps") {
  World2D::Desc desc{};
  desc.thread_count = 0;
  desc.hit_speed_threshold = 1.0f;  // m/s: enables hit events
  desc.max_substeps = 128;
  auto world = make_world(desc);
  auto floor = world.create_body(static_box({0.0f, -0.5f}, {50.0f, 0.5f}));
  REQUIRE(floor.has_value());
  auto ball = world.create_body(ball_at({0.0f, 5.0f}, 0.0f));
  REQUIRE(ball.has_value());

  // 1.5 s in ONE advance() call: ~89 ticks. The impact lands mid-call
  // (t = 0.96 s); its events MUST survive to the call's end.
  REQUIRE(world.advance(1.5f).has_value());
  bool saw_began = false;
  bool saw_hit = false;
  for (const auto& e : world.contact_events()) {
    REQUIRE(e.a.index < e.b.index);
    if (e.phase == ContactPhase::began) {
      saw_began = true;
    }
    if (e.phase == ContactPhase::hit) {
      saw_hit = true;
      CHECK(e.speed > 1.0f);  // the world's threshold
      // Free fall over 4.5 m: sqrt(2 * 9.81 * 4.5) = 9.4 m/s.
      CHECK(e.speed == doctest::Approx(9.4f).epsilon(0.1));
    }
  }
  CHECK(saw_began);
  CHECK(saw_hit);

  // Resting: began fires once per overlap (no repeat) and the impact is
  // over, so a plain one-tick call reports neither.
  REQUIRE(world.advance(1.0f / 60.0f).has_value());
  for (const auto& e : world.contact_events()) {
    CHECK(e.phase != ContactPhase::began);
    CHECK(e.phase != ContactPhase::hit);
  }
}

TEST_CASE("collision filters gate contacts, sensors and events") {
  // The rule is BOTH-directional: (a.collision_layer & b.collision_mask)
  // && (b.collision_layer & a.collision_mask). Every subcase is the same
  // 5 m drop onto a floor whose interface sits at y = 0 (ball center
  // rests at 0.5), so the y position after the steps IS the verdict.

  SUBCASE("a one-directional reject is a reject: the ball falls through") {
    auto world = make_world(World2D::Desc{.thread_count = 0});
    BodyDesc floor_desc = static_box({0.0f, -0.5f}, {50.0f, 0.5f});
    floor_desc.collision_mask = 2u;  // accepts layer 2 only: rejects the layer-1 ball
    auto floor = world.create_body(floor_desc);
    REQUIRE(floor.has_value());
    auto ball = world.create_body(ball_at({0.0f, 5.0f}, 0.0f));
    REQUIRE(ball.has_value());

    bool any_event = false;
    for (int i = 0; i < 90; ++i) {
      REQUIRE(world.step().has_value());
      any_event = any_event || !world.contact_events().empty();
    }
    CHECK_FALSE(any_event);  // the pair never existed
    auto state = world.body_state(*ball);
    REQUIRE(state.has_value());
    CHECK(state->position.y < -1.0f);  // through the interface, still falling
  }

  SUBCASE("runtime set_collision_filter flips a resting pair off") {
    auto world = make_world(World2D::Desc{.thread_count = 0});
    auto floor = world.create_body(static_box({0.0f, -0.5f}, {50.0f, 0.5f}));
    REQUIRE(floor.has_value());
    auto ball = world.create_body(ball_at({0.0f, 5.0f}, 0.0f));
    REQUIRE(ball.has_value());

    step_n(world, 120);  // drop and settle
    auto state = world.body_state(*ball);
    REQUIRE(state.has_value());
    CHECK(std::abs(state->position.y - 0.5f) < 0.05f);  // resting on it

    // Mask 0 accepts nothing; Box2D wakes the island on a filter change,
    // so the re-gated pair dies and the ball drops out of the world.
    REQUIRE(world.set_collision_filter(*ball, 1u, 0u).has_value());
    step_n(world, 90);
    state = world.body_state(*ball);
    REQUIRE(state.has_value());
    CHECK(state->position.y < -1.0f);
  }

  SUBCASE("a sensor fires began, does not respond, and sails the ball through") {
    auto world = make_world(World2D::Desc{.thread_count = 0});
    BodyDesc zone_desc = static_box({0.0f, -0.5f}, {50.0f, 0.5f});
    zone_desc.is_sensor = true;
    auto zone = world.create_body(zone_desc);
    REQUIRE(zone.has_value());
    auto ball = world.create_body(ball_at({0.0f, 5.0f}, 0.0f));
    REQUIRE(ball.has_value());

    bool sensor_began = false;
    for (int i = 0; i < 90; ++i) {
      REQUIRE(world.step().has_value());
      for (const auto& e : world.contact_events()) {
        if (e.phase == ContactPhase::began) {
          sensor_began = true;  // the only pair in the world
        }
      }
    }
    CHECK(sensor_began);
    auto state = world.body_state(*ball);
    REQUIRE(state.has_value());
    CHECK(state->position.y < -1.0f);  // no impulse: it fell straight through
  }

  SUBCASE("a filtered sensor stays silent") {
    auto world = make_world(World2D::Desc{.thread_count = 0});
    BodyDesc zone_desc = static_box({0.0f, -0.5f}, {50.0f, 0.5f});
    zone_desc.is_sensor = true;
    zone_desc.collision_mask = 0u;  // the zone accepts nothing
    auto zone = world.create_body(zone_desc);
    REQUIRE(zone.has_value());
    auto ball = world.create_body(ball_at({0.0f, 5.0f}, 0.0f));
    REQUIRE(ball.has_value());

    bool any_event = false;
    for (int i = 0; i < 90; ++i) {
      REQUIRE(world.step().has_value());
      any_event = any_event || !world.contact_events().empty();
    }
    CHECK_FALSE(any_event);
  }
}

TEST_CASE("runtime set_gravity redirects the fall and wakes the sleepers") {
  SUBCASE("flipping the sign turns the fall into a climb") {
    auto world = make_world(World2D::Desc{.thread_count = 0});
    auto id = world.create_body(dynamic_box({0.0f, 10.0f}));
    REQUIRE(id.has_value());

    step_n(world, 30);  // 0.5 s of free fall: y ~8.7, v ~-4.9
    auto state = world.body_state(*id);
    REQUIRE(state.has_value());

    REQUIRE(world.set_gravity({0.0f, 9.81f}).has_value());
    step_n(world, 60);  // one more second: the climb retraces the fall

    state = world.body_state(*id);
    REQUIRE(state.has_value());
    // Unflipped the box would sit at ~-0.9 and still be diving; flipped
    // the climb retraces the fall, landing back just above the flip
    // height (~8.8) with an upward velocity.
    CHECK(state->position.y > 8.4f);
    CHECK(state->linear_velocity.y > 4.0f);
  }

  SUBCASE("a resting pile sleeps, then wakes and leaves the floor") {
    auto world = make_world(World2D::Desc{.thread_count = 0});
    auto floor = world.create_body(static_box({0.0f, -1.0f}, {50.0f, 1.0f}));
    REQUIRE(floor.has_value());
    auto ball = world.create_body(ball_at({0.0f, 5.0f}, 0.3f));
    REQUIRE(ball.has_value());

    step_n(world, 240);  // 4 s: settle, then outlast the sleep timer
    auto state = world.body_state(*ball);
    REQUIRE(state.has_value());
    CHECK(std::abs(state->position.y - 0.5f) < 0.05f);  // resting on the floor
    REQUIRE(world.step().has_value());
    bool moved_while_sleeping = false;
    for (const auto& m : world.moved_bodies()) {
      if (m.id == *ball) {
        moved_while_sleeping = true;
      }
    }
    CHECK_FALSE(moved_while_sleeping);  // it really slept

    REQUIRE(world.set_gravity({0.0f, 9.81f}).has_value());
    step_n(world, 60);

    state = world.body_state(*ball);
    REQUIRE(state.has_value());
    // Unwoken it would sit at 0.5 forever; the wake-all lifts it out.
    CHECK(state->position.y > 1.5f);
  }
}

TEST_CASE("welded bodies move as one; destroying a body releases the joint") {
  auto world = make_world(World2D::Desc{.gravity = {0.0f, 0.0f},
                                         .thread_count = 0});
  auto a = world.create_body(dynamic_box({0.0f, 0.0f}));
  auto b = world.create_body(dynamic_box({1.0f, 0.0f}));
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  auto joint = world.create_joint(FixedJoint{*a, *b});
  REQUIRE(joint.has_value());

  REQUIRE(world.set_velocity(*a, {1.0f, 0.0f}, 0.0f).has_value());
  REQUIRE(world.set_velocity(*b, {1.0f, 0.0f}, 0.0f).has_value());  // wake both
  step_n(world, 60);
  auto sa = world.body_state(*a);
  auto sb = world.body_state(*b);
  REQUIRE(sa.has_value());
  REQUIRE(sb.has_value());
  CHECK(sb->position.x - sa->position.x == doctest::Approx(1.0f).epsilon(0.01));
  CHECK(sa->linear_velocity.x == doctest::Approx(1.0f).epsilon(0.01));
  CHECK(sb->linear_velocity.x == doctest::Approx(1.0f).epsilon(0.01));

  // The cascade: killing a destroys the joint (engines refuse joints whose
  // bodies are gone - the lib removes the joint first) and the survivor
  // keeps simulating, unconstrained.
  REQUIRE(world.destroy_body(*a).has_value());
  CHECK(has_code(world.destroy_joint(*joint), PhysicsError::invalid_handle));
  step_n(world, 30);
  auto free_b = world.body_state(*b);
  REQUIRE(free_b.has_value());
  CHECK(free_b->linear_velocity.x == doctest::Approx(1.0f).epsilon(0.01));
  CHECK(free_b->position.x > 1.0f);  // kept drifting
}

TEST_CASE("a distance joint holds the pair at its length until destroyed") {
  auto world = make_world(World2D::Desc{.gravity = {0.0f, 0.0f},
                                         .thread_count = 0});
  auto a = world.create_body(dynamic_box({0.0f, 0.0f}));
  auto b = world.create_body(dynamic_box({2.0f, 0.0f}));
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  auto joint = world.create_joint(DistanceJoint{*a, *b, 2.0f});
  REQUIRE(joint.has_value());

  REQUIRE(world.set_velocity(*b, {3.0f, 0.0f}, 0.0f).has_value());  // pulls away
  step_n(world, 60);
  auto sa = world.body_state(*a);
  auto sb = world.body_state(*b);
  REQUIRE(sa.has_value());
  REQUIRE(sb.has_value());
  const float held = std::hypot(sb->position.x - sa->position.x,
                                sb->position.y - sa->position.y);
  CHECK(held == doctest::Approx(2.0f).epsilon(0.02));  // the rod held

  // Explicit destroy: the pair drifts apart freely.
  REQUIRE(world.destroy_joint(*joint).has_value());
  REQUIRE(world.apply_linear_impulse(*b, {2.0f, 0.0f}).has_value());
  step_n(world, 60);
  sb = world.body_state(*b);
  sa = world.body_state(*a);
  REQUIRE(sb.has_value());
  REQUIRE(sa.has_value());
  const float free = std::hypot(sb->position.x - sa->position.x,
                                sb->position.y - sa->position.y);
  CHECK(free > 3.0f);
}

TEST_CASE("revolute joint: the motor spins the arm; limits stop it") {
  const auto make = []() {
    auto world = make_world(World2D::Desc{.gravity = {0.0f, 0.0f},
                                           .thread_count = 0});
    auto anchor = world.create_body(static_box({0.0f, 0.0f}, {0.25f, 0.25f}));
    auto arm = world.create_body(dynamic_box({1.0f, 0.0f}));
    REQUIRE(anchor.has_value());
    REQUIRE(arm.has_value());
    RevoluteJoint joint{};
    joint.a = *anchor;
    joint.b = *arm;
    joint.world_anchor = {0.0f, 0.0f};
    joint.enable_motor = true;
    joint.motor_speed = 2.0f;  // rad/s
    joint.max_motor_torque = 100.0f;
    auto id = world.create_joint(joint);
    REQUIRE(id.has_value());
    return std::pair{std::move(world), *arm};
  };

  SUBCASE("free spin reaches the motor speed") {
    auto [world, arm] = make();
    step_n(world, 60);  // 1 s at ~2 rad/s from a ~0.02 s spin-up
    auto state = world.body_state(arm);
    REQUIRE(state.has_value());
    const float angle = std::atan2(state->position.y, state->position.x);
    CHECK(std::abs(angle - 2.0f) < 0.15f);
  }
  SUBCASE("limits stop the spin at 0.25 rad") {
    auto [world, arm] = make();
    RevoluteJoint limited{};
    // (Re-create with limits: the make() helper left them off.)
    auto world2 = std::move(world);
    // destroy and rebuild the joint with limits on a fresh pair
    auto anchor = world2.create_body(static_box({0.0f, 5.0f}, {0.25f, 0.25f}));
    auto arm2 = world2.create_body(dynamic_box({1.0f, 5.0f}));
    REQUIRE(anchor.has_value());
    REQUIRE(arm2.has_value());
    limited.a = *anchor;
    limited.b = *arm2;
    limited.world_anchor = {0.0f, 5.0f};
    limited.enable_motor = true;
    limited.motor_speed = 2.0f;
    limited.max_motor_torque = 100.0f;
    limited.enable_limits = true;
    limited.lower_angle = -0.25f;
    limited.upper_angle = 0.25f;
    REQUIRE(world2.create_joint(limited).has_value());
    step_n(world2, 60);
    auto state = world2.body_state(*arm2);
    REQUIRE(state.has_value());
    const float angle = std::atan2(state->position.y - 5.0f,
                                   state->position.x);
    CHECK(std::abs(angle - 0.25f) < 0.03f);
  }
}

TEST_CASE("raycast: closest hit, t in world units, a miss is nullopt") {
  auto world = make_world(World2D::Desc{.thread_count = 0});
  auto box = world.create_body(static_box({0.0f, 0.0f}, {0.5f, 0.5f}));
  REQUIRE(box.has_value());

  auto hit = world.raycast({0.0f, -5.0f}, {0.0f, 2.0f}, 10.0f);
  REQUIRE(hit.has_value());   // Result level: no error
  REQUIRE(hit->has_value());  // the optional: there is a hit
  CHECK((*hit)->body == *box);
  CHECK((*hit)->t == doctest::Approx(4.5f).epsilon(0.001));  // up to the face
  CHECK((*hit)->point.y == doctest::Approx(-0.5f).epsilon(0.001));
  CHECK((*hit)->normal.y == doctest::Approx(-1.0f).epsilon(0.001));
  CHECK(std::abs((*hit)->normal.x) < 0.001f);

  // max_distance stops the segment short of the box.
  auto short_ray = world.raycast({0.0f, -5.0f}, {0.0f, 2.0f}, 4.0f);
  REQUIRE(short_ray.has_value());
  CHECK_FALSE(short_ray->has_value());

  // A parallel ray misses cleanly.
  auto miss = world.raycast({5.0f, 0.0f}, {1.0f, 0.0f}, 10.0f);
  REQUIRE(miss.has_value());
  CHECK_FALSE(miss->has_value());
}

TEST_CASE("overlap queries return the touched bodies, sorted by index") {
  auto world = make_world(World2D::Desc{.gravity = {0.0f, 0.0f},
                                         .thread_count = 0});
  auto near_a = world.create_body(static_box({0.0f, 0.0f}, {0.5f, 0.5f}));
  auto near_b = world.create_body(static_box({10.0f, 0.0f}, {0.5f, 0.5f}));
  auto far = world.create_body(static_box({100.0f, 0.0f}, {0.5f, 0.5f}));
  REQUIRE(near_a.has_value());
  REQUIRE(near_b.has_value());
  REQUIRE(far.has_value());

  auto hits = world.overlap_circle({5.0f, 0.0f}, 6.0f);
  REQUIRE(hits.has_value());
  REQUIRE(hits->size() == 2);
  CHECK((*hits)[0] == *near_a);  // sorted by id index, not broad-phase order
  CHECK((*hits)[1] == *near_b);

  auto aabb_hits = world.overlap_aabb(Aabb{{-1.0f, -1.0f}, {11.0f, 1.0f}});
  REQUIRE(aabb_hits.has_value());
  REQUIRE(aabb_hits->size() == 2);
  CHECK((*aabb_hits)[0] == *near_a);
  CHECK((*aabb_hits)[1] == *near_b);

  auto none = world.overlap_circle({50.0f, 50.0f}, 1.0f);
  REQUIRE(none.has_value());
  CHECK(none->empty());
}

TEST_CASE("moved_bodies: kinematics report, statics never do; new bodies "
          "spawn un-lerped") {
  auto world = make_world(World2D::Desc{.gravity = {0.0f, 0.0f},
                                         .thread_count = 0});
  auto floor = world.create_body(static_box({0.0f, -10.0f}, {50.0f, 1.0f}));
  REQUIRE(floor.has_value());
  BodyDesc platform{};
  platform.kind = ws::physics::BodyKind::kinematic;
  platform.shape = Box{ws::math::Vec2{2.0f, 0.25f}};
  auto moving = world.create_body(platform);
  REQUIRE(moving.has_value());
  REQUIRE(world.set_velocity(*moving, {2.0f, 0.0f}, 0.0f).has_value());

  step_n(world, 60);  // one second at 2 m/s -> x = 2
  auto state = world.body_state(*moving);
  REQUIRE(state.has_value());
  CHECK(state->position.x == doctest::Approx(2.0f).epsilon(0.001));

  bool saw_moving = false;
  for (int i = 0; i < 10; ++i) {
    REQUIRE(world.step().has_value());
    for (const auto& m : world.moved_bodies()) {
      CHECK_FALSE(m.id == *floor);  // statics never appear
      if (m.id == *moving) {
        saw_moving = true;
        CHECK(m.position.x == doctest::Approx(
                                  world.transforms()[m.id.index].position.x)
                                  .epsilon(0.001));
      }
    }
  }
  CHECK(saw_moving);
  REQUIRE(world.moved_bodies().size() == 1);  // the kinematic, awake forever

  // A body created after steps spawns un-lerped: prev = creation pose, then
  // prev = pre-tick pose once it simulates.
  BodyDesc falling = dynamic_box({0.0f, 5.0f});
  falling.linear_velocity = {0.0f, -1.0f};
  auto fresh = world.create_body(falling);
  REQUIRE(fresh.has_value());
  CHECK(world.prev_transforms()[fresh->index].position.y ==
        doctest::Approx(5.0f));
  step_n(world, 1);
  CHECK(world.prev_transforms()[fresh->index].position.y ==
        doctest::Approx(5.0f));
  CHECK(world.transforms()[fresh->index].position.y < 5.0f);
}

namespace {

// The global operator new/delete overrides at the bottom of this file bump
// this counter; only before/after deltas around a window are meaningful.
long long g_alloc_count = 0;

}  // namespace

[[nodiscard]] long long allocation_count() { return g_alloc_count; }

TEST_CASE("steady-state stepping allocates zero C++ heap") {
  World2D::Desc desc{};
  desc.max_bodies = 64;
  desc.max_contact_events = 512;
  desc.thread_count = 2;  // the threaded path must hold the promise too
  auto world = make_world(desc);

  auto floor = world.create_body(static_box({0.0f, -10.0f}, {50.0f, 1.0f}));
  REQUIRE(floor.has_value());
  std::vector<BodyId> ids;
  for (int i = 0; i < 24; ++i) {
    auto id = world.create_body(dynamic_box({-4.0f + 1.0f * i,
                                            2.0f + 1.0f * (i % 4)}));
    REQUIRE(id.has_value());
    ids.push_back(*id);
  }
  auto weld = world.create_joint(FixedJoint{ids[0], ids[1]});
  REQUIRE(weld.has_value());
  auto rod = world.create_joint(DistanceJoint{ids[2], ids[3], 1.0f});
  REQUIRE(rod.has_value());

  step_n(world, 30);  // warm-up: settle Box2D's arenas and our buffers

  const long long before = allocation_count();
  for (int i = 0; i < 60; ++i) {
    REQUIRE(world.step().has_value());
    REQUIRE(world.contact_events().size() <= 512);
    REQUIRE(world.moved_bodies().size() <= ids.size());
    (void)world.prev_transforms();
    (void)world.interpolation_alpha();
    if (i == 30) {
      // The rest of the steady-state API surface, mid-window. The batch is
      // caller-owned stack memory - exactly the renderer pattern.
      const BodyMove batch_data[]{BodyMove{ids[4], {0.0f, 30.0f}, 0.5f}};
      REQUIRE(world.set_transforms(std::span{batch_data}).has_value());
      REQUIRE(world.apply_linear_impulse(ids[5], {0.1f, 0.2f}).has_value());
      REQUIRE(world.body_state(ids[6]).has_value());
    }
    if (i == 45) {
      // Spawn/despawn stays allocation-free too (capacity is pre-reserved;
      // Box2D's own growth happens on its C heap, not through new).
      REQUIRE(world.destroy_body(ids[7]).has_value());
      auto fresh = world.create_body(dynamic_box({2.0f, 25.0f}));
      REQUIRE(fresh.has_value());
    }
  }
  CHECK(allocation_count() - before == 0);
}

TEST_CASE("determinism: same binary, same thread count, same call sequence") {
  const auto replay_hash = [](unsigned int thread_count) {
    World2D::Desc desc{};
    desc.thread_count = thread_count;
    desc.max_bodies = 64;
    auto world = make_world(desc);
    auto floor = world.create_body(static_box({0.0f, -10.0f}, {50.0f, 1.0f}));
    REQUIRE(floor.has_value());
    std::vector<BodyId> ids;
    for (int i = 0; i < 5; ++i) {
      BodyDesc d = dynamic_box({-3.0f + 1.5f * i, 5.0f + 0.7f * i});
      d.restitution = 0.3f;
      auto id = world.create_body(d);
      REQUIRE(id.has_value());
      REQUIRE(world.set_velocity(*id, {0.3f * i, -1.0f}, 0.0f).has_value());
      ids.push_back(*id);
    }
    for (int s = 0; s < 120; ++s) {
      if (s % 30 == 0) {
        REQUIRE(world.apply_linear_impulse(ids[(s / 30) % ids.size()],
                                           {0.5f, 1.0f})
                    .has_value());
      }
      REQUIRE(world.step().has_value());
    }
    // FNV-1a over the raw bytes of every body's pose: bit-identical
    // replays must hash identically.
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](const void* p, std::size_t n) {
      const auto* bytes = static_cast<const unsigned char*>(p);
      for (std::size_t i = 0; i < n; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
      }
    };
    for (const auto& id : ids) {
      auto state = world.body_state(id);
      REQUIRE(state.has_value());
      mix(&state->position.x, sizeof(float));
      mix(&state->position.y, sizeof(float));
      mix(&state->rotation, sizeof(float));
    }
    return hash;
  };

  const auto serial_a = replay_hash(0);
  const auto serial_b = replay_hash(0);
  CHECK(serial_a == serial_b);
  const auto threaded_a = replay_hash(2);
  const auto threaded_b = replay_hash(2);
  CHECK(threaded_a == threaded_b);
}

// --- global allocation counters --------------------------------------------
// Replaces the process's operators so the zero-allocation case can see
// every C++ heap allocation. All allocations funnel through one family:
// _aligned_malloc/_aligned_free on MSVC, aligned_alloc/free elsewhere, so
// every delete pairs with its allocator regardless of alignment.

void* operator new(std::size_t n) {
  ++g_alloc_count;
  n = n ? n : 1;
#if defined(_MSC_VER)
  if (void* p = _aligned_malloc(n, 16)) {
    return p;
  }
#else
  if (void* p = std::aligned_alloc(16, ((n + 15u) / 16u) * 16u)) {
    return p;
  }
#endif
  throw std::bad_alloc{};
}

void* operator new[](std::size_t n) { return ::operator new(n); }

void* operator new(std::size_t n, std::align_val_t a) {
  ++g_alloc_count;
  n = n ? n : 1;
  const std::size_t alignment =
      static_cast<std::size_t>(a) < 16u ? 16u : static_cast<std::size_t>(a);
#if defined(_MSC_VER)
  if (void* p = _aligned_malloc(n, alignment)) {
    return p;
  }
#else
  if (void* p = std::aligned_alloc(alignment,
                                  ((n + alignment - 1u) / alignment) * alignment)) {
    return p;
  }
#endif
  throw std::bad_alloc{};
}

void* operator new[](std::size_t n, std::align_val_t a) {
  return ::operator new(n, a);
}

void operator delete(void* p) noexcept {
#if defined(_MSC_VER)
  _aligned_free(p);
#else
  std::free(p);
#endif
}

void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete(void* p, std::align_val_t) noexcept {
  ::operator delete(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
  ::operator delete(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::align_val_t) noexcept {
  ::operator delete(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
  ::operator delete(p);
}
