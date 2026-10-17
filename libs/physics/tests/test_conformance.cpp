// The C7 conformance suite: CONTRACT.md's "must hold on EVERY backend"
// clauses, driven through the real machinery - one WorldCore<Dim2D> per
// case, run over BOTH 2D backends (Box2D and the reference) in this binary.
// The Dim2D traits live in a header (src/core/dim_2d.hpp) exactly so this
// TU can instantiate the same machinery World2D runs; create_body below
// takes PRE-VALIDATED descs (WorldCore bypasses the dimension layer's
// validation), so every desc is well-formed by construction.
//
// Scope, per CONTRACT.md's preamble: only cross-backend clauses are
// encoded. Sleep onset, solver response, material mixing and joints are
// engine compute - their suites stay per-engine (test_world_2d_box2d.cpp,
// test_reference_2d.cpp). First-touch TICK numbers are not asserted
// either: Box2D's 4-substep position staircase trails the reference's
// semi-implicit Euler by a dt^2-scale gap, which shifts the first touch
// by ~a tick - touching cases loop "step until the edge appears" and pin
// the edge itself. The numbers (a = acceleration, N ticks, dt):
//   reference: x = a dt^2 N(N+1)/2,  v = a dt N
//   Box2D:     x = a dt^2 (N(N-1)/2 + 5N/8)
// so the cross-engine position gap is a dt^2 3N/8 while velocity is
// substep-exact in BOTH - velocity checks run tight, position checks run
// banded (CONTRACT.md 10: cross-engine never exact floats).
// The reference is an honest subset: solid pairs pass through (no
// solver), so held-phase behavior legitimately diverges - touching cases
// assert the edges (once-ness, geometry), never rest behavior.

// No DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN - test_common.cpp owns doctest's
// main (one binary, several TUs).

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "check.hpp"
#include "core/backend2d.hpp"
#include "core/dim_2d.hpp"
#include "core/world_core.hpp"

#include <ws/math/vecmath.hpp>
#include <ws/physics/2d/body.hpp>
#include <ws/physics/2d/contact.hpp>
#include <ws/physics/2d/shape.hpp>
#include <ws/physics/common.hpp>
#include <ws/physics/errors.hpp>

namespace {

namespace phys = ws::physics;
namespace pd = ws::physics::detail;
namespace p2 = ws::physics2d;
using ws::math::Vec2;
using wsphys::test::has_code;

using Core = pd::WorldCore<pd::Dim2D>;

// The two backends every clause must hold on.
enum class Backend { box2d, reference };

// std::string, not const char*: doctest's INFO streams a char* as a
// pointer, which would make every failure report an address.
[[nodiscard]] std::string backend_name(Backend k) {
  return k == Backend::box2d ? "box2d" : "reference";
}

// Which phases a body pair produced in THIS call, plus the first event of
// each kind (for the geometry assertions).
struct PairEvents {
  int began = 0;
  int hit = 0;
  int ended = 0;
  std::optional<p2::ContactEvent> first_began;
  std::optional<p2::ContactEvent> first_hit;
  std::optional<p2::ContactEvent> first_ended;
};

[[nodiscard]] PairEvents scan_pair(const Core& core, p2::BodyId a,
                                   p2::BodyId b) {
  PairEvents out;
  for (const p2::ContactEvent& e : core.contact_events()) {
    if (!((e.a == a && e.b == b) || (e.a == b && e.b == a))) {
      continue;
    }
    if (e.phase == p2::ContactPhase::began) {
      ++out.began;
      if (!out.first_began) out.first_began = e;
    } else if (e.phase == p2::ContactPhase::hit) {
      ++out.hit;
      if (!out.first_hit) out.first_hit = e;
    } else {
      ++out.ended;
      if (!out.first_ended) out.first_ended = e;
    }
  }
  return out;
}

// One machinery instance over one backend. Defaults: fixed_dt 1/10 s,
// zero gravity (ballistic cases state their own), hit threshold 0.
struct Rig {
  std::unique_ptr<Core> core;
  Backend which;

