// The C6 reference-backend suite: CONTRACT.md semantics with
// hand-checkable numbers, driven through the RAW seam
// (make_reference_world_2d) rather than World2D - the machinery on top
// (the touching table, the event sort, generational handles) has its own
// suites, and WorldCore<Dim2D> is constructible only from world2d.cpp,
// where the Dim2D traits type lives (C7 moves it to a header when the
// conformance suite needs machinery-over-both-backends in one binary).
//
// The reference has no solver, so every number below is plain kinematics
// the reader can recompute: semi-implicit Euler under gravity is the
// staircase sum v_k = a dt k, x_k = a dt^2 k(k+1)/2, and F = ma /
// tau = I alpha ride the LIB's mass numbers (mass_data_2d), which is also
// what these tests pin end to end - the backend integrates exactly the
// mass data it is handed. The subset's refusals (joints, capsules) and its
// vacuums (nothing ever sleeps, pairs pass through) are pinned as such.

// No DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN - test_common.cpp owns doctest's
// main (one binary, several TUs).

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "check.hpp"
#include "core/backend2d.hpp"
#include "core/mass_properties.hpp"

#include <ws/math/vecmath.hpp>
#include <ws/physics/2d/body.hpp>
#include <ws/physics/2d/shape.hpp>
#include <ws/physics/common.hpp>
#include <ws/physics/errors.hpp>

namespace {

namespace phys = ws::physics;
namespace pd = ws::physics::detail;
namespace p2 = ws::physics2d;
using ws::math::Vec2;
using wsphys::test::has_code;

// Collects the raw feed verbatim, in feed order.
struct Feed final : pd::IEventCollector2D {
  std::vector<pd::BackendEvent2D> events;
  void add(const pd::BackendEvent2D& e) override { events.push_back(e); }
};

// A reference world plus the scratch the seam needs (the moves span, the
// feed). Zero gravity unless a test asks: with no solver and no sleep,
// gravity only ever adds ballistics.
struct Rig {
  std::unique_ptr<pd::IWorld2DBackend> world;
  std::vector<pd::BackendMove2D> moves;
  Feed feed;

  explicit Rig(const pd::BackendDesc2D& desc)
      : world(pd::make_reference_world_2d(desc)) {
    REQUIRE(world != nullptr);
    moves.resize(desc.max_bodies);
  }

  std::size_t step(float dt) {
    feed.events.clear();
    return world->step(dt, feed, std::span<pd::BackendMove2D>(moves));
  }

  // Creates through the seam the way WorldCore does: the lib's mass data
  // for dynamics, zeros for statics/kinematics (their mass semantics are
  // none, CONTRACT.md 4).
  void create(std::uint32_t slot, const p2::BodyDesc& d) {
    pd::MassData2D mass{};
    if (d.kind == phys::BodyKind::dynamic) {
      mass = pd::mass_data_2d(d);
    }
    auto st = world->create_body(slot, d, mass);
    REQUIRE(st.has_value());
  }

  std::size_t find_move(std::uint32_t slot, std::size_t count) const {
    for (std::size_t i = 0; i < count; ++i) {
      if (moves[i].slot == slot) {
        return i;
      }
    }
    REQUIRE(false);
    return 0;
  }
};

pd::BackendDesc2D make_desc(std::uint32_t max_bodies, Vec2 gravity = {},
                            float hit_threshold = 0.0f) {
  pd::BackendDesc2D d{};
  d.max_bodies = max_bodies;
  d.gravity = gravity;
  d.hit_speed_threshold = hit_threshold;
  return d;
}

// A dynamic circle of radius r at (x, y).
p2::BodyDesc circle(float x, float y, float r) {
  p2::BodyDesc d{};
  d.shape = p2::Circle{r};
  d.transform.position = Vec2{x, y};
  return d;
}

}  // namespace

