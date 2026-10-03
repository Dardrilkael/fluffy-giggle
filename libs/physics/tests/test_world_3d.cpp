// No DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here - test_common.cpp owns it.
//
// The 3D mirror of test_world_2d.cpp, deliberately leaner: the machinery
// (tables, accumulator, event buffer, atomicity) is ONE shared template -
// world_core.hpp - already pinned by the 2D suite. This file pins the 3D
// surface: Quat poses, the Y-aligned capsule rules, hinge-axis validation
// and overlap_sphere/overlap_aabb plumbing.

#include <doctest/doctest.h>

#include <cmath>
#include <utility>
#include <vector>

#include "check.hpp"

#include <ws/math/vecmath.hpp>
#include <ws/physics/3d/joint.hpp>
#include <ws/physics/3d/world.hpp>

using ws::math::Quat;
using ws::math::Vec3;
using ws::physics::PhysicsError;
using ws::physics3d::BodyDesc;
using ws::physics3d::BodyId;
using ws::physics3d::BodyMove;
using ws::physics3d::Box;
using ws::physics3d::Capsule;
using ws::physics3d::DistanceJoint;
using ws::physics3d::FixedJoint;
using ws::physics3d::HingeJoint;
using ws::physics3d::Sphere;
using ws::physics3d::Transform;
using ws::physics3d::World3D;
using wsphys::test::has_code;

namespace {

[[nodiscard]] World3D make_world(World3D::Desc desc = {}) {
  auto world = World3D::create(desc);
  REQUIRE(world.has_value());
  return std::move(*world);
}

[[nodiscard]] BodyDesc body_at(Vec3 position) {
  BodyDesc d{};
  d.transform = Transform{position, Quat{}};
  return d;
}

}  // namespace

TEST_CASE("3D create validates Desc; defaults pass") {
  World3D::Desc desc{};
  CHECK(World3D::create(desc).has_value());
  CHECK(World3D::create().has_value());

  desc.gravity = Vec3{0.0f, std::nanf(""), 0.0f};
  CHECK(has_code(World3D::create(desc), PhysicsError::invalid_argument));

  desc = World3D::Desc{};
  desc.fixed_dt = -2.0f;
  CHECK(has_code(World3D::create(desc), PhysicsError::invalid_argument));
}

TEST_CASE("3D body lifetime mirrors the generational contract") {
  auto world = make_world(World3D::Desc{.max_bodies = 2});
  auto a = world.create_body(body_at(Vec3{1.0f, 2.0f, 3.0f}));
  REQUIRE(a.has_value());
  CHECK(a->index == 0);
  CHECK(a->generation != 0);

  CHECK(world.create_body(BodyDesc{}).has_value());
  CHECK(has_code(world.create_body(BodyDesc{}), PhysicsError::world_full));

  SUBCASE("stale handles error on every path") {
    const BodyId stale = *a;
    CHECK(world.destroy_body(stale).has_value());
    CHECK(has_code(world.body_state(stale), PhysicsError::invalid_handle));
    CHECK(has_code(world.destroy_body(stale), PhysicsError::invalid_handle));
    CHECK(has_code(world.apply_linear_impulse(stale, Vec3{}), PhysicsError::invalid_handle));
    CHECK(has_code(world.set_velocity(stale, Vec3{}, Vec3{}), PhysicsError::invalid_handle));
  }
  SUBCASE("the slot is reused with a bumped generation") {
    const BodyId stale = *a;
    CHECK(world.destroy_body(stale).has_value());
    auto revived = world.create_body(BodyDesc{});
    REQUIRE(revived.has_value());
    CHECK(revived->index == stale.index);
    CHECK(revived->generation == stale.generation + 1);
  }
}

TEST_CASE("3D shapes validate: sphere strict, capsule allows degenerate") {
  auto world = make_world();

  BodyDesc sphere{};
  sphere.shape = Sphere{0.0f};
  CHECK(has_code(world.create_body(sphere), PhysicsError::invalid_argument));

  BodyDesc box{};
  box.shape = Box{Vec3{0.5f, 0.5f, 0.0f}};
  CHECK(has_code(world.create_body(box), PhysicsError::invalid_argument));

  // half_height == 0 degenerates to a sphere - legal by contract.
  BodyDesc capsule{};
  capsule.shape = Capsule{0.0f, 0.5f};
  CHECK(world.create_body(capsule).has_value());

  BodyDesc neg_capsule{};
  neg_capsule.shape = Capsule{-1.0f, 0.5f};
  CHECK(has_code(world.create_body(neg_capsule), PhysicsError::invalid_argument));
}

TEST_CASE("3D body_state echoes the Quat pose; transforms() mirrors slots") {
  auto world = make_world(World3D::Desc{.max_bodies = 4});
  const Quat spin{0.0f, 0.70710678f, 0.0f, 0.70710678f};  // 90 deg about +Y
  BodyDesc d = body_at(Vec3{-1.0f, 0.5f, 2.0f});
  d.transform.rotation = spin;
  auto id = world.create_body(d);
  REQUIRE(id.has_value());

  auto state = world.body_state(*id);
  REQUIRE(state.has_value());
  CHECK(state->position.x == doctest::Approx(-1.0f));
  CHECK(state->position.z == doctest::Approx(2.0f));
  CHECK(state->rotation.x == doctest::Approx(spin.x));
  CHECK(state->rotation.w == doctest::Approx(spin.w));
  CHECK(state->linear_velocity == Vec3{});

  CHECK(world.transforms().size() == 4);
  CHECK(world.transforms()[id->index].position.y == doctest::Approx(0.5f));

  CHECK(world.set_transform(*id, Vec3{9.0f, 9.0f, 9.0f}, Quat{}).has_value());
  CHECK(world.transforms()[id->index].position.x == doctest::Approx(9.0f));
}

