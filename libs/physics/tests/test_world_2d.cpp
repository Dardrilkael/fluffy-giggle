// No DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here - test_common.cpp owns doctest's
// main (the net test-suite pattern: one binary, several TUs).
//
// These cases pin the BACKEND-INDEPENDENT machinery through the real Box2D
// backend: generational handle lifetime, capacity, validation, the
// accumulator's exact alpha semantics, bulk-set atomicity and the
// event-buffer discipline. Engine BEHAVIOR (gravity, collision response,
// contact events, queries) is test_world_2d_box2d.cpp's job.

#include <doctest/doctest.h>

#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

#include "check.hpp"

#include <ws/math/vecmath.hpp>
#include <ws/physics/2d/joint.hpp>
#include <ws/physics/2d/world.hpp>

using ws::physics::PhysicsError;
using ws::physics2d::BodyDesc;
using ws::physics2d::BodyId;
using ws::physics2d::BodyMove;
using ws::physics2d::Box;
using ws::physics2d::Capsule;
using ws::physics2d::Circle;
using ws::physics2d::DistanceJoint;
using ws::physics2d::FixedJoint;
using ws::physics2d::JointId;
using ws::physics2d::RevoluteJoint;
using ws::physics2d::Transform;
using ws::physics2d::World2D;
using wsphys::test::has_code;

namespace {

[[nodiscard]] World2D make_world(World2D::Desc desc = {}) {
  auto world = World2D::create(desc);
  REQUIRE(world.has_value());
  return std::move(*world);
}

[[nodiscard]] BodyDesc body_at(ws::math::Vec2 position, float rotation = 0.0f) {
  BodyDesc d{};
  d.transform = Transform{position, rotation};
  return d;
}

}  // namespace

TEST_CASE("create validates Desc") {
  World2D::Desc desc{};

  SUBCASE("fixed_dt must be finite and > 0") {
    desc.fixed_dt = 0.0f;
    CHECK(has_code(World2D::create(desc), PhysicsError::invalid_argument));
    desc.fixed_dt = -1.0f;
    CHECK(has_code(World2D::create(desc), PhysicsError::invalid_argument));
    desc.fixed_dt = std::nanf("");
    CHECK(has_code(World2D::create(desc), PhysicsError::invalid_argument));
  }
  SUBCASE("caps are enforced") {
    desc.max_bodies = ws::physics::kMaxBodiesPerWorld + 1;
    CHECK(has_code(World2D::create(desc), PhysicsError::invalid_argument));
  }
  SUBCASE("max_substeps must be >= 1") {
    desc.max_substeps = 0;
    CHECK(has_code(World2D::create(desc), PhysicsError::invalid_argument));
  }
  SUBCASE("hit_speed_threshold must be finite and >= 0") {
    desc.hit_speed_threshold = -0.1f;
    CHECK(has_code(World2D::create(desc), PhysicsError::invalid_argument));
  }
  SUBCASE("gravity must be finite") {
    desc.gravity = ws::math::Vec2{std::nanf(""), 0.0f};
    CHECK(has_code(World2D::create(desc), PhysicsError::invalid_argument));
  }
  SUBCASE("a valid Desc passes") {
    CHECK(World2D::create(desc).has_value());
    CHECK(World2D::create().has_value());  // defaults are valid
  }
}

