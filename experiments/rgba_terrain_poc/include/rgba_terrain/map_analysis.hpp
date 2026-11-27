#pragma once

#include "rgba_terrain/rgba_terrain.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace rgba_terrain {

struct TerrainMapAnalysis {
    std::size_t width = 0;
    std::size_t depth = 0;
    std::uint32_t min_height = 0;
    std::uint32_t max_height = 0;
    std::size_t distinct_heights = 0;
    double mean_height = 0.0;
    double standard_deviation = 0.0;

    std::uint64_t horizontal_edges = 0;
    std::uint64_t vertical_edges = 0;
    std::uint32_t max_horizontal_step = 0;
    std::uint32_t max_vertical_step = 0;
    double mean_horizontal_step = 0.0;
    double mean_vertical_step = 0.0;

    std::array<std::uint64_t, 256> materials{};
    std::array<std::uint64_t, 256> biomes{};
    std::array<std::uint64_t, 256> densities{};

    std::uint64_t water_columns = 0;
    std::uint64_t forest_columns = 0;
    std::uint64_t mountain_columns = 0;
    std::uint64_t river_columns = 0;
    std::uint64_t reserved_flag_columns = 0;
};

// Elevation statistics and neighbor differences use the original encoded
// channel values (0..255 or 0..65535), not rounded voxel heights. Attribute
// histograms and feature counts are decoded from the same source pixels.
[[nodiscard]] TerrainMapAnalysis analyze_map(
    const RgbaImage& image, const ChannelLayout& layout,
    const TerrainSettings& settings);

}  // namespace rgba_terrain
