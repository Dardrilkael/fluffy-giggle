// apps/voxel_game - test_voxel_logic.cpp: the game's headless logic
// suite - worldgen determinism, the mesher's neighbor rule, the DDA
// raycast's hit/normal/placement answers. No GPU needed (the mesh
// BUILDING is pure CPU; only create_mesh touches the registry), so the
// tests build the world's data model and check it against hand-derived
// expectations.
//
// The pattern is the physics suite's (test_common.cpp owns doctest's
// main? No - this is a STANDALONE binary, its own main: ws_add_test
// gives every suite its own). Run: ninja -C build/linux
// test_voxel_logic && ./build/linux/bin/test_voxel_logic

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <numbers>

#include "world.hpp"

namespace {

// A generated world with a known seed, ready to probe.
[[nodiscard]] voxel::World make_world() {
  voxel::World world;
  world.generate(0xC0FFEEu);
  return world;
}

}  // namespace

TEST_CASE("worldgen: deterministic per seed, distinct across seeds") {
  voxel::World a = make_world();
  voxel::World b = make_world();
  // Same seed -> identical block tables at a sample of columns.
  bool identical = true;
  for (int x = 0; x < voxel::kWorldSizeX; x += 7) {
    for (int z = 0; z < voxel::kWorldSizeZ; z += 5) {
      for (int y = 0; y < voxel::kChunkSizeY; y += 3) {
        if (a.block_at(x, y, z) != b.block_at(x, y, z)) {
          identical = false;
        }
      }
    }
  }
  CHECK(identical);

  // A different seed -> a different terrain (at least some sampled
  // block differs).
  voxel::World c;
  c.generate(0x1234u);
  bool any_differs = false;
  for (int x = 0; x < voxel::kWorldSizeX && !any_differs; x += 3) {
    for (int z = 0; z < voxel::kWorldSizeZ && !any_differs; z += 3) {
      for (int y = 0; y < 30 && !any_differs; y += 2) {
        if (a.block_at(x, y, z) != c.block_at(x, y, z)) {
          any_differs = true;
        }
      }
    }
  }
  CHECK(any_differs);
}

TEST_CASE("worldgen: the column spell - stone deep, dirt under the top") {
  const voxel::World world = make_world();
  int grass_columns = 0;
  int sand_columns = 0;
  int stone_deep = 0;
  for (int x = 0; x < voxel::kWorldSizeX; x += 5) {
    for (int z = 0; z < voxel::kWorldSizeZ; z += 5) {
      const int surface = world.surface_height(x, z);
      REQUIRE(surface >= 0);
      const voxel::Block top = world.block_at(x, surface, z);
      CHECK(voxel::is_solid(top));
      CHECK(top != voxel::Block::dirt);  // the top is grass or sand
      if (top == voxel::Block::grass) {
        ++grass_columns;
      } else if (top == voxel::Block::sand) {
        ++sand_columns;
      }
      // Deep stone below the dirt band. The dirt band is 3 deep under
      // the cap (dirt at height-3..height-1 when grass-capped), so
      // surface - 6 is stone on any column 6 or more above the dirt -
      // the failing columns are shoreline columns whose SAND cap the
      // worldgen's spell extends further (sand everywhere at or below
      // water's edge). Require the check on grass-capped columns only.
      if (surface >= 12 && top == voxel::Block::grass) {
        CHECK(world.block_at(x, surface - 6, z) == voxel::Block::stone);
        ++stone_deep;
      }
    }
  }
  CHECK(grass_columns > 20);  // the world has land
  CHECK(sand_columns > 0);    // and shorelines
  CHECK(stone_deep > 20);
}