TEST_CASE("body lifetime is generational: stale handles error, never alias") {
  auto world = make_world();
  auto first = world.create_body(body_at({1.0f, 2.0f}, 0.25f));
  REQUIRE(first.has_value());
  CHECK(first->index == 0);            // dense slots from 0
  CHECK(first->generation != 0);        // zero handle never issued

  auto second = world.create_body(BodyDesc{});
  REQUIRE(second.has_value());
  CHECK(second->index != first->index);

  SUBCASE("destroy retires the handle everywhere") {
    CHECK(world.destroy_body(*first).has_value());

    CHECK(has_code(world.body_state(*first), PhysicsError::invalid_handle));
    CHECK(has_code(world.body_user_data(*first), PhysicsError::invalid_handle));
    CHECK(has_code(world.set_body_user_data(*first, 1), PhysicsError::invalid_handle));
    CHECK(has_code(world.destroy_body(*first), PhysicsError::invalid_handle));
    CHECK(has_code(world.set_transform(*first, {}, 0.0f), PhysicsError::invalid_handle));
    CHECK(has_code(world.set_velocity(*first, {}, 0.0f), PhysicsError::invalid_handle));
    CHECK(has_code(world.set_collision_filter(*first, 1u, 0xffffffffu),
                   PhysicsError::invalid_handle));
    CHECK(has_code(world.apply_linear_impulse(*first, {}), PhysicsError::invalid_handle));
    CHECK(has_code(world.apply_angular_impulse(*first, 0.0f), PhysicsError::invalid_handle));
    CHECK(has_code(world.apply_force(*first, {}), PhysicsError::invalid_handle));
    CHECK(has_code(world.apply_torque(*first, 0.0f), PhysicsError::invalid_handle));
    CHECK(has_code(world.body_state(BodyId{}), PhysicsError::invalid_handle));
    CHECK(has_code(world.destroy_body(BodyId{}), PhysicsError::invalid_handle));
  }

  SUBCASE("the freed slot is reused with a bumped generation") {
    const BodyId stale = *first;
    CHECK(world.destroy_body(stale).has_value());
    auto revived = world.create_body(BodyDesc{});
    REQUIRE(revived.has_value());
    CHECK(revived->index == stale.index);
    CHECK(revived->generation == stale.generation + 1);
    CHECK(*revived != stale);
    // The stale handle still errors - it did not resurrect.
    CHECK(has_code(world.body_state(stale), PhysicsError::invalid_handle));
  }
}

TEST_CASE("user data is a lib-owned payload: set, read, retire, reuse") {
  auto world = make_world(World2D::Desc{.max_bodies = 2});

  BodyDesc desc{};
  desc.user_data = 0xBEEF;
  const auto body = world.create_body(desc);
  REQUIRE(body.has_value());
  auto data = world.body_user_data(*body);
  REQUIRE(data.has_value());
  CHECK(*data == 0xBEEF);

  SUBCASE("set_body_user_data overwrites") {
    CHECK(world.set_body_user_data(*body, 42).has_value());
    data = world.body_user_data(*body);
    REQUIRE(data.has_value());
    CHECK(*data == 42);
  }

  SUBCASE("a default body reads 0") {
    const auto plain = world.create_body(BodyDesc{});
    REQUIRE(plain.has_value());
    data = world.body_user_data(*plain);
    REQUIRE(data.has_value());
    CHECK(*data == 0);
  }

  SUBCASE("slot reuse never leaks the retired body's payload") {
    const BodyId stale = *body;
    CHECK(world.destroy_body(stale).has_value());
    BodyDesc revived_desc{};
    revived_desc.user_data = 7;
    const auto revived = world.create_body(revived_desc);
    REQUIRE(revived.has_value());
    CHECK(revived->index == stale.index);  // same slot, fresh payload
    data = world.body_user_data(*revived);
    REQUIRE(data.has_value());
    CHECK(*data == 7);
    CHECK(has_code(world.body_user_data(stale), PhysicsError::invalid_handle));
  }

  CHECK(has_code(world.body_user_data(BodyId{}), PhysicsError::invalid_handle));
  CHECK(has_code(world.set_body_user_data(BodyId{}, 1),
                 PhysicsError::invalid_handle));
}

TEST_CASE("world_full is exact capacity, not a suggestion") {
  auto world = make_world(World2D::Desc{.max_bodies = 2, .max_joints = 2});
  const auto first = world.create_body(BodyDesc{});
  const auto second = world.create_body(BodyDesc{});
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  CHECK(has_code(world.create_body(BodyDesc{}), PhysicsError::world_full));

  SUBCASE("destroying opens exactly one slot") {
    CHECK(world.destroy_body(*first).has_value());
    const auto revived = world.create_body(BodyDesc{});
    REQUIRE(revived.has_value());
    CHECK(revived->index == first->index);  // the destroyed slot, reused
    CHECK(revived->generation == first->generation + 1);
    CHECK(has_code(world.create_body(BodyDesc{}), PhysicsError::world_full));
  }
}

