// The C7b reference-backend suite, 3D: CONTRACT.md semantics with
// hand-checkable numbers, driven through the RAW seam
// (make_reference_world_3d) rather than World3D - the machinery on top has
// its own suites, and WorldCore<Dim3D> over BOTH backends is
// test_conformance_3d.cpp's job.
//
// Like the 2D twin (test_reference_2d.cpp), every number below is plain
// kinematics the reader can recompute: semi-implicit Euler under gravity is
// the staircase sum v_k = a dt k, x_k = a dt^2 k(k+1)/2, and F = ma /
// tau = I alpha ride the LIB's mass numbers (mass_data_3d), which is also
// what these tests pin end to end - the backend integrates exactly the
// mass data it is handed.
//
// Rotation is where 3D differs from 2D's scalar angle: the twin steps the
// Euler quaternion derivative dq = dt/2 (omega (x) q) and renormalizes, so
// the one-tick closed forms are exact and pinned as such - a fresh spin
// about a principal axis lands at angle 2 atan(|omega| dt / 2), and a
// z-only spin composes (z, w) += (c w, -c z) with c = omega_z dt / 2 before
// renormalizing (any accumulated z-rotation stays a z-rotation). Jolt
// composes the exact axis-angle exponential map instead; that divergence is
// the conformance suite's banded business, these tests pin the reference's
// own arithmetic. The subset's refusals (joints, capsules) and its vacuums
// (nothing ever sleeps, pairs pass through) are pinned as such.

// No DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN - test_common.cpp owns doctest's
// main (one binary, several TUs).

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "check.hpp"
#include "core/backend3d.hpp"
#include "core/mass_properties.hpp"

#include <ws/math/vecmath.hpp>
#include <ws/physics/3d/body.hpp>
#include <ws/physics/3d/shape.hpp>
#include <ws/physics/common.hpp>
#include <ws/physics/errors.hpp>

namespace {

namespace phys = ws::physics;
namespace pd = ws::physics::detail;
namespace p3 = ws::physics3d;
using ws::math::Quat;
using ws::math::Vec3;
using wsphys::test::has_code;

// Collects the raw feed verbatim, in feed order.
struct Feed final : pd::IEventCollector3D {
  std::vector<pd::BackendEvent3D> events;
  void add(const pd::BackendEvent3D& e) override { events.push_back(e); }
};

// A reference world plus the scratch the seam needs (the moves span, the
// feed). Zero gravity unless a test asks: with no solver and no sleep,
// gravity only ever adds ballistics.
struct Rig {
  std::unique_ptr<pd::IWorld3DBackend> world;
  std::vector<pd::BackendMove3D> moves;
  Feed feed;

  explicit Rig(const pd::BackendDesc3D& desc)
      : world(pd::make_reference_world_3d(desc)) {
    REQUIRE(world != nullptr);
    moves.resize(desc.max_bodies);
  }

  std::size_t step(float dt) {
    feed.events.clear();
    return world->step(dt, feed, std::span<pd::BackendMove3D>(moves));
  }

