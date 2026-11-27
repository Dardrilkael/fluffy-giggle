#include "rgba_terrain/map_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace rgba_terrain {
namespace {

std::uint32_t absolute_difference(std::uint32_t a, std::uint32_t b) {
    return a >= b ? a - b : b - a;
}

}  // namespace

TerrainMapAnalysis analyze_map(const RgbaImage& image,
                               const ChannelLayout& layout,
                               const TerrainSettings& settings) {
    const TerrainColumns terrain = generate_columns(image, layout, settings);

    TerrainMapAnalysis result;
    result.width = terrain.width;
    result.depth = terrain.depth;
    result.min_height = terrain.columns.front().height;
    result.max_height = terrain.columns.front().height;

    std::vector<std::uint32_t> distinct_heights;
    distinct_heights.reserve(terrain.columns.size());

    double mean = 0.0;
    double sum_squared_delta = 0.0;
    double horizontal_step_sum = 0.0;
    double vertical_step_sum = 0.0;

    for (std::size_t z = 0; z < terrain.depth; ++z) {
        for (std::size_t x = 0; x < terrain.width; ++x) {
            const std::size_t index = z * terrain.width + x;
            const VoxelColumn& column = terrain.columns[index];
            const double sample = static_cast<double>(column.height);
            const double count = static_cast<double>(index + 1);
            const double delta = sample - mean;
            mean += delta / count;
            sum_squared_delta += delta * (sample - mean);

            result.min_height = std::min(result.min_height, column.height);
            result.max_height = std::max(result.max_height, column.height);
            distinct_heights.push_back(column.height);

            ++result.materials[column.material_id];
            ++result.biomes[column.biome_id];
            ++result.densities[column.density];

            const bool water = column.water_mask >= settings.water_threshold ||
                               (column.feature_flags & 0x80u) != 0;
            result.water_columns += water ? 1u : 0u;
            result.forest_columns += (column.feature_flags & 0x08u) != 0 ? 1u : 0u;
            result.mountain_columns += (column.feature_flags & 0x10u) != 0 ? 1u : 0u;
            result.river_columns += (column.feature_flags & 0x20u) != 0 ? 1u : 0u;
            result.reserved_flag_columns +=
                (column.feature_flags & 0x40u) != 0 ? 1u : 0u;

            if (x > 0) {
                const auto step = absolute_difference(
                    column.height, terrain.columns[index - 1].height);
                horizontal_step_sum += static_cast<double>(step);
                result.max_horizontal_step =
                    std::max(result.max_horizontal_step, step);
                ++result.horizontal_edges;
            }
            if (z > 0) {
                const auto step = absolute_difference(
                    column.height,
                    terrain.columns[index - terrain.width].height);
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