TEST_CASE("a zero-capacity world refuses the first body") {
  auto world = make_world(World2D::Desc{.max_bodies = 0, .max_joints = 0});
  CHECK(has_code(world.create_body(BodyDesc{}), PhysicsError::world_full));
}

TEST_CASE("create_body validates shapes and material values") {
  auto world = make_world();

  SUBCASE("circle radius must be > 0") {
    BodyDesc d{};
    d.shape = Circle{0.0f};
    CHECK(has_code(world.create_body(d), PhysicsError::invalid_argument));
  }
  SUBCASE("box half extents must be > 0") {
    BodyDesc d{};
    d.shape = Box{ws::math::Vec2{0.5f, 0.0f}};
    CHECK(has_code(world.create_body(d), PhysicsError::invalid_argument));
  }
  SUBCASE("capsule needs a != b") {
    BodyDesc d{};
    d.shape = Capsule{ws::math::Vec2{0.0f, -0.5f}, ws::math::Vec2{0.0f, -0.5f}, 0.5f};
    CHECK(has_code(world.create_body(d), PhysicsError::invalid_argument));
  }
  SUBCASE("dynamic bodies need density > 0") {
    BodyDesc d{};
    d.density = 0.0f;
    CHECK(has_code(world.create_body(d), PhysicsError::invalid_argument));
  }
  SUBCASE("NaN transforms are rejected") {
    BodyDesc d = body_at({std::nanf(""), 0.0f});
    CHECK(has_code(world.create_body(d), PhysicsError::invalid_argument));
  }
  SUBCASE("static bodies may carry density 0 (it is unused)") {
    BodyDesc d{};
    d.kind = ws::physics::BodyKind::static_;
    d.density = 0.0f;
    CHECK(world.create_body(d).has_value());
  }
  SUBCASE("friction and restitution must be >= 0") {
    BodyDesc d{};
    d.friction = -0.1f;
    CHECK(has_code(world.create_body(d), PhysicsError::invalid_argument));
    d.friction = 0.2f;
    d.restitution = -1.0f;
    CHECK(has_code(world.create_body(d), PhysicsError::invalid_argument));
  }
}

TEST_CASE("runtime setters reject non-finite values before the engine sees them") {
  auto world = make_world();
  auto id = world.create_body(BodyDesc{});
  REQUIRE(id.has_value());
  const auto nan = std::nanf("");

  CHECK(has_code(world.set_transform(*id, {nan, 0.0f}, 0.0f),
                 PhysicsError::invalid_argument));
  CHECK(has_code(world.set_transform(*id, {0.0f, nan}, 0.0f),
                 PhysicsError::invalid_argument));
  CHECK(has_code(world.set_transform(*id, {0.0f, 0.0f}, nan),
                 PhysicsError::invalid_argument));
  CHECK(has_code(world.set_velocity(*id, {nan, 0.0f}, 0.0f),
                 PhysicsError::invalid_argument));
  CHECK(has_code(world.set_velocity(*id, {0.0f, 0.0f}, nan),
                 PhysicsError::invalid_argument));
  CHECK(has_code(world.apply_linear_impulse(*id, {0.0f, nan}),
                 PhysicsError::invalid_argument));
  CHECK(has_code(world.apply_angular_impulse(*id, nan),
                 PhysicsError::invalid_argument));
  CHECK(has_code(world.apply_force(*id, {nan, 0.0f}),
                 PhysicsError::invalid_argument));
  CHECK(has_code(world.apply_torque(*id, nan),
                 PhysicsError::invalid_argument));

  // The body survived all of that un-corrupted.
  auto state = world.body_state(*id);
  REQUIRE(state.has_value());
  CHECK(std::isfinite(state->position.x));
  CHECK(std::isfinite(state->position.y));
  CHECK(std::isfinite(state->rotation));
}

