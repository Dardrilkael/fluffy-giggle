#include "rgba_terrain/map_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace rgba_terrain {
namespace {

int find_role(const ChannelLayout& layout, ChannelRole role) {
    for (std::size_t i = 0; i < layout.channels.size(); ++i) {
        if (layout.channels[i] == role) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::uint8_t channel_value(const Rgba8& pixel, int channel) {
    switch (channel) {
        case 0: return pixel.r;
        case 1: return pixel.g;
        case 2: return pixel.b;
        case 3: return pixel.a;
        default: return 0;
    }
}

std::uint32_t encoded_height(const Rgba8& pixel, int height8,
                             int height_msb, int height_lsb) {
    if (height8 >= 0) {
        return channel_value(pixel, height8);
    }
    return (static_cast<std::uint32_t>(channel_value(pixel, height_msb)) << 8u) |
           channel_value(pixel, height_lsb);
}

std::uint32_t absolute_difference(std::uint32_t a, std::uint32_t b) {
    return a >= b ? a - b : b - a;
}

}  // namespace

TerrainMapAnalysis analyze_map(const RgbaImage& image,
                               const ChannelLayout& layout,
                               const TerrainSettings& settings) {
    // Reuse the production decoder for settings/layout validation and for
    // material/biome/feature extraction, but measure elevation directly from
    // the source channel bytes so 16-bit precision is not quantized to voxels.
    const TerrainColumns terrain = generate_columns(image, layout, settings);
    const int height8 = find_role(layout, ChannelRole::Height8);
    const int height_msb = find_role(layout, ChannelRole::Height16Msb);
    const int height_lsb = find_role(layout, ChannelRole::Height16Lsb);

    TerrainMapAnalysis result;
    result.width = terrain.width;
    result.depth = terrain.depth;

    std::vector<std::uint32_t> distinct_heights;
    distinct_heights.reserve(terrain.columns.size());
    result.min_height = height8 >= 0 ? 255u : 65535u;
    result.max_height = 0;

    double mean = 0.0;
    double sum_squared_delta = 0.0;
    double horizontal_step_sum = 0.0;
    double vertical_step_sum = 0.0;

    for (std::size_t z = 0; z < terrain.depth; ++z) {
        for (std::size_t x = 0; x < terrain.width; ++x) {
            const std::size_t index = z * terrain.width + x;
            const VoxelColumn& column = terrain.columns[index];
            const std::uint32_t sample = encoded_height(
                image.at(x, z), height8, height_msb, height_lsb);
            const double sample_double = static_cast<double>(sample);
            const double count = static_cast<double>(index + 1);
            const double delta = sample_double - mean;
            mean += delta / count;
            sum_squared_delta += delta * (sample_double - mean);

            result.min_height = std::min(result.min_height, sample);
            result.max_height = std::max(result.max_height, sample);
            distinct_heights.push_back(sample);

            ++result.materials[column.material_id];
            ++result.biomes[column.biome_id];
            ++result.densities[column.density];

            const bool water = column.water_mask >= settings.water_threshold ||
                               (column.feature_flags & 0x80u) != 0;
            result.water_columns += water ? 1u : 0u;
            result.forest_columns +=
                (column.feature_flags & 0x08u) != 0 ? 1u : 0u;
            result.mountain_columns +=
                (column.feature_flags & 0x10u) != 0 ? 1u : 0u;
            result.river_columns +=
                (column.feature_flags & 0x20u) != 0 ? 1u : 0u;
            result.reserved_flag_columns +=
                (column.feature_flags & 0x40u) != 0 ? 1u : 0u;

            if (x > 0) {
                const auto step = absolute_difference(
                    sample, encoded_height(image.at(x - 1, z), height8,
                                           height_msb, height_lsb));
                horizontal_step_sum += static_cast<double>(step);
                result.max_horizontal_step =
                    std::max(result.max_horizontal_step, step);
                ++result.horizontal_edges;
            }
            if (z > 0) {
                const auto step = absolute_difference(
                    sample, encoded_height(image.at(x, z - 1), height8,
                                           height_msb, height_lsb));
                vertical_step_sum += static_cast<double>(step);
                result.max_vertical_step =
                    std::max(result.max_vertical_step, step);
                ++result.vertical_edges;
            }
        }
    }

    const double sample_count = static_cast<double>(terrain.columns.size());
    result.mean_height = mean;
    result.standard_deviation =
        std::sqrt(sum_squared_delta / sample_count);

    std::sort(distinct_heights.begin(), distinct_heights.end());
    result.distinct_heights = static_cast<std::size_t>(
        std::unique(distinct_heights.begin(), distinct_heights.end()) -
        distinct_heights.begin());

    if (result.horizontal_edges != 0) {
        result.mean_horizontal_step =
            horizontal_step_sum / static_cast<double>(result.horizontal_edges);
    }
    if (result.vertical_edges != 0) {
        result.mean_vertical_step =
            vertical_step_sum / static_cast<double>(result.vertical_edges);
    }

    return result;
}

}  // namespace rgba_terrain