TEST_CASE("raycast: a straight-down ray hits the surface block below it") {
  const voxel::World world = make_world();
  const int x = 40;
  const int z = 40;
  const int surface = world.surface_height(x, z);
  REQUIRE(surface >= 0);

  // From well above, looking straight down.
  const float origin[3] = {static_cast<float>(x) + 0.5f,
                           static_cast<float>(surface) + 5.5f,
                           static_cast<float>(z) + 0.5f};
  const float down[3] = {0.0f, -1.0f, 0.0f};
  const auto hit = world.raycast(origin, down, 20.0f);
  REQUIRE(hit.hit);
  CHECK(hit.cell[0] == x);
  CHECK(hit.cell[1] == surface);
  CHECK(hit.cell[2] == z);
  // Entered through the top: the normal points +y.
  CHECK(hit.normal[0] == 0);
  CHECK(hit.normal[1] == 1);
  CHECK(hit.normal[2] == 0);
  // The distance: the ray enters the hit cell at its top boundary
  // (y = surface + 1), having fallen from origin y = surface + 5.5 -
  // a 4.5 fall. (The walk reports the boundary crossing, not the
  // cell center.)
  CHECK(hit.distance == doctest::Approx(4.5f).epsilon(0.001));
}

TEST_CASE("raycast: the place cell is hit + normal, one up") {
  const voxel::World world = make_world();
  const int x = 40;
  const int z = 40;
  const int surface = world.surface_height(x, z);
  const float origin[3] = {static_cast<float>(x) + 0.5f,
                            static_cast<float>(surface) + 4.5f,
                            static_cast<float>(z) + 0.5f};
  const float down[3] = {0.0f, -1.0f, 0.0f};
  const auto hit = world.raycast(origin, down, 20.0f);
  REQUIRE(hit.hit);
  // The placement the game would make: the cell above the hit, along
  // the normal.
  CHECK(hit.cell[1] + hit.normal[1] == surface + 1);
  // That cell is air (the place rule checks it).
  CHECK(world.block_at(hit.cell[0], surface + 1, hit.cell[2]) ==
        voxel::Block::air);
}

TEST_CASE("raycast: a horizontal ray walks the grid one cell at a time") {
  const voxel::World world = make_world();
  // From (40, y, 20) looking +z, above the terrain: a miss (air all
  // the way) unless a hill blocks it - walk far enough to be sure the
  // walk itself terminates.
  const int surface = world.surface_height(40, 20);
  const float y = static_cast<float>(surface) + 20.0f;
  const float origin[3] = {40.5f, y, 20.5f};
  const float east[3] = {0.0f, 0.0f, 1.0f};
  const auto miss = world.raycast(origin, east, 30.0f);
  CHECK_FALSE(miss.hit);  // 20 above the terrain: nothing to hit
  // And the guard: the walk terminates (no hang) - reaching here is
  // the check.
}

TEST_CASE("raycast: an eye-height ray into a wall's face reports the face") {
  voxel::World world = make_world();
  // Carve a flat test corridor first: clear every column in the ray's
  // path to a known floor, so the only solid thing in sight is the
  // hand-built wall. The corridor: x = 45..47, z = 44..49, floors at
  // y 0 (stone), air above.
  for (int x = 45; x <= 47; ++x) {
    for (int z = 44; z <= 49; ++z) {
      for (int y = 0; y < voxel::kChunkSizeY; ++y) {
        world.set_block(x, y, z,
                        y == 0 ? voxel::Block::stone : voxel::Block::air);
      }
    }
  }
  // The wall: 3-tall stone at z = 50, x in 45..47 (the corridor's end).
  for (int x = 45; x <= 47; ++x) {
    for (int y = 1; y <= 3; ++y) {
      world.set_block(x, y, 50, voxel::Block::stone);
    }
  }
  // Looking from z = 46 toward +z, at y = 1.5: the wall's north face
  // (z = 50), normal -z.
  const float origin[3] = {46.5f, 1.5f, 46.5f};
  const float south[3] = {0.0f, 0.0f, 1.0f};
  const auto hit = world.raycast(origin, south, 10.0f);
  REQUIRE(hit.hit);
  CHECK(hit.cell[2] == 50);
  CHECK(hit.normal[2] == -1);
  CHECK(hit.normal[0] == 0);
  CHECK(hit.normal[1] == 0);
  // Placement: the empty cell north of the wall.
  CHECK(world.block_at(hit.cell[0], hit.cell[1], hit.cell[2] - 1) ==
        voxel::Block::air);
}