TEST_CASE("body_state echoes the pose; the mirror serves transforms()") {
  auto world = make_world(World2D::Desc{.max_bodies = 8});
  auto id = world.create_body(body_at({3.0f, -4.0f}, 0.5f));
  REQUIRE(id.has_value());

  auto state = world.body_state(*id);
  REQUIRE(state.has_value());
  CHECK(state->position.x == doctest::Approx(3.0f));
  CHECK(state->position.y == doctest::Approx(-4.0f));
  CHECK(state->rotation == doctest::Approx(0.5f));
  CHECK(state->linear_velocity == ws::math::Vec2{});
  CHECK(state->angular_velocity == doctest::Approx(0.0f));

  // Slot-indexed bulk mirror: one entry per capacity slot.
  const auto poses = world.transforms();
  CHECK(poses.size() == 8);
  CHECK(poses[id->index].position.x == doctest::Approx(3.0f));

  // set_transform updates both the readback and the mirror.
  CHECK(world.set_transform(*id, {10.0f, 20.0f}, 1.0f).has_value());
  auto moved = world.body_state(*id);
  REQUIRE(moved.has_value());
  CHECK(moved->position.x == doctest::Approx(10.0f));
  CHECK(moved->position.y == doctest::Approx(20.0f));
  CHECK(moved->rotation == doctest::Approx(1.0f));
  CHECK(world.transforms()[id->index].position.y == doctest::Approx(20.0f));
}

TEST_CASE("set_transforms is atomic: one stale id moves nothing") {
  auto world = make_world(World2D::Desc{.max_bodies = 8});
  auto a = world.create_body(body_at({0.0f, 0.0f}));
  auto b = world.create_body(body_at({1.0f, 1.0f}));
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());

  const BodyId stale = *b;
  CHECK(world.destroy_body(stale).has_value());

  const BodyMove good{*a, {100.0f, 100.0f}, 0.0f};
  const BodyMove bad{stale, {200.0f, 200.0f}, 0.0f};
  const std::vector<BodyMove> mixed{good, bad};
  CHECK(has_code(world.set_transforms(mixed), PhysicsError::invalid_handle));

  auto state = world.body_state(*a);
  REQUIRE(state.has_value());
  CHECK(state->position.x == doctest::Approx(0.0f));  // nothing moved
  CHECK(state->position.y == doctest::Approx(0.0f));

  // An empty batch is a valid no-op; a fully valid batch applies.
  CHECK(world.set_transforms({}).has_value());
  const std::vector<BodyMove> all_good{BodyMove{*a, {5.0f, 6.0f}, 0.25f}};
  CHECK(world.set_transforms(all_good).has_value());
  auto applied = world.body_state(*a);
  REQUIRE(applied.has_value());
  CHECK(applied->position.x == doctest::Approx(5.0f));
  CHECK(applied->rotation == doctest::Approx(0.25f));

  SUBCASE("one non-finite VALUE fails the whole batch too - ids and values are both atomic") {
    BodyMove bad_value{};
    bad_value.id = *a;
    bad_value.position = {std::nanf(""), 0.0f};
    const BodyMove good_value{*a, {7.0f, 8.0f}, 0.0f};
    const std::vector<BodyMove> batch_with_nan{good_value, bad_value};
    CHECK(has_code(world.set_transforms(batch_with_nan),
                   PhysicsError::invalid_argument));
    // doctest re-runs the whole case per subcase, so the earlier all_good
    // batch already applied: atomicity means the (5, 6) pose SURVIVES the
    // failed batch - its good entry did not sneak in.
    auto after = world.body_state(*a);
    REQUIRE(after.has_value());
    CHECK(after->position.x == doctest::Approx(5.0f));
    CHECK(after->position.y == doctest::Approx(6.0f));
    CHECK(after->rotation == doctest::Approx(0.25f));
  }
}

TEST_CASE("the accumulator drives alpha; the backlog is dropped at the clamp") {
  // 0.25 is exactly representable in binary - the alpha math is exact here.
  auto world = make_world(World2D::Desc{.fixed_dt = 0.25f, .max_bodies = 4});

  CHECK(world.advance(0.0f).has_value());
  CHECK(world.interpolation_alpha() == doctest::Approx(0.0f));

  CHECK(world.advance(0.125f).has_value());  // below one tick: no step
  CHECK(world.interpolation_alpha() == doctest::Approx(0.5f));

  CHECK(world.advance(0.125f).has_value());  // completes the tick exactly
  CHECK(world.interpolation_alpha() == doctest::Approx(0.0f));

  CHECK(world.advance(2.0f).has_value());  // exactly 8 ticks: fits the default cap
  CHECK(world.interpolation_alpha() == doctest::Approx(0.0f));

  SUBCASE("max_substeps caps the ticks and drops the backlog") {
    auto capped = make_world(
        World2D::Desc{.fixed_dt = 0.25f, .max_bodies = 4, .max_substeps = 3});
    CHECK(capped.advance(10.625f).has_value());
    // 42 ticks wanted, 3 run; backlog fmod-dropped, leaving 0.125/0.25.
    CHECK(capped.interpolation_alpha() == doctest::Approx(0.5f));
  }

  SUBCASE("negative or NaN frame time is invalid") {
    CHECK(has_code(world.advance(-1.0f), PhysicsError::invalid_argument));
    CHECK(has_code(world.advance(std::nanf("")), PhysicsError::invalid_argument));
  }
}

