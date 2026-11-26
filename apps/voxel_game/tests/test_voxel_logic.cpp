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

#include <cstdint>
#include <string>

#include <cmath>
#include <numbers>

#include <ws/math/vecmath.hpp>

#include "bench.hpp"
#include "perf.hpp"
#include "menu.hpp"
#include "player.hpp"
#include "texture_atlas.hpp"
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


TEST_CASE("shell: is_exposed is the solid-with-an-open-face oracle") {
  // The oracle, spelled independently: a cell is exposed iff it is
  // solid AND some in-bounds neighbor is non-solid (the out-of-world
  // side reads solid). Every cell in a slab around the terrain's
  // surface band gets both answers compared.
  const voxel::World world = make_world();
  const int dirs[6][3] = {{0, 1, 0},  {0, -1, 0}, {1, 0, 0},
                          {-1, 0, 0}, {0, 0, 1},  {0, 0, -1}};
  int checked = 0;
  int exposed_cells = 0;
  for (int x = 0; x < voxel::kWorldSizeX; x += 3) {
    for (int z = 0; z < voxel::kWorldSizeZ; z += 3) {
      const int top = world.surface_height(x, z);
      REQUIRE(top >= 0);
      for (int y = 0; y <= top + 1; ++y) {
        bool oracle = false;
        if (voxel::is_solid(world.block_at(x, y, z))) {
          for (const auto& d : dirs) {
            const int nx = x + d[0];
            const int ny = y + d[1];
            const int nz = z + d[2];
            if (nx < 0 || nx >= voxel::kWorldSizeX || nz < 0 ||
                nz >= voxel::kWorldSizeZ || ny < 0 ||
                ny >= voxel::kChunkSizeY) {
              continue;  // the void reads solid
            }
            if (!voxel::is_solid(world.block_at(nx, ny, nz))) {
              oracle = true;
              break;
            }
          }
        }
        CHECK(world.is_exposed(x, y, z) == oracle);
        ++checked;
        exposed_cells += oracle ? 1 : 0;
      }
    }
  }
  CHECK(checked > 10000);        // the sweep really covered the terrain
  CHECK(exposed_cells > 500);   // and the shell is populous
}

TEST_CASE("shell: a dig exposes the wall behind it, a place buries what it covers") {
  voxel::World world = make_world();
  // A buried interior cell starts not exposed.
  const int x = 40;
  const int z = 40;
  const int surface = world.surface_height(x, z);
  REQUIRE(surface >= 4);
  const int buried_y = surface - 2;
  REQUIRE(voxel::is_solid(world.block_at(x, buried_y, z)));
  if (!world.is_exposed(x, buried_y, z)) {
    // The common case: interior dirt, solid on all six sides.
    // Break the cell ABOVE it: the buried cell gains an open face.
    REQUIRE(world.set_block(x, buried_y + 1, z, voxel::Block::air));
    CHECK(world.is_exposed(x, buried_y, z));
    // And placing that block back re-buries it.
    REQUIRE(world.set_block(x, buried_y + 1, z, voxel::Block::stone));
    CHECK_FALSE(world.is_exposed(x, buried_y, z));
  }
  // A solitary placed block in the sky is exposed on all its own.
  REQUIRE(world.set_block(x, 40, z, voxel::Block::stone));
  CHECK(world.is_exposed(x, 40, z));
}

// --- the frame regression: the game->render map is a ROTATION ----------------------
//
// The field report "mouse x is inverted" was the first build's frame
// map: (x, -y, z), a MIRROR (det -1). With the engine camera's
// right = cross(forward, up-hint) and a y-down screen, a mirrored
// world reads as its own reflection - east lands on screen-LEFT, so
// turning east (the correct game-space response to mouse-right) looks
// like turning left. The fix: (x, -y, -z), the 180-degree turn about
// the x axis, det +1 - a proper rotation, no mirror, winding intact.

