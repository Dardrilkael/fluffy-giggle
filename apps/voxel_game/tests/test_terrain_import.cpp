#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>

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
  // of the x=16 chunk seam and a water cell above the ground.
  volume.at(15, 4, 20) = {VoxelKind::Solid, 1, 1, 255, 0x08};
  volume.at(16, 4, 20) = {VoxelKind::Solid, 2, 2, 255, 0x10};
  volume.at(17, 4, 20) = {VoxelKind::Solid, 3, 3, 255, 0x20};
  volume.at(18, 4, 20) = {VoxelKind::Solid, 4, 0, 255, 0x00};
  volume.at(19, 4, 20) = {VoxelKind::Solid, 5, 0, 255, 0x00};
  volume.at(20, 5, 20) = {VoxelKind::Water, 0, 0, 128, 0x00};

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

  CHECK(world.column_metadata(15, 20).biome == voxel::Biome::forest);
  CHECK(world.column_metadata(15, 20).feature_flags == 0x08);
  CHECK(world.column_metadata(16, 20).biome == voxel::Biome::desert);
  CHECK(world.column_metadata(16, 20).feature_flags == 0x10);
  CHECK(imported->column_count == 256u * 256u);
  CHECK(imported->voxel_count == 256u * 256u * 64u);
  CHECK(imported->block_counts[static_cast<std::size_t>(voxel::Block::grass)] == 1);
  CHECK(imported->block_counts[static_cast<std::size_t>(voxel::Block::dirt)] == 1);
  CHECK(imported->block_counts[static_cast<std::size_t>(voxel::Block::stone)] == 2);
  CHECK(imported->block_counts[static_cast<std::size_t>(voxel::Block::sand)] == 1);
  CHECK(imported->block_counts[static_cast<std::size_t>(voxel::Block::water)] == 1);
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