TEST_CASE("step and the event-buffer discipline hold without a backend") {
  auto world = make_world();
  CHECK(world.contact_events().empty());  // before any tick

  CHECK(world.step().has_value());
  CHECK(world.contact_events().empty());  // null backend: no events, valid span
  CHECK(world.contact_events().empty());  // still valid until the next tick

  // advance() runs ticks through the same path; alpha stays defined.
  CHECK(world.advance(1.0f / 60.0f).has_value());
  CHECK(world.interpolation_alpha() >= 0.0f);
  CHECK(world.interpolation_alpha() <= 1.0f);
  CHECK(world.fixed_dt() == doctest::Approx(1.0f / 60.0f));
}

TEST_CASE("joints validate, live and cascade with their bodies") {
  auto world = make_world(World2D::Desc{.max_bodies = 8, .max_joints = 4});
  auto a = world.create_body(BodyDesc{});
  auto b = world.create_body(BodyDesc{});
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());

  SUBCASE("a joint never binds a body to itself") {
    CHECK(has_code(world.create_joint(FixedJoint{*a, *a}),
                   PhysicsError::invalid_argument));
  }
  SUBCASE("joints need live bodies") {
    const BodyId stale{*b};
    CHECK(world.destroy_body(stale).has_value());
    CHECK(has_code(world.create_joint(FixedJoint{*a, stale}),
                   PhysicsError::invalid_handle));
    CHECK(has_code(world.create_joint(DistanceJoint{stale, *a, 1.0f}),
                   PhysicsError::invalid_handle));
  }
  SUBCASE("distance joints take length > 0") {
    CHECK(has_code(world.create_joint(DistanceJoint{*a, *b, -1.0f}),
                   PhysicsError::invalid_argument));
    // Zero is degenerate (that is a weld's job) and Box2D hard-asserts
    // length > 0 - the lib must reject it before the engine sees it.
    CHECK(has_code(world.create_joint(DistanceJoint{*a, *b, 0.0f}),
                   PhysicsError::invalid_argument));
    CHECK(world.create_joint(DistanceJoint{*a, *b, 0.5f}).has_value());
  }
  SUBCASE("revolute limits need lower <= upper when enabled") {
    RevoluteJoint bad{};
    bad.a = *a;
    bad.b = *b;
    bad.enable_limits = true;
    bad.lower_angle = 0.5f;
    bad.upper_angle = -0.5f;
    CHECK(has_code(world.create_joint(bad), PhysicsError::invalid_argument));

    RevoluteJoint ok_limits{};
    ok_limits.a = *a;
    ok_limits.b = *b;
    ok_limits.enable_limits = true;
    ok_limits.lower_angle = -0.5f;
    ok_limits.upper_angle = 0.5f;
    CHECK(world.create_joint(ok_limits).has_value());
  }
  SUBCASE("revolute limits must stay within one turn (+-0.99*pi)") {
    RevoluteJoint beyond{};
    beyond.a = *a;
    beyond.b = *b;
    beyond.enable_limits = true;
    beyond.lower_angle = -0.5f;
    beyond.upper_angle = 4.0f;  // beyond 0.99*pi
    CHECK(has_code(world.create_joint(beyond), PhysicsError::invalid_argument));

    beyond.lower_angle = -4.0f;  // below -0.99*pi
    beyond.upper_angle = 0.5f;
    CHECK(has_code(world.create_joint(beyond), PhysicsError::invalid_argument));
  }
  SUBCASE("a joint needs at least one dynamic body") {
    BodyDesc static_desc{};
    static_desc.kind = ws::physics::BodyKind::static_;
    auto s1 = world.create_body(static_desc);
    auto s2 = world.create_body(static_desc);
    REQUIRE(s1.has_value());
    REQUIRE(s2.has_value());
    CHECK(has_code(world.create_joint(FixedJoint{*s1, *s2}),
                   PhysicsError::invalid_argument));
    // Dynamic-to-static is the normal anchor case.
    CHECK(world.create_joint(FixedJoint{*a, *s1}).has_value());
  }
  SUBCASE("an enabled motor needs max_motor_torque > 0") {
    RevoluteJoint bad{};
    bad.a = *a;
    bad.b = *b;
    bad.enable_motor = true;
    bad.max_motor_torque = 0.0f;
    CHECK(has_code(world.create_joint(bad), PhysicsError::invalid_argument));

    bad.enable_motor = false;  // motor off: garbage torque is ignored
    CHECK(world.create_joint(bad).has_value());
  }
  SUBCASE("destroy_joint is generational like bodies") {
    auto joint = world.create_joint(FixedJoint{*a, *b});
    REQUIRE(joint.has_value());
    CHECK(joint->generation != 0);

    CHECK(world.destroy_joint(*joint).has_value());
    CHECK(has_code(world.destroy_joint(*joint), PhysicsError::invalid_handle));
    CHECK(has_code(world.destroy_joint(JointId{}), PhysicsError::invalid_handle));
  }
  SUBCASE("destroying a body cascades to its joints") {
    auto joint = world.create_joint(FixedJoint{*a, *b});
    REQUIRE(joint.has_value());
    CHECK(world.destroy_body(*a).has_value());
    CHECK(has_code(world.destroy_joint(*joint), PhysicsError::invalid_handle));
  }
  SUBCASE("joint capacity is enforced") {
    auto tight = make_world(World2D::Desc{.max_bodies = 4, .max_joints = 1});
    auto x = tight.create_body(BodyDesc{});
    auto y = tight.create_body(BodyDesc{});
    REQUIRE(x.has_value());
    REQUIRE(y.has_value());
    CHECK(tight.create_joint(FixedJoint{*x, *y}).has_value());
    CHECK(has_code(tight.create_joint(FixedJoint{*x, *y}), PhysicsError::world_full));
  }
}

