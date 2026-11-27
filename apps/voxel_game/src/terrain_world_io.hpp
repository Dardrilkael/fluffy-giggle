#pragma once

#include <cstdint>
#include <filesystem>

#include "terrain_import.hpp"

namespace voxel::terrain_world_io {

struct MapFiles {
  std::filesystem::path png;
  std::filesystem::path metadata;
  std::filesystem::path channel_preview;
  std::filesystem::path rgb_preview;
  std::filesystem::path diagnostics_directory;
};

// Stable app-data location used by the title screen's Generate/Load actions.
// On Android the app changes its working directory to internalDataPath.
[[nodiscard]] MapFiles generated_map_files();

// Generates the authoritative RGBA v2 PNG, sidecar and optional diagnostic PNGs. The game has a
// fixed 256x256x64 volume, so the map is generated at native X/Z resolution.
void generate_png_map(const MapFiles& files, std::uint32_t seed);

// Writes a display-only 2x2 grayscale sheet of the PNG's exact R, G, B and A
// bytes. The authoritative terrain PNG and its encoded pixels are untouched.
void generate_channel_preview_png(const MapFiles& files);

// Writes a display-only RGB image using the original R, G and B bytes
// unchanged as visible color channels; source PNG and alpha data are untouched.
void generate_combined_rgb_preview_png(const MapFiles& files);

// Reconstructs voxels from PNG + sidecar and imports them into the game.
// The .rgba-voxels file is intentionally not required or written here.
[[nodiscard]] TerrainImportReport import_png_map(
    World& world, const MapFiles& files);

}  // namespace voxel::terrain_world_io