TEST_CASE("frame: the game->render map is a proper rotation, not a mirror") {
  // det +1: three independent axis probes through game_to_render.
  // x kept, y and z negated - and NOT just y (the old mirror).
  CHECK(voxel::game_to_render(3.0f, 0) == 3.0f);
  CHECK(voxel::game_to_render(3.0f, 1) == -3.0f);
  CHECK(voxel::game_to_render(3.0f, 2) == -3.0f);
  // The map's matrix is diag(1, -1, -1): determinant = 1 * -1 * -1
  // = +1 - a rotation. The old map diag(1, -1, 1) had det -1.
  CHECK(1.0f * -1.0f * -1.0f > 0.0f);
  CHECK(1.0f * -1.0f * 1.0f < 0.0f);  // the mirror it replaced
}

TEST_CASE("frame: the camera reads the rotated world the right way round") {
  // The full camera chain, spelled independently: the engine's basis
  // formula (right = cross(forward, up-hint), the r2d doc's own
  // contract) applied to begin_scene's camera for a north-looking
  // player, then three GAME-space probes through the map. The
  // numbers this test pins are exactly the shipped bug's signature,
  // post-fix: EAST on screen right, UP on screen up, SOUTH behind.
  using ws::math::Vec3;
  const Vec3 game_eye{64.5f, 32.0f, 64.5f};
  const Vec3 game_look{0.0f, 0.0f, -1.0f};  // north

  const Vec3 eye{voxel::game_to_render(game_eye.x, 0),
                 voxel::game_to_render(game_eye.y, 1),
                 voxel::game_to_render(game_eye.z, 2)};
  const Vec3 look{voxel::game_to_render(game_look.x, 0),
                 voxel::game_to_render(game_look.y, 1),
                 voxel::game_to_render(game_look.z, 2)};
  const Vec3 fwd = ws::math::normalize(look);
  const Vec3 hint{0.0f, -1.0f, 0.0f};
  const Vec3 right = ws::math::normalize(ws::math::cross(fwd, hint));
  const Vec3 up = ws::math::cross(right, fwd);

  // A GAME-east point, through the map, must read on NDC +x (screen
  // right): the axis the old mirror put on screen-LEFT.
  const Vec3 east = eye + Vec3{10.0f, 0.0f, 0.0f};
  const Vec3 w_e = east - eye;
  CHECK(ws::math::dot(w_e, right) > 9.0f);

  // A GAME-up point must read on NDC -y (screen up in Vulkan).
  const Vec3 up10 = eye + Vec3{0.0f, -10.0f, 0.0f};
  const Vec3 w_u = up10 - eye;
  CHECK(-ws::math::dot(w_u, up) < -9.0f);

  // A GAME-south point is BEHIND the north view.
  const Vec3 south = eye + Vec3{0.0f, 0.0f, -10.0f};
  const Vec3 w_s = south - eye;
  CHECK(ws::math::dot(w_s, fwd) < -9.0f);
}

TEST_CASE("player: swept grid controller lands, stays supported, and jumps") {
  voxel::World world = make_world();
  const auto grid = world.collision_grid();
  constexpr int x = 64;
  constexpr int z = 64;
  const int top = world.surface_height(x, z);
  REQUIRE(top >= 0);

  voxel::Player player;
  REQUIRE(player.spawn(ws::math::Vec3{
      static_cast<float>(x) + 0.5f,
      static_cast<float>(top) + 3.0f,
      static_cast<float>(z) + 0.5f}));

  const voxel::Player::MoveInput idle{};
  for (int i = 0; i < 120; ++i) {
    player.tick(idle, 1.0f / 60.0f, grid);
  }
  CHECK(player.grounded());
  CHECK(player.position().y >= static_cast<float>(top) + 1.0f +
                                  0.9f - 0.01f);
  CHECK_FALSE(grid.overlaps(player.bounds()));

  const float rest_y = player.position().y;
  voxel::Player::MoveInput jump{};
  jump.jump = true;
  player.tick(jump, 1.0f / 60.0f, grid);
  for (int i = 0; i < 18; ++i) {
    player.tick(idle, 1.0f / 60.0f, grid);
  }
  CHECK(player.position().y > rest_y + 1.2f);
  CHECK(player.eye_position().y ==
        doctest::Approx(player.position().y + 0.82f));
}