TEST_CASE("ballistics: semi-implicit Euler under gravity and gravity_scale") {
  Rig rig(make_desc(4, Vec2{0.0f, -10.0f}));
  rig.create(0, circle(0.0f, 0.0f, 0.5f));
  p2::BodyDesc scaled = circle(0.0f, 0.0f, 0.5f);
  scaled.gravity_scale = 2.0f;
  rig.create(1, scaled);

  constexpr float dt = 0.1f;
  for (int i = 0; i < 3; ++i) {
    const std::size_t moved = rig.step(dt);
    CHECK(moved == 2);  // both dynamics, never asleep, slot order
  }
  // v_k = a dt k = -k; y_3 = a dt^2 k(k+1)/2 = -0.6.
  const auto va = rig.world->get_velocity(0);
  CHECK(va.linear.y == doctest::Approx(-3.0f));
  CHECK(va.linear.x == 0.0f);
  CHECK(va.angular == 0.0f);
  CHECK(rig.moves[0].transform.position.y == doctest::Approx(-0.6f));
  CHECK(rig.moves[0].transform.position.x == doctest::Approx(0.0f));
  CHECK(rig.moves[0].transform.rotation == doctest::Approx(0.0f));
  // gravity_scale multiplies g and nothing else.
  const auto vb = rig.world->get_velocity(1);
  CHECK(vb.linear.y == doctest::Approx(-6.0f));
  CHECK(rig.moves[1].transform.position.y == doctest::Approx(-1.2f));
}

TEST_CASE("dynamics: forces and torques drive the lib's mass numbers") {
  Rig rig(make_desc(4));
  p2::BodyDesc box{};
  box.shape = p2::Box{Vec2{0.5f, 0.5f}};  // 1x1 at density 3
  box.density = 3.0f;
  rig.create(0, box);

  // The backend must integrate exactly the LIB's mass data: m = 3,
  // I = m (hx^2 + hy^2) / 3 = 0.5.
  const pd::MassData2D mass = pd::mass_data_2d(box);
  CHECK(mass.mass == doctest::Approx(3.0f));
  CHECK(mass.inertia == doctest::Approx(0.5f));

  constexpr float dt = 0.1f;
  rig.world->apply_force(0, Vec2{3.0f, 0.0f});  // a = F/m = 1
  rig.world->apply_torque(0, 0.5f);            // alpha = tau/I = 1
  rig.step(dt);
  rig.world->apply_force(0, Vec2{3.0f, 0.0f});  // accumulate again per tick
  rig.world->apply_torque(0, 0.5f);
  rig.step(dt);
  const auto v = rig.world->get_velocity(0);
  CHECK(v.linear.x == doctest::Approx(0.2f));
  CHECK(v.angular == doctest::Approx(0.2f));
  CHECK(rig.moves[0].transform.position.x == doctest::Approx(0.03f));
  CHECK(rig.moves[0].transform.rotation == doctest::Approx(0.03f));

  // No re-application: the accumulation cleared at each tick end.
  rig.step(dt);
  const auto held = rig.world->get_velocity(0);
  CHECK(held.linear.x == doctest::Approx(0.2f));
  CHECK(held.angular == doctest::Approx(0.2f));
  CHECK(rig.moves[0].transform.position.x == doctest::Approx(0.05f));

  // Forces never move a kinematic (no mass semantics, CONTRACT.md 4).
  p2::BodyDesc kin{};
  kin.kind = phys::BodyKind::kinematic;
  kin.shape = p2::Box{Vec2{0.5f, 0.5f}};
  rig.create(1, kin);
  rig.world->apply_force(1, Vec2{100.0f, 0.0f});
  rig.world->apply_torque(1, 100.0f);
  rig.step(dt);
  CHECK(rig.world->get_velocity(1).linear.x == 0.0f);
  CHECK(rig.world->get_velocity(1).angular == 0.0f);
}

