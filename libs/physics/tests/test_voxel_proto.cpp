// Headless prototype of the voxel game's player controller: a Box body
// standing on a static ground grid, driven by the WASD/jump/gravity plan
// the app will use. Validates the controller design BEFORE the app exists
// (the highest-risk piece: feel, gravity, jump height, no-drift).
//
// The static ground: a 1-block grid of static boxes (the voxel world's
// floor stand-in). The player: a 0.6 x 1.8 x 0.6 Box (Minecraft's own
// numbers), gravity -32 m/s^2 (5x earth: the snappy arcade feel - this
// is about jump feel, not realism), jump impulse 9.8 m/s for ~1.5 m
// apex (a full block cleared with margin - the stronger jump).
//
// Run: LD_LIBRARY_PATH=... ninja -C build/linux test_voxel_proto &&
//       ./build/linux/bin/test_voxel_proto

// test_common.cpp owns doctest's main (the one-binary convention).
#include <doctest/doctest.h>

#include <cmath>
#include <numbers>
#include <vector>

#include <ws/math/vecmath.hpp>
#include <ws/physics/3d/body.hpp>
#include <ws/physics/3d/contact.hpp>
#include <ws/physics/3d/shape.hpp>
#include <ws/physics/3d/world.hpp>

namespace {

namespace pd = ws::physics3d;
using ws::math::Vec3;

constexpr float kPlayerHalfW = 0.3f;   // 0.6 wide/deep
constexpr float kPlayerHalfH = 0.9f;   // 1.8 tall
constexpr float kGravity = -32.0f;     // arcade: 5x earth, snappy fall
constexpr float kJumpSpeed = 9.8f;      // ~1.5 m apex (sqrt(2*32*1.5))
constexpr float kMoveSpeed = 6.0f;     // walk speed, m/s
constexpr float kMaxFall = 55.0f;      // terminal, m/s

// A 7x7 ground grid of static 1x1x1 blocks at y=0 (top face at 0.5).
// The player spawns at y=2 over the center.
[[nodiscard]] pd::World3D make_world() {
  pd::World3D::Desc desc{};
  desc.gravity = Vec3{0.0f, kGravity, 0.0f};
  desc.fixed_dt = 1.0f / 60.0f;
  desc.thread_count = 0;  // deterministic, the simple mode
  auto world = pd::World3D::create(desc);
  REQUIRE(world.has_value());
  for (int gx = -3; gx <= 3; ++gx) {
    for (int gz = -3; gz <= 3; ++gz) {
      pd::BodyDesc ground{};
      ground.kind = ws::physics::BodyKind::static_;
      ground.shape = pd::Box{Vec3{0.5f, 0.5f, 0.5f}};
      ground.transform = pd::Transform{Vec3{static_cast<float>(gx), 0.0f,
                                            static_cast<float>(gz)}, {}};
      auto id = world->create_body(ground);
      REQUIRE(id.has_value());
    }
  }
  return std::move(*world);
}

}  // namespace

TEST_CASE("proto: the player rests on the ground, does not sink or drift") {
  auto world = make_world();
  pd::BodyDesc desc{};
  desc.kind = ws::physics::BodyKind::dynamic;
  desc.shape = pd::Box{Vec3{kPlayerHalfW, kPlayerHalfH, kPlayerHalfW}};
  desc.transform = pd::Transform{Vec3{0.0f, 2.0f, 0.0f}, {}};
  desc.friction = 0.0f;  // the app zeroes friction: the controller's own
                         // velocity control (below) is the horizontal
                         // authority, not the engine's friction
  desc.allow_sleep = false;  // a player never sleeps
  auto player = world.create_body(desc);
  REQUIRE(player.has_value());

  // Settle: 2 seconds of stepping. The player must land on the grid and
  // rest with its feet at y=0.5 (center 1.4).
  for (int i = 0; i < 120; ++i) {
    REQUIRE(world.advance(1.0f / 60.0f).has_value());
  }
  auto state = world.body_state(*player);
  REQUIRE(state.has_value());
  CHECK(state->position.y > 1.2f);   // did not sink into the floor
  CHECK(state->position.y < 1.7f);   // came down from the 2.0 spawn
  CHECK(std::abs(state->position.x) < 0.05f);
  CHECK(std::abs(state->position.z) < 0.05f);
}