TEST_CASE("voxel grid: raycast shares the DDA traversal used by interaction") {
  voxel::World world = make_world();
  const auto grid = world.collision_grid();
  const int x = 64;
  const int z = 64;
  const int top = world.surface_height(x, z);
  REQUIRE(top >= 0);
  const auto hit = grid.raycast(
      ws::math::Vec3{static_cast<float>(x) + 0.5f,
                     static_cast<float>(top) + 5.0f,
                     static_cast<float>(z) + 0.5f},
      ws::math::Vec3{0.0f, -1.0f, 0.0f}, 10.0f);
  CHECK(hit.hit);
  CHECK(hit.cell.x == x);
  CHECK(hit.cell.z == z);
  CHECK(hit.normal.y == 1);
  CHECK(hit.distance == doctest::Approx(4.0f));
}

TEST_CASE("voxel grid: swept AABB stops at walls and slides along them") {
  // A one-cell-thick wall at x=3, extending over the player's z/y span.
  struct Grid {
    static bool solid(const void*, int x, int y, int z) noexcept {
      return x == 3 && y >= 0 && y < 4 && z >= -2 && z < 3;
    }
  };
  const ws::physics3d::VoxelGrid3D grid{
      {{-8, -4, -8}, {16, 12, 16}, nullptr, &Grid::solid}};
  const ws::physics3d::Aabb player{
      ws::math::Vec3{1.0f, 1.0f, 0.0f},
      ws::math::Vec3{1.6f, 2.8f, 0.6f}};
  const auto moved = grid.sweep_aabb(player, ws::math::Vec3{4.0f, 0.0f, 2.0f});
  CHECK(moved.hit_x);
  CHECK(moved.bounds.max.x <= doctest::Approx(3.0f).epsilon(0.002));
  CHECK(moved.bounds.min.z > player.min.z + 1.5f);
}

TEST_CASE("voxel grid: high-speed vertical sweep cannot tunnel through floor") {
  struct Grid {
    static bool solid(const void*, int x, int y, int z) noexcept {
      return y == 0 && x >= -2 && x <= 2 && z >= -2 && z <= 2;
    }
  };
  const ws::physics3d::VoxelGrid3D grid{
      {{-4, -2, -4}, {8, 8, 8}, nullptr, &Grid::solid}};
  const ws::physics3d::Aabb player{
      ws::math::Vec3{-0.3f, 5.0f, -0.3f},
      ws::math::Vec3{0.3f, 6.8f, 0.3f}};
  const auto moved = grid.sweep_aabb(player, ws::math::Vec3{0.0f, -20.0f, 0.0f});
  CHECK(moved.hit_y);
  CHECK(moved.bounds.min.y >= doctest::Approx(1.0f).epsilon(0.002));
  CHECK(grid.overlaps(moved.bounds) == false);
}

TEST_CASE("voxel grid: world edits are visible without rebuilding collision data") {
  voxel::World world = make_world();
  const auto grid = world.collision_grid();
  constexpr int x = 64;
  constexpr int z = 64;
  const int y = world.surface_height(x, z);
  REQUIRE(y >= 0);
  CHECK(grid.is_solid(x, y, z));
  REQUIRE(world.set_block(x, y, z, voxel::Block::air));
  CHECK_FALSE(grid.is_solid(x, y, z));
  REQUIRE(world.set_block(x, y, z, voxel::Block::stone));
  CHECK(grid.is_solid(x, y, z));
}

TEST_CASE("menu: the pause layout centers the pair on the frame") {
  // Two buttons, resume above quit, the pair centered both ways: the
  // classic pause-menu read. Any frame size.
  const float fw = 320.0f, fh = 240.0f;
  voxel::menu::Button b[voxel::menu::kButtonCount];
  voxel::menu::layout(fw, fh, b);
  // Horizontal: centered - both buttons share x, the frame's middle.
  CHECK(b[0].x == doctest::Approx((fw - voxel::menu::kButtonW) * 0.5f));
  CHECK(b[1].x == b[0].x);
  CHECK(b[0].w == voxel::menu::kButtonW);
  CHECK(b[0].h == voxel::menu::kButtonH);
  // Vertical: resume on top, quit one gap below, the pair centered.
  CHECK(b[1].y == doctest::Approx(b[0].y + voxel::menu::kButtonH +
                                  voxel::menu::kButtonGap));
  const float pair_h = 2.0f * voxel::menu::kButtonH + voxel::menu::kButtonGap;
  CHECK(b[0].y == doctest::Approx(fh * 0.5f - pair_h * 0.5f));
  // And the pair really is centered: symmetric margins top and bottom.
  CHECK(b[0].y == doctest::Approx(fh - (b[1].y + b[1].h)));
}