TEST_CASE("composite COM: the origin reconstructs from COM-frame motion") {
  Rig rig(make_desc(4));
  p2::BodyDesc body{};
  body.part_count = 1;
  body.parts[0].shape = p2::Box{Vec2{0.5f, 0.5f}};
  body.parts[0].offset = Vec2{1.0f, 0.0f};  // COM at (1, 0), body-local
  body.angular_velocity = 1.0f;             // spin about the COM
  rig.create(0, body);

  const pd::MassData2D mass = pd::mass_data_2d(body);
  CHECK(mass.center.x == doctest::Approx(1.0f));
  CHECK(mass.center.y == doctest::Approx(0.0f));

  rig.step(0.1f);  // zero gravity: the COM holds still, only the angle turns
  // origin = com_world - R(theta_new) com_local with com_world = (1, 0).
  const float c = std::cos(0.1f);
  const float s = std::sin(0.1f);
  CHECK(rig.moves[0].transform.position.x == doctest::Approx(1.0f - c));
  CHECK(rig.moves[0].transform.position.y == doctest::Approx(-s));
  CHECK(rig.moves[0].transform.rotation == doctest::Approx(0.1f));
  // Spinning about the COM drifts the COM frame not at all.
  const auto v = rig.world->get_velocity(0);
  CHECK(v.linear.x == 0.0f);
  CHECK(v.linear.y == 0.0f);
}

TEST_CASE("impulses: immediate, mass-scaled, and no-ops without mass") {
  Rig rig(make_desc(4));
  p2::BodyDesc box{};
  box.shape = p2::Box{Vec2{0.5f, 0.5f}};
  box.density = 3.0f;  // m = 3, I = 0.5
  rig.create(0, box);
  p2::BodyDesc wall{};
  wall.kind = phys::BodyKind::static_;
  wall.shape = p2::Box{Vec2{0.5f, 0.5f}};
  rig.create(1, wall);

  // No step needed: impulses land immediately.
  rig.world->apply_linear_impulse(0, Vec2{3.0f, 0.0f});
  CHECK(rig.world->get_velocity(0).linear.x == doctest::Approx(1.0f));
  rig.world->apply_angular_impulse(0, 0.5f);
  CHECK(rig.world->get_velocity(0).angular == doctest::Approx(1.0f));

  // Statics carry infinite-mass semantics: no effect, no error.
  rig.world->apply_linear_impulse(1, Vec2{3.0f, 0.0f});
  rig.world->apply_angular_impulse(1, 0.5f);
  CHECK(rig.world->get_velocity(1).linear.x == 0.0f);
  CHECK(rig.world->get_velocity(1).angular == 0.0f);
}

TEST_CASE("touch lifecycle: began with geometry, silent while held, zeroed ended") {
  Rig rig(make_desc(4));
  rig.create(0, circle(0.0f, 0.0f, 0.5f));
  rig.create(1, circle(0.9f, 0.0f, 0.5f));  // 0.1 overlap

  const std::size_t moved = rig.step(0.1f);
  CHECK(moved == 2);
  REQUIRE(rig.feed.events.size() == 1);
  const auto& began = rig.feed.events[0];
  CHECK(began.phase == p2::ContactPhase::began);
  CHECK(began.slot_a == 0);
  CHECK(began.slot_b == 1);
  CHECK(began.part_a == 0);
  CHECK(began.part_b == 0);
  // Witness: the midpoint of the overlap segment; normal a -> b.
  CHECK(began.point.x == doctest::Approx(0.45f));
  CHECK(began.point.y == doctest::Approx(0.0f));
  CHECK(began.normal.x == doctest::Approx(1.0f));
  CHECK(began.normal.y == doctest::Approx(0.0f));
  CHECK(began.speed == 0.0f);  // began events carry no approach speed

  rig.step(0.1f);  // held: no solver, no motion, zero gravity -> silent
  CHECK(rig.feed.events.empty());

  rig.world->set_transform(1, p2::Transform{Vec2{3.0f, 0.0f}, 0.0f});
  rig.step(0.1f);
  REQUIRE(rig.feed.events.size() == 1);
  const auto& ended = rig.feed.events[0];
  CHECK(ended.phase == p2::ContactPhase::ended);
  CHECK(ended.slot_a == 0);
  CHECK(ended.slot_b == 1);
  CHECK(ended.point.x == 0.0f);  // ended carries no geometry (Box2D's shape)
  CHECK(ended.point.y == 0.0f);
  CHECK(ended.normal.x == 0.0f);
  CHECK(ended.normal.y == 0.0f);
  CHECK(ended.speed == 0.0f);
}