TEST_CASE("3D set_transforms is atomic; the accumulator drives alpha") {
  auto world = make_world(World3D::Desc{.fixed_dt = 0.25f, .max_bodies = 4});
  auto a = world.create_body(body_at(Vec3{0.0f, 0.0f, 0.0f}));
  auto b = world.create_body(body_at(Vec3{1.0f, 0.0f, 0.0f}));
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());

  const BodyId stale = *b;
  CHECK(world.destroy_body(stale).has_value());
  const std::vector<BodyMove> mixed{BodyMove{*a, Vec3{5.0f, 0.0f, 0.0f}, Quat{}},
                                    BodyMove{stale, Vec3{6.0f, 0.0f, 0.0f}, Quat{}}};
  CHECK(has_code(world.set_transforms(mixed), PhysicsError::invalid_handle));
  auto state = world.body_state(*a);
  REQUIRE(state.has_value());
  CHECK(state->position.x == doctest::Approx(0.0f));  // atomic: nothing moved

  CHECK(world.advance(0.125f).has_value());
  CHECK(world.interpolation_alpha() == doctest::Approx(0.5f));
  CHECK(world.advance(0.125f).has_value());
  CHECK(world.interpolation_alpha() == doctest::Approx(0.0f));
  CHECK(world.step().has_value());
  CHECK(world.contact_events().empty());
}

TEST_CASE("hinge joints validate axis, limits and motor") {
  auto world = make_world(World3D::Desc{.max_bodies = 8, .max_joints = 4});
  auto a = world.create_body(BodyDesc{});
  auto b = world.create_body(BodyDesc{});
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());

  SUBCASE("the axis must be finite and non-zero") {
    HingeJoint zero_axis{};
    zero_axis.a = *a;
    zero_axis.b = *b;
    zero_axis.world_axis = Vec3{};
    CHECK(has_code(world.create_joint(zero_axis), PhysicsError::invalid_argument));

    HingeJoint nan_axis{};
    nan_axis.a = *a;
    nan_axis.b = *b;
    nan_axis.world_axis = Vec3{std::nanf(""), 1.0f, 0.0f};
    CHECK(has_code(world.create_joint(nan_axis), PhysicsError::invalid_argument));
  }
  SUBCASE("enabled limits need lower <= upper") {
    HingeJoint bad{};
    bad.a = *a;
    bad.b = *b;
    bad.enable_limits = true;
    bad.lower_angle = 1.0f;
    bad.upper_angle = -1.0f;
    CHECK(has_code(world.create_joint(bad), PhysicsError::invalid_argument));
  }
  SUBCASE("an enabled motor needs positive torque") {
    HingeJoint motor{};
    motor.a = *a;
    motor.b = *b;
    motor.enable_motor = true;
    motor.max_motor_torque = 0.0f;
    CHECK(has_code(world.create_joint(motor), PhysicsError::invalid_argument));

    motor.enable_motor = false;
    CHECK(world.create_joint(motor).has_value());
  }
  SUBCASE("distance and self-joint rules mirror 2D") {
    CHECK(has_code(world.create_joint(DistanceJoint{*a, *b, -0.5f}),
                   PhysicsError::invalid_argument));
    CHECK(has_code(world.create_joint(FixedJoint{*a, *a}),
                   PhysicsError::invalid_argument));
    CHECK(world.create_joint(FixedJoint{*a, *b}).has_value());
  }
  SUBCASE("destroying a body cascades to its joints") {
    auto joint = world.create_joint(FixedJoint{*a, *b});
    REQUIRE(joint.has_value());
    CHECK(world.destroy_body(*a).has_value());
    CHECK(has_code(world.destroy_joint(*joint), PhysicsError::invalid_handle));
  }
}

TEST_CASE("3D queries validate arguments even without a backend") {
  auto world = make_world();
  CHECK(world.create_body(BodyDesc{}).has_value());

  SUBCASE("raycast rejects zero direction and NaN distance") {
    CHECK(has_code(world.raycast(Vec3{}, Vec3{}, 1.0f),
                   PhysicsError::invalid_argument));
    CHECK(has_code(world.raycast(Vec3{}, Vec3{0.0f, 0.0f, 1.0f}, std::nanf("")),
                   PhysicsError::invalid_argument));
  }
  SUBCASE("overlap_sphere rejects negative radius; empty is fine") {
    CHECK(has_code(world.overlap_sphere(Vec3{}, -1.0f),
                   PhysicsError::invalid_argument));
    auto hits = world.overlap_sphere(Vec3{}, 4.0f);
    REQUIRE(hits.has_value());
    CHECK(hits->empty());  // null backend: no hits, valid result
  }
  SUBCASE("overlap_aabb rejects inverted boxes") {
    using ws::physics3d::Aabb;
    CHECK(has_code(world.overlap_aabb(Aabb{Vec3{1.0f, 1.0f, 1.0f}, Vec3{0.0f, 1.0f, 1.0f}}),
                   PhysicsError::invalid_argument));
    auto hits = world.overlap_aabb(Aabb{Vec3{-8.0f, -8.0f, -8.0f}, Vec3{8.0f, 8.0f, 8.0f}});
    REQUIRE(hits.has_value());
    CHECK(hits->empty());
  }
}