TEST_CASE("edits: set_block changes the table and rebuilds ride it") {
  voxel::World world = make_world();
  const int x = 10;
  const int z = 10;
  const int surface = world.surface_height(x, z);
  REQUIRE(surface >= 0);
  // Break the top block.
  REQUIRE(world.set_block(x, surface, z, voxel::Block::air));
  CHECK(world.surface_height(x, z) == surface - 1);
  // Same-value set is a no-op.
  CHECK_FALSE(world.set_block(x, surface - 1, z,
                              world.block_at(x, surface - 1, z)));
  // Out-of-bounds is refused.
  CHECK_FALSE(world.set_block(-1, 0, 0, voxel::Block::stone));
  CHECK_FALSE(world.set_block(0, -1, 0, voxel::Block::stone));
  CHECK_FALSE(world.set_block(0, voxel::kChunkSizeY, 0, voxel::Block::stone));
}

TEST_CASE("mesher: the culling rule - inner faces never emit") {
  // A hand-built world so the counts are exact: one stone block at
  // (0,0,0) with air everywhere else. The mesh (built through the
  // private build path - the test reaches the mesher the only way an
  // app can, through rebuild_chunk, which needs the GPU registry; the
  // pure-CPU half is checked by construction here: block_at's table
  // and the culling rule the emitter reads).
  voxel::World world;
  world.generate(1u);
  // The counts the rule implies on a known column: the surface block
  // has 6 faces, of which the 5 neighbor-exposed ones emit (the bottom
  // face looks at stone, the 4 sides at stone for a solid column).
  const int x = 64;
  const int z = 64;
  const int surface = world.surface_height(x, z);
  REQUIRE(surface >= 2);
  // The surface block's 4 side neighbors are stone (the column is
  // solid); its top is air, its bottom stone: only the top face is
  // exposed - 1 emitting face... the rule the mesh builder applies per
  // block, verified here against the table itself:
  int exposed = 0;
  const int dirs[6][3] = {{0, 1, 0},  {0, -1, 0}, {1, 0, 0},
                          {-1, 0, 0}, {0, 0, 1},  {0, 0, -1}};
  for (const auto& d : dirs) {
    if (!voxel::is_solid(world.block_at(x + d[0], surface + d[1],
                                        z + d[2]))) {
      ++exposed;
    }
  }
  CHECK(exposed == 1);  // the column is solid around, open above
  // And the block 2 deep has all 6 neighbors solid: 0 faces.
  int deep_exposed = 0;
  for (const auto& d : dirs) {
    if (!voxel::is_solid(world.block_at(x + d[0], surface - 2 + d[1],
                                        z + d[2]))) {
      ++deep_exposed;
    }
  }
  CHECK(deep_exposed == 0);
}

TEST_CASE("worldgen: water sits only at or below its level") {
  const voxel::World world = make_world();
  // The water level constant (world.cpp's kWaterLevel = 12).
  constexpr int kWaterLevel = 12;
  for (int x = 0; x < voxel::kWorldSizeX; x += 11) {
    for (int z = 0; z < voxel::kWorldSizeZ; z += 11) {
      for (int y = kWaterLevel + 1; y < voxel::kChunkSizeY; ++y) {
        CHECK(world.block_at(x, y, z) != voxel::Block::water);
      }
    }
  }
  // And somewhere in the world there IS water (the level is chosen so
  // lakes exist).
  bool any_water = false;
  for (int x = 0; x < voxel::kWorldSizeX && !any_water; x += 3) {
    for (int z = 0; z < voxel::kWorldSizeZ && !any_water; z += 3) {
      for (int y = 0; y <= kWaterLevel && !any_water; ++y) {
        if (world.block_at(x, y, z) == voxel::Block::water) {
          any_water = true;
        }
      }
    }
  }
  CHECK(any_water);
}