TEST_CASE("hits: above threshold, on began only, never for sensors") {
  Rig rig(make_desc(8, {}, 1.0f));
  p2::BodyDesc a = circle(-2.0f, 0.0f, 0.5f);
  a.linear_velocity = Vec2{5.0f, 0.0f};
  rig.create(0, a);
  p2::BodyDesc b = circle(2.0f, 0.0f, 0.5f);
  b.linear_velocity = Vec2{-5.0f, 0.0f};
  rig.create(1, b);

  constexpr float dt = 0.1f;
  rig.step(dt);  // -1.5 / 1.5: gap 3.0
  CHECK(rig.feed.events.empty());
  rig.step(dt);  // -1.0 / 1.0: gap 2.0
  CHECK(rig.feed.events.empty());
  rig.step(dt);  // -0.5 / 0.5: exact contact -> began + hit (closing at 10)
  REQUIRE(rig.feed.events.size() == 2);
  CHECK(rig.feed.events[0].phase == p2::ContactPhase::began);
  CHECK(rig.feed.events[1].phase == p2::ContactPhase::hit);
  CHECK(rig.feed.events[1].slot_a == 0);
  CHECK(rig.feed.events[1].slot_b == 1);
  CHECK(rig.feed.events[1].speed == doctest::Approx(10.0f));
  CHECK(rig.feed.events[1].point.x == doctest::Approx(0.0f));
  CHECK(rig.feed.events[1].point.y == doctest::Approx(0.0f));
  CHECK(rig.feed.events[1].normal.x == doctest::Approx(1.0f));

  // No solver: they pass through. Ticks 4-5 stay overlapped (silent);
  // tick 6 reopens the gap -> ended.
  rig.step(dt);
  rig.step(dt);
  CHECK(rig.feed.events.empty());
  rig.step(dt);
  REQUIRE(rig.feed.events.size() == 1);
  CHECK(rig.feed.events[0].phase == p2::ContactPhase::ended);

  // Sensor pairs report began/ended with NO geometry and never a hit,
  // however fast the overlap begins.
  Rig sens(make_desc(8, {}, 0.001f));
  p2::BodyDesc zone{};
  zone.kind = phys::BodyKind::static_;
  zone.is_sensor = true;
  zone.shape = p2::Circle{0.5f};
  sens.create(0, zone);
  p2::BodyDesc fly = circle(-2.0f, 0.0f, 0.05f);
  fly.linear_velocity = Vec2{5.0f, 0.0f};
  sens.create(1, fly);
  sens.step(dt);  // -1.5: 1.0 apart
  sens.step(dt);  // -1.0: 0.5 from the rim... still 0.45 short of contact
  CHECK(sens.feed.events.empty());
  sens.step(dt);  // -0.5: overlapping -> began only, fast enough to hit
  REQUIRE(sens.feed.events.size() == 1);
  CHECK(sens.feed.events[0].phase == p2::ContactPhase::began);
  CHECK(sens.feed.events[0].point.x == 0.0f);
  CHECK(sens.feed.events[0].point.y == 0.0f);
  CHECK(sens.feed.events[0].normal.x == 0.0f);
  CHECK(sens.feed.events[0].normal.y == 0.0f);
  CHECK(sens.feed.events[0].speed == 0.0f);

  // Sensor + sensor never reports at all.
  p2::BodyDesc zone2 = zone;
  zone2.kind = phys::BodyKind::dynamic;
  zone2.transform.position = Vec2{0.2f, 5.0f};
  p2::BodyDesc zone3 = zone;
  zone3.transform.position = Vec2{-0.2f, 5.0f};
  sens.create(2, zone2);
  sens.create(3, zone3);
  sens.step(dt);
  CHECK(sens.feed.events.empty());  // the passing fly is long gone by now
}