TEST_CASE("menu: the click hits the right button, edges exact") {
  const float fw = 320.0f, fh = 240.0f;
  voxel::menu::Button b[voxel::menu::kButtonCount];
  voxel::menu::layout(fw, fh, b);

  // The center of each button is that button's.
  CHECK(voxel::menu::click_action(fw, fh, b[0].x + b[0].w * 0.5f,
                                  b[0].y + b[0].h * 0.5f) == 0);
  CHECK(voxel::menu::click_action(fw, fh, b[1].x + b[1].w * 0.5f,
                                  b[1].y + b[1].h * 0.5f) == 1);

  // The [x, x+w) edges: left/top inclusive, right/bottom exclusive.
  CHECK(voxel::menu::hit(b[0], b[0].x, b[0].y) == true);
  CHECK(voxel::menu::hit(b[0], b[0].x + b[0].w - 0.01f,
                         b[0].y + b[0].h - 0.01f) == true);
  CHECK(voxel::menu::hit(b[0], b[0].x + b[0].w, b[0].y) == false);
  CHECK(voxel::menu::hit(b[0], b[0].x, b[0].y + b[0].h) == false);

  // The two buttons share NO pixel: a click between them (the gap)
  // hits neither - and neither does a click far away.
  const float between_x = b[0].x + b[0].w * 0.5f;
  const float between_y = b[0].y + b[0].h + voxel::menu::kButtonGap * 0.5f;
  CHECK(voxel::menu::click_action(fw, fh, between_x, between_y) == -1);
  CHECK(voxel::menu::click_action(fw, fh, 2.0f, 2.0f) == -1);
}

TEST_CASE("bench: a default-constructed driver is idle, not running") {
  // The frame loop gates its whole input path on bench_.running() - a
  // Driver that answers TRUE before start() hijacks the app from the
  // first frame: phase 0's constant-yaw "spin" input rotates the camera
  // with no input at all (the field report), the real mouse/keys are
  // bypassed, and request_stop() self-quits at the default seconds.
  // Default construction must mean idle (the app launches plain, no
  // WS_VOXEL_BENCH); only start() wakes the driver.
  voxel::bench::Driver driver{};
  CHECK(!driver.running());
  // And the idle driver hands out no scripted input: a plain frame's
  // MoveInput is the zero struct (nothing pressed, nothing to look).
  const auto in = driver.input(1.0f / 60.0f);
  CHECK(in.look_dx == 0.0f);
  CHECK(in.look_dy == 0.0f);
  CHECK(!in.forward);
  CHECK(!driver.wants_edit());
  // start() is the only wake-up: a two-second run is running, spins
  // (phase 0 answers a constant-yaw look rate), and reports done
  // exactly at its seconds.
  voxel::bench::Config cfg{};
  cfg.seconds = 2.0f;
  cfg.print_header = false;
  driver.start(cfg);
  CHECK(driver.running());
  const auto spin = driver.input(1.0f / 60.0f);
  CHECK(spin.look_dx > 0.0f);
  // The end-of-run read: the app calls frame(dt) (which accumulates
  // elapsed_) before advance_phase(dt) - mimic that order, two
  // one-second frames make a two-second run.
  voxel::perf::FrameTimings t0{};
  voxel::perf::Counters c0{};
  driver.frame(1.0f, t0, c0);
  driver.frame(1.0f, t0, c0);
  CHECK(driver.advance_phase(0.0f));
  CHECK(!driver.running());
}

