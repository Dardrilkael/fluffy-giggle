// The shared voxel character controller: direct public-API tests, no game headers.
#include <doctest/doctest.h>

#include <bit>
#include <cstdint>

#include <ws/math/vecmath.hpp>
#include <ws/physics/3d/character_controller.hpp>

namespace {
using ws::math::Vec3;
using ws::physics3d::CharacterController;
using ws::physics3d::VoxelGrid3D;

struct Floor {
  static bool solid(const void*, int, int y, int) noexcept {
    return y == 0;
  }
};

[[nodiscard]] VoxelGrid3D floor_grid() {
  return VoxelGrid3D{{{-64, -8, -64}, {128, 32, 128}, nullptr, &Floor::solid}};
}

[[nodiscard]] bool same_bits(float a, float b) {
  return std::bit_cast<std::uint32_t>(a) ==
         std::bit_cast<std::uint32_t>(b);
}
}  // namespace

TEST_CASE("character controller: falls onto voxel floor, rests, and jumps") {
  const auto grid = floor_grid();
  CharacterController character;
  REQUIRE(character.spawn(Vec3{0.5f, 5.0f, 0.5f}));

  const CharacterController::MoveInput idle{};
  for (int i = 0; i < 180; ++i) {
    character.tick(idle, 1.0f / 60.0f, grid);
  }
  CHECK(character.grounded());
  CHECK(character.velocity().y == doctest::Approx(0.0f));
  CHECK(character.bounds().min.y >= doctest::Approx(1.0f).epsilon(0.002));
  CHECK_FALSE(grid.overlaps(character.bounds()));

  const float rest_y = character.position().y;
  for (int i = 0; i < 120; ++i) {
    character.tick(idle, 1.0f / 60.0f, grid);
  }
  CHECK(character.grounded());
  CHECK(same_bits(character.position().y, rest_y));

  CharacterController::MoveInput jump{};
  jump.jump = true;
  character.tick(jump, 1.0f / 60.0f, grid);
  for (int i = 0; i < 12; ++i) {
    character.tick(idle, 1.0f / 60.0f, grid);
  }
  CHECK(character.position().y > rest_y + 0.5f);
}

TEST_CASE("character controller: identical input streams produce identical state") {
  const auto grid = floor_grid();
  CharacterController a;
  CharacterController b;
  REQUIRE(a.spawn(Vec3{0.5f, 4.0f, 0.5f}));
  REQUIRE(b.spawn(Vec3{0.5f, 4.0f, 0.5f}));

  for (int frame = 0; frame < 480; ++frame) {
    CharacterController::MoveInput input{};
    input.forward = frame < 150 || (frame >= 300 && frame < 400);
    input.right = frame >= 150 && frame < 300;
    input.sprint = frame % 120 >= 80;
    input.jump = frame == 0 || frame == 300;
    input.look_dx = static_cast<float>((frame * 13) % 17 - 8) * 0.125f;
    input.look_dy = static_cast<float>((frame * 7) % 11 - 5) * 0.0625f;
    const float dt = frame % 2 == 0 ? 1.0f / 60.0f : 1.0f / 120.0f;

    a.tick(input, dt, grid);
    b.tick(input, dt, grid);

    CHECK(same_bits(a.position().x, b.position().x));
    CHECK(same_bits(a.position().y, b.position().y));
    CHECK(same_bits(a.position().z, b.position().z));
    CHECK(same_bits(a.velocity().x, b.velocity().x));
    CHECK(same_bits(a.velocity().y, b.velocity().y));
    CHECK(same_bits(a.velocity().z, b.velocity().z));
    CHECK(same_bits(a.yaw_radians(), b.yaw_radians()));
    CHECK(a.grounded() == b.grounded());
  }
}

TEST_CASE("character controller: invalid delta does not advance the state") {
  const auto grid = floor_grid();
  CharacterController character;
  REQUIRE(character.spawn(Vec3{0.5f, 5.0f, 0.5f}));
  const Vec3 before = character.position();
  CharacterController::MoveInput input{};
  input.forward = true;
  input.look_dx = 100.0f;
  character.tick(input, 0.0f, grid);
  CHECK(same_bits(character.position().x, before.x));
  CHECK(same_bits(character.position().y, before.y));
  CHECK(same_bits(character.position().z, before.z));
  CHECK(character.yaw_radians() == 0.0f);
}