TEST_CASE("filters: both directions must accept; queries ignore them") {
  Rig rig(make_desc(4));
  p2::BodyDesc a = circle(0.0f, 0.0f, 0.5f);
  a.collision_layer = 1u;
  a.collision_mask = 0b1110u;  // refuses layer 1
  rig.create(0, a);
  p2::BodyDesc b = circle(0.9f, 0.0f, 0.5f);  // defaults: layer 1, mask ~0
  rig.create(1, b);

  rig.step(0.1f);
  CHECK(rig.feed.events.empty());  // b's layer rejected by a's mask

  rig.world->set_filter(0, 1u, 0xffffffffu);
  rig.step(0.1f);
  REQUIRE(rig.feed.events.size() == 1);
  CHECK(rig.feed.events[0].phase == p2::ContactPhase::began);

  // Queries see all bodies regardless of filters (CONTRACT.md 9).
  const auto hit = rig.world->raycast(Vec2{-5.0f, 0.0f}, Vec2{1.0f, 0.0f},
                                      20.0f);
  REQUIRE(hit.has_value());
  CHECK(hit->slot == 0);  // the filtered body, closest along the ray
}

TEST_CASE("parts: one event per shape pair, part indexes ride the slots") {
  Rig rig(make_desc(4));
  p2::BodyDesc twin{};
  twin.part_count = 2;
  twin.parts[0].shape = p2::Circle{0.5f};
  twin.parts[0].offset = Vec2{-0.5f, 0.0f};
  twin.parts[1].shape = p2::Circle{0.5f};
  twin.parts[1].offset = Vec2{0.5f, 0.0f};
  rig.create(0, twin);
  p2::BodyDesc pin{};
  pin.kind = phys::BodyKind::static_;
  pin.shape = p2::Circle{0.05f};
  rig.create(1, pin);  // at the origin: 0.5 from both part centers

  rig.step(0.1f);
  REQUIRE(rig.feed.events.size() == 2);  // one began per touching shape pair
  CHECK(rig.feed.events[0].phase == p2::ContactPhase::began);
  CHECK(rig.feed.events[0].part_a == 0);
  CHECK(rig.feed.events[0].part_b == 0);
  CHECK(rig.feed.events[0].point.x == doctest::Approx(-0.025f));
  CHECK(rig.feed.events[1].phase == p2::ContactPhase::began);
  CHECK(rig.feed.events[1].part_a == 1);
  CHECK(rig.feed.events[1].part_b == 0);
  CHECK(rig.feed.events[1].point.x == doctest::Approx(0.025f));

  // Sliding off one part keeps that pair alive and ends the other, in the
  // same tick.
  rig.world->set_transform(1, p2::Transform{Vec2{-0.5f, 0.0f}, 0.0f});
  rig.step(0.1f);
  REQUIRE(rig.feed.events.size() == 1);
  CHECK(rig.feed.events[0].phase == p2::ContactPhase::ended);
  CHECK(rig.feed.events[0].part_a == 1);

  rig.world->set_transform(1, p2::Transform{Vec2{3.0f, 0.0f}, 0.0f});
  rig.step(0.1f);
  REQUIRE(rig.feed.events.size() == 1);
  CHECK(rig.feed.events[0].phase == p2::ContactPhase::ended);
  CHECK(rig.feed.events[0].part_a == 0);
}