TEST_CASE("proto: the controller walks and the velocity is exact") {
  auto world = make_world();
  pd::BodyDesc desc{};
  desc.kind = ws::physics::BodyKind::dynamic;
  desc.shape = pd::Box{Vec3{kPlayerHalfW, kPlayerHalfH, kPlayerHalfW}};
  desc.transform = pd::Transform{Vec3{0.0f, 2.0f, 0.0f}, {}};
  desc.friction = 0.0f;
  desc.allow_sleep = false;
  auto player = world.create_body(desc);
  REQUIRE(player.has_value());

  // Settle first.
  for (int i = 0; i < 120; ++i) {
    REQUIRE(world.advance(1.0f / 60.0f).has_value());
  }

  // The controller: every tick sets the horizontal velocity exactly
  // (the classic FPS pattern - the engine's solver never fights it),
  // gravity owns the vertical.
  for (int i = 0; i < 60; ++i) {
    auto s = world.body_state(*player);
    REQUIRE(s.has_value());
    const Vec3 want{4.0f, s->linear_velocity.y, 0.0f};  // walk +x
    REQUIRE(world.set_velocity(*player, want, Vec3{}).has_value());
    REQUIRE(world.advance(1.0f / 60.0f).has_value());
    // Clamp the fall: the engine never outruns the controller's vertical
    const auto v = world.body_state(*player);
    REQUIRE(v.has_value());
    if (v->linear_velocity.y < -kMaxFall) {
      REQUIRE(world.set_velocity(*player, Vec3{4.0f, -kMaxFall, 0.0f}, Vec3{})
                  .has_value());
    }
  }
  auto state = world.body_state(*player);
  REQUIRE(state.has_value());
  // 1 s of 4 m/s walk: ~4 m, minus the engine's own contact jitter.
  CHECK(state->position.x > 3.7f);
  CHECK(state->position.x < 4.3f);
}

TEST_CASE("proto: the jump apex is ~1.5 m above the ground") {
  auto world = make_world();
  pd::BodyDesc desc{};
  desc.kind = ws::physics::BodyKind::dynamic;
  desc.shape = pd::Box{Vec3{kPlayerHalfW, kPlayerHalfH, kPlayerHalfW}};
  desc.transform = pd::Transform{Vec3{0.0f, 2.0f, 0.0f}, {}};
  desc.friction = 0.0f;
  desc.allow_sleep = false;
  auto player = world.create_body(desc);
  REQUIRE(player.has_value());
  for (int i = 0; i < 120; ++i) {
    REQUIRE(world.advance(1.0f / 60.0f).has_value());
  }
  auto state = world.body_state(*player);
  REQUIRE(state.has_value());
  const float rest_y = state->position.y;

  // Jump: vertical velocity to kJumpSpeed; then 60 ticks of free flight.
  REQUIRE(world.set_velocity(*player, Vec3{0.0f, kJumpSpeed, 0.0f}, Vec3{})
              .has_value());
  float apex = rest_y;
  for (int i = 0; i < 90; ++i) {
    REQUIRE(world.advance(1.0f / 60.0f).has_value());
    auto s = world.body_state(*player);
    REQUIRE(s.has_value());
    apex = std::max(apex, s->position.y);
  }
  // Feet apex = apex - kPlayerHalfH; above the rest feet by ~1.5 m.
  const float feet_apex = apex - kPlayerHalfH;
  CHECK(feet_apex > 1.9f);  // landed, then jumped ~1.5
  CHECK(feet_apex < 2.1f);  // (0.5 rest + 1.5 apex = 2.0, 0.1 band)
}
