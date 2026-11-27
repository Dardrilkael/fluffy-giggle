#include "terrain_import.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>

namespace voxel {
namespace {

using ws::rgba_terrain_core::Voxel;
using ws::rgba_terrain_core::VoxelKind;
using ws::rgba_terrain_core::VoxelVolume;

[[nodiscard]] ws::core::unexpected<ws::core::Error> import_error(
    TerrainImportError code, std::string message) {
  return ws::core::fail(static_cast<std::int32_t>(code),
                        "voxel.terrain_import", std::move(message));
}

[[nodiscard]] bool map_solid_material(std::uint8_t material,
                                      Block& block) noexcept {
  switch (material) {
    case 1: block = Block::grass; return true;
    case 2: block = Block::dirt; return true;
    case 3: block = Block::stone; return true;
    case 4: block = Block::sand; return true;
    case 5: block = Block::stone; return true;
    default: return false;
  }
}

[[nodiscard]] bool map_biome(std::uint8_t biome, Biome& result) noexcept {
  // Preserve the legacy v1 biome meanings (IDs 0..7). The v2 generator
  // deliberately starts detailed land biomes at ID 8 so old PNGs never
  // acquire a different biome simply because the vocabulary expanded.
  switch (biome) {
    case 0: result = Biome::plains; return true;  // ocean
    case 1: result = Biome::forest; return true;
    case 2: result = Biome::forest; return true;
    case 3: result = Biome::forest; return true;
    case 4: result = Biome::desert; return true;
    case 5: result = Biome::mountain; return true;
    case 6: result = Biome::forest; return true;
    case 7: result = Biome::mountain; return true;
    default:
      if (biome < 8 || biome > 27) return false;
      result = static_cast<Biome>(biome - 4);
      return true;
  }
}

[[nodiscard]] bool map_voxel(const Voxel& voxel, Block& block) noexcept {
  switch (voxel.kind) {
    case VoxelKind::Air:
      block = Block::air;
      return true;
    case VoxelKind::Water:
      block = Block::water;
      return true;
    case VoxelKind::Solid:
      return map_solid_material(voxel.material_id, block);
  }
  return false;
}

}  // namespace

ws::core::Result<TerrainImportReport> import_terrain_volume(
    World& world, const VoxelVolume& volume) {
  if (volume.width != static_cast<std::size_t>(kWorldSizeX) ||
      volume.depth != static_cast<std::size_t>(kWorldSizeZ) ||
      volume.height != static_cast<std::size_t>(kChunkSizeY)) {
    return import_error(
        TerrainImportError::dimensions_mismatch,
        "terrain volume must be exactly 256x256x64; the game does not resample");
  }

  try {
    volume.validate();
  } catch (const std::exception& exception) {
    return import_error(TerrainImportError::invalid_volume, exception.what());
  }

  TerrainImportReport report;
  report.voxel_count = static_cast<std::uint64_t>(volume.voxels.size());
  report.column_count =
      static_cast<std::uint64_t>(volume.width * volume.depth);

  // Validate the complete source before clearing or changing the destination
  // world. This keeps a failed import from leaving a partially replaced world.
  for (std::size_t y = 0; y < volume.height; ++y) {
    for (std::size_t z = 0; z < volume.depth; ++z) {
      for (std::size_t x = 0; x < volume.width; ++x) {
        const Voxel& voxel = volume.at(x, y, z);
        Block block = Block::air;
        if (!map_voxel(voxel, block)) {
          const auto kind = static_cast<unsigned>(voxel.kind);
          if (voxel.kind == VoxelKind::Solid) {
            return import_error(
                TerrainImportError::unsupported_material_id,
                "unsupported solid material ID " +
                    std::to_string(voxel.material_id) + " at x=" +
                    std::to_string(x) + " y=" + std::to_string(y) +
                    " z=" + std::to_string(z));
          }
          return import_error(
              TerrainImportError::unsupported_voxel_kind,
              "unsupported voxel kind " + std::to_string(kind) +
                  " at x=" + std::to_string(x) + " y=" +
                  std::to_string(y) + " z=" + std::to_string(z));
        }
        const bool density_supported =
            (voxel.kind == VoxelKind::Air && voxel.density == 0) ||
            (voxel.kind == VoxelKind::Solid && voxel.density == 255) ||
            (voxel.kind == VoxelKind::Water && voxel.density == 255);
        if (!density_supported) {
          return import_error(
              TerrainImportError::unsupported_density,
              "game blocks are fully occupied; unsupported density " +
                  std::to_string(voxel.density) + " at x=" +
                  std::to_string(x) + " y=" + std::to_string(y) +
                  " z=" + std::to_string(z));
        }
        ++report.block_counts[static_cast<std::size_t>(block)];
      }
    }
  }

  // The game's metadata model is column-oriented. The RGBA terrain pipeline
  // currently repeats biome and feature metadata through a whole column;
  // reject inconsistent voxel files rather than silently choosing one layer.
  for (std::size_t z = 0; z < volume.depth; ++z) {
    for (std::size_t x = 0; x < volume.width; ++x) {
      const Voxel& first = volume.at(x, 0, z);
      Biome biome = Biome::plains;
      if (!map_biome(first.biome_id, biome)) {
        return import_error(
            TerrainImportError::unsupported_biome_id,
            "unsupported biome ID " + std::to_string(first.biome_id) +
                " at x=" + std::to_string(x) + " z=" + std::to_string(z));
      }
      for (std::size_t y = 1; y < volume.height; ++y) {
        const Voxel& layer = volume.at(x, y, z);
        if (layer.biome_id != first.biome_id ||
            layer.feature_flags != first.feature_flags) {
          return import_error(
              TerrainImportError::inconsistent_column_metadata,
              "biome/feature metadata changes within column x=" +
                  std::to_string(x) + " z=" + std::to_string(z));
        }
      }
      ++report.biome_columns[static_cast<std::size_t>(biome)];
    }
  }

  // All dimensions, voxel kinds, materials and per-column metadata are now
  // known to be representable. Replace the world in its native coordinate
  // frame (X east, Y up, Z south), preserving every X/Z sample exactly.
  world.reset_empty();
  for (std::size_t y = 0; y < volume.height; ++y) {
    for (std::size_t z = 0; z < volume.depth; ++z) {
      for (std::size_t x = 0; x < volume.width; ++x) {
        const Voxel& voxel = volume.at(x, y, z);
        Block block = Block::air;
        (void)map_voxel(voxel, block);  // guaranteed by the preflight pass
        (void)world.set_block(static_cast<int>(x), static_cast<int>(y),
                              static_cast<int>(z), block);
      }
    }
  }
  for (std::size_t z = 0; z < volume.depth; ++z) {
    for (std::size_t x = 0; x < volume.width; ++x) {
      const Voxel& first = volume.at(x, 0, z);
      Biome biome = Biome::plains;
      (void)map_biome(first.biome_id, biome);  // guaranteed by preflight
      (void)world.set_column_metadata(
          static_cast<int>(x), static_cast<int>(z), biome,
          first.feature_flags);
    }
  }

  return report;
}

}  // namespace voxel
