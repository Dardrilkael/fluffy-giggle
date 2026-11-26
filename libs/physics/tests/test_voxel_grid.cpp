// Backend-independent voxel-grid query and swept-AABB conformance tests.
// test_common.cpp owns doctest's main.

#include <doctest/doctest.h>

#include <ws/math/vecmath.hpp>
#include <ws/physics/3d/voxel_grid.hpp>

namespace {

using ws::math::Vec3;
using ws::physics3d::Aabb;
using ws::physics3d::VoxelGrid3D;

struct TestGrid {
  static bool floor_and_wall(const void*, int x, int y, int z) noexcept {
    return (y == 0 && x >= -4 && x <= 4 && z >= -4 && z <= 4) ||
           (x == 3 && y >= 1 && y <= 4 && z >= -4 && z <= 4);
  }

  static bool one_block(const void*, int x, int y, int z) noexcept {
    return x == 0 && y == 0 && z == 0;
  }
};

[[nodiscard]] VoxelGrid3D make_grid() {
  return VoxelGrid3D{VoxelGrid3D::Desc{
      {-8, -4, -8}, {16, 16, 16}, nullptr, &TestGrid::floor_and_wall}};
}

}  // namespace

TEST_CASE("voxel grid: DDA raycast returns the first occupied cell and face") {
  const VoxelGrid3D grid{VoxelGrid3D::Desc{
      {-2, -2, -2}, {4, 4, 4}, nullptr, &TestGrid::one_block}};
  const auto hit = grid.raycast(Vec3{0.5f, 2.5f, 0.5f},
                                Vec3{0.0f, -4.0f, 0.0f}, 8.0f);
  REQUIRE(hit.hit);
  CHECK(hit.cell == ws::math::IVec3{0, 0, 0});
  CHECK(hit.normal == ws::math::IVec3{0, 1, 0});
  CHECK(hit.distance == doctest::Approx(2.5f));
  CHECK_FALSE(grid.raycast(Vec3{2.5f, 2.5f, 2.5f},
                           Vec3{1.0f, 0.0f, 0.0f}, 8.0f).hit);
}

TEST_CASE("voxel grid: swept AABB cannot tunnel through a floor") {
  const auto grid = make_grid();
  const Aabb player{Vec3{-0.3f, 5.0f, -0.3f},
                    Vec3{0.3f, 6.8f, 0.3f}};
  const auto moved =
      grid.sweep_aabb(player, Vec3{0.0f, -20.0f, 0.0f});
  CHECK(moved.hit_y);
  CHECK(moved.bounds.min.y >= doctest::Approx(1.0f).epsilon(0.002));
  CHECK_FALSE(grid.overlaps(moved.bounds));
}

TEST_CASE("voxel grid: swept AABB slides along a wall without snagging") {
  const auto grid = make_grid();
  const Aabb player{Vec3{1.0f, 1.0f, 0.0f},
                    Vec3{1.6f, 2.8f, 0.6f}};
  const auto moved = grid.sweep_aabb(player, Vec3{4.0f, 0.0f, 2.0f});
  CHECK(moved.hit_x);
  CHECK(moved.bounds.max.x <= doctest::Approx(3.0f).epsilon(0.002));
  CHECK(moved.bounds.min.z > player.min.z + 1.5f);
  CHECK_FALSE(grid.overlaps(moved.bounds));
}

TEST_CASE("voxel grid: initial overlap is recovered before movement") {
  const VoxelGrid3D grid{VoxelGrid3D::Desc{
      {-2, -2, -2}, {5, 5, 5}, nullptr, &TestGrid::one_block}};
  const Aabb embedded{Vec3{0.2f, 0.2f, 0.2f},
                      Vec3{0.8f, 0.8f, 0.8f}};
  REQUIRE(grid.overlaps(embedded));
  const auto moved = grid.sweep_aabb(embedded, Vec3{0.0f, 0.0f, 0.0f});
  CHECK_FALSE(grid.overlaps(moved.bounds));
}

TEST_CASE("voxel grid: out-of-bounds cells are empty and invalid grids miss") {
  const VoxelGrid3D grid{VoxelGrid3D::Desc{
      {0, 0, 0}, {4, 4, 4}, nullptr, &TestGrid::one_block}};
  CHECK_FALSE(grid.is_solid(-1, 0, 0));
  CHECK_FALSE(grid.is_solid(4, 0, 0));
  CHECK_FALSE(grid.is_solid(0, 4, 0));
  CHECK_FALSE(grid.is_solid(0, 0, 4));
  const VoxelGrid3D invalid{};
  CHECK_FALSE(invalid.valid());
  CHECK_FALSE(invalid.raycast(Vec3{}, Vec3{1.0f, 0.0f, 0.0f}, 10.0f).hit);
}