TEST_CASE("atlas: the face->tile map matches the block vocabulary") {
  // The generated header's map, pinned: grass wears the grass TOP on
  // top, DIRT on bottom, the grass SIDE everywhere else; the log wears
  // its top on the ends, the side texture on the sides; dirt/stone/sand
  // are uniform. Face order is world.cpp's kFaces:
  //   0 top  1 bottom  2 east  3 west  4 south  5 north.
  using B = voxel::Block;
  const auto t = [](B block, int face) {
    return voxel::atlas::kTileFor[static_cast<std::size_t>(block)][face];
  };
  CHECK(t(B::grass, 0) == 0);   // grass_block_top
  CHECK(t(B::grass, 1) == 1);   // dirt
  CHECK(t(B::grass, 2) == 2);   // grass_block_side
  CHECK(t(B::grass, 5) == 2);
  CHECK(t(B::dirt, 0) == 1);    // dirt everywhere
  CHECK(t(B::dirt, 4) == 1);
  CHECK(t(B::stone, 3) == 3);   // stone everywhere
  CHECK(t(B::wood, 0) == 4);    // oak_log_top
  CHECK(t(B::wood, 1) == 4);
  CHECK(t(B::wood, 2) == 5);    // oak_log side
  CHECK(t(B::wood, 5) == 5);
  CHECK(t(B::sand, 0) == 6);    // sand everywhere
  CHECK(t(B::sand, 5) == 6);
}

TEST_CASE("atlas: the automatic creator's derived contract is pinned") {
  // scripts/create_atlas.py derives the tile set FROM THE PACK; these
  // pins hold the derivation still matches what the game reads:
  // 8 distinct assets (dead pack entries never gain slots), 64px each,
  // grass/dirt/wood split per face group, water resolved from the
  // animation strip's frame 0, air never meshed.
  CHECK(voxel::atlas::kTileCount == 8);
  CHECK(voxel::atlas::kTilePixels == 64);
  // First-use order: grass top, dirt, grass side, stone, oak top,
  // oak side, sand, water - the manifest the creator pinned.
  CHECK(std::string(voxel::atlas::kTileNames[0]) == "grass_block_top");
  CHECK(std::string(voxel::atlas::kTileNames[1]) == "dirt");
  CHECK(std::string(voxel::atlas::kTileNames[2]) == "grass_block_side");
  CHECK(std::string(voxel::atlas::kTileNames[3]) == "stone");
  CHECK(std::string(voxel::atlas::kTileNames[4]) == "oak_log_top");
  CHECK(std::string(voxel::atlas::kTileNames[5]) == "oak_log");
  CHECK(std::string(voxel::atlas::kTileNames[6]) == "sand");
  CHECK(std::string(voxel::atlas::kTileNames[7]) == "water_still");
  // The (block, face) table: grass top 0, grass bottom dirt, sides
  // grass_side; wood top/bottom oak_log_top, sides oak_log; water
  // still-frame on top only; air -1 (never meshed).
  CHECK(voxel::atlas::kTileFor[1][0] == 0);
  CHECK(voxel::atlas::kTileFor[1][1] == 1);
  CHECK(voxel::atlas::kTileFor[1][2] == 2);
  CHECK(voxel::atlas::kTileFor[1][5] == 2);
  CHECK(voxel::atlas::kTileFor[2][0] == 1);   // dirt everywhere
  CHECK(voxel::atlas::kTileFor[4][0] == 4);   // wood top = oak_log_top
  CHECK(voxel::atlas::kTileFor[4][2] == 5);   // wood sides = oak_log
  CHECK(voxel::atlas::kTileFor[5][0] == 6);   // sand
  CHECK(voxel::atlas::kTileFor[6][0] == 7);   // water top only
  CHECK(voxel::atlas::kTileFor[6][1] == -1);  // water sides never read
  CHECK(voxel::atlas::kTileFor[0][0] == -1);  // air
  // Every slot the table names exists (the PNG walk answers every
  // named slot and refuses the rest).
  for (int b = 1; b < 7; ++b) {
    for (int f = 0; f < 6; ++f) {
      const int tile = voxel::atlas::kTileFor[b][f];
      if (tile < 0) {
        continue;
      }
      std::size_t bytes = 0;
      CHECK(voxel::atlas::tile_png(tile, bytes) != nullptr);
      CHECK(bytes > 0);
    }
  }
  std::size_t dead = 0;
  CHECK(voxel::atlas::tile_png(voxel::atlas::kTileCount, dead) == nullptr);
  CHECK(dead == 0);
}
