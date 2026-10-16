// No DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here - test_common.cpp owns doctest's
// main (one binary, several TUs).
//
// The Jolt CONTRACT suite: the 3D mirror of test_world_2d_box2d.cpp - engine
// BEHAVIOR through the public API. Gravity and drift, bounce, stacking +
// sleep, per-tick force clearing, contact events (pair order, normal
// direction, the per-CALL union across an advance()'s substeps, hit events),
// joints (weld, distance hold, hinge motor + limits, the body-destroy
// cascade), raycast/overlap results, moved_bodies()/prev_transforms()
// semantics, the replay determinism contract, and the zero-allocation
// steady state. The backend-independent machinery is pinned in
// test_world_3d.cpp.
//
// Jolt's raw feed removes a sleeper's contacts when it falls asleep and
// re-adds them on wake. Since C3 the lib owns the touching table
// (world_core.hpp) and eats both: sleep and wake never produce began or
// ended (CONTRACT.md 7), so the observable shape now matches Box2D's. The
// stack case below pins the silence; the reconcile case pins the one edge
// sleep CAN owe - a real separation while a side sleeps.
//
// The zero-allocation case reuses the global operator new/delete counters
// that test_world_2d_box2d.cpp installs for the whole test binary; Jolt's
// own allocations ride the C heap (JPH_OVERRIDE_NEW_DELETE is
// unconditional), so the counters measure exactly this wrapper.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "check.hpp"

#include <ws/math/vecmath.hpp>
#include <ws/physics/3d/joint.hpp>
#include <ws/physics/3d/world.hpp>

using ws::physics::PhysicsError;
using ws::physics3d::Aabb;
using ws::physics3d::BodyDesc;
using ws::physics3d::BodyId;
using ws::physics3d::BodyMove;
using ws::physics3d::Box;
using ws::physics3d::Capsule;
using ws::physics3d::ContactPhase;
using ws::physics3d::DistanceJoint;
using ws::physics3d::FixedJoint;
using ws::physics3d::HingeJoint;
using ws::physics3d::Part;
using ws::physics3d::RayHit;
using ws::physics3d::Sphere;
using ws::physics3d::Transform;
using ws::physics3d::World3D;
using ws::math::Quat;
using ws::math::Vec3;
using wsphys::test::has_code;

// Installed by test_world_2d_box2d.cpp (one binary-wide replacement).
[[nodiscard]] long long allocation_count();

