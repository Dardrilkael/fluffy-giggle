#include "terrain_world_io.hpp"

#include <ws/image/image.hpp>
#include <ws/rgba_terrain_core/metadata.hpp>
#include <ws/rgba_terrain_core/rgba_terrain.hpp>
#include <ws/rgba_terrain_core/world_plan.hpp>
#include <ws/rgba_terrain_core/world_sampler.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace voxel::terrain_world_io {
namespace {

constexpr float kMinimumHeight = 0.0f;
constexpr float kMaximumHeight = 56.0f;
constexpr std::uint32_t kVolumeHeight = 64;
constexpr std::uint32_t kSeaLevel = 12;

[[nodiscard]] ws::rgba_terrain_core::TerrainSettings terrain_settings() {
  ws::rgba_terrain_core::TerrainSettings settings;
  settings.min_height = kMinimumHeight;
  settings.max_height = kMaximumHeight;
  settings.volume_height = kVolumeHeight;
  settings.sea_level = kSeaLevel;
  settings.subsurface_depth = 4;
  return settings;
}

[[nodiscard]] std::vector<std::uint8_t> flatten_rgba(
    const ws::rgba_terrain_core::RgbaImage& image) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(image.pixels.size() * 4u);
  for (const auto& pixel : image.pixels) {
    bytes.push_back(pixel.r);
    bytes.push_back(pixel.g);
    bytes.push_back(pixel.b);
    bytes.push_back(pixel.a);
  }
  return bytes;
}

[[nodiscard]] std::string metadata_text() {
  return
      "# RGBA terrain data map v2\n"
      "format_version=2\n"
      "channel.r=height16_msb\n"
      "channel.g=height16_lsb\n"
      "channel.b=material_feature_flags\n"
      "channel.a=biome_id\n"
      "min_height=0\n"
      "max_height=56\n"
      "volume_height=64\n"
      "sea_level=12\n"
      "subsurface_depth=4\n"
      "water_threshold=128\n"
      "default_material=1\n"
      "subsurface_material=2\n"
      "deep_material=3\n"
      "water_material=6\n"
      "default_biome=0\n"
      "solid_density=255\n";
}

}  // namespace

MapFiles generated_map_files() {
  const auto directory =
      std::filesystem::path("worlds") / "generated-world";
  return {directory / "terrain.png", directory / "terrain.rgba-terrain"};
}

void generate_png_map(const MapFiles& files, std::uint32_t seed) {
  if (files.png.empty() || files.metadata.empty()) {
    throw std::invalid_argument("terrain output paths must not be empty");
  }

  const auto width = static_cast<std::size_t>(kWorldSizeX);
  const auto depth = static_cast<std::size_t>(kWorldSizeZ);
  auto plan = ws::rgba_terrain_core::make_default_world_plan(
      static_cast<std::int64_t>(width), static_cast<std::int64_t>(depth), seed);

  // Preserve the POC's multi-scale relationships while adapting its
  // absolute sampling scales to the game's finite 256x256 map.
  const double span = static_cast<double>(std::max(width, depth));
  plan.scales.continent_scale = std::max(16.0, span * 0.52);
  plan.scales.province_scale = std::max(8.0, span * 0.23);
  plan.scales.ecosystem_scale = std::max(4.0, span * 0.12);
  plan.scales.sub_biome_scale = std::max(2.0, span * 0.045);
  plan.scales.habitat_scale = std::max(1.0, span * 0.015);
  plan.hydrology.routing_cell_scale = std::max(1.0, span / 32.0);
  plan.validate();

  const ws::rgba_terrain_core::WorldSampler sampler(std::move(plan));
  const auto settings = terrain_settings();
  const auto image = sampler.generate_rgba_region(
      0, 0, width, depth, settings);
  const auto bytes = flatten_rgba(image);

  std::filesystem::create_directories(files.png.parent_path());
  const auto png_status = ws::image::write_rgba8_png_file(
      files.png.string(), static_cast<std::uint32_t>(width),
      static_cast<std::uint32_t>(depth), bytes);
  if (!png_status) {
    throw std::runtime_error(
        "could not write generated terrain PNG: " +
        png_status.error().message);
  }

  std::filesystem::create_directories(files.metadata.parent_path());
  std::ofstream sidecar(files.metadata, std::ios::binary | std::ios::trunc);
  if (!sidecar) {
    throw std::runtime_error("could not create terrain metadata sidecar");
  }
  sidecar << metadata_text();
  sidecar.flush();
  if (!sidecar) {
    throw std::runtime_error("could not finish writing terrain metadata sidecar");
  }
}

TerrainImportReport import_png_map(
    World& world, const MapFiles& files) {
  const auto metadata =
      ws::rgba_terrain_core::load_metadata(files.metadata.string());
  const auto image =
      ws::rgba_terrain_core::load_rgba_image(files.png.string());
  if (image.width != static_cast<std::size_t>(kWorldSizeX) ||
      image.height != static_cast<std::size_t>(kWorldSizeZ)) {
    throw std::runtime_error(
        "saved terrain PNG must be exactly 256x256 for this game build");
  }
  const auto volume = ws::rgba_terrain_core::generate_voxel_volume(
      image, metadata.layout, metadata.settings);
  const auto imported = import_terrain_volume(world, volume);
  if (!imported) {
    throw std::runtime_error(imported.error().message);
  }
  return *imported;
}

}  // namespace voxel::terrain_world_io