TEST_CASE("kinds: statics never move, kinematics integrate velocity only") {
  Rig rig(make_desc(8, Vec2{0.0f, -10.0f}));
  p2::BodyDesc wall{};
  wall.kind = phys::BodyKind::static_;
  wall.shape = p2::Box{Vec2{1.0f, 1.0f}};
  rig.create(0, wall);
  p2::BodyDesc cart = circle(0.0f, 0.0f, 0.25f);
  cart.kind = phys::BodyKind::kinematic;
  cart.linear_velocity = Vec2{1.0f, 0.0f};
  rig.create(1, cart);
  rig.create(2, circle(0.0f, 5.0f, 0.25f));

  for (int i = 0; i < 3; ++i) {
    const std::size_t moved = rig.step(0.1f);
    CHECK(moved == 2);  // the kinematic and the dynamic, never the static
  }
  // Kinematics: position-driven - velocity only, no gravity.
  const auto k = rig.world->get_velocity(1);
  CHECK(k.linear.x == doctest::Approx(1.0f));
  CHECK(k.linear.y == 0.0f);
  const std::size_t ki = rig.find_move(1, 2);
  CHECK(rig.moves[ki].transform.position.x == doctest::Approx(0.3f));
  CHECK(rig.moves[ki].transform.position.y == doctest::Approx(0.0f));
  // The dynamic fell the staircase; the static never appears.
  const std::size_t di = rig.find_move(2, 2);
  CHECK(rig.moves[di].transform.position.y == doctest::Approx(5.0f - 0.6f));
  CHECK(rig.moves[0].slot != 0);
  CHECK(rig.moves[1].slot != 0);

  // Nothing sleeps here: is_sleeping is false for every kind, and the
  // sleep controls are harmless no-ops (CONTRACT.md 6 holds vacuously).
  CHECK_FALSE(rig.world->is_sleeping(0));
  CHECK_FALSE(rig.world->is_sleeping(1));
  CHECK_FALSE(rig.world->is_sleeping(2));
  rig.world->set_sleepable(2, false);
  rig.world->wake(2);
  const std::size_t moved = rig.step(0.1f);
  CHECK(moved == 2);
}

TEST_CASE("queries: closest hit, lowest slot on ties, sensors included") {
  Rig rig(make_desc(8));
  p2::BodyDesc wall{};
  wall.kind = phys::BodyKind::static_;
  wall.shape = p2::Box{Vec2{1.0f, 1.0f}};
  rig.create(0, wall);
  p2::BodyDesc twin = wall;  // same pose: a true tie for the ray
  rig.create(1, twin);
  p2::BodyDesc sens{};
  sens.kind = phys::BodyKind::static_;
  sens.is_sensor = true;
  sens.shape = p2::Circle{0.5f};
  sens.transform.position = Vec2{5.0f, 0.0f};
  rig.create(2, sens);

  // Box: entry at the -x face, normal back toward the ray origin.
  const auto hit = rig.world->raycast(Vec2{-5.0f, 0.0f}, Vec2{1.0f, 0.0f},
                                      100.0f);
  REQUIRE(hit.has_value());
  CHECK(hit->slot == 0);  // tie with slot 1: lowest slot wins
  CHECK(hit->t == doctest::Approx(4.0f));
  CHECK(hit->point.x == doctest::Approx(-1.0f));
  CHECK(hit->point.y == doctest::Approx(0.0f));
  CHECK(hit->normal.x == doctest::Approx(-1.0f));
  CHECK(hit->normal.y == doctest::Approx(0.0f));

  // Circle: entry at the rim, radial normal; sensors are visible.
  const auto chit = rig.world->raycast(Vec2{3.0f, 0.0f}, Vec2{1.0f, 0.0f},
                                       10.0f);
  REQUIRE(chit.has_value());
  CHECK(chit->slot == 2);
  CHECK(chit->t == doctest::Approx(1.5f));
  CHECK(chit->point.x == doctest::Approx(4.5f));
  CHECK(chit->normal.x == doctest::Approx(-1.0f));

  // An origin inside a shape is a miss, both shapes (entering rays only).
  // The +x rays also prove the closest-hit rule: a ray from INSIDE the
  // boxes still reaches the sensor at 4.5 (the boxes cannot block it),
  // while the +y ray from inside clears every body entirely.
  CHECK_FALSE(rig.world->raycast(Vec2{5.0f, 0.0f}, Vec2{1.0f, 0.0f}, 10.0f)
                  .has_value());
  CHECK_FALSE(rig.world->raycast(Vec2{0.0f, 0.0f}, Vec2{0.0f, 1.0f}, 10.0f)
                  .has_value());
  const auto through = rig.world->raycast(Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f},
                                          10.0f);
  REQUIRE(through.has_value());  // the sensor, past the boxes it started in
  CHECK(through->slot == 2);
  CHECK(through->t == doctest::Approx(4.5f));

  std::vector<std::uint32_t> out;
  rig.world->overlap_circle(Vec2{0.0f, 0.0f}, 1.0f, out);
  REQUIRE(out.size() == 2);
  CHECK(out[0] == 0);  // slot-ascending (CONTRACT.md 9)
  CHECK(out[1] == 1);
  out.clear();
  rig.world->overlap_circle(Vec2{5.0f, 0.0f}, 1.0f, out);
  REQUIRE(out.size() == 1);
  CHECK(out[0] == 2);  // the sensor

  out.clear();
  rig.world->overlap_aabb(p2::Aabb{Vec2{0.0f, -1.0f}, Vec2{2.0f, 1.0f}}, out);
  REQUIRE(out.size() == 2);
  CHECK(out[0] == 0);
  CHECK(out[1] == 1);
}

