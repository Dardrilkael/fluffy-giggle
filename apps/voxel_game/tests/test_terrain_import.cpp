#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <array>

#include <ws/rgba_terrain_core/rgba_terrain.hpp>

#include "terrain_import.hpp"

namespace {

ws::rgba_terrain_core::VoxelVolume make_empty_game_volume() {
  ws::rgba_terrain_core::VoxelVolume volume;
  volume.width = voxel::kWorldSizeX;
  volume.depth = voxel::kWorldSizeZ;
  volume.height = voxel::kChunkSizeY;
  volume.voxels.resize(volume.width * volume.depth * volume.height);
  for (auto& v : volume.voxels) {
    v.kind = ws::rgba_terrain_core::VoxelKind::Air;
    v.biome_id = 0;
    v.feature_flags = 0;
  }
  return volume;
}

}  // namespace

TEST_CASE("terrain import: maps generic IDs and preserves X/Z and column metadata") {
  using ws::rgba_terrain_core::VoxelKind;
  auto volume = make_empty_game_volume();

  // Exercise all supported game mappings, including positions on both sides
  // of the x=16 chunk seam and a water cell above the ground. Biome and
  // feature metadata is repeated through each column, as the core generator
  // writes it, and the game stores it once per X/Z column.
  for (std::size_t y = 0; y < volume.height; ++y) {
    volume.at(15, y, 20).biome_id = 1;
    volume.at(15, y, 20).feature_flags = 0x08;
    volume.at(16, y, 20).biome_id = 2;
    volume.at(16, y, 20).feature_flags = 0x10;
    volume.at(17, y, 20).biome_id = 3;
    volume.at(17, y, 20).feature_flags = 0x20;
  }
  volume.at(15, 4, 20) = {VoxelKind::Solid, 1, 1, 255, 0x08};
  volume.at(16, 4, 20) = {VoxelKind::Solid, 2, 2, 255, 0x10};
  volume.at(17, 4, 20) = {VoxelKind::Solid, 3, 3, 255, 0x20};
  volume.at(18, 4, 20) = {VoxelKind::Solid, 4, 0, 255, 0x00};
  volume.at(19, 4, 20) = {VoxelKind::Solid, 5, 0, 255, 0x00};
  volume.at(20, 5, 20) = {VoxelKind::Water, 0, 0, 255, 0x00};

  voxel::World world;
  world.generate(123u);
  const auto imported = voxel::import_terrain_volume(world, volume);
  REQUIRE(imported.has_value());

  CHECK(world.block_at(15, 4, 20) == voxel::Block::grass);
  CHECK(world.block_at(16, 4, 20) == voxel::Block::dirt);
  CHECK(world.block_at(17, 4, 20) == voxel::Block::stone);
  CHECK(world.block_at(18, 4, 20) == voxel::Block::sand);
  CHECK(world.block_at(19, 4, 20) == voxel::Block::stone);
  CHECK(world.block_at(20, 5, 20) == voxel::Block::water);
  CHECK(world.block_at(20, 4, 20) == voxel::Block::air);

  CHECK(world.column_metadata(15, 20).biome == voxel::Biome::tropical_rainforest);
  CHECK(world.column_metadata(15, 20).feature_flags == 0x08);
  CHECK(world.column_metadata(16, 20).biome == voxel::Biome::tropical_seasonal_forest);
  CHECK(world.column_metadata(16, 20).feature_flags == 0x10);
  CHECK(world.column_metadata(17, 20).biome == voxel::Biome::temperate_deciduous_forest);
  CHECK(world.column_metadata(17, 20).feature_flags == 0x20);
  CHECK(imported->column_count == 256u * 256u);
  CHECK(imported->voxel_count == 256u * 256u * 64u);
  CHECK(imported->block_counts[static_cast<std::size_t>(voxel::Block::grass)] == 1);
  CHECK(imported->block_counts[static_cast<std::size_t>(voxel::Block::dirt)] == 1);
  CHECK(imported->block_counts[static_cast<std::size_t>(voxel::Block::stone)] == 2);
  CHECK(imported->block_counts[static_cast<std::size_t>(voxel::Block::sand)] == 1);
  CHECK(imported->block_counts[static_cast<std::size_t>(voxel::Block::water)] == 1);
}

