#include "terrain_world_io.hpp"
#include "terrain_world_profile.hpp"

#include <ws/image/image.hpp>
#include <ws/rgba_terrain_core/metadata.hpp>
#include <ws/rgba_terrain_core/rgba_terrain.hpp>
#include <ws/rgba_terrain_core/world_sampler.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace voxel::terrain_world_io {
namespace {

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

void write_diagnostic_png(
    const std::filesystem::path& path,
    const ws::rgba_terrain_core::RgbaImage& image) {
  const auto bytes = flatten_rgba(image);
  const auto status = ws::image::write_rgba8_png_file(
      path.string(), static_cast<std::uint32_t>(image.width),
      static_cast<std::uint32_t>(image.height), bytes);
  if (!status) {
    throw std::runtime_error(
        "could not write terrain diagnostic PNG '" + path.string() +
        "': " + status.error().message);
  }
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
  return {directory / "terrain.png", directory / "terrain.rgba-terrain",
          directory / "terrain.channels.png", directory / "terrain.rgb.png",
          directory / "diagnostics"};
}

void generate_png_map(const MapFiles& files, std::uint32_t seed) {
  if (files.png.empty() || files.metadata.empty()) {
    throw std::invalid_argument("terrain output paths must not be empty");
  }

  const auto width = static_cast<std::size_t>(kWorldSizeX);
  const auto depth = static_cast<std::size_t>(kWorldSizeZ);
  auto plan = terrain_world_profile::make_world_plan(
      static_cast<std::int64_t>(width), static_cast<std::int64_t>(depth), seed);

  const ws::rgba_terrain_core::WorldSampler sampler(std::move(plan));
  const auto settings = terrain_world_profile::settings();
  ws::rgba_terrain_core::WorldClimateDiagnosticImages climate_diagnostics;
  ws::rgba_terrain_core::WorldHydrologyDiagnosticImages hydrology_diagnostics;
  const auto image = sampler.generate_rgba_region(
      0, 0, width, depth, settings,
      &climate_diagnostics, &hydrology_diagnostics);
  const auto bytes = flatten_rgba(image);

  if (!files.diagnostics_directory.empty()) {
    std::filesystem::create_directories(files.diagnostics_directory);
    write_diagnostic_png(files.diagnostics_directory / "temperature.png",
                         climate_diagnostics.temperature);
    write_diagnostic_png(files.diagnostics_directory / "humidity.png",
                         climate_diagnostics.humidity);
    write_diagnostic_png(files.diagnostics_directory / "precipitation.png",
                         climate_diagnostics.precipitation);
    write_diagnostic_png(files.diagnostics_directory / "orographic-lift.png",
                         climate_diagnostics.orographic_lift);
    write_diagnostic_png(files.diagnostics_directory / "rain-shadow.png",
                         climate_diagnostics.rain_shadow);
    write_diagnostic_png(files.diagnostics_directory / "ocean-moisture.png",
                         climate_diagnostics.ocean_moisture);
    write_diagnostic_png(files.diagnostics_directory / "flow-accumulation.png",
                         hydrology_diagnostics.flow_accumulation);
    write_diagnostic_png(files.diagnostics_directory / "river-strength.png",
                         hydrology_diagnostics.river_strength);
    write_diagnostic_png(files.diagnostics_directory / "drainage-direction.png",
                         hydrology_diagnostics.drainage_direction);
    write_diagnostic_png(files.diagnostics_directory / "lake-depth.png",
                         hydrology_diagnostics.lake_depth);
    write_diagnostic_png(files.diagnostics_directory / "lake-basins.png",
                         hydrology_diagnostics.lake_basins);
  }

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

void generate_channel_preview_png(const MapFiles& files) {
  if (files.png.empty() || files.channel_preview.empty()) {
    throw std::invalid_argument("terrain preview paths must not be empty");
  }

  const auto loaded = ws::image::load_rgba8_file(files.png.string());
  if (!loaded) {
    throw std::runtime_error("could not decode saved terrain PNG for preview: " +
                             loaded.error().message);
  }
  const auto& source = *loaded;
  if (source.width == 0 || source.height == 0 ||
      source.width > std::numeric_limits<std::uint32_t>::max() / 2u ||
      source.height > std::numeric_limits<std::uint32_t>::max() / 2u) {
    throw std::runtime_error("saved terrain PNG dimensions cannot be previewed");
  }

  // Each quadrant is one original byte channel, visualized as grayscale.
  // Alpha is deliberately made opaque in this display-only sheet so biome
  // IDs remain visible instead of being interpreted as transparency.
  const std::uint32_t width = source.width * 2u;
  const std::uint32_t height = source.height * 2u;
  std::vector<std::uint8_t> preview(
      static_cast<std::size_t>(width) * height * 4u, 255u);
  for (std::uint32_t y = 0; y < source.height; ++y) {
    for (std::uint32_t x = 0; x < source.width; ++x) {
      const std::size_t source_offset =
          (static_cast<std::size_t>(y) * source.width + x) * 4u;
      for (std::uint32_t channel = 0; channel < 4u; ++channel) {
        const std::uint32_t dst_x = x + (channel & 1u) * source.width;
        const std::uint32_t dst_y = y + (channel >> 1u) * source.height;
        const std::size_t dst_offset =
            (static_cast<std::size_t>(dst_y) * width + dst_x) * 4u;
        const std::uint8_t value = source.pixels[source_offset + channel];
        preview[dst_offset + 0u] = value;
        preview[dst_offset + 1u] = value;
        preview[dst_offset + 2u] = value;
        preview[dst_offset + 3u] = 255u;
      }
    }
  }

  std::filesystem::create_directories(files.channel_preview.parent_path());
  const auto written = ws::image::write_rgba8_png_file(
      files.channel_preview.string(), width, height, preview);
  if (!written) {
    throw std::runtime_error("could not write terrain channel preview: " +
                             written.error().message);
  }
}

void generate_combined_rgb_preview_png(const MapFiles& files) {
  if (files.png.empty() || files.rgb_preview.empty()) {
    throw std::invalid_argument("terrain RGB preview paths must not be empty");
  }

  const auto loaded = ws::image::load_rgba8_file(files.png.string());
  if (!loaded) {
    throw std::runtime_error("could not decode saved terrain PNG for RGB preview: " +
                             loaded.error().message);
  }
  const auto& source = *loaded;
  if (source.width == 0 || source.height == 0) {
    throw std::runtime_error("saved terrain PNG has invalid dimensions");
  }

  // Direct channel composition: no palette, scaling, normalization or
  // reinterpretation. Alpha is opaque only in this display derivative;
  // the original alpha byte (biome ID) remains in terrain.png and the RGBA view.
  std::vector<std::uint8_t> preview(source.pixels.size(), 255u);
  for (std::size_t i = 0; i < source.pixels.size(); i += 4u) {
    preview[i + 0u] = source.pixels[i + 0u];
    preview[i + 1u] = source.pixels[i + 1u];
    preview[i + 2u] = source.pixels[i + 2u];
    preview[i + 3u] = 255u;
  }

  std::filesystem::create_directories(files.rgb_preview.parent_path());
  const auto written = ws::image::write_rgba8_png_file(
      files.rgb_preview.string(), source.width, source.height, preview);
  if (!written) {
    throw std::runtime_error("could not write terrain RGB preview: " +
                             written.error().message);
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