TEST_CASE("subset errors: joints, capsules and bad slots fail cleanly") {
  Rig rig(make_desc(2));
  p2::BodyDesc capsule{};
  capsule.shape = p2::Capsule{};
  CHECK(has_code(rig.world->create_body(0, capsule, pd::MassData2D{}),
                 phys::PhysicsError::backend_error));

  p2::BodyDesc ok = circle(0.0f, 0.0f, 0.5f);
  CHECK(rig.world->create_body(0, ok, pd::MassData2D{}).has_value());

  CHECK(has_code(rig.world->create_body(2, ok, pd::MassData2D{}),
                 phys::PhysicsError::backend_error));  // slot >= capacity

  const pd::BackendJoint2D joint{};
  CHECK(has_code(rig.world->create_joint(0, joint),
                 phys::PhysicsError::backend_error));
  rig.world->destroy_joint(0);  // a no-op that must not crash

  // Capacity zero refuses the first body, the seam's defensive guard.
  Rig empty(make_desc(0));
  CHECK(has_code(empty.world->create_body(0, ok, pd::MassData2D{}),
                 phys::PhysicsError::backend_error));
}

TEST_CASE("destroy: pairs die with the body, silently") {
  Rig rig(make_desc(4));
  rig.create(0, circle(0.0f, 0.0f, 0.5f));
  rig.create(1, circle(0.9f, 0.0f, 0.5f));
  rig.step(0.1f);
  REQUIRE(rig.feed.events.size() == 1);
  CHECK(rig.feed.events[0].phase == p2::ContactPhase::began);

  rig.world->destroy_body(1);
  rig.step(0.1f);
  CHECK(rig.feed.events.empty());  // no ended ever names a dead slot
}

TEST_CASE("setters: velocity, transform and gravity take effect next tick") {
  Rig rig(make_desc(4));
  p2::BodyDesc box{};
  box.shape = p2::Box{Vec2{0.5f, 0.5f}};
  rig.create(0, box);  // symmetric: COM == origin

  rig.world->set_velocity(0, Vec2{1.0f, 0.0f}, 0.25f);
  rig.step(0.1f);
  CHECK(rig.moves[0].transform.position.x == doctest::Approx(0.1f));
  CHECK(rig.moves[0].transform.rotation == doctest::Approx(0.025f));

  rig.world->set_transform(0, p2::Transform{Vec2{5.0f, 0.0f}, 0.5f});
  rig.world->set_bullet(0, true);  // stored, never read: must not crash
  rig.step(0.1f);
  CHECK(rig.moves[0].transform.position.x == doctest::Approx(5.1f));
  CHECK(rig.moves[0].transform.rotation == doctest::Approx(0.525f));

  // A gravity change applies from the next tick (nothing to wake here).
  rig.world->set_gravity(Vec2{0.0f, 10.0f});
  rig.step(0.1f);
  CHECK(rig.world->get_velocity(0).linear.y == doctest::Approx(1.0f));
  rig.step(0.1f);
  CHECK(rig.moves[0].transform.position.y == doctest::Approx(0.3f));
}