  // Creates through the seam the way WorldCore does: the lib's mass data
  // for dynamics, zeros for statics/kinematics (their mass semantics are
  // none, CONTRACT.md 4).
  void create(std::uint32_t slot, const p3::BodyDesc& d) {
    pd::MassData3D mass{};
    if (d.kind == phys::BodyKind::dynamic) {
      mass = pd::mass_data_3d(d);
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

pd::BackendDesc3D make_desc(std::uint32_t max_bodies, Vec3 gravity = {},
                            float hit_threshold = 0.0f) {
  pd::BackendDesc3D d{};
  d.max_bodies = max_bodies;
  d.gravity = gravity;
  d.hit_speed_threshold = hit_threshold;
  return d;
}

// A dynamic sphere of radius r centered at p.
p3::BodyDesc sphere(Vec3 p, float r) {
  p3::BodyDesc d{};
  d.shape = p3::Sphere{r};
  d.transform.position = p;
  return d;
}

// The twin's exact Euler quaternion tick under a z-only spin: c = omega_z
// dt / 2, (z, w) += (c w, -c z), renormalize. From the identity that is a
// z-rotation of angle 2 atan(c); applied to an accumulated z-rotation it
// stays one (the closed forms the spin pins below are exactly this).
Quat euler_z_tick(const Quat& q, float omega_z, float dt) {
  const float c = omega_z * 0.5f * dt;
  return ws::math::normalize(
      Quat{q.x, q.y, q.z + c * q.w, q.w - c * q.z});
}

}  // namespace

TEST_CASE("ballistics: semi-implicit Euler under gravity and gravity_scale") {
  Rig rig(make_desc(4, Vec3{0.0f, -10.0f, 0.0f}));
  rig.create(0, sphere(Vec3{}, 0.5f));
  p3::BodyDesc scaled = sphere(Vec3{}, 0.5f);
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
  CHECK(va.angular == Vec3{});
  CHECK(rig.moves[0].transform.position.y == doctest::Approx(-0.6f));
  CHECK(rig.moves[0].transform.position.x == doctest::Approx(0.0f));
  // No spin: the rotation is untouched, exactly identity.
  CHECK(rig.moves[0].transform.rotation.x == 0.0f);
  CHECK(rig.moves[0].transform.rotation.z == 0.0f);
  CHECK(rig.moves[0].transform.rotation.w == 1.0f);
  // gravity_scale multiplies g and nothing else.
  const auto vb = rig.world->get_velocity(1);
  CHECK(vb.linear.y == doctest::Approx(-6.0f));
  CHECK(rig.moves[1].transform.position.y == doctest::Approx(-1.2f));
}

TEST_CASE("dynamics: forces and torques drive the lib's mass numbers") {
  Rig rig(make_desc(4));
  p3::BodyDesc box{};
  box.shape = p3::Box{Vec3{0.5f, 0.5f, 0.5f}};  // 1x1x1 at density 3
  box.density = 3.0f;
  rig.create(0, box);

  // The backend must integrate exactly the LIB's mass data: m = 3, and a
  // unit cube's inertia about the COM is m/12 (a^2 + a^2) = 0.5 on every
  // axis (column-major: element [c][r]).
  const pd::MassData3D mass = pd::mass_data_3d(box);
  CHECK(mass.mass == doctest::Approx(3.0f));
  CHECK(mass.inertia[0][0] == doctest::Approx(0.5f));
  CHECK(mass.inertia[1][1] == doctest::Approx(0.5f));
  CHECK(mass.inertia[2][2] == doctest::Approx(0.5f));
  CHECK(mass.center == Vec3{});

  constexpr float dt = 0.1f;
  rig.world->apply_force(0, Vec3{3.0f, 0.0f, 0.0f});  // a = F/m = 1
  rig.world->apply_torque(0, Vec3{0.0f, 0.5f, 0.0f});  // alpha = tau/I = 1
  rig.step(dt);
  rig.world->apply_force(0, Vec3{3.0f, 0.0f, 0.0f});  // accumulate per tick
  rig.world->apply_torque(0, Vec3{0.0f, 0.5f, 0.0f});
  rig.step(dt);
  const auto v = rig.world->get_velocity(0);
  CHECK(v.linear.x == doctest::Approx(0.2f));
  CHECK(v.angular.y == doctest::Approx(0.2f));
  CHECK(rig.moves[0].transform.position.x == doctest::Approx(0.03f));

  // No re-application: the accumulation cleared at each tick end.
  rig.step(dt);
  const auto held = rig.world->get_velocity(0);
  CHECK(held.linear.x == doctest::Approx(0.2f));
  CHECK(held.angular.y == doctest::Approx(0.2f));
  CHECK(rig.moves[0].transform.position.x == doctest::Approx(0.05f));

  // Forces never move a kinematic (no mass semantics, CONTRACT.md 4).
  p3::BodyDesc kin{};
  kin.kind = phys::BodyKind::kinematic;
  kin.shape = p3::Box{Vec3{0.5f, 0.5f, 0.5f}};
  rig.create(1, kin);
  rig.world->apply_force(1, Vec3{100.0f, 0.0f, 0.0f});
  rig.world->apply_torque(1, Vec3{0.0f, 100.0f, 0.0f});
  rig.step(dt);
  CHECK(rig.world->get_velocity(1).linear == Vec3{});
  CHECK(rig.world->get_velocity(1).angular == Vec3{});
}

TEST_CASE("composite COM: the origin reconstructs from COM-frame motion") {
  Rig rig(make_desc(4));
  p3::BodyDesc body{};
  body.part_count = 1;
  body.parts[0].shape = p3::Box{Vec3{0.5f, 0.5f, 0.5f}};
  body.parts[0].offset = Vec3{1.0f, 0.0f, 0.0f};  // COM at (1, 0, 0), body-local
  body.angular_velocity = Vec3{0.0f, 0.0f, 1.0f};   // spin about the COM
  rig.create(0, body);

  const pd::MassData3D mass = pd::mass_data_3d(body);
  CHECK(mass.center.x == doctest::Approx(1.0f));
  CHECK(mass.center.y == doctest::Approx(0.0f));
  CHECK(mass.center.z == doctest::Approx(0.0f));

  rig.step(0.1f);  // zero gravity: the COM holds still, only the angle turns
  // One Euler tick from the identity under the z spin (|omega| dt / 2 = 0.05).
  const Quat q = euler_z_tick(Quat{}, 1.0f, 0.1f);
  CHECK(rig.moves[0].transform.rotation.x == 0.0f);
  CHECK(rig.moves[0].transform.rotation.y == 0.0f);
  CHECK(rig.moves[0].transform.rotation.z == doctest::Approx(q.z));
  CHECK(rig.moves[0].transform.rotation.w == doctest::Approx(q.w));
  // origin = com_world - R(theta) com_local with com_world = (1, 0, 0) and
  // theta = 2 atan(0.05): the origin traces the unit circle about (1, 0).
  const float theta = 2.0f * std::atan(0.05f);
  CHECK(rig.moves[0].transform.position.x ==
        doctest::Approx(1.0f - std::cos(theta)));
  CHECK(rig.moves[0].transform.position.y == doctest::Approx(-std::sin(theta)));
  CHECK(rig.moves[0].transform.position.z == doctest::Approx(0.0f));
  // Spinning about the COM drifts the COM frame not at all.
  const auto v = rig.world->get_velocity(0);
  CHECK(v.linear == Vec3{});
}

TEST_CASE("impulses: immediate, mass-scaled, and no-ops without mass") {
  Rig rig(make_desc(4));
  p3::BodyDesc box{};
  box.shape = p3::Box{Vec3{0.5f, 0.5f, 0.5f}};
  box.density = 3.0f;  // m = 3, I = 0.5 (unit cube)
  rig.create(0, box);
  p3::BodyDesc wall{};
  wall.kind = phys::BodyKind::static_;
  wall.shape = p3::Box{Vec3{0.5f, 0.5f, 0.5f}};
  rig.create(1, wall);

  // No step needed: impulses land immediately.
  rig.world->apply_linear_impulse(0, Vec3{3.0f, 0.0f, 0.0f});
  CHECK(rig.world->get_velocity(0).linear.x == doctest::Approx(1.0f));
  rig.world->apply_angular_impulse(0, Vec3{0.0f, 0.5f, 0.0f});
  CHECK(rig.world->get_velocity(0).angular.y == doctest::Approx(1.0f));

  // Statics carry infinite-mass semantics: no effect, no error.
  rig.world->apply_linear_impulse(1, Vec3{3.0f, 0.0f, 0.0f});
  rig.world->apply_angular_impulse(1, Vec3{0.0f, 0.5f, 0.0f});
  CHECK(rig.world->get_velocity(1).linear == Vec3{});
  CHECK(rig.world->get_velocity(1).angular == Vec3{});
}

TEST_CASE("touch lifecycle: began with geometry, silent while held, zeroed ended") {
  Rig rig(make_desc(4));
  rig.create(0, sphere(Vec3{}, 0.5f));
  rig.create(1, sphere(Vec3{0.9f, 0.0f, 0.0f}, 0.5f));  // 0.1 overlap

  const std::size_t moved = rig.step(0.1f);
  CHECK(moved == 2);
  REQUIRE(rig.feed.events.size() == 1);
  const auto& began = rig.feed.events[0];
  CHECK(began.phase == p3::ContactPhase::began);
  CHECK(began.slot_a == 0);
  CHECK(began.slot_b == 1);
  CHECK(began.part_a == 0);
  CHECK(began.part_b == 0);
  // Witness: the midpoint of the overlap segment; normal a -> b.
  CHECK(began.point.x == doctest::Approx(0.45f));
  CHECK(began.point.y == doctest::Approx(0.0f));
  CHECK(began.point.z == doctest::Approx(0.0f));
  CHECK(began.normal.x == doctest::Approx(1.0f));
  CHECK(began.normal.y == doctest::Approx(0.0f));
  CHECK(began.normal.z == doctest::Approx(0.0f));
  CHECK(began.speed == 0.0f);  // began events carry no approach speed

  rig.step(0.1f);  // held: no solver, no motion, zero gravity -> silent
  CHECK(rig.feed.events.empty());

  rig.world->set_transform(1, p3::Transform{Vec3{3.0f, 0.0f, 0.0f}, Quat{}});
  rig.step(0.1f);
  REQUIRE(rig.feed.events.size() == 1);
  const auto& ended = rig.feed.events[0];
  CHECK(ended.phase == p3::ContactPhase::ended);
  CHECK(ended.slot_a == 0);
  CHECK(ended.slot_b == 1);
  CHECK(ended.point == Vec3{});  // ended carries no geometry (Box2D's shape)
  CHECK(ended.normal == Vec3{});
  CHECK(ended.speed == 0.0f);
}

TEST_CASE("hits: above threshold, on began only, never for sensors") {
  Rig rig(make_desc(8, {}, 1.0f));
  p3::BodyDesc a = sphere(Vec3{-2.0f, 0.0f, 0.0f}, 0.5f);
  a.linear_velocity = Vec3{5.0f, 0.0f, 0.0f};
  rig.create(0, a);
  p3::BodyDesc b = sphere(Vec3{2.0f, 0.0f, 0.0f}, 0.5f);
  b.linear_velocity = Vec3{-5.0f, 0.0f, 0.0f};
  rig.create(1, b);

  constexpr float dt = 0.1f;
  rig.step(dt);  // -1.5 / 1.5: gap 3.0
  CHECK(rig.feed.events.empty());
  rig.step(dt);  // -1.0 / 1.0: gap 2.0
  CHECK(rig.feed.events.empty());
  rig.step(dt);  // -0.5 / 0.5: exact contact -> began + hit (closing at 10)
  REQUIRE(rig.feed.events.size() == 2);
  CHECK(rig.feed.events[0].phase == p3::ContactPhase::began);
  CHECK(rig.feed.events[1].phase == p3::ContactPhase::hit);
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
  CHECK(rig.feed.events[0].phase == p3::ContactPhase::ended);

  // Sensor pairs report began/ended with NO geometry and never a hit,
  // however fast the overlap begins.
  Rig sens(make_desc(8, {}, 0.001f));
  p3::BodyDesc zone{};
  zone.kind = phys::BodyKind::static_;
  zone.is_sensor = true;
  zone.shape = p3::Sphere{0.5f};
  sens.create(0, zone);
  p3::BodyDesc fly = sphere(Vec3{-2.0f, 0.0f, 0.0f}, 0.05f);
  fly.linear_velocity = Vec3{5.0f, 0.0f, 0.0f};
  sens.create(1, fly);
  sens.step(dt);  // -1.5: 1.0 apart
  sens.step(dt);  // -1.0: 0.5 from the rim... still 0.45 short of contact
  CHECK(sens.feed.events.empty());
  sens.step(dt);  // -0.5: overlapping -> began only, fast enough to hit
  REQUIRE(sens.feed.events.size() == 1);
  CHECK(sens.feed.events[0].phase == p3::ContactPhase::began);
  CHECK(sens.feed.events[0].point == Vec3{});
  CHECK(sens.feed.events[0].normal == Vec3{});
  CHECK(sens.feed.events[0].speed == 0.0f);

  // Sensor + sensor never reports at all.
  p3::BodyDesc zone2 = zone;
  zone2.kind = phys::BodyKind::dynamic;
  zone2.transform.position = Vec3{0.2f, 5.0f, 0.0f};
  p3::BodyDesc zone3 = zone;
  zone3.transform.position = Vec3{-0.2f, 5.0f, 0.0f};
  sens.create(2, zone2);
  sens.create(3, zone3);
  sens.step(dt);
  CHECK(sens.feed.events.empty());  // the passing fly is long gone by now
}

TEST_CASE("filters: both directions must accept; queries ignore them") {
  Rig rig(make_desc(4));
  p3::BodyDesc a = sphere(Vec3{}, 0.5f);
  a.collision_layer = 1u;
  a.collision_mask = 0b1110u;  // refuses layer 1
  rig.create(0, a);
  p3::BodyDesc b = sphere(Vec3{0.9f, 0.0f, 0.0f}, 0.5f);  // defaults: layer 1, mask ~0
  rig.create(1, b);

  rig.step(0.1f);
  CHECK(rig.feed.events.empty());  // b's layer rejected by a's mask

  rig.world->set_filter(0, 1u, 0xffffffffu);
  rig.step(0.1f);
  REQUIRE(rig.feed.events.size() == 1);
  CHECK(rig.feed.events[0].phase == p3::ContactPhase::began);

  // Queries see all bodies regardless of filters (CONTRACT.md 9).
  const auto hit = rig.world->raycast(Vec3{-5.0f, 0.0f, 0.0f},
                                      Vec3{1.0f, 0.0f, 0.0f}, 20.0f);
  REQUIRE(hit.has_value());
  CHECK(hit->slot == 0);  // the filtered body, closest along the ray
}

TEST_CASE("parts: one event per shape pair, part indexes ride the slots") {
  Rig rig(make_desc(4));
  p3::BodyDesc twin{};
  twin.part_count = 2;
  twin.parts[0].shape = p3::Sphere{0.5f};
  twin.parts[0].offset = Vec3{-0.5f, 0.0f, 0.0f};
  twin.parts[1].shape = p3::Sphere{0.5f};
  twin.parts[1].offset = Vec3{0.5f, 0.0f, 0.0f};
  rig.create(0, twin);
  p3::BodyDesc pin{};
  pin.kind = phys::BodyKind::static_;
  pin.shape = p3::Sphere{0.05f};
  rig.create(1, pin);  // at the origin: 0.5 from both part centers

  rig.step(0.1f);
  REQUIRE(rig.feed.events.size() == 2);  // one began per touching shape pair
  CHECK(rig.feed.events[0].phase == p3::ContactPhase::began);
  CHECK(rig.feed.events[0].part_a == 0);
  CHECK(rig.feed.events[0].part_b == 0);
  CHECK(rig.feed.events[0].point.x == doctest::Approx(-0.025f));
  CHECK(rig.feed.events[1].phase == p3::ContactPhase::began);
  CHECK(rig.feed.events[1].part_a == 1);
  CHECK(rig.feed.events[1].part_b == 0);
  CHECK(rig.feed.events[1].point.x == doctest::Approx(0.025f));

  // Sliding off one part keeps that pair alive and ends the other, in the
  // same tick.
  rig.world->set_transform(1, p3::Transform{Vec3{-0.5f, 0.0f, 0.0f}, Quat{}});
  rig.step(0.1f);
  REQUIRE(rig.feed.events.size() == 1);
  CHECK(rig.feed.events[0].phase == p3::ContactPhase::ended);
  CHECK(rig.feed.events[0].part_a == 1);

  rig.world->set_transform(1, p3::Transform{Vec3{3.0f, 0.0f, 0.0f}, Quat{}});
  rig.step(0.1f);
  REQUIRE(rig.feed.events.size() == 1);
  CHECK(rig.feed.events[0].phase == p3::ContactPhase::ended);
  CHECK(rig.feed.events[0].part_a == 0);
}

TEST_CASE("kinds: statics never move, kinematics integrate velocity only") {
  Rig rig(make_desc(8, Vec3{0.0f, -10.0f, 0.0f}));
  p3::BodyDesc wall{};
  wall.kind = phys::BodyKind::static_;
  wall.shape = p3::Box{Vec3{1.0f, 1.0f, 1.0f}};
  rig.create(0, wall);
  p3::BodyDesc cart = sphere(Vec3{}, 0.25f);
  cart.kind = phys::BodyKind::kinematic;
  cart.linear_velocity = Vec3{1.0f, 0.0f, 0.0f};
  cart.angular_velocity = Vec3{0.0f, 0.0f, 1.0f};
  rig.create(1, cart);
  rig.create(2, sphere(Vec3{0.0f, 5.0f, 0.0f}, 0.25f));

  // Tick 1: the kinematic slides and turns by the one-tick closed form;
  // gravity never touches it (position-driven, CONTRACT.md 4).
  std::size_t moved = rig.step(0.1f);
  CHECK(moved == 2);  // the kinematic and the dynamic, never the static
  const std::size_t ki = rig.find_move(1, 2);
  CHECK(rig.moves[ki].transform.position.x == doctest::Approx(0.1f));
  CHECK(rig.moves[ki].transform.position.y == doctest::Approx(0.0f));
  const Quat spin1 = euler_z_tick(Quat{}, 1.0f, 0.1f);
  CHECK(rig.moves[ki].transform.rotation.z == doctest::Approx(spin1.z));
  CHECK(rig.moves[ki].transform.rotation.w == doctest::Approx(spin1.w));

  for (int i = 0; i < 2; ++i) {
    moved = rig.step(0.1f);
    CHECK(moved == 2);
  }
  // Kinematics: velocity only - linear speed held, spin held, no gravity.
  const auto k = rig.world->get_velocity(1);
  CHECK(k.linear == Vec3{1.0f, 0.0f, 0.0f});
  CHECK(k.angular == Vec3{0.0f, 0.0f, 1.0f});
  CHECK(rig.moves[ki].transform.position.x == doctest::Approx(0.3f));
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
  moved = rig.step(0.1f);
  CHECK(moved == 2);
}

TEST_CASE("queries: closest hit, lowest slot on ties, sensors included") {
  Rig rig(make_desc(8));
  p3::BodyDesc wall{};
  wall.kind = phys::BodyKind::static_;
  wall.shape = p3::Box{Vec3{1.0f, 1.0f, 1.0f}};
  rig.create(0, wall);
  p3::BodyDesc twin = wall;  // same pose: a true tie for the ray
  rig.create(1, twin);
  p3::BodyDesc sens{};
  sens.kind = phys::BodyKind::static_;
  sens.is_sensor = true;
  sens.shape = p3::Sphere{0.5f};
  sens.transform.position = Vec3{5.0f, 0.0f, 0.0f};
  rig.create(2, sens);

  // Box: entry at the -x face, normal back toward the ray origin.
  const auto hit = rig.world->raycast(Vec3{-5.0f, 0.0f, 0.0f},
                                      Vec3{1.0f, 0.0f, 0.0f}, 100.0f);
  REQUIRE(hit.has_value());
  CHECK(hit->slot == 0);  // tie with slot 1: lowest slot wins
  CHECK(hit->t == doctest::Approx(4.0f));
  CHECK(hit->point.x == doctest::Approx(-1.0f));
  CHECK(hit->point.y == doctest::Approx(0.0f));
  CHECK(hit->normal.x == doctest::Approx(-1.0f));
  CHECK(hit->normal.y == doctest::Approx(0.0f));

  // Sphere: entry at the rim, radial normal; sensors are visible.
  const auto chit = rig.world->raycast(Vec3{3.0f, 0.0f, 0.0f},
                                       Vec3{1.0f, 0.0f, 0.0f}, 10.0f);
  REQUIRE(chit.has_value());
  CHECK(chit->slot == 2);
  CHECK(chit->t == doctest::Approx(1.5f));
  CHECK(chit->point.x == doctest::Approx(4.5f));
  CHECK(chit->normal.x == doctest::Approx(-1.0f));

  // An origin inside a shape is a miss, both shapes (entering rays only).
  // The +x rays also prove the closest-hit rule: a ray from INSIDE the
  // boxes still reaches the sensor at 4.5 (the boxes cannot block it),
  // while the +y ray from inside clears every body entirely.
  CHECK_FALSE(rig.world->raycast(Vec3{5.0f, 0.0f, 0.0f},
                                Vec3{1.0f, 0.0f, 0.0f}, 10.0f)
                  .has_value());
  CHECK_FALSE(rig.world->raycast(Vec3{0.0f, 0.0f, 0.0f},
                                Vec3{0.0f, 1.0f, 0.0f}, 10.0f)
                  .has_value());
  const auto through = rig.world->raycast(Vec3{0.0f, 0.0f, 0.0f},
                                          Vec3{1.0f, 0.0f, 0.0f}, 10.0f);
  REQUIRE(through.has_value());  // the sensor, past the boxes it started in
  CHECK(through->slot == 2);
  CHECK(through->t == doctest::Approx(4.5f));

  std::vector<std::uint32_t> out;
  rig.world->overlap_sphere(Vec3{}, 1.0f, out);
  REQUIRE(out.size() == 2);
  CHECK(out[0] == 0);  // slot-ascending (CONTRACT.md 9)
  CHECK(out[1] == 1);
  out.clear();
  rig.world->overlap_sphere(Vec3{5.0f, 0.0f, 0.0f}, 1.0f, out);
  REQUIRE(out.size() == 1);
  CHECK(out[0] == 2);  // the sensor

  out.clear();
  rig.world->overlap_aabb(p3::Aabb{Vec3{0.0f, -1.0f, -1.0f},
                                   Vec3{2.0f, 1.0f, 1.0f}}, out);
  REQUIRE(out.size() == 2);
  CHECK(out[0] == 0);
  CHECK(out[1] == 1);
}

TEST_CASE("subset errors: joints, capsules and bad slots fail cleanly") {
  Rig rig(make_desc(2));
  p3::BodyDesc capsule{};
  capsule.shape = p3::Capsule{};
  CHECK(has_code(rig.world->create_body(0, capsule, pd::MassData3D{}),
                 phys::PhysicsError::backend_error));

  p3::BodyDesc ok = sphere(Vec3{}, 0.5f);
  CHECK(rig.world->create_body(0, ok, pd::MassData3D{}).has_value());

  CHECK(has_code(rig.world->create_body(2, ok, pd::MassData3D{}),
                 phys::PhysicsError::backend_error));  // slot >= capacity

  const pd::BackendJoint3D joint{};
  CHECK(has_code(rig.world->create_joint(0, joint),
                 phys::PhysicsError::backend_error));
  rig.world->destroy_joint(0);  // a no-op that must not crash

  // Capacity zero refuses the first body, the seam's defensive guard.
  Rig empty(make_desc(0));
  CHECK(has_code(empty.world->create_body(0, ok, pd::MassData3D{}),
                 phys::PhysicsError::backend_error));
}

TEST_CASE("destroy: pairs die with the body, silently") {
  Rig rig(make_desc(4));
  rig.create(0, sphere(Vec3{}, 0.5f));
  rig.create(1, sphere(Vec3{0.9f, 0.0f, 0.0f}, 0.5f));
  rig.step(0.1f);
  REQUIRE(rig.feed.events.size() == 1);
  CHECK(rig.feed.events[0].phase == p3::ContactPhase::began);

  rig.world->destroy_body(1);
  rig.step(0.1f);
  CHECK(rig.feed.events.empty());  // no ended ever names a dead slot
}

TEST_CASE("setters: velocity, transform and gravity take effect next tick") {
  Rig rig(make_desc(4));
  p3::BodyDesc box{};
  box.shape = p3::Box{Vec3{0.5f, 0.5f, 0.5f}};
  rig.create(0, box);  // symmetric: COM == origin

  rig.world->set_velocity(0, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.25f});
  rig.step(0.1f);
  CHECK(rig.moves[0].transform.position.x == doctest::Approx(0.1f));
  // One Euler tick under the z spin, c = omega_z dt / 2 = 0.0125 (angle
  // 2 atan(0.0125) - the 2D suite's 0.025 rad is its small-angle limit).
  const Quat q1 = euler_z_tick(Quat{}, 0.25f, 0.1f);
  CHECK(rig.moves[0].transform.rotation.z == doctest::Approx(q1.z));
  CHECK(rig.moves[0].transform.rotation.w == doctest::Approx(q1.w));

  // A teleported pose integrates from what it landed on, rotation included.
  const Quat q0 = ws::math::normalize(
      Quat{0.0f, 0.0f, std::sin(0.25f), std::cos(0.25f)});  // 0.5 rad about z
  rig.world->set_transform(0, p3::Transform{Vec3{5.0f, 0.0f, 0.0f}, q0});
  rig.world->set_bullet(0, true);  // stored, never read: must not crash
  rig.step(0.1f);
  CHECK(rig.moves[0].transform.position.x == doctest::Approx(5.1f));
  const Quat q2 = euler_z_tick(q0, 0.25f, 0.1f);
  CHECK(rig.moves[0].transform.rotation.z == doctest::Approx(q2.z));
  CHECK(rig.moves[0].transform.rotation.w == doctest::Approx(q2.w));

  // A gravity change applies from the next tick (nothing to wake here).
  rig.world->set_gravity(Vec3{0.0f, 10.0f, 0.0f});
  rig.step(0.1f);
  CHECK(rig.world->get_velocity(0).linear.y == doctest::Approx(1.0f));
  rig.step(0.1f);
  CHECK(rig.moves[0].transform.position.y == doctest::Approx(0.3f));
}
