#pragma once

#include <array>
#include <cstdint>

#include <ws/core/result.hpp>
#include <ws/rgba_terrain_core/rgba_terrain.hpp>

#include "world.hpp"

namespace voxel {

enum class TerrainImportError : std::int32_t {
  dimensions_mismatch = 1,
  invalid_volume = 2,
  unsupported_voxel_kind = 3,
  unsupported_material_id = 4,
  unsupported_biome_id = 5,
  inconsistent_column_metadata = 6,
  unsupported_density = 7,
};

struct TerrainImportReport {
  std::uint64_t voxel_count = 0;
  std::uint64_t column_count = 0;
  std::array<std::uint64_t, static_cast<std::size_t>(Block::count_)>
      block_counts{};
  std::array<std::uint64_t, static_cast<std::size_t>(Biome::count_)>
      biome_columns{};
};

// Imports only the game's fixed 128x128x64 world dimensions. No scaling,
// cropping, or resampling is performed. Material and biome IDs are mapped
// explicitly at this app boundary; unknown IDs are rejected before World
// is modified. Biome and feature data is retained as per-column metadata.
[[nodiscard]] ws::core::Result<TerrainImportReport> import_terrain_volume(
    World& world, const ws::rgba_terrain_core::VoxelVolume& volume);

}  // namespace voxel