namespace {

constexpr float kDt = 1.0f / 60.0f;

[[nodiscard]] World3D make_world(World3D::Desc desc = {}) {
  auto world = World3D::create(desc);
  REQUIRE(world.has_value());
  return std::move(*world);
}

// A 1 m^3 dynamic box (density 1 -> mass 1 kg, I_yy = 1/6 kg*m^2): the
// exact numbers the force/torque case below depends on.
[[nodiscard]] BodyDesc dynamic_box(Vec3 position) {
  BodyDesc d{};
  d.shape = Box{Vec3{0.5f, 0.5f, 0.5f}};
  d.transform = Transform{position, Quat{}};
  return d;
}

[[nodiscard]] BodyDesc static_box(Vec3 position, Vec3 half) {
  BodyDesc d{};
  d.kind = ws::physics::BodyKind::static_;
  d.shape = Box{half};
  d.transform = Transform{position, Quat{}};
  return d;
}

[[nodiscard]] BodyDesc ball_at(Vec3 position, float restitution) {
  BodyDesc d{};
  d.shape = Sphere{0.5f};
  d.transform = Transform{position, Quat{}};
  d.restitution = restitution;
  return d;
}

void step_n(World3D& world, int n) {
  for (int i = 0; i < n; ++i) {
    REQUIRE(world.step().has_value());
  }
}

// The shared setup of the touching-table cases: two dynamic boxes stacked
// on the floor (the same deliberate 0.75/1.75 gaps as the stack case - both
// really fall, slam, settle), waited to full sleep.
[[nodiscard]] bool both_asleep(World3D& world, BodyId lower, BodyId upper) {
  for (int i = 0; i < 300; ++i) {
    REQUIRE(world.step().has_value());
    auto l = world.is_sleeping(lower);
    auto u = world.is_sleeping(upper);
    REQUIRE(l.has_value());
    REQUIRE(u.has_value());
    if (*l && *u) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("3D gravity accelerates straight down; x/z stay put") {
  auto world = make_world();  // the default pool: real threaded stepping
  auto id = world.create_body(dynamic_box(Vec3{0.0f, 10.0f, 0.0f}));
  REQUIRE(id.has_value());

  step_n(world, 60);  // one second of free fall

  auto state = world.body_state(*id);
  REQUIRE(state.has_value());
  CHECK(std::abs(state->position.x) < 0.01f);
  CHECK(std::abs(state->position.z) < 0.01f);
  CHECK(state->position.y < 8.0f);  // fell
  CHECK(state->position.y > 3.0f);  // ~5.1 m expected after 1 s from rest
  CHECK(state->linear_velocity.y == doctest::Approx(-9.81f).epsilon(0.02));
  CHECK(std::abs(state->linear_velocity.x) < 0.01f);
}

TEST_CASE("3D zero-gravity world preserves inertial drift exactly") {
  auto world = make_world(World3D::Desc{.gravity = Vec3{0.0f, 0.0f, 0.0f},
                                        .thread_count = 0});
  auto id = world.create_body(dynamic_box(Vec3{0.0f, 0.0f, 0.0f}));
  REQUIRE(id.has_value());
  REQUIRE(world.set_velocity(*id, Vec3{2.0f, 1.0f, -0.5f}, Vec3{})
              .has_value());

  step_n(world, 60);

  auto state = world.body_state(*id);
  REQUIRE(state.has_value());
  CHECK(state->position.x == doctest::Approx(2.0f).epsilon(0.001));
  CHECK(state->position.y == doctest::Approx(1.0f).epsilon(0.001));
  CHECK(state->position.z == doctest::Approx(-0.5f).epsilon(0.001));
  CHECK(state->linear_velocity.x == doctest::Approx(2.0f).epsilon(0.001));
  CHECK(state->linear_velocity.z == doctest::Approx(-0.5f).epsilon(0.001));
}

TEST_CASE("3D restitution 1 bounces: the impact flips the velocity sign") {
  auto world = make_world(World3D::Desc{.thread_count = 0});
  auto floor = world.create_body(static_box(Vec3{0.0f, -1.0f, 0.0f},
                                             Vec3{50.0f, 1.0f, 50.0f}));
  REQUIRE(floor.has_value());
  auto ball = world.create_body(ball_at(Vec3{0.0f, 5.0f, 0.0f}, 1.0f));
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

TEST_CASE("3D multi-shape parts: the listener recombines materials every step") {
  auto world = make_world(World3D::Desc{.thread_count = 0});

  // Compound floor: rough part left, ice part right. The floor's BODY-level
  // friction is the default 0.2 on purpose - it is exactly what the ice side
  // would revert to if the listener stopped overriding persisted contacts
  // (Jolt rebuilds contact constraints from body-level defaults every step;
  // only OnContactAdded + OnContactPersisted together keep part materials).
  BodyDesc floor{};
  floor.kind = ws::physics::BodyKind::static_;
  floor.transform = Transform{Vec3{0.0f, -1.0f, 0.0f}, Quat{}};
  floor.parts[0] = Part{Box{Vec3{10.0f, 1.0f, 10.0f}},
                        Vec3{-10.0f, 0.0f, 0.0f}, 1.0f, 0.0f};
  floor.parts[1] = Part{Box{Vec3{10.0f, 1.0f, 10.0f}},
                        Vec3{10.0f, 0.0f, 0.0f}, 0.0f, 0.0f};
  floor.part_count = 2;
  REQUIRE(world.create_body(floor).has_value());

  // Sliding boxes (friction 1 so the sqrt mix is the part's alone): the
  // rough side's combined friction is sqrt(1*1)=1, the ice side's
  // sqrt(0*1)=0. Both start AT REST on the surface (floor top y=0) so the
  // whole second is pure sliding.
  BodyDesc rough_desc = dynamic_box(Vec3{-15.0f, 0.5f, 0.0f});
  BodyDesc ice_desc = dynamic_box(Vec3{5.0f, 0.5f, 0.0f});
  rough_desc.friction = 1.0f;
  ice_desc.friction = 1.0f;
  auto rough = world.create_body(rough_desc);
  auto ice = world.create_body(ice_desc);
  REQUIRE(rough.has_value());
  REQUIRE(ice.has_value());
  REQUIRE(world.set_velocity(*rough, Vec3{3.0f, 0.0f, 0.0f},
                             Vec3{}).has_value());
  REQUIRE(world.set_velocity(*ice, Vec3{3.0f, 0.0f, 0.0f},
                             Vec3{}).has_value());

  step_n(world, 60);  // 1 s of sliding, all of it PERSISTED contacts
  auto r = world.body_state(*rough);
  auto i = world.body_state(*ice);
  REQUIRE(r.has_value());
  REQUIRE(i.has_value());
  // Ice keeps gliding - if the override were Added-only, the ~0.45 combined
  // friction from the body-level floor value would have stopped it too.
  CHECK(i->linear_velocity.x > 2.5f);
  // Rough: combined 1.0 eats 3 m/s in ~0.3 s (then it sleeps).
  CHECK(r->linear_velocity.x < 0.5f);
}

TEST_CASE("3D multi-shape parts: per-part restitution beats body-level") {
  auto world = make_world(World3D::Desc{.thread_count = 0});

  // Dead part left, trampoline right; body-level restitution stays 0 (the
  // ignored-for-parts default) - the listener must combine from the parts.
  BodyDesc floor{};
  floor.kind = ws::physics::BodyKind::static_;
  floor.transform = Transform{Vec3{0.0f, -1.0f, 0.0f}, Quat{}};
  floor.parts[0] = Part{Box{Vec3{1.5f, 1.0f, 1.5f}},
                        Vec3{-1.5f, 0.0f, 0.0f}, 0.2f, 0.0f};
  floor.parts[1] = Part{Box{Vec3{1.5f, 1.0f, 1.5f}},
                        Vec3{1.5f, 0.0f, 0.0f}, 0.2f, 1.0f};
  floor.part_count = 2;
  REQUIRE(world.create_body(floor).has_value());

  auto dead = world.create_body(ball_at(Vec3{-1.5f, 5.0f, 0.0f}, 0.0f));
  auto live = world.create_body(ball_at(Vec3{1.5f, 5.0f, 0.0f}, 0.0f));
  REQUIRE(dead.has_value());
  REQUIRE(live.has_value());

  float dead_apex = 0.0f;
  float live_apex = 0.0f;
  for (int i = 0; i < 90; ++i) {  // 1.5 s: fall, hit, bounce back up
    REQUIRE(world.step().has_value());
    // The drop from y=5 takes ~0.96 s (58 steps) - apex sampling starts
    // after landing, so the checks measure the BOUNCE, not the fall.
    if (i < 60) {
      continue;
    }
    auto d = world.body_state(*dead);
    auto l = world.body_state(*live);
    REQUIRE(d.has_value());
    REQUIRE(l.has_value());
    dead_apex = std::max(dead_apex, d->position.y);
    live_apex = std::max(live_apex, l->position.y);
  }
  CHECK(dead_apex < 1.5f);  // never meaningfully bounced (rests at ~0.5)
  CHECK(live_apex > 3.0f);  // back near the 5.0 drop height
}

TEST_CASE("3D multi-shape parts: geometry rides the body-local offset") {
  auto world = make_world(World3D::Desc{.thread_count = 0});
  // A static body at the origin carrying ONE part translated to (3, 0, 0):
  // the shape exists only there - the body origin itself is empty space.
  BodyDesc offset_body{};
  offset_body.kind = ws::physics::BodyKind::static_;
  offset_body.parts[0] = Part{Box{Vec3{1.0f, 1.0f, 1.0f}},
                               Vec3{3.0f, 0.0f, 0.0f}, 0.2f, 0.0f};
  offset_body.part_count = 1;
  auto shelf = world.create_body(offset_body);
  REQUIRE(shelf.has_value());

  auto on_shelf = world.create_body(ball_at(Vec3{3.0f, 5.0f, 0.0f}, 0.0f));
  auto through = world.create_body(ball_at(Vec3{0.0f, 5.0f, 0.0f}, 0.0f));
  REQUIRE(on_shelf.has_value());
  REQUIRE(through.has_value());

  step_n(world, 90);  // 1.5 s: plenty to land (and to fall far past)

  auto landed = world.body_state(*on_shelf);
  auto fell = world.body_state(*through);
  REQUIRE(landed.has_value());
  REQUIRE(fell.has_value());
  // Shelf top is y=1 (origin + half 1), ball radius 0.5 -> rests at 1.5.
  CHECK(landed->position.y == doctest::Approx(1.5f).epsilon(0.05));
  CHECK(landed->position.x == doctest::Approx(3.0f).epsilon(0.05));
  // No shape at the origin: free fall from y=5 over 1.5 s reaches ~-6.
  CHECK(fell->position.y < -5.0f);
}

TEST_CASE("3D two part-pairs of one body pair begin and end independently") {
  // The touching table keys on the full SHAPE pair (world_core.hpp), so a
  // second part's began must never read as a duplicate of the first, and
  // each part-pair ends on its own. Zero gravity keeps the geometry
  // deterministic: a static two-pillar base (different heights) and a
  // dynamic two-footed body, with the feet staged to touch two ticks
  // apart. Only began events carry a witness point, so the part pairs are
  // told apart by where their began points sit (z ~ 0 vs z ~ 2); ended is
  // pinned by COUNT: both part-pairs must end, each exactly once.
  auto world = make_world(World3D::Desc{.gravity = Vec3{0.0f, 0.0f, 0.0f},
                                        .thread_count = 0});
  BodyDesc base{};
  base.kind = ws::physics::BodyKind::static_;
  base.transform = Transform{Vec3{0.0f, 0.0f, 0.0f}, Quat{}};
  base.parts[0] = Part{Box{Vec3{0.5f, 0.5f, 0.5f}},
                        Vec3{0.0f, 0.5f, 0.0f}, 0.2f, 0.0f};   // top y = 1.0
  base.parts[1] = Part{Box{Vec3{0.5f, 0.25f, 0.5f}},
                        Vec3{0.0f, 0.25f, 2.0f}, 0.2f, 0.0f};  // top y = 0.5
  base.part_count = 2;
  auto base_id = world.create_body(base);
  REQUIRE(base_id.has_value());

  BodyDesc bridge{};
  bridge.transform = Transform{Vec3{0.0f, 1.05f, 0.0f}, Quat{}};
  bridge.parts[0] = Part{Box{Vec3{0.25f, 0.25f, 0.25f}},
                         Vec3{0.0f, 0.0f, 0.0f}, 0.2f, 0.0f};
  bridge.parts[1] = Part{Box{Vec3{0.25f, 0.25f, 0.25f}},
                         Vec3{0.0f, 0.0f, 2.0f}, 0.2f, 0.0f};
  bridge.part_count = 2;
  auto bridge_id = world.create_body(bridge);
  REQUIRE(bridge_id.has_value());

  int began_low = 0;   // began with a witness point over pillar 0
  int began_high = 0;  // began with a witness point over pillar 1
  int ended = 0;
  const auto collect = [&](int ticks) {
    for (int i = 0; i < ticks; ++i) {
      REQUIRE(world.step().has_value());
      for (const auto& e : world.contact_events()) {
        if (e.phase == ContactPhase::began) {
          // Pillar 0's contact points sit at z in [-0.25, 0.25]; pillar
          // 1's at z in [1.75, 2.25].
          (e.point.z < 1.0f ? began_low : began_high) += 1;
        } else if (e.phase == ContactPhase::ended) {
          ++ended;
        }
      }
    }
  };

  // Phase 1: foot 0 spawns 0.2 into pillar 0 (bottom 0.8 vs top 1.0) while
  // foot 1 clears pillar 1 by 0.3 - exactly one began, over pillar 0.
  collect(2);
  CHECK(began_low == 1);
  CHECK(began_high == 0);

  // Phase 2: the teleport sinks the whole body - foot 0 stays in contact
  // the entire time (no gap ever opens: no ended, no re-began), while
  // foot 1's 0.1 overlap with pillar 1 brings a SECOND began for the SAME
  // body pair. A slot-keyed table would swallow it as a duplicate.
  REQUIRE(world.set_transform(*bridge_id, Vec3{0.0f, 0.65f, 0.0f}, Quat{})
              .has_value());
  collect(2);
  CHECK(began_low == 1);   // foot 0 never re-began (never separated)
  CHECK(began_high == 1);  // the second part-pair's began survived

  // Phase 3: lift the whole body clear - both part-pairs separate, and
  // each ends exactly once. (Zero-g depenetration only rests the feet ON
  // the pillars, it never separates them, so the separation is driven by
  // the transform, not the solver.)
  REQUIRE(world.set_transform(*bridge_id, Vec3{0.0f, 1.65f, 0.0f}, Quat{})
              .has_value());
  for (int i = 0; i < 10 && ended < 2; ++i) {
    collect(1);
  }
  CHECK(ended == 2);      // one per part-pair, no more
  CHECK(began_low == 1);  // and never a duplicate began
  CHECK(began_high == 1);
}

TEST_CASE("3D stack settles, sleeps and leaves moved_bodies() empty; "
          "sleep and wake never report contact events") {
  auto world = make_world(World3D::Desc{.thread_count = 0});
  auto floor = world.create_body(static_box(Vec3{0.0f, -0.5f, 0.0f},
                                             Vec3{50.0f, 0.5f, 50.0f}));
  REQUIRE(floor.has_value());
  std::vector<BodyId> boxes;
  for (int i = 0; i < 3; ++i) {
    // Deliberate gaps: the stack falls together, slams, then settles.
    // (0.75 base, not 0.5: the bottom box must NOT spawn exactly on the
    // floor - Jolt solves a zero-gap contact perfectly still in tick 1,
    // and the falling checks below need every box to actually fall.)
    auto id = world.create_body(dynamic_box(Vec3{0.0f, 0.75f + 1.5f * i,
                                                  0.0f}));
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

  // CONTRACT.md 7: sleep is silent. Jolt purges a sleeper's contacts the
  // moment it falls asleep - the old backend divergence - and re-detects
  // them on wake. The lib's touching table eats both, so a settling stack
  // reports no `ended` at all, and once the world goes quiet (an empty
  // moved list: nothing simulated, all three slept through it) it reports
  // no began/ended either. The slam and wobble before that legitimately
  // fire began/hit.
  int quiet_from = -1;
  int ended_total = 0;
  int edges_after_quiet = 0;
  for (int i = 0; i < 300; ++i) {  // five seconds: slam, wobble, settle, sleep
    REQUIRE(world.step().has_value());
    if (world.moved_bodies().empty() && quiet_from < 0) {
      quiet_from = i;
    }
    for (const auto& e : world.contact_events()) {
      if (e.phase == ContactPhase::ended) {
        ++ended_total;
      }
      if (quiet_from >= 0 && e.phase != ContactPhase::hit) {
        ++edges_after_quiet;
      }
    }
  }
  REQUIRE(quiet_from >= 0);       // they really slept
  CHECK(ended_total == 0);        // and never reported `ended` on the way
  CHECK(edges_after_quiet == 0);  // sleep itself is event-silent

  for (std::size_t i = 0; i < boxes.size(); ++i) {
    auto state = world.body_state(boxes[i]);
    REQUIRE(state.has_value());
    CHECK(std::abs(state->linear_velocity.x) < 0.05f);
    CHECK(std::abs(state->linear_velocity.y) < 0.05f);
    CHECK(std::abs(state->linear_velocity.z) < 0.05f);
    CHECK(std::abs(state->angular_velocity.x) < 0.05f);
    // Rest heights preserved: 0.5 / 1.5 / 2.5 (+- small solver overlap).
    CHECK(std::abs(state->position.y - (0.5f + 1.0f * i)) < 0.05f);
  }

  // Asleep: nothing moved on the last call, so the sleep sync cost is
  // zero and the prev mirror equals the current one.
  CHECK(world.moved_bodies().empty());
  for (const auto& id : boxes) {
    const auto& cur = world.transforms()[id.index];
    const auto& prev = world.prev_transforms()[id.index];
    CHECK(cur.position.y == doctest::Approx(prev.position.y));
  }

  // The wake half: Jolt re-detects the sleeper's contacts the moment it
  // wakes, and the table eats that too - a pair that never stopped
  // touching stays `began` from its original contact. The wake is proven
  // real by the moved list, so the zero is the suppression at work.
  REQUIRE(world.set_velocity(boxes[0], Vec3{0.01f, 0.0f, 0.0f},
                             Vec3{0.0f, 0.01f, 0.0f})
              .has_value());
  bool wake_moved = false;
  int wake_edges = 0;
  for (int i = 0; i < 3; ++i) {
    REQUIRE(world.step().has_value());
    wake_moved = wake_moved || !world.moved_bodies().empty();
    for (const auto& e : world.contact_events()) {
      if (e.phase != ContactPhase::hit) {
        ++wake_edges;
      }
    }
  }
  CHECK(wake_moved);        // the boxes really woke and simulated
  CHECK(wake_edges == 0);  // yet not one began/ended surfaced
}

TEST_CASE("3D a sleeping pair's real separation still reports ended") {
  // The freeze path of the touching table: Jolt already dropped the pair
  // when the stack fell asleep, so the engine will never say `ended` - the
  // table froze it, the wake movement stamps its deadline, and reconcile
  // owes the consumer the edge (CONTRACT.md 7: ended fires exactly once
  // per touching -> not-touching transition, however the backend loses
  // track).
  auto world = make_world(World3D::Desc{.thread_count = 0});
  auto floor = world.create_body(static_box(Vec3{0.0f, -0.5f, 0.0f},
                                             Vec3{50.0f, 0.5f, 50.0f}));
  REQUIRE(floor.has_value());
  auto lower = world.create_body(dynamic_box(Vec3{0.0f, 0.75f, 0.0f}));
  auto upper = world.create_body(dynamic_box(Vec3{0.0f, 1.75f, 0.0f}));
  REQUIRE(lower.has_value());
  REQUIRE(upper.has_value());
  REQUIRE(both_asleep(world, *lower, *upper));

  // The upper box teleports away - into the air, far from the stack - so
  // the pair really separates while the lower box stays asleep.
  REQUIRE(world.set_transform(*upper, Vec3{5.0f, 5.0f, 0.0f}, Quat{})
              .has_value());

  const auto names_pair = [&](const auto& e) {
    return (e.a == *lower && e.b == *upper) || (e.a == *upper && e.b == *lower);
  };
  bool saw_ended = false;
  bool saw_other = false;
  for (int i = 0; i < 10 && !saw_ended; ++i) {
    REQUIRE(world.step().has_value());
    for (const auto& e : world.contact_events()) {
      if (!names_pair(e)) {
        continue;
      }
      if (e.phase == ContactPhase::ended) {
        saw_ended = true;
      } else {
        saw_other = true;  // nothing re-touched: no began/hit for this pair
      }
    }
  }
  CHECK(saw_ended);        // the lib delivered the engine's missing edge
  CHECK_FALSE(saw_other);

  // The lower box slept through its own pair's end: no wake, no motion.
  auto still = world.is_sleeping(*lower);
  REQUIRE(still.has_value());
  CHECK(*still);
}

TEST_CASE("3D destroying a touching body is silent; the reused slot "
          "re-begins cleanly") {
  auto world = make_world(World3D::Desc{.thread_count = 0});
  auto floor = world.create_body(static_box(Vec3{0.0f, -0.5f, 0.0f},
                                             Vec3{50.0f, 0.5f, 50.0f}));
  REQUIRE(floor.has_value());
  // The two-box stack of the reconcile case, settled to sleep: the
  // upper->lower pair is in the table when its owner dies.
  auto lower = world.create_body(dynamic_box(Vec3{0.0f, 0.75f, 0.0f}));
  auto upper = world.create_body(dynamic_box(Vec3{0.0f, 1.75f, 0.0f}));
  REQUIRE(lower.has_value());
  REQUIRE(upper.has_value());
  REQUIRE(both_asleep(world, *lower, *upper));

  REQUIRE(world.destroy_body(*upper).has_value());

  // Silence (CONTRACT.md 7: destroying a body never reports its pairs):
  // the engine's late removal resolves to a dead body and drops, and the
  // table purged the pair - no edge names the dead slot in the quiet
  // ticks that follow.
  for (int i = 0; i < 5; ++i) {
    REQUIRE(world.step().has_value());
    for (const auto& e : world.contact_events()) {
      CHECK_FALSE(e.a == *upper);
      CHECK_FALSE(e.b == *upper);
    }
  }

  // Slot reuse: the freed slot is the only one, so the next body lands in
  // it - same index, bumped generation. Dropped onto the survivor, it must
  // begin EXACTLY once: the dead pair's stale key may neither suppress the
  // new contact nor duplicate it.
  auto reuser = world.create_body(dynamic_box(Vec3{0.0f, 1.75f, 0.0f}));
  REQUIRE(reuser.has_value());
  CHECK(reuser->index == upper->index);
  CHECK_FALSE(*reuser == *upper);  // generation moved on: the stale id is dead

  int began = 0;
  for (int i = 0; i < 120; ++i) {  // 2 s: fall (13 ticks), land, settle, sleep
    REQUIRE(world.step().has_value());
    for (const auto& e : world.contact_events()) {
      if (e.phase == ContactPhase::began &&
          (e.a == *reuser || e.b == *reuser)) {
        ++began;
      }
    }
  }
  CHECK(began == 1);
}

TEST_CASE("3D forces accumulate within a tick and clear at its end") {
  auto world = make_world(World3D::Desc{.gravity = Vec3{0.0f, 0.0f, 0.0f},
                                        .thread_count = 0});
  auto id = world.create_body(dynamic_box(Vec3{0.0f, 0.0f, 0.0f}));
  REQUIRE(id.has_value());

  // One tick of F = 10 N on 1 kg: dv = F/m * dt = 1/6 m/s.
  REQUIRE(world.apply_force(*id, Vec3{10.0f, 0.0f, 0.0f}).has_value());
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
  REQUIRE(world.apply_force(*id, Vec3{10.0f, 0.0f, 0.0f}).has_value());
  REQUIRE(world.apply_force(*id, Vec3{10.0f, 0.0f, 0.0f}).has_value());
  step_n(world, 1);
  state = world.body_state(*id);
  REQUIRE(state.has_value());
  CHECK(std::abs(state->linear_velocity.x - 3.0f * 10.0f * kDt) < 1e-3f);

  // Torque behaves identically: I_yy = 1/6 for the 1 m^3 box, so
  // dw = tau/I * dt = 6 * dt rad/s.
  REQUIRE(world.apply_torque(*id, Vec3{0.0f, 1.0f, 0.0f}).has_value());
  step_n(world, 1);
  state = world.body_state(*id);
  REQUIRE(state.has_value());
  CHECK(std::abs(state->angular_velocity.y - 6.0f * kDt) < 1e-3f);
  step_n(world, 1);
  state = world.body_state(*id);
  REQUIRE(state.has_value());
  CHECK(std::abs(state->angular_velocity.y - 6.0f * kDt) < 1e-3f);
}

TEST_CASE("3D mass override: F = ma and tau = I alpha on the lib numbers") {
  // C4 (CONTRACT.md 4): the lib computes mass/inertia closed-form and the
  // adapter pushes them through BodyCreationSettings::mOverrideMassProperties
  // (MassAndInertiaProvided). For a single box the numbers equal Jolt's own
  // derivation (same physics), so this proves the override round-trips
  // cleanly; the composite spin case below is the structural
  // discriminator, and test_mass_properties.cpp pins the formulas.
  // Jolt takes one semi-implicit step per tick: v = a*dt*N exactly,
  // x = a*dt^2*N*(N+1)/2.
  auto world = make_world(World3D::Desc{.gravity = Vec3{0.0f, 0.0f, 0.0f},
                                        .thread_count = 0});
  BodyDesc d{};
  d.shape = Box{Vec3{1.0f, 1.0f, 1.0f}};  // rho=1: m = 8, I = 16/3
  d.allow_sleep = false;
  auto id = world.create_body(d);
  REQUIRE(id.has_value());

  // F = (16,0,0) on 8 kg: a = 2; 60 ticks land v exactly on 2.
  for (int i = 0; i < 60; ++i) {
    REQUIRE(world.apply_force(*id, Vec3{16.0f, 0.0f, 0.0f}).has_value());
    REQUIRE(world.step().has_value());
  }
  auto state = world.body_state(*id);
  REQUIRE(state.has_value());
  CHECK(std::abs(state->linear_velocity.x - 2.0f) < 1e-3f);
  const float x = 2.0f * kDt * kDt * (60.0f * 61.0f / 2.0f);
  CHECK(std::abs(state->position.x - x) < 3e-3f);

  // Torque about x: tau = 32/3 on I = 16/3 -> alpha = 2; omega lands
  // exactly on 2.
  for (int i = 0; i < 60; ++i) {
    REQUIRE(world.apply_torque(*id, Vec3{32.0f / 3.0f, 0.0f, 0.0f})
                .has_value());
    REQUIRE(world.step().has_value());
  }
  state = world.body_state(*id);
  REQUIRE(state.has_value());
  CHECK(std::abs(state->angular_velocity.x - 2.0f) < 1e-3f);
}

TEST_CASE("3D mass override: composite mass and tensor") {
  // Box half=(1,1,1) at the origin (m = 8) + sphere r = 0.5 at (3,0,0)
  // (m = pi/6), body-level density 1. Hand-derived: m = 8 + pi/6, com =
  // (3*m_s/(8+m_s), 0, 0), and - every part offset on the x axis - the
  // tensor is diagonal with I_zz = I_yy = 16/3 + 8*cx^2 +
  // (2/5)*m_s*0.25 + m_s*(3-cx)^2.
  auto world = make_world(World3D::Desc{.gravity = Vec3{0.0f, 0.0f, 0.0f},
                                        .thread_count = 0});
  BodyDesc d{};
  d.allow_sleep = false;
  d.parts[0] = Part{Box{Vec3{1.0f, 1.0f, 1.0f}},
                    Vec3{0.0f, 0.0f, 0.0f}, 0.2f, 0.0f};
  d.parts[1] = Part{Sphere{0.5f}, Vec3{3.0f, 0.0f, 0.0f}, 0.2f, 0.0f};
  d.part_count = 2;
  auto id = world.create_body(d);
  REQUIRE(id.has_value());

  const float m_b = 8.0f;
  const float m_s = 3.14159265f / 6.0f;
  const float cx = 3.0f * m_s / (m_b + m_s);

  // Force scaled to the composite mass: a = 2; v lands exactly on 2.
  for (int i = 0; i < 60; ++i) {
    REQUIRE(world.apply_force(*id, Vec3{2.0f * (m_b + m_s), 0.0f, 0.0f})
                .has_value());
    REQUIRE(world.step().has_value());
  }
  auto state = world.body_state(*id);
  REQUIRE(state.has_value());
  CHECK(std::abs(state->linear_velocity.x - 2.0f) < 1e-3f);

  // Torque about z through the hand-derived I_zz: alpha = 2, omega
  // lands on 2. z is a principal axis (all part offsets sit on x), so
  // omega stays on it and the other components read zero.
  const float i_zz = 16.0f / 3.0f + m_b * cx * cx + 0.4f * m_s * 0.25f +
                     m_s * (3.0f - cx) * (3.0f - cx);
  for (int i = 0; i < 60; ++i) {
    REQUIRE(world.apply_torque(*id, Vec3{0.0f, 0.0f, 2.0f * i_zz})
                .has_value());
    REQUIRE(world.step().has_value());
  }
  state = world.body_state(*id);
  REQUIRE(state.has_value());
  CHECK(std::abs(state->angular_velocity.z - 2.0f) < 1e-3f);
  CHECK(std::abs(state->angular_velocity.x) < 1e-3f);
  CHECK(std::abs(state->angular_velocity.y) < 1e-3f);
}

TEST_CASE("3D mass override: a composite spins about its COM, not its origin") {
  // THE structural discriminator. Jolt derives its rotation pivot from
  // the shape - StaticCompoundShape's volume-weighted child centroid -
  // and never sees the lib's COM as a number; under the body-level
  // density that centroid IS the lib's mass-weighted COM, so they agree
  // by construction. A free spin (zero gravity, no forces) keeps the
  // world COM fixed while the ORIGIN orbits it:
  //   origin = com_world - R(theta) * com.
  // Jolt rotates about mPosition (the COM) and reports the origin
  // through GetPosition() = mPosition - R*shapeCOM (Body.h), so the
  // numbers below pin the agreement end to end: a mismatched pivot
  // would leave the origin sitting still.
  auto world = make_world(World3D::Desc{.gravity = Vec3{0.0f, 0.0f, 0.0f},
                                        .thread_count = 0});
  BodyDesc d{};
  d.angular_velocity = Vec3{0.0f, 0.0f, 1.0f};  // 60 ticks -> 1 rad
  d.allow_sleep = false;
  d.parts[0] = Part{Box{Vec3{1.0f, 1.0f, 1.0f}},
                    Vec3{0.0f, 0.0f, 0.0f}, 0.2f, 0.0f};
  d.parts[1] = Part{Sphere{0.5f}, Vec3{3.0f, 0.0f, 0.0f}, 0.2f, 0.0f};
  d.part_count = 2;
  auto id = world.create_body(d);
  REQUIRE(id.has_value());

  step_n(world, 60);
  auto state = world.body_state(*id);
  REQUIRE(state.has_value());
  // Pure z rotation: theta = 2*atan2(qz, qw).
  const float theta =
      2.0f * std::atan2(state->rotation.z, state->rotation.w);
  CHECK(std::abs(theta - 1.0f) < 1e-3f);

  const float m_s = 3.14159265f / 6.0f;
  const float c = 3.0f * m_s / (8.0f + m_s);  // hand-derived mass COM
  CHECK(std::abs(state->position.x - c * (1.0f - std::cos(theta))) < 1e-3f);
  CHECK(std::abs(state->position.y + c * std::sin(theta)) < 1e-3f);
  CHECK(std::abs(state->position.z) < 1e-3f);
}

TEST_CASE("3D contact events: pair order fixes the normal to point a -> b") {
  const auto run = [](bool floor_first) {
    auto world = make_world(World3D::Desc{.thread_count = 0});
    const BodyDesc floor_desc = static_box(Vec3{0.0f, -0.5f, 0.0f},
                                           Vec3{50.0f, 0.5f, 50.0f});
    const BodyDesc ball_desc = ball_at(Vec3{0.0f, 5.0f, 0.0f}, 0.0f);
    BodyId floor{};
    BodyId ball{};
    if (floor_first) {
      auto f = world.create_body(floor_desc);
      auto b = world.create_body(ball_desc);
      REQUIRE(f.has_value());
      REQUIRE(b.has_value());
      floor = *f;
      ball = *b;
    } else {
      auto b = world.create_body(ball_desc);
      auto f = world.create_body(floor_desc);
      REQUIRE(b.has_value());
      REQUIRE(f.has_value());
      ball = *b;
      floor = *f;
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
    // The interface sits at y=0; Jolt's speculative margin can begin the
    // contact slightly BEFORE geometric touch, hence the loose bound.
    CHECK(std::abs(e.point.y) < 0.1f);
    if (floor_first) {
      CHECK(e.a == floor);  // lower slot index wins the "a" role
      CHECK(e.b == ball);
      CHECK(e.normal.y == doctest::Approx(1.0f).epsilon(0.01));  // floor -> ball
      CHECK(std::abs(e.normal.x) < 0.01f);
      CHECK(std::abs(e.normal.z) < 0.01f);
    } else {
      CHECK(e.a == ball);
      CHECK(e.b == floor);
      CHECK(e.normal.y == doctest::Approx(-1.0f).epsilon(0.01));  // ball -> floor
      CHECK(std::abs(e.normal.x) < 0.01f);
      CHECK(std::abs(e.normal.z) < 0.01f);
    }
  };
  run(true);   // floor created first: a = floor
  run(false);  // ball created first: a = ball, normal re-faced
}

TEST_CASE("3D advance() unions contact events across its substeps") {
  World3D::Desc desc{};
  desc.thread_count = 0;
  desc.hit_speed_threshold = 1.0f;  // m/s: enables hit events
  desc.max_substeps = 128;
  auto world = make_world(desc);
  auto floor = world.create_body(static_box(Vec3{0.0f, -0.5f, 0.0f},
                                             Vec3{50.0f, 0.5f, 50.0f}));
  REQUIRE(floor.has_value());
  auto ball = world.create_body(ball_at(Vec3{0.0f, 5.0f, 0.0f}, 0.0f));
  REQUIRE(ball.has_value());

  // 1.5 s in ONE advance() call: ~89 ticks. The impact lands mid-call
  // (t ~ 0.96 s); its events MUST survive to the call's end.
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
  // over, so a plain one-tick call reports neither. (A sleep `ended`
  // would be legal here - this check excludes only began/hit.)
  REQUIRE(world.advance(1.0f / 60.0f).has_value());
  for (const auto& e : world.contact_events()) {
    CHECK(e.phase != ContactPhase::began);
    CHECK(e.phase != ContactPhase::hit);
  }
}

TEST_CASE("3D collision filters gate contacts, sensors and events") {
  // The rule is BOTH-directional: (a.collision_layer & b.collision_mask)
  // && (b.collision_layer & a.collision_mask). Every subcase is the same
  // 5 m drop onto a floor whose interface sits at y = 0 (ball center
  // rests at 0.5), so the y position after the steps IS the verdict.

  SUBCASE("a one-directional reject is a reject: the ball falls through") {
    auto world = make_world(World3D::Desc{.thread_count = 0});
    BodyDesc floor_desc =
        static_box(Vec3{0.0f, -0.5f, 0.0f}, Vec3{50.0f, 0.5f, 50.0f});
    floor_desc.collision_mask = 2u;  // accepts layer 2 only: rejects the layer-1 ball
    auto floor = world.create_body(floor_desc);
    REQUIRE(floor.has_value());
    auto ball = world.create_body(ball_at(Vec3{0.0f, 5.0f, 0.0f}, 0.0f));
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
    auto world = make_world(World3D::Desc{.thread_count = 0});
    auto floor = world.create_body(static_box(Vec3{0.0f, -0.5f, 0.0f},
                                              Vec3{50.0f, 0.5f, 50.0f}));
    REQUIRE(floor.has_value());
    auto ball = world.create_body(ball_at(Vec3{0.0f, 5.0f, 0.0f}, 0.0f));
    REQUIRE(ball.has_value());

    step_n(world, 120);  // drop, settle, most likely sleep
    auto state = world.body_state(*ball);
    REQUIRE(state.has_value());
    CHECK(std::abs(state->position.y - 0.5f) < 0.05f);  // resting on it

    // Mask 0 accepts nothing. Jolt's SetCollisionGroup does NOT activate
    // a sleeping body, so the wake is explicit (set_velocity activates) -
    // the documented public contract.
    REQUIRE(world.set_collision_filter(*ball, 1u, 0u).has_value());
    REQUIRE(world.set_velocity(*ball, Vec3{0.0f, -0.5f, 0.0f}, Vec3{})
                .has_value());
    step_n(world, 90);
    state = world.body_state(*ball);
    REQUIRE(state.has_value());
    CHECK(state->position.y < -1.0f);
  }

  SUBCASE("a sensor fires began, does not respond, and sails the ball through") {
    auto world = make_world(World3D::Desc{.thread_count = 0});
    BodyDesc zone_desc =
        static_box(Vec3{0.0f, -0.5f, 0.0f}, Vec3{50.0f, 0.5f, 50.0f});
    zone_desc.is_sensor = true;
    auto zone = world.create_body(zone_desc);
    REQUIRE(zone.has_value());
    auto ball = world.create_body(ball_at(Vec3{0.0f, 5.0f, 0.0f}, 0.0f));
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
    auto world = make_world(World3D::Desc{.thread_count = 0});
    BodyDesc zone_desc =
        static_box(Vec3{0.0f, -0.5f, 0.0f}, Vec3{50.0f, 0.5f, 50.0f});
    zone_desc.is_sensor = true;
    zone_desc.collision_mask = 0u;  // the zone accepts nothing
    auto zone = world.create_body(zone_desc);
    REQUIRE(zone.has_value());
    auto ball = world.create_body(ball_at(Vec3{0.0f, 5.0f, 0.0f}, 0.0f));
    REQUIRE(ball.has_value());

    bool any_event = false;
    for (int i = 0; i < 90; ++i) {
      REQUIRE(world.step().has_value());
      any_event = any_event || !world.contact_events().empty();
    }
    CHECK_FALSE(any_event);
  }
}

TEST_CASE("3D runtime set_gravity redirects the fall and wakes the sleepers") {
  SUBCASE("flipping the sign turns the fall into a climb") {
    auto world = make_world(World3D::Desc{.thread_count = 0});
    auto id = world.create_body(dynamic_box(Vec3{0.0f, 10.0f, 0.0f}));
    REQUIRE(id.has_value());

    step_n(world, 30);  // 0.5 s of free fall: y ~8.7, v ~-4.9
    auto state = world.body_state(*id);
    REQUIRE(state.has_value());

    REQUIRE(world.set_gravity(Vec3{0.0f, 9.81f, 0.0f}).has_value());
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
    auto world = make_world(World3D::Desc{.thread_count = 0});
    auto floor = world.create_body(
        static_box(Vec3{0.0f, -1.0f, 0.0f}, Vec3{50.0f, 1.0f, 50.0f}));
    REQUIRE(floor.has_value());
    auto ball = world.create_body(ball_at(Vec3{0.0f, 5.0f, 0.0f}, 0.3f));
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

    REQUIRE(world.set_gravity(Vec3{0.0f, 9.81f, 0.0f}).has_value());
    step_n(world, 60);

    state = world.body_state(*ball);
    REQUIRE(state.has_value());
    // Unwoken it would sit at 0.5 forever; the wake-all lifts it out.
    CHECK(state->position.y > 1.5f);
  }
}

TEST_CASE("3D is_bullet/set_bullet: LinearCast stops a fast body at a thin wall") {
  // The start offset phases the ticks so no end pose lands in the
  // overlap window (per-tick travel 1.67 m vs a 0.3 m window, clearance
  // 0.6+ m): Discrete sees no contact at all and walks through;
  // LinearCast sweeps the travel and stops the body at the wall face
  // (~19.85). Jolt's Discrete has NO auto-CCD - unlike Box2D, which
  // sweeps fast bodies vs statics for free - so the flag is the only
  // tunneling protection in 3D.
  const auto run = [](bool bullet, bool via_runtime) {
    auto world = make_world(World3D::Desc{.gravity = {0.0f, 0.0f, 0.0f},
                                          .thread_count = 0});
    REQUIRE(world.create_body(static_box(Vec3{20.0f, 0.0f, 0.0f},
                                         Vec3{0.05f, 5.0f, 5.0f}))
                .has_value());
    BodyDesc d = dynamic_box(Vec3{0.767f, 0.0f, 0.0f});
    d.shape = Sphere{0.1f};
    d.linear_velocity = Vec3{100.0f, 0.0f, 0.0f};
    if (!via_runtime) {
      d.is_bullet = bullet;
    }
    auto id = world.create_body(d);
    REQUIRE(id.has_value());
    if (via_runtime) {
      REQUIRE(world.set_bullet(*id, bullet).has_value());
    }
    step_n(world, 30);  // half a second: 50 m of clear-track travel
    auto state = world.body_state(*id);
    REQUIRE(state.has_value());
    return state->position.x;
  };
  CHECK(run(false, false) > 30.0f);  // discrete tunneled: far past the wall
  CHECK(run(true, false) < 20.0f);   // created a bullet: stopped at the face
  CHECK(run(false, true) > 30.0f);  // runtime flip OFF: same discrete body
  CHECK(run(true, true) < 20.0f);   // runtime flip ON: stops too
}

TEST_CASE("3D weld holds the pair rigid; destroying a body cascades the "
          "joint away") {
  auto world = make_world(World3D::Desc{.gravity = Vec3{0.0f, 0.0f, 0.0f},
                                        .thread_count = 0});
  auto a = world.create_body(dynamic_box(Vec3{0.0f, 0.0f, 0.0f}));
  auto b = world.create_body(dynamic_box(Vec3{1.0f, 0.0f, 0.0f}));
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());  // touching at x = 0.5: the classic weld setup
  auto weld = world.create_joint(FixedJoint{*a, *b});
  REQUIRE(weld.has_value());

  // Drive the pair, like the 2D twin: a passive weld cannot create
  // momentum, so kicking only a would leave the pair drifting at 0.5 m/s
  // (conservation) - not the 1.0 the rest of this case pins. What the weld
  // adds is keeping the two at ONE velocity and one separation.
  REQUIRE(world.set_velocity(*a, Vec3{1.0f, 0.0f, 0.0f}, Vec3{})
              .has_value());
  REQUIRE(world.set_velocity(*b, Vec3{1.0f, 0.0f, 0.0f}, Vec3{})
              .has_value());  // wake both
  step_n(world, 60);

  auto sa = world.body_state(*a);
  auto sb = world.body_state(*b);
  REQUIRE(sa.has_value());
  REQUIRE(sb.has_value());
  CHECK(sa->position.x == doctest::Approx(1.0f).epsilon(0.05));
  CHECK(sb->position.x == doctest::Approx(2.0f).epsilon(0.05));
  CHECK(std::abs(sb->position.x - sa->position.x - 1.0f) < 0.05f);
  CHECK(std::abs(sb->position.y) < 1e-3f);  // zero-g: no sag, no drift
  CHECK(sb->linear_velocity.x == doctest::Approx(1.0f).epsilon(0.05));

  // The cascade: destroying a removes the weld with it, so the joint
  // handle dies with the body it connected.
  REQUIRE(world.destroy_body(*a).has_value());
  CHECK(has_code(world.destroy_joint(*weld), PhysicsError::invalid_handle));

  // b is now free and keeps its welded drift velocity.
  const float before = sb->position.x;
  step_n(world, 10);
  sb = world.body_state(*b);
  REQUIRE(sb.has_value());
  CHECK(sb->position.x > before + 0.1f);
}

TEST_CASE("3D distance joint holds the rod; destroy + impulse frees it") {
  auto world = make_world(World3D::Desc{.gravity = Vec3{0.0f, 0.0f, 0.0f},
                                        .thread_count = 0});
  auto a = world.create_body(dynamic_box(Vec3{0.0f, 0.0f, 0.0f}));
  auto b = world.create_body(dynamic_box(Vec3{2.0f, 0.0f, 0.0f}));
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  // Rod length == the initial separation: born unstressed. Note the joint
  // owns the length, not the initial pose - same contract as 2D.
  auto rod = world.create_joint(DistanceJoint{*a, *b, 2.0f});
  REQUIRE(rod.has_value());

  // Fling b outward: the rod must hold 2 m while momentum spreads over
  // the pair (1 kg each, total 3 kg*m/s -> both end up near +1.5 m/s).
  REQUIRE(world.set_velocity(*b, Vec3{3.0f, 0.0f, 0.0f}, Vec3{})
              .has_value());
  step_n(world, 60);

  auto sa = world.body_state(*a);
  auto sb = world.body_state(*b);
  REQUIRE(sa.has_value());
  REQUIRE(sb.has_value());
  CHECK(std::abs((sb->position.x - sa->position.x) - 2.0f) < 0.1f);
  CHECK(sa->position.x > 1.0f);   // dragged along by the rod
  CHECK(sb->position.x > 3.0f);   // but held at arm's length

  // Snap the rod and shove: the separation now grows past every bound
  // the rod ever enforced.
  REQUIRE(world.destroy_joint(*rod).has_value());
  REQUIRE(world.apply_linear_impulse(*b, Vec3{2.0f, 0.0f, 0.0f}).has_value());
  step_n(world, 30);
  sa = world.body_state(*a);
  sb = world.body_state(*b);
  REQUIRE(sa.has_value());
  REQUIRE(sb.has_value());
  CHECK(sb->position.x - sa->position.x > 2.5f);
}

TEST_CASE("3D hinge motor spins the arm; limits clamp the swing") {
  auto world = make_world(World3D::Desc{.gravity = Vec3{0.0f, 0.0f, 0.0f},
                                        .thread_count = 0});
  // Two independent pairs in one world: an unconstrained motorized hinge,
  // and the same hinge driven INTO its limits.
  const Vec3 anchor1_pos{0.0f, 0.0f, 0.0f};
  const Vec3 anchor2_pos{0.0f, 5.0f, 0.0f};
  auto anchor1 = world.create_body(static_box(anchor1_pos,
                                               Vec3{0.25f, 0.25f, 0.25f}));
  auto arm1 = world.create_body(dynamic_box(Vec3{1.0f, 0.0f, 0.0f}));
  auto anchor2 = world.create_body(static_box(anchor2_pos,
                                               Vec3{0.25f, 0.25f, 0.25f}));
  auto arm2 = world.create_body(dynamic_box(Vec3{1.0f, 5.0f, 0.0f}));
  REQUIRE(anchor1.has_value());
  REQUIRE(arm1.has_value());
  REQUIRE(anchor2.has_value());
  REQUIRE(arm2.has_value());

  HingeJoint free_hinge{};
  free_hinge.a = *anchor1;
  free_hinge.b = *arm1;
  free_hinge.world_anchor = anchor1_pos;
  free_hinge.enable_motor = true;
  free_hinge.motor_speed = 2.0f;  // rad/s about +Y
  free_hinge.max_motor_torque = 100.0f;
  auto j1 = world.create_joint(free_hinge);
  REQUIRE(j1.has_value());

  HingeJoint limited_hinge = free_hinge;
  limited_hinge.a = *anchor2;
  limited_hinge.b = *arm2;
  limited_hinge.world_anchor = anchor2_pos;
  limited_hinge.enable_limits = true;
  limited_hinge.lower_angle = -0.25f;
  limited_hinge.upper_angle = 0.25f;
  auto j2 = world.create_joint(limited_hinge);
  REQUIRE(j2.has_value());

  step_n(world, 60);  // one second of swing

  // Right-handed rotation about +Y moves +X toward -Z, so the swing angle
  // reads as atan2(-z, x) on the arm's COM (radius 1 from the anchor).
  auto s1 = world.body_state(*arm1);
  auto s2 = world.body_state(*arm2);
  REQUIRE(s1.has_value());
  REQUIRE(s2.has_value());
  const float swung = std::atan2(-s1->position.z, s1->position.x);
  CHECK(swung == doctest::Approx(2.0f).epsilon(0.15));  // 2 rad/s for 1 s
  const float clamped =
      std::atan2(-(s2->position.z - anchor2_pos.z), s2->position.x);
  CHECK(clamped == doctest::Approx(0.25f).epsilon(0.05));  // the limit won
  // The hinge pins the arm to its swing plane through the anchor: no
  // drift off y, whatever pushes on it.
  CHECK(std::abs(s2->position.y - anchor2_pos.y) < 0.05f);
}

TEST_CASE("3D raycast hits the near face; misses report nullopt") {
  auto world = make_world(World3D::Desc{.thread_count = 0});
  auto box = world.create_body(static_box(Vec3{0.0f, 0.0f, 0.0f},
                                           Vec3{0.5f, 0.5f, 0.5f}));
  REQUIRE(box.has_value());

  // Straight up at the box's underside: 4.5 m of travel, the hit reports
  // distance-along-ray (not a fraction), the point on the surface, and a
  // normal facing BACK toward the origin.
  auto hit = world.raycast(Vec3{0.0f, -5.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},
                           10.0f);
  REQUIRE(hit.has_value());
  REQUIRE(hit->has_value());
  CHECK((*hit)->body == *box);
  CHECK((*hit)->t == doctest::Approx(4.5f).epsilon(0.01));
  CHECK((*hit)->point.y == doctest::Approx(-0.5f).epsilon(0.01));
  CHECK(std::abs((*hit)->point.x) < 0.01f);
  CHECK((*hit)->normal.y == doctest::Approx(-1.0f).epsilon(0.01));
  CHECK(std::abs((*hit)->normal.x) < 0.01f);

  // Too short a reach: the surface is 4.5 m out, 4.0 m is not enough.
  auto short_ray = world.raycast(Vec3{0.0f, -5.0f, 0.0f},
                                 Vec3{0.0f, 1.0f, 0.0f}, 4.0f);
  REQUIRE(short_ray.has_value());
  CHECK_FALSE(short_ray->has_value());

  // Parallel and above: never intersects.
  auto parallel = world.raycast(Vec3{-5.0f, 2.0f, 0.0f}, Vec3{1.0f, 0.0f,
                                                                0.0f},
                                10.0f);
  REQUIRE(parallel.has_value());
  CHECK_FALSE(parallel->has_value());
}

TEST_CASE("3D overlap queries return exactly the touching bodies, "
          "index-sorted") {
  auto world = make_world(World3D::Desc{.thread_count = 0});
  auto near_a = world.create_body(static_box(Vec3{0.0f, 0.0f, 0.0f},
                                             Vec3{0.5f, 0.5f, 0.5f}));
  auto near_b = world.create_body(static_box(Vec3{10.0f, 0.0f, 0.0f},
                                             Vec3{0.5f, 0.5f, 0.5f}));
  auto far = world.create_body(static_box(Vec3{100.0f, 0.0f, 0.0f},
                                          Vec3{0.5f, 0.5f, 0.5f}));
  REQUIRE(near_a.has_value());
  REQUIRE(near_b.has_value());
  REQUIRE(far.has_value());

  // A fat sphere between the two near boxes (broadphase fidelity: Box2D's
  // overlap queries are fat-AABB tree walks too - the dimensions agree).
  auto hits = world.overlap_sphere(Vec3{5.0f, 0.0f, 0.0f}, 6.0f);
  REQUIRE(hits.has_value());
  REQUIRE(hits->size() == 2);
  CHECK((*hits)[0] == *near_a);  // ids_for_slots sorts: creation order
  CHECK((*hits)[1] == *near_b);

  auto boxed = world.overlap_aabb(Aabb{Vec3{-1.0f, -1.0f, -1.0f},
                                       Vec3{11.0f, 1.0f, 1.0f}});
  REQUIRE(boxed.has_value());
  REQUIRE(boxed->size() == 2);
  CHECK((*boxed)[0] == *near_a);
  CHECK((*boxed)[1] == *near_b);

  auto empty = world.overlap_sphere(Vec3{50.0f, 50.0f, 50.0f}, 1.0f);
  REQUIRE(empty.has_value());
  CHECK(empty->empty());
}

TEST_CASE("3D moved_bodies: the kinematic platform reports every tick; "
          "statics never do") {
  auto world = make_world(World3D::Desc{.thread_count = 0});
  auto floor = world.create_body(static_box(Vec3{0.0f, -10.0f, 0.0f},
                                             Vec3{50.0f, 1.0f, 50.0f}));
  BodyDesc platform_desc{};
  platform_desc.kind = ws::physics::BodyKind::kinematic;
  platform_desc.shape = Box{Vec3{2.0f, 0.25f, 2.0f}};
  platform_desc.transform = Transform{Vec3{0.0f, 0.0f, 0.0f}, Quat{}};
  platform_desc.linear_velocity = Vec3{2.0f, 0.0f, 0.0f};
  auto platform = world.create_body(platform_desc);
  REQUIRE(floor.has_value());
  REQUIRE(platform.has_value());

  // Ten ticks of the platform gliding over the floor: exactly one mover
  // every tick, and it is always the platform - the static floor wakes
  // for nothing.
  for (int i = 0; i < 10; ++i) {
    REQUIRE(world.step().has_value());
    const auto moved = world.moved_bodies();
    REQUIRE(moved.size() == 1);
    CHECK(moved[0].id == *platform);
  }
  step_n(world, 50);
  const auto pose = world.transforms()[platform->index];
  CHECK(pose.position.x == doctest::Approx(2.0f).epsilon(0.02));  // 2 m/s
  CHECK(pose.position.y == doctest::Approx(0.0f).epsilon(0.001));

  // A fresh body is born awake: its prev pose is its creation pose, and
  // the first tick moves cur off it while prev holds.
  BodyDesc falling = dynamic_box(Vec3{0.0f, 5.0f, 0.0f});
  falling.linear_velocity = Vec3{0.0f, -1.0f, 0.0f};
  auto fresh = world.create_body(falling);
  REQUIRE(fresh.has_value());
  CHECK(world.transforms()[fresh->index].position.y ==
        doctest::Approx(5.0f));
  CHECK(world.prev_transforms()[fresh->index].position.y ==
        doctest::Approx(5.0f));
  REQUIRE(world.step().has_value());
  CHECK(world.transforms()[fresh->index].position.y < 5.0f);
  CHECK(world.prev_transforms()[fresh->index].position.y ==
        doctest::Approx(5.0f));  // the pre-tick pose, kept for lerping
}

TEST_CASE("3D sleep control: the wake rules are the lib's, not the engine's") {
  auto world = make_world();
  auto floor = world.create_body(static_box(Vec3{0.0f, -1.0f, 0.0f},
                                            Vec3{20.0f, 1.0f, 20.0f}));
  REQUIRE(floor.has_value());

  // Every is_sleeping read below flows through this: a failed read must
  // never look like a pass.
  const auto sleeping = [&world](BodyId id) {
    const auto result = world.is_sleeping(id);
    REQUIRE(result.has_value());
    return *result;
  };

  // Twins from the same height: one pinned awake for life at create, one
  // left free to doze off.
  BodyDesc pinned_desc = dynamic_box(Vec3{-2.0f, 0.6f, 0.0f});
  pinned_desc.allow_sleep = false;
  auto pinned = world.create_body(pinned_desc);
  REQUIRE(pinned.has_value());
  auto twin = world.create_body(dynamic_box(Vec3{2.0f, 0.6f, 0.0f}));
  REQUIRE(twin.has_value());

  // Both settle onto the floor; only the free twin falls asleep (bounded
  // wait - a sleeper must appear or the test says so, no hang).
  bool slept = false;
  for (int i = 0; i < 300 && !slept; ++i) {
    REQUIRE(world.step().has_value());
    slept = sleeping(*twin);
  }
  REQUIRE(slept);
  CHECK(sleeping(*pinned) == false);
  CHECK(sleeping(*floor) == false);  // statics never sleep (CONTRACT.md 6)

  // wake() is unconditional; no step is needed to observe it.
  REQUIRE(world.wake(*twin).has_value());
  CHECK(sleeping(*twin) == false);

  // Resettle, then set_velocity: the wake rule is "iff the new velocity is
  // nonzero" (CONTRACT.md 6) - a zero set must leave a sleeper asleep.
  slept = false;
  for (int i = 0; i < 300 && !slept; ++i) {
    REQUIRE(world.step().has_value());
    slept = sleeping(*twin);
  }
  REQUIRE(slept);
  REQUIRE(world.set_velocity(*twin, Vec3{0.0f, 0.0f, 0.0f},
                             Vec3{0.0f, 0.0f, 0.0f}).has_value());
  CHECK(sleeping(*twin));
  REQUIRE(world.set_velocity(*twin, Vec3{0.1f, 0.0f, 0.0f},
                             Vec3{0.0f, 0.0f, 0.0f}).has_value());
  CHECK(sleeping(*twin) == false);

  // Resettle, then set_sleepable: permitting sleep never wakes; forbidding
  // it wakes immediately, even from sleep.
  slept = false;
  for (int i = 0; i < 300 && !slept; ++i) {
    REQUIRE(world.step().has_value());
    slept = sleeping(*twin);
  }
  REQUIRE(slept);
  REQUIRE(world.set_sleepable(*twin, true).has_value());
  CHECK(sleeping(*twin));
  REQUIRE(world.set_sleepable(*twin, false).has_value());
  CHECK(sleeping(*twin) == false);

  // The pinned twin stayed awake through all of it.
  CHECK(sleeping(*pinned) == false);
}

TEST_CASE("3D steady state steps without one heap allocation") {
  World3D::Desc desc{};
  desc.max_bodies = 64;
  desc.max_contact_events = 512;
  desc.thread_count = 2;  // the threaded engine must hold the line too
  auto world = make_world(desc);
  auto floor = world.create_body(static_box(Vec3{0.0f, -10.0f, 0.0f},
                                             Vec3{50.0f, 1.0f, 50.0f}));
  REQUIRE(floor.has_value());

  std::vector<BodyId> ids;
  for (int i = 0; i < 24; ++i) {
    // Two capsules ride the mix so the capsule shape path is warm too;
    // everything else exercises the box path.
    BodyDesc d{};
    if (i == 20 || i == 21) {
      d.shape = Capsule{0.5f, 0.25f};
    } else {
      d.shape = Box{Vec3{0.5f, 0.5f, 0.5f}};
    }
    d.transform = Transform{Vec3{-3.0f + 0.6f * i, 20.0f + i, 0.0f},
                             Quat{}};
    auto id = world.create_body(d);
    REQUIRE(id.has_value());
    ids.push_back(*id);
  }
  auto weld = world.create_joint(FixedJoint{ids[0], ids[1]});
  REQUIRE(weld.has_value());
  auto rod = world.create_joint(DistanceJoint{ids[2], ids[3], 1.0f});
  REQUIRE(rod.has_value());

  step_n(world, 30);  // warm-up: whatever grows once must grow HERE

  // The window mixes EVERY mid-tick control call - pose batches, an
  // impulse, a read, and a full body destroy + spawn - because that is
  // exactly where the old BodyID->slot map would have node-allocated.
  const long long before = allocation_count();
  for (int i = 0; i < 60; ++i) {
    REQUIRE(world.step().has_value());
    if (i == 30) {
      const BodyMove batch[]{BodyMove{ids[4], Vec3{0.0f, 30.0f, 0.0f},
                                      Quat{}}};
      REQUIRE(world.set_transforms(batch).has_value());
      REQUIRE(world.apply_linear_impulse(ids[5],
                                         Vec3{0.5f, 1.0f, 0.0f})
                  .has_value());
      auto state = world.body_state(ids[6]);
      REQUIRE(state.has_value());
    }
    if (i == 45) {
      REQUIRE(world.destroy_body(ids[7]).has_value());
      auto fresh = world.create_body(dynamic_box(Vec3{2.0f, 25.0f, 0.0f}));
      REQUIRE(fresh.has_value());
      // A compound spawn mid-window: parts ride a fixed array (the desc
      // copy stays free), the shape cache stays hit-only because BOTH part
      // shapes are already warm from the rain bodies, and the flat part
      // tables were sized at construction.
      BodyDesc compound = dynamic_box(Vec3{-2.0f, 25.0f, 0.0f});
      compound.parts[0] = Part{Box{Vec3{0.5f, 0.5f, 0.5f}}, Vec3{}, 0.2f,
                                0.0f};
      compound.parts[1] = Part{Capsule{0.5f, 0.25f},
                               Vec3{1.5f, 0.0f, 0.0f}, 0.2f, 0.0f};
      compound.part_count = 2;
      auto compound_id = world.create_body(compound);
      REQUIRE(compound_id.has_value());
    }
  }
  CHECK(allocation_count() - before == 0);
}

TEST_CASE("3D determinism: same binary, same thread count, same sequence") {
  const auto replay_hash = [](std::uint32_t thread_count) {
    World3D::Desc desc{};
    desc.thread_count = thread_count;
    desc.max_bodies = 64;
    auto world = make_world(desc);
    auto floor = world.create_body(static_box(Vec3{0.0f, -10.0f, 0.0f},
                                               Vec3{50.0f, 1.0f, 50.0f}));
    REQUIRE(floor.has_value());
    std::vector<BodyId> ids;
    for (int i = 0; i < 5; ++i) {
      BodyDesc d = dynamic_box(Vec3{-3.0f + 1.5f * i, 5.0f + 0.7f * i,
                                    0.0f});
      d.restitution = 0.3f;
      auto id = world.create_body(d);
      REQUIRE(id.has_value());
      REQUIRE(world.set_velocity(*id, Vec3{0.3f * i, -1.0f, 0.0f}, Vec3{})
                  .has_value());
      ids.push_back(*id);
    }
    for (int s = 0; s < 120; ++s) {
      if (s % 30 == 0) {
        REQUIRE(world.apply_linear_impulse(ids[(s / 30) % ids.size()],
                                           Vec3{0.5f, 1.0f, 0.0f})
                    .has_value());
      }
      REQUIRE(world.step().has_value());
    }
    // FNV-1a over the raw bytes of every body's full 3D pose (position +
    // quaternion - the quat is the stricter witness, any solver drift
    // shows in its low bits): bit-identical replays hash identically.
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
      mix(&state->position.z, sizeof(float));
      mix(&state->rotation.x, sizeof(float));
      mix(&state->rotation.y, sizeof(float));
      mix(&state->rotation.z, sizeof(float));
      mix(&state->rotation.w, sizeof(float));
    }
    return hash;
  };

  const auto serial_a = replay_hash(0);
  const auto serial_b = replay_hash(0);
  CHECK(serial_a == serial_b);  // the contract: same thread count replays
  const auto threaded_a = replay_hash(2);
  const auto threaded_b = replay_hash(2);
  CHECK(threaded_a == threaded_b);
}