TEST_CASE("terrain import: collapses detailed ecological biomes into game visual families") {
  auto volume = make_empty_game_volume();
  constexpr std::array<voxel::Biome, 21> expected{
      voxel::Biome::plains, voxel::Biome::tropical_rainforest,
      voxel::Biome::tropical_seasonal_forest, voxel::Biome::temperate_deciduous_forest,
      voxel::Biome::temperate_grassland, voxel::Biome::hot_desert,
      voxel::Biome::boreal_forest, voxel::Biome::tundra, voxel::Biome::savanna,
      voxel::Biome::alpine_meadow, voxel::Biome::alpine_rock, voxel::Biome::wetlands,
      voxel::Biome::mangrove, voxel::Biome::mediterranean_woodland,
      voxel::Biome::shrubland, voxel::Biome::cold_steppe, voxel::Biome::badlands,
      voxel::Biome::snowy_mountain, voxel::Biome::rocky_plateau,
      voxel::Biome::temperate_rainforest, voxel::Biome::salt_marsh};
  for (std::size_t x = 0; x < expected.size(); ++x) {
    for (std::size_t y = 0; y < volume.height; ++y) {
      volume.at(x, y, 0).biome_id = static_cast<std::uint8_t>(x);
    }
  }
  voxel::World world;
  const auto imported = voxel::import_terrain_volume(world, volume);
  REQUIRE(imported.has_value());
  for (std::size_t x = 0; x < expected.size(); ++x) {
    CHECK(world.column_metadata(static_cast<int>(x), 0).biome == expected[x]);
  }
}

TEST_CASE("terrain import: rejects unsupported material without modifying world") {
  using ws::rgba_terrain_core::VoxelKind;
  auto volume = make_empty_game_volume();
  volume.at(8, 9, 10) = {VoxelKind::Solid, 250, 0, 255, 0};

  voxel::World world;
  world.generate(456u);
  const voxel::Block before = world.block_at(8, 9, 10);
  const auto imported = voxel::import_terrain_volume(world, volume);

  CHECK_FALSE(imported.has_value());
  CHECK(imported.error().code == static_cast<std::int32_t>(
      voxel::TerrainImportError::unsupported_material_id));
  CHECK(world.block_at(8, 9, 10) == before);
}

TEST_CASE("terrain import: rejects wrong dimensions and inconsistent column metadata") {
  using ws::rgba_terrain_core::VoxelKind;
  auto volume = make_empty_game_volume();
  voxel::World world;
  world.generate(789u);

  --volume.width;
  const auto wrong_size = voxel::import_terrain_volume(world, volume);
  CHECK_FALSE(wrong_size.has_value());
  CHECK(wrong_size.error().code == static_cast<std::int32_t>(
      voxel::TerrainImportError::dimensions_mismatch));

  volume = make_empty_game_volume();
  volume.at(3, 2, 4) = {VoxelKind::Solid, 1, 0, 255, 0};
  volume.at(3, 3, 4) = {VoxelKind::Solid, 1, 1, 255, 0};
  const auto mixed_column = voxel::import_terrain_volume(world, volume);
  CHECK_FALSE(mixed_column.has_value());
  CHECK(mixed_column.error().code == static_cast<std::int32_t>(
      voxel::TerrainImportError::inconsistent_column_metadata));
}

TEST_CASE("terrain import: rejects partial density because game blocks are binary") {
  using ws::rgba_terrain_core::VoxelKind;
  auto volume = make_empty_game_volume();
  volume.at(12, 7, 14) = {VoxelKind::Solid, 1, 0, 127, 0};

  voxel::World world;
  world.generate(321u);
  const voxel::Block before = world.block_at(12, 7, 14);
  const auto imported = voxel::import_terrain_volume(world, volume);

  CHECK_FALSE(imported.has_value());
  CHECK(imported.error().code == static_cast<std::int32_t>(
      voxel::TerrainImportError::unsupported_density));
  CHECK(world.block_at(12, 7, 14) == before);
}

TEST_CASE("terrain import: rejects biome IDs outside the game vocabulary") {
  auto volume = make_empty_game_volume();
  for (std::size_t y = 0; y < volume.height; ++y) {
    volume.at(2, y, 3).biome_id = 250;
  }
  voxel::World world;
  world.generate(654u);
  const auto imported = voxel::import_terrain_volume(world, volume);
  CHECK_FALSE(imported.has_value());
  CHECK(imported.error().code == static_cast<std::int32_t>(
      voxel::TerrainImportError::unsupported_biome_id));
}


TEST_CASE("terrain import maps all v2 surface materials to game blocks") {
  using ws::rgba_terrain_core::VoxelKind;
  auto volume = make_empty_game_volume();
  constexpr std::array<voxel::Block, 15> expected{
      voxel::Block::grass, voxel::Block::dirt, voxel::Block::stone,
      voxel::Block::sand, voxel::Block::stone, voxel::Block::gravel,
      voxel::Block::clay, voxel::Block::snow, voxel::Block::ice,
      voxel::Block::sandstone, voxel::Block::mud, voxel::Block::mossy_stone,
      voxel::Block::red_sand, voxel::Block::basalt, voxel::Block::peat};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    for (std::size_t y = 0; y < volume.height; ++y)
      volume.at(i, y, 0).biome_id = 4;
    volume.at(i, 2, 0).material_id = static_cast<std::uint8_t>(i + 1);
    volume.at(i, 2, 0).kind = VoxelKind::Solid;
    volume.at(i, 2, 0).density = 255;
  }
  voxel::World world;
  REQUIRE(voxel::import_terrain_volume(world, volume).has_value());
  for (std::size_t i = 0; i < expected.size(); ++i)
    CHECK(world.block_at(static_cast<int>(i), 2, 0) == expected[i]);
}
