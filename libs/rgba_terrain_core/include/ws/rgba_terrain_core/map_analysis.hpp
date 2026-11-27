#pragma once

#include <ws/rgba_terrain_core/rgba_terrain.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace ws::rgba_terrain_core {

struct ConnectedRegionStats {
    std::uint64_t component_count = 0;
    std::uint64_t largest_component = 0;
    std::uint64_t isolated_pixels = 0;
};

struct TerrainMapAnalysis {
    std::size_t width = 0;
    std::size_t depth = 0;
    std::uint32_t height_encoding_max = 65535;
    std::uint32_t min_height = 0;
    std::uint32_t max_height = 0;
    std::size_t distinct_heights = 0;
    double mean_height = 0.0;
    double standard_deviation = 0.0;

    // Sixteen equal-width bins over the encoded elevation range and 0..90
    // degrees of slope respectively.
    std::array<std::uint64_t, 16> elevation_histogram{};
    std::array<std::uint64_t, 16> slope_histogram{};

    std::uint64_t horizontal_edges = 0;
    std::uint64_t vertical_edges = 0;
    std::uint32_t max_horizontal_step = 0;
    std::uint32_t max_vertical_step = 0;
    std::size_t max_horizontal_step_x = 0;
    std::size_t max_horizontal_step_z = 0;
    std::size_t max_vertical_step_x = 0;
    std::size_t max_vertical_step_z = 0;
    double mean_horizontal_step = 0.0;
    double mean_vertical_step = 0.0;

    // Slope assumes one horizontal world unit per image pixel. Elevation is
    // converted through TerrainSettings before computing the gradient.
    double mean_slope_degrees = 0.0;
    double slope_p50_degrees = 0.0;
    double slope_p90_degrees = 0.0;
    double slope_p95_degrees = 0.0;
    double slope_p99_degrees = 0.0;
    double max_slope_degrees = 0.0;
    std::size_t max_slope_x = 0;
    std::size_t max_slope_z = 0;

    std::array<std::uint64_t, 256> materials{};
    std::array<std::uint64_t, 256> biomes{};
    std::array<std::uint64_t, 256> densities{};

    std::uint64_t water_columns = 0;
    std::uint64_t forest_columns = 0;
    std::uint64_t mountain_columns = 0;
    std::uint64_t river_columns = 0;
    std::uint64_t lake_columns = 0;

    // Components use 8-neighbor connectivity. This is a map-analysis
    // convention, not a claim about physical drainage connectivity.
    ConnectedRegionStats water_regions{};
    ConnectedRegionStats forest_regions{};
    ConnectedRegionStats mountain_regions{};
    ConnectedRegionStats river_regions{};
    ConnectedRegionStats lake_regions{};
    std::array<ConnectedRegionStats, 8> biome_regions{};
};

struct TerrainDiagnosticImages {
    RgbaImage elevation;
    RgbaImage slope;
    RgbaImage biome;
    RgbaImage material;
    RgbaImage water;
    RgbaImage forest;
    RgbaImage mountain;
    RgbaImage river;
    RgbaImage lake;
};

// Elevation statistics and neighbor differences use the original encoded
// channel values (0..255 or 0..65535), not rounded voxel heights. Slope is
// computed from decoded terrain heights with a one-cell horizontal run.
[[nodiscard]] TerrainMapAnalysis analyze_map(
    const RgbaImage& image, const ChannelLayout& layout,
    const TerrainSettings& settings);

// Creates independent RGBA8 visualization images. These are diagnostic
// outputs only and must never be fed back into terrain reconstruction.
[[nodiscard]] TerrainDiagnosticImages make_diagnostic_images(
    const RgbaImage& image, const ChannelLayout& layout,
    const TerrainSettings& settings);

}  // namespace ws::rgba_terrain_core