TEST_CASE("a moved-from world fails cleanly on every path") {
  auto world = make_world(World2D::Desc{.max_bodies = 4});
  auto id = world.create_body(BodyDesc{});
  REQUIRE(id.has_value());

  World2D moved_to = std::move(world);
  CHECK(has_code(world.step(), PhysicsError::invalid_state));
  CHECK(has_code(world.advance(0.0f), PhysicsError::invalid_state));
  CHECK(has_code(world.create_body(BodyDesc{}), PhysicsError::invalid_state));
  CHECK(has_code(world.body_state(*id), PhysicsError::invalid_state));
  CHECK(has_code(world.body_user_data(*id), PhysicsError::invalid_state));
  CHECK(has_code(world.set_body_user_data(*id, 7), PhysicsError::invalid_state));
  CHECK(has_code(world.destroy_body(*id), PhysicsError::invalid_state));
  CHECK(has_code(world.set_transform(*id, {}, 0.0f), PhysicsError::invalid_state));
  CHECK(has_code(world.set_collision_filter(*id, 1u, 0xffffffffu),
                 PhysicsError::invalid_state));
  CHECK(has_code(world.set_gravity(ws::math::Vec2{0.0f, -9.81f}),
                 PhysicsError::invalid_state));
  CHECK(has_code(world.create_joint(FixedJoint{*id, *id}), PhysicsError::invalid_state));
  CHECK(world.contact_events().empty());
  CHECK(world.transforms().empty());
  CHECK(world.prev_transforms().empty());
  CHECK(world.moved_bodies().empty());
  CHECK(world.interpolation_alpha() == doctest::Approx(0.0f));

  // The move target owns everything the source created.
  auto state = moved_to.body_state(*id);
  CHECK(state.has_value());
}