  explicit Rig(Backend k, Vec2 gravity = {}, float fixed_dt = 0.1f,
               float hit_threshold = 0.0f)
      : which(k) {
    pd::CoreDesc cd{};
    cd.fixed_dt = fixed_dt;
    cd.max_bodies = 32;
    cd.max_joints = 4;
    cd.max_contact_events = 64;
    cd.thread_count = 1;
    cd.hit_speed_threshold = hit_threshold;
    // BackendDesc2D aggregate order: gravity, thread_count,
    // hit_speed_threshold, max_bodies, max_joints.
    auto backend =
        k == Backend::box2d
            ? pd::make_box2d_world_2d(pd::BackendDesc2D{
                  gravity, 1u, hit_threshold, 32u, 4u})
            : pd::make_reference_world_2d(pd::BackendDesc2D{
                  gravity, 1u, hit_threshold, 32u, 4u});
    REQUIRE(backend != nullptr);
    core = std::make_unique<Core>(cd, std::move(backend));
  }

  void step() { REQUIRE(core->step().has_value()); }
};

// Desc factories: every field the dimension layer would validate is
// well-formed by construction (finite, density > 0, friction >= 0).
[[nodiscard]] p2::BodyDesc dynamic_circle(Vec2 pos, float radius = 0.5f) {
  p2::BodyDesc d{};
  d.shape = p2::Circle{radius};
  d.transform.position = pos;
  return d;
}

[[nodiscard]] p2::BodyDesc dynamic_box(Vec2 pos, Vec2 half, float density) {
  p2::BodyDesc d{};
  d.shape = p2::Box{half};
  d.transform.position = pos;
  d.density = density;
  return d;
}

[[nodiscard]] p2::BodyDesc static_box(Vec2 pos,
                                      Vec2 half = Vec2{10.0f, 0.5f}) {
  p2::BodyDesc d{};
  d.kind = phys::BodyKind::static_;
  d.shape = p2::Box{half};
  d.transform.position = pos;
  return d;
}

[[nodiscard]] p2::BodyId create(Rig& rig, const p2::BodyDesc& d) {
  const auto id = rig.core->create_body(d);
  REQUIRE(id.has_value());
  return *id;
}

// Steps until a began for the pair appears (or the tick budget runs out).
// The began tick itself is engine compute; the budget only has to be
// generous enough for the slower staircase to reach the floor.
struct Drop {
  int began = 0;
  int ticks = 0;
  std::optional<p2::ContactEvent> first_began;
};

[[nodiscard]] Drop step_until_began(Rig& rig, p2::BodyId a, p2::BodyId b,
                                    int max_ticks) {
  Drop out;
  while (out.ticks < max_ticks && out.began == 0) {
    rig.step();
    ++out.ticks;
    const PairEvents ev = scan_pair(*rig.core, a, b);
    out.began += ev.began;
    if (ev.first_began && !out.first_began) {
      out.first_began = ev.first_began;
    }
  }
  return out;
}

// The seam takes a unit direction; call it the way the public wrapper does
// (world2d.cpp normalizes first).
[[nodiscard]] std::optional<pd::BackendRayHit2D> ray(const Core& core,
                                                    Vec2 origin, Vec2 dir,
                                                    float max_distance) {
  return core.backend().raycast(origin, ws::math::normalize(dir),
                                max_distance);
}

TEST_CASE("ballistics: exact velocity, banded position, gravity scale") {
  for (const Backend k : {Backend::box2d, Backend::reference}) {
    INFO("backend " << backend_name(k));
    Rig rig(k, Vec2{0.0f, -10.0f});
    const p2::BodyId plain = create(rig, dynamic_circle(Vec2{0.0f, 0.0f}));
    p2::BodyDesc heavy_desc = dynamic_circle(Vec2{5.0f, 0.0f});
    heavy_desc.gravity_scale = 2.0f;
    const p2::BodyId heavy = create(rig, heavy_desc);
    for (int i = 0; i < 3; ++i) {
      rig.step();
    }
    const auto fall = rig.core->body_state(plain);
    REQUIRE(fall.has_value());
    // v = a dt N: substep-exact in both backends.
    CHECK(fall->linear_velocity.y == doctest::Approx(-3.0f).epsilon(0.001f));
    CHECK(std::abs(fall->linear_velocity.x) < 1e-3f);
    // Euler -0.6 vs Box2D staircase -0.4875: the gap is a dt^2 3N/8
    // = 0.1125, so the band is +-25% of the Euler value.
    CHECK(fall->position.y == doctest::Approx(-0.6f).epsilon(0.25f));
    CHECK(std::abs(fall->position.x) < 1e-4f);
    // The pose mirror (slot-indexed) agrees with the readback.
    CHECK(rig.core->transforms()[plain.index].position.y ==
          doctest::Approx(fall->position.y));
    // Double gravity_scale doubles the fall; same shape of numbers.
    const auto heavy_fall = rig.core->body_state(heavy);
    REQUIRE(heavy_fall.has_value());
    CHECK(heavy_fall->linear_velocity.y ==
          doctest::Approx(-6.0f).epsilon(0.002f));
    CHECK(heavy_fall->position.y == doctest::Approx(-1.2f).epsilon(0.25f));
  }
}

TEST_CASE("F = ma and tau = I alpha ride the lib's mass numbers") {
  for (const Backend k : {Backend::box2d, Backend::reference}) {
    INFO("backend " << backend_name(k));
    Rig rig(k);  // zero gravity: only the applied forces act
    // 1x1 box, density 3 -> the lib's mass data: m = 3, I = 0.5.
    const p2::BodyId b =
        create(rig, dynamic_box(Vec2{0.0f, 0.0f}, Vec2{0.5f, 0.5f}, 3.0f));
    // F = 3 N on m = 3: a = 1 m/s^2. Forces clear at every tick end
    // (CONTRACT.md 7), so the force rides each tick it should.
    for (int i = 0; i < 2; ++i) {
      REQUIRE(rig.core->apply_force(b, Vec2{3.0f, 0.0f}).has_value());
      rig.step();
    }
    const auto pushed = rig.core->body_state(b);
    REQUIRE(pushed.has_value());
    CHECK(pushed->linear_velocity.x == doctest::Approx(0.2f).epsilon(0.01f));
    // Euler x = 0.03 vs Box2D staircase x = 0.0225 (the same 3N/8 gap).
    CHECK(pushed->position.x == doctest::Approx(0.03f).epsilon(0.3f));
    // Clearing: a tick with no apply_force must not grow the velocity.
    rig.step();
    const auto coasting = rig.core->body_state(b);
    REQUIRE(coasting.has_value());
    CHECK(coasting->linear_velocity.x ==
          doctest::Approx(0.2f).epsilon(0.01f));
    // tau = 0.5 N*m on I = 0.5: alpha = 1 rad/s^2, same shape.
    for (int i = 0; i < 2; ++i) {
      REQUIRE(rig.core->apply_torque(b, 0.5f).has_value());
      rig.step();
    }
    const auto spun = rig.core->body_state(b);
    REQUIRE(spun.has_value());
    CHECK(spun->angular_velocity == doctest::Approx(0.2f).epsilon(0.01f));
    CHECK(spun->rotation == doctest::Approx(0.03f).epsilon(0.3f));
    // Statics carry no mass semantics: the same calls no-op (CONTRACT.md 4).
    const p2::BodyId wall =
        create(rig, static_box(Vec2{50.0f, 30.0f}, Vec2{1.0f, 1.0f}));
    REQUIRE(rig.core->apply_force(wall, Vec2{100.0f, 0.0f}).has_value());
    REQUIRE(rig.core->apply_linear_impulse(wall, Vec2{100.0f, 0.0f})
                .has_value());
    rig.step();
    const auto wall_state = rig.core->body_state(wall);
    REQUIRE(wall_state.has_value());
    CHECK(std::abs(wall_state->linear_velocity.x) < 1e-4f);
    CHECK(std::abs(wall_state->position.x - 50.0f) < 1e-4f);
  }
}

TEST_CASE("impulses are immediate velocity changes that survive the tick") {
  for (const Backend k : {Backend::box2d, Backend::reference}) {
    INFO("backend " << backend_name(k));
    Rig rig(k);
    const p2::BodyId b =
        create(rig, dynamic_box(Vec2{0.0f, 0.0f}, Vec2{0.5f, 0.5f}, 3.0f));
    // J = 3 kg*m/s on m = 3: dv = 1, visible BEFORE any step.
    REQUIRE(rig.core->apply_linear_impulse(b, Vec2{3.0f, 0.0f}).has_value());
    const auto kicked = rig.core->body_state(b);
    REQUIRE(kicked.has_value());
    CHECK(kicked->linear_velocity.x == doctest::Approx(1.0f).epsilon(0.001f));
    // An impulse is not a force: the velocity holds and integrates.
    rig.step();
    const auto moved = rig.core->body_state(b);
    REQUIRE(moved.has_value());
    CHECK(moved->linear_velocity.x == doctest::Approx(1.0f).epsilon(0.001f));
    // Constant velocity integrates exactly in both engines (the
    // staircase gap only exists under acceleration).
    CHECK(moved->position.x == doctest::Approx(0.1f).epsilon(0.001f));
    // Angular: J = 0.5 on I = 0.5 -> dw = 1.
    REQUIRE(rig.core->apply_angular_impulse(b, 0.5f).has_value());
    const auto spinning = rig.core->body_state(b);
    REQUIRE(spinning.has_value());
    CHECK(spinning->angular_velocity == doctest::Approx(1.0f).epsilon(0.001f));
    rig.step();
    const auto turned = rig.core->body_state(b);
    REQUIRE(turned.has_value());
    CHECK(turned->rotation == doctest::Approx(0.1f).epsilon(0.001f));
  }
}

TEST_CASE("touching: began exactly once, held without edges, ended exactly once") {
  for (const Backend k : {Backend::box2d, Backend::reference}) {
    INFO("backend " << backend_name(k));
    Rig rig(k, Vec2{0.0f, -10.0f});
    const p2::BodyId floor = create(rig, static_box(Vec2{0.0f, -0.5f}));
    const p2::BodyId ball = create(rig, dynamic_circle(Vec2{0.0f, 3.0f}));

    const Drop drop = step_until_began(rig, floor, ball, 30);
    REQUIRE(drop.began == 1);
    REQUIRE(drop.first_began.has_value());
    // Canonical pair order: a = the lower slot = the floor, normal a -> b.
    CHECK(drop.first_began->a == floor);
    CHECK(drop.first_began->b == ball);
    CHECK(drop.first_began->speed == 0.0f);  // began carries zero speed
    // Witness at the contact under the ball, unit normal floor -> ball.
    CHECK(std::abs(drop.first_began->point.x) < 0.1f);
    CHECK(std::abs(drop.first_began->point.y) < 0.1f);
    CHECK(std::abs(drop.first_began->normal.y - 1.0f) < 0.1f);
    CHECK(std::abs(drop.first_began->normal.x) < 0.1f);

    // Held: never a second began. The reference's pass-through may end
    // the pair here (no solver) - rest behavior is backend compute.
    int began_total = 1;
    int ended_total = 0;
    std::optional<p2::ContactEvent> first_ended;
    for (int i = 0; i < 5; ++i) {
      rig.step();
      const PairEvents held = scan_pair(*rig.core, floor, ball);
      began_total += held.began;
      ended_total += held.ended;
      if (held.first_ended && !first_ended) {
        first_ended = held.first_ended;
      }
    }
    CHECK(began_total == 1);
    // Still touching (Box2D): force the separation, then the ended edge
    // lands - from the engine, or from the touching table's sleep-safe
    // two-tick deadline if the pair had gone to rest.
    if (ended_total == 0) {
      REQUIRE(rig.core
                  ->set_transform(ball, p2::Transform{Vec2{0.0f, 5.0f}, 0.0f})
                  .has_value());
      for (int i = 0; i < 3 && ended_total == 0; ++i) {
        rig.step();
        const PairEvents sep = scan_pair(*rig.core, floor, ball);
        ended_total += sep.ended;
        if (sep.first_ended && !first_ended) {
          first_ended = sep.first_ended;
        }
      }
    }
    REQUIRE(ended_total == 1);
    REQUIRE(first_ended.has_value());
    // Ended feeds zero geometry in both backends.
    CHECK(first_ended->point.x == 0.0f);
    CHECK(first_ended->point.y == 0.0f);
    CHECK(first_ended->normal.x == 0.0f);
    CHECK(first_ended->normal.y == 0.0f);
    CHECK(first_ended->speed == 0.0f);

    // Statics never read asleep (the reference never sleeps anything;
    // Box2D statics are not sleep candidates - CONTRACT.md 6).
    const auto floor_asleep = rig.core->is_sleeping(floor);
    REQUIRE(floor_asleep.has_value());
    CHECK(*floor_asleep == false);
  }
}

TEST_CASE("hits fire above the threshold; sensors never hit") {
  for (const Backend k : {Backend::box2d, Backend::reference}) {
    INFO("backend " << backend_name(k));
    // Solid impact: threshold 1 m/s, approach 10 m/s -> began + hit in
    // the impact call, hit carrying the approach speed. The impact lands
    // on different ticks per backend (the reference detects after
    // integrating the tick; Box2D's pair pass only sees the crossing at
    // the NEXT tick's tree state), so step until the edge appears
    // instead of pinning the tick - CONTRACT.md promises the event, not
    // its tick.
    Rig rig(k, Vec2{}, 0.1f, 1.0f);
    const p2::BodyId floor = create(rig, static_box(Vec2{0.0f, -0.5f}));
    const p2::BodyId ball = create(rig, dynamic_circle(Vec2{0.0f, 0.55f}));
    REQUIRE(rig.core->set_velocity(ball, Vec2{0.0f, -10.0f}, 0.0f).has_value());
    PairEvents ev{};
    for (int i = 0; i < 6 && ev.began == 0; ++i) {
      rig.step();
      ev = scan_pair(*rig.core, floor, ball);
    }
    CHECK(ev.began == 1);
    CHECK(ev.hit >= 1);
    REQUIRE(ev.first_hit.has_value());
    // Box2D may shed a little speed inside the substeps that detect the
    // contact; the reference reports the pre-tick velocity exactly.
    CHECK(ev.first_hit->speed == doctest::Approx(10.0f).epsilon(0.25f));
    CHECK(ev.first_hit->speed > 1.0f);

    // Threshold 0 disables hit events entirely (the public Desc promise).
    Rig quiet(k, Vec2{}, 0.1f, 0.0f);
    const p2::BodyId qfloor = create(quiet, static_box(Vec2{0.0f, -0.5f}));
    const p2::BodyId qball = create(quiet, dynamic_circle(Vec2{0.0f, 0.55f}));
    REQUIRE(quiet.core->set_velocity(qball, Vec2{0.0f, -10.0f}, 0.0f)
                .has_value());
    PairEvents qev{};
    for (int i = 0; i < 6 && qev.began == 0; ++i) {
      quiet.step();
      qev = scan_pair(*quiet.core, qfloor, qball);
    }
    CHECK(qev.began == 1);
    CHECK(qev.hit == 0);

    // Sensors report edges with zero geometry, never hits - even at an
    // approach speed that would trip a solid pair.
    Rig sense(k, Vec2{}, 0.1f, 1.0f);
    p2::BodyDesc zone_desc = static_box(Vec2{0.0f, -0.5f});
    zone_desc.is_sensor = true;
    const p2::BodyId zone = create(sense, zone_desc);
    const p2::BodyId flyer = create(sense, dynamic_circle(Vec2{0.0f, 0.55f}));
    REQUIRE(sense.core->set_velocity(flyer, Vec2{0.0f, -10.0f}, 0.0f)
                .has_value());
    PairEvents sev{};
    for (int i = 0; i < 6 && sev.began == 0; ++i) {
      sense.step();
      sev = scan_pair(*sense.core, zone, flyer);
    }
    CHECK(sev.began == 1);
    CHECK(sev.hit == 0);
    REQUIRE(sev.first_began.has_value());
    CHECK(sev.first_began->point.x == 0.0f);
    CHECK(sev.first_began->point.y == 0.0f);
    CHECK(sev.first_began->normal.x == 0.0f);
    CHECK(sev.first_began->normal.y == 0.0f);
    CHECK(sev.first_began->speed == 0.0f);
  }
}

TEST_CASE("collision filters gate pairs at create and re-open at runtime") {
  for (const Backend k : {Backend::box2d, Backend::reference}) {
    INFO("backend " << backend_name(k));
    Rig rig(k, Vec2{0.0f, -10.0f});
    const p2::BodyId floor = create(rig, static_box(Vec2{0.0f, -0.5f}));
    p2::BodyDesc blind_desc = dynamic_circle(Vec2{0.0f, 3.0f});
    blind_desc.collision_mask = 0u;  // accepts nobody: the AND fails
    const p2::BodyId blind = create(rig, blind_desc);
    // Falls straight through: no pair, no events, either backend.
    for (int i = 0; i < 10; ++i) {
      rig.step();
      const PairEvents ev = scan_pair(*rig.core, floor, blind);
      CHECK(ev.began + ev.ended + ev.hit == 0);
    }
    // Runtime re-enable: re-drop the ball with the filter open.
    REQUIRE(rig.core
                ->set_transform(blind, p2::Transform{Vec2{0.0f, 1.0f}, 0.0f})
                .has_value());
    REQUIRE(rig.core->set_velocity(blind, Vec2{0.0f, 0.0f}, 0.0f).has_value());
    REQUIRE(rig.core->set_collision_filter(blind, 1u, 0xffffffffu).has_value());
    const Drop drop = step_until_began(rig, floor, blind, 30);
    REQUIRE(drop.began == 1);
    REQUIRE(drop.first_began.has_value());
  }
}

TEST_CASE("destroying a body silences its pairs and frees its touching key") {
  for (const Backend k : {Backend::box2d, Backend::reference}) {
    INFO("backend " << backend_name(k));
    Rig rig(k, Vec2{0.0f, -10.0f});
    const p2::BodyId floor = create(rig, static_box(Vec2{0.0f, -0.5f}));
    const p2::BodyId ball = create(rig, dynamic_circle(Vec2{0.0f, 3.0f}));
    const Drop drop = step_until_began(rig, floor, ball, 30);
    REQUIRE(drop.began == 1);

    REQUIRE(rig.core->destroy_body(ball).has_value());
    // Destroy silences: no ended for the destroyed side (CONTRACT.md 7).
    rig.step();
    const PairEvents gone = scan_pair(*rig.core, floor, ball);
    CHECK(gone.began + gone.ended + gone.hit == 0);

    // The slot is reused by the next create; a stale (floor, slot) key
    // would suppress this began forever - the purge makes it fire.
    const p2::BodyId fresh = create(rig, dynamic_circle(Vec2{0.0f, 3.0f}));
    CHECK(fresh.index == ball.index);
    CHECK(fresh != ball);
    const Drop again = step_until_began(rig, floor, fresh, 30);
    REQUIRE(again.began == 1);
    REQUIRE(again.first_began.has_value());
  }
}

TEST_CASE("queries: closest hit only, sees sensors, ignores filters") {
  for (const Backend k : {Backend::box2d, Backend::reference}) {
    INFO("backend " << backend_name(k));
    Rig rig(k);  // zero gravity; this case never steps
    const p2::BodyId solid =
        create(rig, static_box(Vec2{0.0f, 0.0f}, Vec2{1.0f, 1.0f}));
    p2::BodyDesc zone_desc{};
    zone_desc.kind = phys::BodyKind::static_;
    zone_desc.shape = p2::Circle{0.5f};
    zone_desc.transform.position = Vec2{4.5f, 0.0f};
    zone_desc.is_sensor = true;
    const p2::BodyId zone = create(rig, zone_desc);
    p2::BodyDesc blind_desc{};
    blind_desc.kind = phys::BodyKind::static_;
    blind_desc.shape = p2::Circle{0.5f};
    blind_desc.transform.position = Vec2{2.0f, 1.0f};
    blind_desc.collision_mask = 0u;  // refuses every pair - still queried
    const p2::BodyId blind = create(rig, blind_desc);

    // Closest hit only: the box shadows the sensor behind it on the line.
    const auto first = ray(*rig.core, Vec2{-2.5f, 0.0f}, Vec2{1.0f, 0.0f}, 10.0f);
    REQUIRE(first.has_value());
    CHECK(first->slot == solid.index);
    CHECK(first->t == doctest::Approx(1.5f).epsilon(0.001f));
    CHECK(first->point.x == doctest::Approx(-1.0f).epsilon(0.001f));
    CHECK(std::abs(first->point.y) < 1e-3f);
    CHECK(first->normal.x == doctest::Approx(-1.0f).epsilon(0.001f));
    CHECK(std::abs(first->normal.y) < 1e-3f);
    // Sensors are seen.
    const auto sensor_hit = ray(*rig.core, Vec2{2.0f, 0.0f}, Vec2{1.0f, 0.0f},
                                 10.0f);
    REQUIRE(sensor_hit.has_value());
    CHECK(sensor_hit->slot == zone.index);
    CHECK(sensor_hit->t == doctest::Approx(2.0f).epsilon(0.001f));
    // Collision filters are ignored by queries.
    const auto blind_hit = ray(*rig.core, Vec2{0.0f, 1.0f}, Vec2{1.0f, 0.0f},
                               10.0f);
    REQUIRE(blind_hit.has_value());
    CHECK(blind_hit->slot == blind.index);
    CHECK(blind_hit->t == doctest::Approx(1.5f).epsilon(0.001f));
    // max_distance caps the ray: the box face sits at t = 1.5 > 1.0.
    const auto beyond = ray(*rig.core, Vec2{-2.5f, 0.0f}, Vec2{1.0f, 0.0f},
                            1.0f);
    REQUIRE_FALSE(beyond.has_value());
    // Entering rays only: a ray from INSIDE the box cannot hit the box,
    // but it keeps traveling - the sensor down the line is the hit (the
    // reference suite pinned this shape in C6; Box2D agrees).
    const auto through = ray(*rig.core, Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f},
                             10.0f);
    REQUIRE(through.has_value());
    CHECK(through->slot == zone.index);
    CHECK(through->t == doctest::Approx(4.0f).epsilon(0.001f));

    // Overlaps see all bodies too, in ascending slot order, sensors and
    // filtered bodies included (ids_for_slots does the sorting).
    std::vector<std::uint32_t> slots;
    rig.core->backend().overlap_circle(Vec2{0.0f, 0.0f}, 2.0f, slots);
    auto ids = rig.core->ids_for_slots(slots);
    REQUIRE(ids.has_value());
    CHECK((*ids == std::vector<p2::BodyId>{solid, blind}));
    slots.clear();
    rig.core->backend().overlap_circle(Vec2{4.5f, 0.0f}, 0.1f, slots);
    ids = rig.core->ids_for_slots(slots);
    REQUIRE(ids.has_value());
    CHECK((*ids == std::vector<p2::BodyId>{zone}));
    slots.clear();
    rig.core->backend().overlap_aabb(
        p2::Aabb{Vec2{-10.0f, -10.0f}, Vec2{10.0f, 10.0f}}, slots);
    ids = rig.core->ids_for_slots(slots);
    REQUIRE(ids.has_value());
    CHECK((*ids == std::vector<p2::BodyId>{solid, zone, blind}));
  }
}

