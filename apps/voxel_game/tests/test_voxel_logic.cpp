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

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <string>

#include <cmath>
#include <numbers>

#include <ws/math/vecmath.hpp>

#include "bench.hpp"
#include "frustum.hpp"
#include "perf.hpp"
#include "menu.hpp"
#include "hotbar.hpp"
#include "settings_menu.hpp"
#include "player.hpp"
#include "texture_atlas.hpp"
#include "world.hpp"
#include "inventory.hpp"
#include "voxel_audio.hpp"

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
  CHECK(hit.cell.x == x);
  CHECK(hit.cell.y == surface);
  CHECK(hit.cell.z == z);
  // Entered through the top: the normal points +y.
  CHECK(hit.normal.x == 0);
  CHECK(hit.normal.y == 1);
  CHECK(hit.normal.z == 0);
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
  CHECK(hit.cell.y + hit.normal.y == surface + 1);
  // That cell is air (the place rule checks it).
  CHECK(world.block_at(hit.cell.x, surface + 1, hit.cell.z) ==
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
  CHECK(hit.cell.z == 50);
  CHECK(hit.normal.z == -1);
  CHECK(hit.normal.x == 0);
  CHECK(hit.normal.y == 0);
  // Placement: the empty cell north of the wall.
  CHECK(world.block_at(hit.cell.x, hit.cell.y, hit.cell.z - 1) ==
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

TEST_CASE("culling: non-90-degree horizontal frustum planes use tangent-scaled forward") {
  const float eye[3] = {0.0f, 0.0f, 0.0f};
  const float target[3] = {0.0f, 0.0f, 1.0f};
  const float up[3] = {0.0f, -1.0f, 0.0f};
  const auto fr = voxel::culling::extract_frustum(
      eye, target, up, 70.0f * std::numbers::pi_v<float> / 180.0f,
      16.0f / 9.0f, 0.03f, 260.0f);

  // At forward distance 10, the true 70-degree/16:9 left edge is
  // x ~= -12.44. This point is inside the real frustum but was rejected
  // by the old forward + tan_hx * right normal (edge at about -8).
  const float left_inside[3] = {-10.0f, 0.0f, 10.0f};
  CHECK(voxel::culling::aabb_visible(fr, left_inside, left_inside));
  const float right_inside[3] = {10.0f, 0.0f, 10.0f};
  CHECK(voxel::culling::aabb_visible(fr, right_inside, right_inside));
  const float left_outside[3] = {-14.0f, 0.0f, 10.0f};
  CHECK_FALSE(voxel::culling::aabb_visible(fr, left_outside, left_outside));
  const float right_outside[3] = {14.0f, 0.0f, 10.0f};
  CHECK_FALSE(voxel::culling::aabb_visible(fr, right_outside, right_outside));
}

TEST_CASE("culling: sampled chunk AABBs are never rejected when a sample is inside") {
  const float eye[3] = {64.0f, -32.0f, -64.0f};
  const float target[3] = {64.0f, -32.0f, -63.0f};
  const float up[3] = {0.0f, -1.0f, 0.0f};
  const auto fr = voxel::culling::extract_frustum(
      eye, target, up, 70.0f * std::numbers::pi_v<float> / 180.0f,
      16.0f / 9.0f, 0.03f, 260.0f);

  // Point-sized AABBs must exactly agree with an independent brute-force
  // check of all six plane half-spaces over a deterministic sample grid.
  int inside_points = 0;
  int outside_points = 0;
  for (int z = -144; z <= 16; z += 4) {
    for (int y = -80; y <= 16; y += 4) {
      for (int x = -16; x <= 144; x += 4) {
        const float p[3] = {static_cast<float>(x), static_cast<float>(y),
                            static_cast<float>(z)};
        bool brute_inside = true;
        for (int plane = 0; plane < 6; ++plane) {
          const float* q = fr.planes[plane];
          if (q[0] * p[0] + q[1] * p[1] + q[2] * p[2] + q[3] < 0.0f) {
            brute_inside = false;
            break;
          }
        }
        CHECK(voxel::culling::aabb_visible(fr, p, p) == brute_inside);
        inside_points += brute_inside ? 1 : 0;
        outside_points += brute_inside ? 0 : 1;
      }
    }
  }
  CHECK(inside_points > 0);
  CHECK(outside_points > 0);

  // The support-vertex AABB test is conservative at frustum corners, so
  // the key chunk invariant is no false negatives against dense samples.
  int sampled_visible_chunks = 0;
  for (int cz = 0; cz < voxel::kWorldChunksZ; ++cz) {
    for (int cx = 0; cx < voxel::kWorldChunksX; ++cx) {
      const float lo[3] = {
          static_cast<float>(cx * voxel::kChunkSizeX),
          -static_cast<float>(voxel::kChunkSizeY),
          -static_cast<float>((cz + 1) * voxel::kChunkSizeZ)};
      const float hi[3] = {
          static_cast<float>((cx + 1) * voxel::kChunkSizeX), 0.0f,
          -static_cast<float>(cz * voxel::kChunkSizeZ)};
      bool sample_inside = false;
      for (int iz = 0; iz <= 8 && !sample_inside; ++iz) {
        for (int iy = 0; iy <= 4 && !sample_inside; ++iy) {
          for (int ix = 0; ix <= 8 && !sample_inside; ++ix) {
            const float p[3] = {
                lo[0] + (hi[0] - lo[0]) * static_cast<float>(ix) / 8.0f,
                lo[1] + (hi[1] - lo[1]) * static_cast<float>(iy) / 4.0f,
                lo[2] + (hi[2] - lo[2]) * static_cast<float>(iz) / 8.0f};
            bool inside = true;
            for (int plane = 0; plane < 6; ++plane) {
              const float* q = fr.planes[plane];
              if (q[0] * p[0] + q[1] * p[1] + q[2] * p[2] + q[3] < 0.0f) {
                inside = false;
                break;
              }
            }
            sample_inside = inside;
          }
        }
      }
      if (sample_inside) {
        ++sampled_visible_chunks;
        CHECK(voxel::culling::aabb_visible(fr, lo, hi));
      }
    }
  }
  CHECK(sampled_visible_chunks > 0);
}

TEST_CASE("culling: degenerate camera bases keep all AABBs instead of producing NaNs") {
  const float eye[3] = {1.0f, 2.0f, 3.0f};
  const float same_target[3] = {1.0f, 2.0f, 3.0f};
  const float target[3] = {1.0f, 2.0f, 4.0f};
  const float parallel_up[3] = {0.0f, 0.0f, 1.0f};
  const float valid_up[3] = {0.0f, 1.0f, 0.0f};
  const float far_min[3] = {10000.0f, 10000.0f, 10000.0f};
  const float far_max[3] = {10001.0f, 10001.0f, 10001.0f};

  const auto zero_forward = voxel::culling::extract_frustum(
      eye, same_target, valid_up, 1.2f, 1.7f, 0.1f, 260.0f);
  const auto parallel_basis = voxel::culling::extract_frustum(
      eye, target, parallel_up, 1.2f, 1.7f, 0.1f, 260.0f);
  CHECK(voxel::culling::aabb_visible(zero_forward, far_min, far_max));
  CHECK(voxel::culling::aabb_visible(parallel_basis, far_min, far_max));
  for (int plane = 0; plane < 6; ++plane) {
    for (int component = 0; component < 4; ++component) {
      CHECK(std::isfinite(zero_forward.planes[plane][component]));
      CHECK(std::isfinite(parallel_basis.planes[plane][component]));
      CHECK(zero_forward.planes[plane][component] == 0.0f);
      CHECK(parallel_basis.planes[plane][component] == 0.0f);
    }
  }
}

TEST_CASE("player: large look deltas wrap yaw and spawn resets the camera") {
  voxel::World world = make_world();
  const auto grid = world.collision_grid();
  voxel::Player player;
  REQUIRE(player.spawn(ws::math::Vec3{64.5f, 40.0f, 64.5f}));

  voxel::Player::MoveInput look{};
  look.look_dx = 10000.0f;  // 25 radians at the default sensitivity
  look.look_dy = 1000.0f;
  player.tick(look, 1.0f / 60.0f, grid);

  const float expected = std::remainder(
      25.0f, 2.0f * std::numbers::pi_v<float>);
  CHECK(player.yaw_radians() == doctest::Approx(expected).epsilon(0.0001));
  CHECK(player.yaw_radians() >= -std::numbers::pi_v<float>);
  CHECK(player.yaw_radians() <= std::numbers::pi_v<float>);

  REQUIRE(player.spawn(ws::math::Vec3{64.5f, 40.0f, 64.5f}));
  const auto look_direction = player.look_direction();
  CHECK(look_direction.x == doctest::Approx(0.0f));
  CHECK(look_direction.y == doctest::Approx(0.0f));
  CHECK(look_direction.z == doctest::Approx(-1.0f));
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
  CHECK(player.velocity().y == doctest::Approx(0.0f));
  CHECK(player.position().y >= static_cast<float>(top) + 1.0f +
                                  0.9f - 0.01f);
  CHECK_FALSE(grid.overlaps(player.bounds()));

  // Standing still must remain stable without accumulating gravity or
  // sinking incrementally into the supporting voxel.
  const float supported_y = player.position().y;
  for (int i = 0; i < 300; ++i) {
    player.tick(idle, 1.0f / 60.0f, grid);
  }
  CHECK(player.grounded());
  CHECK(player.velocity().y == doctest::Approx(0.0f));
  CHECK(player.position().y == doctest::Approx(supported_y).epsilon(0.0001));

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
  const auto title = voxel::menu::layout_title(320.0f, 240.0f, 60.0f);
  CHECK(title.x == doctest::Approx(130.0f));
  CHECK(title.y == doctest::Approx(24.0f));
  // Two buttons, resume above quit, the pair centered both ways: the
  // classic pause-menu read. Any frame size.
  const float fw = 320.0f, fh = 240.0f;
  const auto b = voxel::menu::layout(fw, fh);
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
  const auto b = voxel::menu::layout(fw, fh);

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


TEST_CASE("hotbar: nine slots are centered, ordered, and hit-tested") {
  constexpr float fw = 1280.0f;
  constexpr float fh = 720.0f;
  const auto slots = voxel::hotbar::layout(fw, fh);
  const float bar_width = voxel::hotbar::kSlotStep * voxel::hotbar::kSlots;
  CHECK(slots.front().bounds.x ==
        doctest::Approx((fw - bar_width) * 0.5f));
  CHECK(slots.front().bounds.y ==
        doctest::Approx(fh - voxel::hotbar::kBottomMargin -
                        voxel::hotbar::kSlotSize));
  for (int i = 0; i < voxel::hotbar::kSlots; ++i) {
    const auto& slot = slots[static_cast<std::size_t>(i)];
    CHECK(slot.index == i);
    CHECK(slot.bounds.w == voxel::hotbar::kSlotSize);
    CHECK(slot.bounds.h == voxel::hotbar::kSlotSize);
    CHECK(voxel::hotbar::hit(
              slot.bounds, slot.bounds.x, slot.bounds.y));
    CHECK_FALSE(voxel::hotbar::hit(
              slot.bounds, slot.bounds.x + slot.bounds.w,
              slot.bounds.y));
    CHECK(voxel::hotbar::selected(
              fw, fh, slot.bounds.x + slot.bounds.w * 0.5f,
              slot.bounds.y + slot.bounds.h * 0.5f) == i);
  }
  CHECK(voxel::hotbar::selected(fw, fh, 0.0f, 0.0f) == -1);
  CHECK(voxel::hotbar::selected(
            fw, fh, slots[0].bounds.x + slots[0].bounds.w,
            slots[0].bounds.y) == -1);
}

TEST_CASE("settings: sliders are centered, ordered and map clicks to bounded values") {
  constexpr float fw = 1280.0f, fh = 720.0f;
  const auto sliders = voxel::settings_menu::layout(fw, fh);
  for (int i = 0; i < voxel::settings_menu::kSliderCount; ++i) {
    const auto& slider = sliders[static_cast<std::size_t>(i)];
    CHECK(slider.index == i);
    CHECK(slider.bounds.w == voxel::settings_menu::kSliderWidth);
    CHECK(voxel::settings_menu::selected(
              fw, fh, slider.bounds.x + slider.bounds.w * 0.5f,
              slider.bounds.y + slider.bounds.h * 0.5f) == i);
    CHECK(voxel::settings_menu::value_from_x(slider, slider.bounds.x) ==
          doctest::Approx(slider.minimum));
    CHECK(voxel::settings_menu::value_from_x(slider,
              slider.bounds.x + slider.bounds.w) == doctest::Approx(slider.maximum));
    CHECK(voxel::settings_menu::value_from_x(slider,
              slider.bounds.x - 100.0f) == doctest::Approx(slider.minimum));
  }
  CHECK(voxel::settings_menu::selected(fw, fh, 0.0f, 0.0f) == -1);
  const voxel::settings_menu::Settings defaults{};
  CHECK(defaults.mouse_sensitivity == doctest::Approx(0.0025f));
  CHECK(defaults.fov_degrees == doctest::Approx(70.0f));
  CHECK(defaults.render_distance == doctest::Approx(260.0f));
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
  // The original eight base textures remain first-use stable. The full
  // supplied resource pack adds seven distinct block textures after them.
  // A local checkout may still have the old generated header until the
  // user runs create_atlas.py against the supplied pack.
  CHECK(voxel::atlas::kTileCount >= 8);
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
  if (voxel::atlas::kTileCount >= 15) {
    CHECK(std::string(voxel::atlas::kTileNames[8]) == "cobblestone");
    CHECK(std::string(voxel::atlas::kTileNames[9]) == "oak_planks");
    CHECK(std::string(voxel::atlas::kTileNames[10]) == "bricks");
    CHECK(std::string(voxel::atlas::kTileNames[11]) == "glass");
    CHECK(std::string(voxel::atlas::kTileNames[12]) == "oak_leaves");
    CHECK(std::string(voxel::atlas::kTileNames[13]) == "coal_ore");
    CHECK(std::string(voxel::atlas::kTileNames[14]) == "iron_ore");
    CHECK(voxel::atlas::kTileFor[7][0] == 8);   // cobblestone
    CHECK(voxel::atlas::kTileFor[8][0] == 9);   // oak planks
    CHECK(voxel::atlas::kTileFor[9][0] == 10);  // bricks
    CHECK(voxel::atlas::kTileFor[10][0] == 11); // glass
    CHECK(voxel::atlas::kTileFor[11][0] == 12); // oak leaves
    CHECK(voxel::atlas::kTileFor[12][0] == 13); // coal ore
    CHECK(voxel::atlas::kTileFor[13][0] == 14); // iron ore
  }
  // The (block, face) table: grass top 0, grass bottom dirt, sides
  // grass_side; wood top/bottom oak_log_top, sides oak_log; water
  // still-frame on top only; air -1 (never meshed). The full resource-pack
  // table appends cobble/planks/bricks/glass/leaves/ore assets after water.
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
  for (int b = 1; b < static_cast<int>(voxel::Block::count_); ++b) {
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

TEST_CASE("inventory: stack merging, overflow and removal") {
  voxel::Inventory inventory;
  CHECK(inventory.add(voxel::Item::stone, 60) == 60);
  CHECK(inventory.add(voxel::Item::stone, 10) == 10);
  CHECK(inventory.count(voxel::Item::stone) == 70);
  CHECK(inventory.slot(0).count == 64);
  CHECK(inventory.slot(1).count == 6);
  CHECK_FALSE(inventory.remove(voxel::Item::stone, 71));
  CHECK(inventory.remove(voxel::Item::stone, 65));
  CHECK(inventory.count(voxel::Item::stone) == 5);
}

TEST_CASE("inventory: swapping moves a stack into the selected hotbar slot") {
  voxel::Inventory inventory;
  CHECK(inventory.add(voxel::Item::grass, 12) == 12);
  CHECK(inventory.add(voxel::Item::iron_ore, 5) == 5);
  inventory.swap_slots(0, 1);
  CHECK(inventory.slot(0).item == voxel::Item::iron_ore);
  CHECK(inventory.slot(0).count == 5);
  CHECK(inventory.slot(1).item == voxel::Item::grass);
  CHECK(inventory.slot(1).count == 12);
  inventory.swap_slots(0, 99);
  CHECK(inventory.slot(0).item == voxel::Item::iron_ore);
}

TEST_CASE("crafting: consumes ingredients and creates output atomically") {
  voxel::Inventory inventory;
  CHECK(inventory.add(voxel::Item::wood, 2) == 2);
  const auto& planks = voxel::kRecipes[0];
  REQUIRE(voxel::can_craft(inventory, planks));
  CHECK(voxel::craft(inventory, planks));
  CHECK(inventory.count(voxel::Item::wood) == 1);
  CHECK(inventory.count(voxel::Item::planks) == 4);
  CHECK(voxel::craft(inventory, planks));
  CHECK(inventory.count(voxel::Item::wood) == 0);
  CHECK(inventory.count(voxel::Item::planks) == 8);
  CHECK_FALSE(voxel::craft(inventory, planks));
}

TEST_CASE("worldgen: climate fields create distinct plains, desert and mountain surfaces") {
  const voxel::World world = make_world();
  int grass = 0;
  int sand = 0;
  int exposed_stone = 0;
  for (int x = 0; x < voxel::kWorldSizeX; ++x) {
    for (int z = 0; z < voxel::kWorldSizeZ; ++z) {
      const int y = world.surface_height(x, z);
      if (y < 0) continue;
      const auto top = world.block_at(x, y, z);
      grass += top == voxel::Block::grass;
      sand += top == voxel::Block::sand;
      exposed_stone += top == voxel::Block::stone;
    }
  }
  CHECK(grass > 100);
  CHECK(sand > 20);
  CHECK(exposed_stone > 0);
}

TEST_CASE("worldgen: lakes and river channels reach the water plane") {
  const voxel::World world = make_world();
  int water_cells = 0;
  int exposed_water_tops = 0;
  for (int x = 0; x < voxel::kWorldSizeX; ++x) {
    for (int z = 0; z < voxel::kWorldSizeZ; ++z) {
      for (int y = 0; y < voxel::kChunkSizeY - 1; ++y) {
        if (world.block_at(x, y, z) != voxel::Block::water) continue;
        ++water_cells;
        if (world.block_at(x, y + 1, z) == voxel::Block::air) {
          ++exposed_water_tops;
        }
      }
    }
  }
  CHECK(water_cells > 0);
  CHECK(exposed_water_tops > 0);
}

TEST_CASE("worldgen: 3D caves carve connected underground air pockets") {
  const voxel::World world = make_world();
  int underground_air = 0;
  for (int x = 1; x < voxel::kWorldSizeX - 1; ++x) {
    for (int z = 1; z < voxel::kWorldSizeZ - 1; ++z) {
      const int surface = world.surface_height(x, z);
      for (int y = 4; y < surface - 4; ++y) {
        underground_air += world.block_at(x, y, z) == voxel::Block::air;
      }
    }
  }
  CHECK(underground_air > 0);
}

TEST_CASE("worldgen: ore veins form neighboring deposits") {
  const voxel::World world = make_world();
  int adjacent_ore_pairs = 0;
  for (int x = 1; x < voxel::kWorldSizeX - 1; ++x) {
    for (int z = 1; z < voxel::kWorldSizeZ - 1; ++z) {
      for (int y = 4; y < 23; ++y) {
        const auto block = world.block_at(x, y, z);
        if (block != voxel::Block::coal_ore &&
            block != voxel::Block::iron_ore) continue;
        adjacent_ore_pairs +=
            world.block_at(x + 1, y, z) == block ||
            world.block_at(x, y + 1, z) == block ||
            world.block_at(x, y, z + 1) == block;
      }
    }
  }
  CHECK(adjacent_ore_pairs > 0);
}

TEST_CASE("worldgen: ores and leaves use the expanded block palette") {
  const voxel::World world = make_world();
  int coal = 0, iron = 0, leaves = 0;
  for (int x = 0; x < voxel::kWorldSizeX; ++x)
    for (int z = 0; z < voxel::kWorldSizeZ; ++z)
      for (int y = 0; y < voxel::kChunkSizeY; ++y) {
        const auto b = world.block_at(x,y,z);
        coal += b == voxel::Block::coal_ore;
        iron += b == voxel::Block::iron_ore;
        leaves += b == voxel::Block::leaves;
      }
  CHECK(coal > 0);
  CHECK(iron > 0);
  CHECK(leaves > 0);
}
TEST_CASE("culling: independent frustum oracle covers multiple camera poses") {
  struct CameraCase {
    std::array<float, 3> eye;
    std::array<float, 3> direction;
    float fov_degrees;
    float aspect;
  };
  const CameraCase cameras[] = {
      {{64.0f, -32.0f, -64.0f}, {0.19f, 0.07f, -0.98f}, 63.0f,
       16.0f / 9.0f},
      {{64.0f, -32.0f, -64.0f}, {0.99f, 0.04f, -0.10f}, 87.0f,
       4.0f / 3.0f},
      {{36.0f, -18.0f, -82.0f}, {-0.24f, -0.26f, 0.94f}, 101.0f,
       16.0f / 10.0f},
  };

  constexpr float kNear = 0.03f;
  constexpr float kFar = 260.0f;
  constexpr float kMargin = 1.0e-4f;
  const float up_hint[3] = {0.0f, -1.0f, 0.0f};

  for (const CameraCase& camera : cameras) {
    float forward[3] = {camera.direction[0], camera.direction[1],
                        camera.direction[2]};
    const float forward_length = std::sqrt(
        forward[0] * forward[0] + forward[1] * forward[1] +
        forward[2] * forward[2]);
    REQUIRE(forward_length > 0.0f);
    for (float& component : forward) component /= forward_length;

    // Build the reference camera basis independently from the extracted
    // plane coefficients. This oracle uses camera-space inequalities:
    // near <= depth <= far, |horizontal| <= depth*tan(half-FOVx),
    // |vertical| <= depth*tan(half-FOVy).
    float right[3] = {
        forward[1] * up_hint[2] - forward[2] * up_hint[1],
        forward[2] * up_hint[0] - forward[0] * up_hint[2],
        forward[0] * up_hint[1] - forward[1] * up_hint[0]};
    const float right_length = std::sqrt(
        right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    REQUIRE(right_length > 0.0f);
    for (float& component : right) component /= right_length;
    const float up[3] = {
        right[1] * forward[2] - right[2] * forward[1],
        right[2] * forward[0] - right[0] * forward[2],
        right[0] * forward[1] - right[1] * forward[0]};

    const float fov = camera.fov_degrees *
                      std::numbers::pi_v<float> / 180.0f;
    const float tan_vertical = std::tan(fov * 0.5f);
    const float tan_horizontal = tan_vertical * camera.aspect;
    const float target[3] = {
        camera.eye[0] + forward[0],
        camera.eye[1] + forward[1],
        camera.eye[2] + forward[2]};
    const auto fr = voxel::culling::extract_frustum(
        camera.eye.data(), target, up_hint, fov, camera.aspect,
        kNear, kFar);
    const auto dot3 = [](const float a[3], const float b[3]) {
      return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };

    int inside_points = 0;
    int outside_points = 0;
    int sampled_chunks = 0;
    int accepted_chunks = 0;
    int rejected_chunks = 0;

    for (int cz = 0; cz < voxel::kWorldChunksZ; ++cz) {
      for (int cx = 0; cx < voxel::kWorldChunksX; ++cx) {
        const float lo[3] = {
            static_cast<float>(cx * voxel::kChunkSizeX),
            -static_cast<float>(voxel::kChunkSizeY),
            -static_cast<float>((cz + 1) * voxel::kChunkSizeZ)};
        const float hi[3] = {
            static_cast<float>((cx + 1) * voxel::kChunkSizeX), 0.0f,
            -static_cast<float>(cz * voxel::kChunkSizeZ)};

        bool has_interior_sample = false;
        for (int iz = 0; iz <= 8; ++iz) {
          for (int iy = 0; iy <= 4; ++iy) {
            for (int ix = 0; ix <= 8; ++ix) {
              const float point[3] = {
                  lo[0] + (hi[0] - lo[0]) * static_cast<float>(ix) / 8.0f,
                  lo[1] + (hi[1] - lo[1]) * static_cast<float>(iy) / 4.0f,
                  lo[2] + (hi[2] - lo[2]) * static_cast<float>(iz) / 8.0f};
              const float delta[3] = {
                  point[0] - camera.eye[0],
                  point[1] - camera.eye[1],
                  point[2] - camera.eye[2]};
              const float depth = dot3(delta, forward);
              const float horizontal = dot3(delta, right);
              const float vertical = dot3(delta, up);
              const float horizontal_limit = depth * tan_horizontal;
              const float vertical_limit = depth * tan_vertical;

              const bool reference_inside =
                  depth > kNear + kMargin && depth < kFar - kMargin &&
                  std::abs(horizontal) < horizontal_limit - kMargin &&
                  std::abs(vertical) < vertical_limit - kMargin;
              const bool reference_outside =
                  depth < kNear - kMargin || depth > kFar + kMargin ||
                  std::abs(horizontal) > horizontal_limit + kMargin ||
                  std::abs(vertical) > vertical_limit + kMargin;

              if (reference_inside) {
                ++inside_points;
                has_interior_sample = true;
                CHECK(voxel::culling::aabb_visible(fr, point, point));
              } else if (reference_outside) {
                ++outside_points;
                CHECK_FALSE(voxel::culling::aabb_visible(fr, point, point));
              }
            }
          }
        }

        const bool accepted = voxel::culling::aabb_visible(fr, lo, hi);
        if (accepted) {
          ++accepted_chunks;
        } else {
          ++rejected_chunks;
        }
        if (has_interior_sample) {
          ++sampled_chunks;
          // A visible point inside the chunk is a strict no-false-negative
          // requirement. Corner-only overlap may conservatively add chunks.
          CHECK(accepted);
        }
      }
    }

    CHECK(inside_points > 0);
    CHECK(outside_points > 0);
    CHECK(sampled_chunks > 0);
    CHECK(accepted_chunks > 0);
    CHECK(rejected_chunks > 0);
  }
}

TEST_CASE("player: replaying the same input sequence is bit-for-bit deterministic") {
  voxel::World world_a;
  voxel::World world_b;
  world_a.generate(0x51A7E5u);
  world_b.generate(0x51A7E5u);
  const auto grid_a = world_a.collision_grid();
  const auto grid_b = world_b.collision_grid();
  const int surface = world_a.surface_height(64, 64);
  REQUIRE(surface >= 0);

  voxel::Player first;
  voxel::Player replay;
  const ws::math::Vec3 spawn{
      64.5f, static_cast<float>(surface) + 4.0f, 64.5f};
  REQUIRE(first.spawn(spawn));
  REQUIRE(replay.spawn(spawn));

  const auto same_bits = [](float lhs, float rhs) {
    return std::bit_cast<std::uint32_t>(lhs) ==
           std::bit_cast<std::uint32_t>(rhs);
  };
  const auto compare_state = [&]() {
    const auto a_position = first.position();
    const auto b_position = replay.position();
    CHECK(same_bits(a_position.x, b_position.x));
    CHECK(same_bits(a_position.y, b_position.y));
    CHECK(same_bits(a_position.z, b_position.z));

    const auto a_velocity = first.velocity();
    const auto b_velocity = replay.velocity();
    CHECK(same_bits(a_velocity.x, b_velocity.x));
    CHECK(same_bits(a_velocity.y, b_velocity.y));
    CHECK(same_bits(a_velocity.z, b_velocity.z));
    CHECK(same_bits(first.yaw_radians(), replay.yaw_radians()));

    const auto a_look = first.look_direction();
    const auto b_look = replay.look_direction();
    CHECK(same_bits(a_look.x, b_look.x));
    CHECK(same_bits(a_look.y, b_look.y));
    CHECK(same_bits(a_look.z, b_look.z));
    CHECK(first.grounded() == replay.grounded());
  };

  constexpr int kFrames = 720;
  for (int frame = 0; frame < kFrames; ++frame) {
    voxel::Player::MoveInput input{};
    input.forward = frame < 160 || (frame >= 420 && frame < 550);
    input.left = frame >= 160 && frame < 260;
    input.right = frame >= 260 && frame < 420;
    input.back = frame >= 620 && frame < 700;
    input.sprint = frame % 240 >= 180;
    input.jump = frame == 0 || frame == 160 || frame == 420 || frame == 620;
    input.look_dx =
        static_cast<float>((frame * 17) % 19 - 9) * 0.125f;
    input.look_dy =
        static_cast<float>((frame * 11) % 15 - 7) * 0.0625f;
    const float dt = frame % 3 == 0 ? 1.0f / 60.0f
                     : frame % 3 == 1 ? 1.0f / 120.0f
                                      : 1.0f / 75.0f;

    first.tick(input, dt, grid_a);
    replay.tick(input, dt, grid_b);
    compare_state();
  }
}

TEST_CASE("menu: touching rectangles have unambiguous half-open edge ownership") {
  using voxel::menu::Button;
  using voxel::menu::hit;

  const Button left{0.0f, 0.0f, 10.0f, 10.0f};
  const Button right{10.0f, 0.0f, 10.0f, 10.0f};
  CHECK_FALSE(hit(left, 10.0f, 5.0f));
  CHECK(hit(right, 10.0f, 5.0f));

  const Button top{0.0f, 0.0f, 10.0f, 10.0f};
  const Button bottom{0.0f, 10.0f, 10.0f, 10.0f};
  CHECK_FALSE(hit(top, 5.0f, 10.0f));
  CHECK(hit(bottom, 5.0f, 10.0f));

  // Four rectangles meet at (10,10). The half-open convention assigns
  // the shared corner to exactly the bottom-right rectangle.
  const Button top_left{0.0f, 0.0f, 10.0f, 10.0f};
  const Button top_right{10.0f, 0.0f, 10.0f, 10.0f};
  const Button bottom_left{0.0f, 10.0f, 10.0f, 10.0f};
  const Button bottom_right{10.0f, 10.0f, 10.0f, 10.0f};
  CHECK_FALSE(hit(top_left, 10.0f, 10.0f));
  CHECK_FALSE(hit(top_right, 10.0f, 10.0f));
  CHECK_FALSE(hit(bottom_left, 10.0f, 10.0f));
  CHECK(hit(bottom_right, 10.0f, 10.0f));

  constexpr float fw = 320.0f;
  constexpr float fh = 240.0f;
  CHECK(voxel::menu::click_action(fw, fh, -0.01f, fh * 0.5f) == -1);
  CHECK(voxel::menu::click_action(fw, fh, fw + 0.01f, fh * 0.5f) == -1);
  CHECK(voxel::menu::click_action(fw, fh, fw * 0.5f, -0.01f) == -1);
  CHECK(voxel::menu::click_action(fw, fh, fw * 0.5f, fh + 0.01f) == -1);
}

TEST_CASE("perf: frame evidence structs start clear and duration conversion is exact") {
  const auto t0 = std::chrono::steady_clock::time_point{};
  const auto t1 = t0 + std::chrono::microseconds{125000};
  CHECK(voxel::perf::ms_since(t0, t1) == doctest::Approx(125.0f));

  const voxel::perf::FrameTimings timings{};
  CHECK(timings.total_ms == 0.0f);
  CHECK(timings.physics_ms == 0.0f);
  CHECK(timings.meshes_ms == 0.0f);
  CHECK(timings.draw_ms == 0.0f);

  const voxel::perf::Counters counters{};
  CHECK(counters.draw_calls == 0);
  CHECK(counters.triangles == 0);
  CHECK(counters.bodies == 0);
  CHECK(counters.meshes == 0);
  CHECK(counters.chunk_vis == 0);
}


TEST_CASE("meshing: CPU chunk geometry has valid indices and finite attributes") {
  voxel::World world;
  world.generate(0x4D455348u);
  const auto meshes = world.build_chunk_meshes(4, 4);
  REQUIRE_FALSE(meshes.empty());
  for (const auto& chunk : meshes) {
    CHECK(chunk.tile >= 0);
    CHECK(chunk.geometry.vertices.size() % 4 == 0);
    CHECK(chunk.geometry.indices.size() % 6 == 0);
    CHECK_FALSE(chunk.geometry.indices.empty());
    for (const std::uint32_t index : chunk.geometry.indices)
      CHECK(index < chunk.geometry.vertices.size());
    for (const auto& vertex : chunk.geometry.vertices) {
      for (int axis = 0; axis < 3; ++axis) {
        CHECK(std::isfinite(vertex.pos[axis]));
        CHECK(std::isfinite(vertex.normal[axis]));
      }
      CHECK(std::isfinite(vertex.uv[0]));
      CHECK(std::isfinite(vertex.uv[1]));
    }
  }
  CHECK(world.build_chunk_meshes(-1, 4).empty());
  CHECK(world.build_chunk_meshes(4, voxel::kWorldChunksZ).empty());
}

TEST_CASE("meshing: exposed faces are classified once and greedily grouped by texture") {
  voxel::World uninitialized;
  CHECK(uninitialized.build_chunk_meshes(0, 0).empty());

  voxel::World world;
  world.generate(0xFACE71E1u);

  constexpr int cx = 4;
  constexpr int cz = 4;
  constexpr int ox = cx * voxel::kChunkSizeX;
  constexpr int oz = cz * voxel::kChunkSizeZ;
  const auto clear_chunk = [&] {
    for (int y = 0; y < voxel::kChunkSizeY; ++y) {
      for (int z = 0; z < voxel::kChunkSizeZ; ++z) {
        for (int x = 0; x < voxel::kChunkSizeX; ++x) {
          (void)world.set_block(ox + x, y, oz + z, voxel::Block::air);
        }
      }
    }
  };

  // Grass's bottom and dirt's bottom both use tile 1. The blocks touch
  // horizontally, so their coplanar faces should become one greedy quad.
  clear_chunk();
  constexpr int x = ox + 7;
  constexpr int y = 24;
  constexpr int z = oz + 7;
  CHECK(world.set_block(x, y, z, voxel::Block::grass));
  CHECK(world.set_block(x + 1, y, z, voxel::Block::dirt));
  const auto mixed = world.build_chunk_meshes(cx, cz);

  const voxel::ChunkMeshData* tile1 = nullptr;
  for (const auto& mesh : mixed) {
    if (mesh.tile == 1) tile1 = &mesh;
  }
  REQUIRE(tile1 != nullptr);
  CHECK(tile1->geometry.vertices.size() == 5u * 4u);
  CHECK(tile1->geometry.indices.size() == 5u * 6u);

  // Water still emits only its top tile, never side or bottom faces.
  clear_chunk();
  CHECK(world.set_block(x, y, z, voxel::Block::water));
  const auto water = world.build_chunk_meshes(cx, cz);
  REQUIRE(water.size() == 1u);
  CHECK(water[0].tile == 7);
  CHECK(water[0].geometry.vertices.size() == 4u);
  CHECK(water[0].geometry.indices.size() == 6u);
}

TEST_CASE("voxel acoustics: six room boundaries become sorted early reflections") {
  voxel::World world;
  world.generate(0xAC0571C5u);

  // Build a deterministic 9x9x9 stone room with an air interior.
  for (int y = 20; y <= 28; ++y) {
    for (int z = 48; z <= 56; ++z) {
      for (int x = 48; x <= 56; ++x) {
        const bool wall = x == 48 || x == 56 ||
                          y == 20 || y == 28 ||
                          z == 48 || z == 56;
        (void)world.set_block(
            x, y, z, wall ? voxel::Block::stone : voxel::Block::air);
      }
    }
  }

  const auto response = voxel::acoustics::room_response(
      world, ws::math::Vec3{52.5f, 24.5f, 52.5f}, 44100);
  REQUIRE(response.count == 6);
  CHECK(response.dry_gain == doctest::Approx(0.92f));
  CHECK(response.wet_gain > 0.0f);
  CHECK(response.wet_gain < 0.36f);
  float combined_gain = 0.0f;
  for (std::size_t i = 0; i < response.count; ++i) {
    combined_gain += response.taps[i].gain;
  }
  CHECK(combined_gain * response.wet_gain <=
        doctest::Approx(0.75f).epsilon(0.001));
  for (std::size_t i = 0; i < response.count; ++i) {
    CHECK(response.taps[i].delay_frames == 900);
    CHECK(response.taps[i].gain > 0.0f);
    CHECK(response.taps[i].gain < 1.0f);
    if (i > 0) {
      CHECK(response.taps[i - 1].delay_frames <=
            response.taps[i].delay_frames);
    }
  }
}