TEST_CASE("events are per call; moves and the prev mirror are per tick") {
  for (const Backend k : {Backend::box2d, Backend::reference}) {
    INFO("backend " << backend_name(k));
    Rig rig(k, Vec2{0.0f, -10.0f});  // fixed_dt = 0.1
    const p2::BodyId floor = create(rig, static_box(Vec2{0.0f, -0.5f}));
    const p2::BodyId ball = create(rig, dynamic_circle(Vec2{0.0f, 0.55f}));
    // One advance spanning two ticks: the mid-call began unions into the
    // call (events are PER CALL).
    REQUIRE(rig.core->advance(0.25f).has_value());
    const PairEvents ev = scan_pair(*rig.core, floor, ball);
    CHECK(ev.began == 1);
    // Statics never appear in moved_bodies; the list is one entry per
    // body, not per tick.
    const std::span<const p2::BodyMove> moved = rig.core->moved_bodies();
    REQUIRE(moved.size() == 1);
    CHECK(moved[0].id == ball);
    // The prev mirror snapped at the call's FIRST tick: the pre-call pose.
    CHECK(rig.core->prev_transforms()[ball.index].position.y ==
          doctest::Approx(0.55f));
    CHECK(rig.core->transforms()[ball.index].position.y < 0.55f);
    // A zero-tick advance empties events but keeps moves + prev standing
    // (interpolation endpoints stay stable while alpha grows).
    REQUIRE(rig.core->advance(0.04f).has_value());
    CHECK(rig.core->contact_events().empty());
    CHECK(rig.core->moved_bodies().size() == 1);
    CHECK(rig.core->moved_bodies()[0].id == ball);
    CHECK(rig.core->prev_transforms()[ball.index].position.y ==
          doctest::Approx(0.55f));
    CHECK(rig.core->interpolation_alpha() ==
          doctest::Approx(0.9f).epsilon(0.01f));
  }
}

TEST_CASE("generational handles retire on destroy; reuse never resurrects") {
  for (const Backend k : {Backend::box2d, Backend::reference}) {
    INFO("backend " << backend_name(k));
    Rig rig(k);
    const p2::BodyId first = create(rig, dynamic_circle(Vec2{0.0f, 0.0f}));
    REQUIRE(rig.core->destroy_body(first).has_value());
    CHECK(has_code(rig.core->body_state(first),
                   phys::PhysicsError::invalid_handle));
    CHECK(has_code(rig.core->apply_force(first, Vec2{1.0f, 0.0f}),
                   phys::PhysicsError::invalid_handle));
    // The slot is reused, but with a new generation: the stale handle
    // stays dead, the fresh one reads live.
    const p2::BodyId second = create(rig, dynamic_circle(Vec2{0.0f, 5.0f}));
    CHECK(second.index == first.index);
    CHECK(second != first);
    CHECK(has_code(rig.core->body_state(first),
                   phys::PhysicsError::invalid_handle));
    const auto state = rig.core->body_state(second);
    REQUIRE(state.has_value());
    CHECK(state->position.y == doctest::Approx(5.0f));
  }
}

}  // namespace
