#include <ws/rgba_terrain_core/map_analysis.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace ws::rgba_terrain_core {
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

Rgba8 rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return {r, g, b, 255};
}

std::uint8_t gray(double value) {
    return static_cast<std::uint8_t>(
        std::clamp(std::lround(value), 0l, 255l));
}

template <typename Predicate>
ConnectedRegionStats measure_components(
    const TerrainColumns& terrain, Predicate&& matches) {
    ConnectedRegionStats stats;
    const std::size_t count = terrain.columns.size();
    std::vector<std::uint8_t> visited(count, 0);
    std::vector<std::size_t> pending;
    pending.reserve(std::min<std::size_t>(count, 4096));

    for (std::size_t start = 0; start < count; ++start) {
        if (visited[start] != 0 || !matches(terrain.columns[start])) {
            continue;
        }

        ++stats.component_count;
        std::uint64_t component_size = 0;
        visited[start] = 1;
        pending.clear();
        pending.push_back(start);

        while (!pending.empty()) {
            const std::size_t index = pending.back();
            pending.pop_back();
            ++component_size;
            const std::size_t x = index % terrain.width;
            const std::size_t z = index / terrain.width;

            const std::size_t z_begin = z == 0 ? 0 : z - 1;
            const std::size_t z_end = std::min(z + 1, terrain.depth - 1);
            const std::size_t x_begin = x == 0 ? 0 : x - 1;
            const std::size_t x_end = std::min(x + 1, terrain.width - 1);
            for (std::size_t nz = z_begin; nz <= z_end; ++nz) {
                for (std::size_t nx = x_begin; nx <= x_end; ++nx) {
                    if (nx == x && nz == z) {
                        continue;
                    }
                    const std::size_t neighbor = nz * terrain.width + nx;
                    if (visited[neighbor] == 0 &&
                        matches(terrain.columns[neighbor])) {
                        visited[neighbor] = 1;
                        pending.push_back(neighbor);
                    }
                }
            }
        }

        stats.largest_component =
            std::max(stats.largest_component, component_size);
        stats.isolated_pixels += component_size == 1 ? 1u : 0u;
    }

    return stats;
}

double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double position = fraction * static_cast<double>(sorted.size() - 1);
    const auto index = static_cast<std::size_t>(std::ceil(position));
    return sorted[std::min(index, sorted.size() - 1)];
}

double decoded_world_height(std::uint32_t encoded,
                            std::uint32_t encoding_max,
                            const TerrainSettings& settings) {
    const double fraction = static_cast<double>(encoded) /
                            static_cast<double>(encoding_max);
    return static_cast<double>(settings.min_height) +
           fraction * (static_cast<double>(settings.max_height) -
                       settings.min_height);
}

double slope_at(const RgbaImage& image, std::size_t x, std::size_t z,
                int height8, int height_msb, int height_lsb,
                std::uint32_t encoding_max, const TerrainSettings& settings) {
    const std::size_t left = x == 0 ? x : x - 1;
    const std::size_t right = std::min(x + 1, image.width - 1);
    const std::size_t north = z == 0 ? z : z - 1;
    const std::size_t south = std::min(z + 1, image.height - 1);

    const auto height = [&](std::size_t sx, std::size_t sz) {
        return decoded_world_height(
            encoded_height(image.at(sx, sz), height8, height_msb, height_lsb),
            encoding_max, settings);
    };

    const double dx_run = static_cast<double>(right - left);
    const double dz_run = static_cast<double>(south - north);
    const double dx = dx_run > 0.0
        ? (height(right, z) - height(left, z)) / dx_run : 0.0;
    const double dz = dz_run > 0.0
        ? (height(x, south) - height(x, north)) / dz_run : 0.0;
    return std::atan(std::hypot(dx, dz)) * 180.0 / std::numbers::pi;
}


}  // namespace

TerrainMapAnalysis analyze_map(const RgbaImage& image,
                               const ChannelLayout& layout,
                               const TerrainSettings& settings) {
    image.validate();
    const TerrainColumns terrain = generate_columns(image, layout, settings);
    const int height8 = find_role(layout, ChannelRole::Height8);
    const int height_msb = find_role(layout, ChannelRole::Height16Msb);
    const int height_lsb = find_role(layout, ChannelRole::Height16Lsb);

    TerrainMapAnalysis result;
    result.width = terrain.width;
    result.depth = terrain.depth;
    result.height_encoding_max = height8 >= 0 ? 255u : 65535u;

    std::vector<std::uint32_t> distinct_heights;
    std::vector<double> slopes;
    distinct_heights.reserve(terrain.columns.size());
    slopes.reserve(terrain.columns.size());
    result.min_height = result.height_encoding_max;
    result.max_height = 0;

    double mean = 0.0;
    double sum_squared_delta = 0.0;
    double horizontal_step_sum = 0.0;
    double vertical_step_sum = 0.0;
    double slope_sum = 0.0;

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
            const std::size_t elevation_bin = std::min<std::size_t>(
                static_cast<std::size_t>(
                    (static_cast<std::uint64_t>(sample) * 16u) /
                    (static_cast<std::uint64_t>(result.height_encoding_max) + 1u)),
                15u);
            ++result.elevation_histogram[elevation_bin];

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
            result.lake_columns +=
                (column.feature_flags & 0x40u) != 0 ? 1u : 0u;

            const double slope = slope_at(
                image, x, z, height8, height_msb, height_lsb,
                result.height_encoding_max, settings);
            slopes.push_back(slope);
            slope_sum += slope;
            result.max_slope_degrees = std::max(result.max_slope_degrees, slope);
            if (slope >= result.max_slope_degrees) {
                result.max_slope_x = x;
                result.max_slope_z = z;
            }
            const std::size_t slope_bin = std::min<std::size_t>(
                static_cast<std::size_t>(slope / 90.0 * 16.0), 15u);
            ++result.slope_histogram[slope_bin];

            if (x > 0) {
                const auto step = absolute_difference(
                    sample, encoded_height(image.at(x - 1, z), height8,
                                           height_msb, height_lsb));
                horizontal_step_sum += static_cast<double>(step);
                if (step > result.max_horizontal_step) {
                    result.max_horizontal_step = step;
                    result.max_horizontal_step_x = x;
                    result.max_horizontal_step_z = z;
                }
                ++result.horizontal_edges;
            }
            if (z > 0) {
                const auto step = absolute_difference(
                    sample, encoded_height(image.at(x, z - 1), height8,
                                           height_msb, height_lsb));
                vertical_step_sum += static_cast<double>(step);
                if (step > result.max_vertical_step) {
                    result.max_vertical_step = step;
                    result.max_vertical_step_x = x;
                    result.max_vertical_step_z = z;
                }
                ++result.vertical_edges;
            }
        }
    }

    const double sample_count = static_cast<double>(terrain.columns.size());
    result.mean_height = mean;
    result.standard_deviation =
        std::sqrt(sum_squared_delta / sample_count);
    result.mean_slope_degrees = slope_sum / sample_count;

    std::sort(distinct_heights.begin(), distinct_heights.end());
    result.distinct_heights = static_cast<std::size_t>(
        std::unique(distinct_heights.begin(), distinct_heights.end()) -
        distinct_heights.begin());
    std::sort(slopes.begin(), slopes.end());
    result.slope_p50_degrees = percentile(slopes, 0.50);
    result.slope_p90_degrees = percentile(slopes, 0.90);
    result.slope_p95_degrees = percentile(slopes, 0.95);
    result.slope_p99_degrees = percentile(slopes, 0.99);

    if (result.horizontal_edges != 0) {
        result.mean_horizontal_step =
            horizontal_step_sum / static_cast<double>(result.horizontal_edges);
    }
    if (result.vertical_edges != 0) {
        result.mean_vertical_step =
            vertical_step_sum / static_cast<double>(result.vertical_edges);
    }

    const auto has_flag = [](std::uint8_t flag) {
        return [flag](const VoxelColumn& column) {
            return (column.feature_flags & flag) != 0;
        };
    };
    result.water_regions = measure_components(terrain, [&](const VoxelColumn& c) {
        return c.water_mask >= settings.water_threshold ||
               (c.feature_flags & 0x80u) != 0;
    });
    result.forest_regions = measure_components(terrain, has_flag(0x08u));
    result.mountain_regions = measure_components(terrain, has_flag(0x10u));
    result.river_regions = measure_components(terrain, has_flag(0x20u));
    result.lake_regions = measure_components(terrain, has_flag(0x40u));
    for (std::size_t biome = 0; biome < result.biome_regions.size(); ++biome) {
        result.biome_regions[biome] = measure_components(
            terrain, [biome](const VoxelColumn& column) {
                return column.biome_id == biome;
            });
    }

    return result;
}

TerrainDiagnosticImages make_diagnostic_images(
    const RgbaImage& image, const ChannelLayout& layout,
    const TerrainSettings& settings) {
    image.validate();
    const TerrainColumns terrain = generate_columns(image, layout, settings);
    const int height8 = find_role(layout, ChannelRole::Height8);
    const int height_msb = find_role(layout, ChannelRole::Height16Msb);
    const int height_lsb = find_role(layout, ChannelRole::Height16Lsb);
    const std::uint32_t encoding_max = height8 >= 0 ? 255u : 65535u;

    TerrainDiagnosticImages result;
    const auto initialize = [&](RgbaImage& target) {
        target.width = image.width;
        target.height = image.height;
        target.pixels.resize(image.pixels.size());
    };
    initialize(result.elevation);
    initialize(result.slope);
    initialize(result.biome);
    initialize(result.material);
    initialize(result.water);
    initialize(result.forest);
    initialize(result.mountain);
    initialize(result.river);
    initialize(result.lake);

    std::uint32_t min_height = encoding_max;
    std::uint32_t max_height = 0;
    for (std::size_t z = 0; z < image.height; ++z) {
        for (std::size_t x = 0; x < image.width; ++x) {
            const auto value = encoded_height(
                image.at(x, z), height8, height_msb, height_lsb);
            min_height = std::min(min_height, value);
            max_height = std::max(max_height, value);
        }
    }
    const double height_span = static_cast<double>(
        std::max<std::uint32_t>(1u, max_height - min_height));

    constexpr std::array<Rgba8, 8> biome_palette{
        Rgba8{28, 78, 130, 255}, Rgba8{133, 181, 100, 255},
        Rgba8{42, 112, 65, 255}, Rgba8{222, 185, 94, 255},
        Rgba8{135, 132, 126, 255}, Rgba8{83, 151, 143, 255},
        Rgba8{190, 207, 214, 255}, Rgba8{195, 170, 91, 255},
    };
    constexpr std::array<Rgba8, 8> material_palette{
        Rgba8{20, 20, 20, 255}, Rgba8{122, 155, 83, 255},
        Rgba8{105, 78, 55, 255}, Rgba8{75, 64, 54, 255},
        Rgba8{42, 105, 178, 255}, Rgba8{130, 130, 130, 255},
        Rgba8{40, 145, 210, 255}, Rgba8{195, 185, 155, 255},
    };

    for (std::size_t z = 0; z < image.height; ++z) {
        for (std::size_t x = 0; x < image.width; ++x) {
            const std::size_t index = z * image.width + x;
            const VoxelColumn& column = terrain.columns[index];
            const std::uint32_t height = encoded_height(
                image.at(x, z), height8, height_msb, height_lsb);
            result.elevation.pixels[index] = rgba(
                gray((static_cast<double>(height - min_height) /
                      height_span) * 255.0),
                gray((static_cast<double>(height - min_height) /
                      height_span) * 255.0),
                gray((static_cast<double>(height - min_height) /
                      height_span) * 255.0));

            const double slope = slope_at(
                image, x, z, height8, height_msb, height_lsb,
                encoding_max, settings);
            const std::uint8_t slope_gray = gray(
                std::clamp(slope / 60.0, 0.0, 1.0) * 255.0);
            result.slope.pixels[index] =
                rgba(slope_gray, slope_gray, slope_gray);

            if (column.biome_id < biome_palette.size()) {
                result.biome.pixels[index] = biome_palette[column.biome_id];
            } else {
                const std::uint32_t hash =
                    static_cast<std::uint32_t>(column.biome_id) * 2246822519u;
                result.biome.pixels[index] = rgba(
                    static_cast<std::uint8_t>(64u + ((hash >> 16u) & 0xBFu)),
                    static_cast<std::uint8_t>(64u + ((hash >> 8u) & 0xBFu)),
                    static_cast<std::uint8_t>(64u + (hash & 0xBFu)));
            }
            const std::uint8_t material_id = column.material_id;
            if (material_id < material_palette.size()) {
                result.material.pixels[index] =
                    material_palette[material_id];
            } else {
                const std::uint32_t hash =
                    static_cast<std::uint32_t>(material_id) * 2654435761u;
                result.material.pixels[index] = rgba(
                    static_cast<std::uint8_t>(64u + ((hash >> 16u) & 0xBFu)),
                    static_cast<std::uint8_t>(64u + ((hash >> 8u) & 0xBFu)),
                    static_cast<std::uint8_t>(64u + (hash & 0xBFu)));
            }

            const bool water = column.water_mask >= settings.water_threshold ||
                               (column.feature_flags & 0x80u) != 0;
            result.water.pixels[index] = water
                ? rgba(30, 105, 205) : rgba(205, 190, 155);
            result.forest.pixels[index] =
                (column.feature_flags & 0x08u) ? rgba(255, 255, 255)
                                                : rgba(0, 0, 0);
            result.mountain.pixels[index] =
                (column.feature_flags & 0x10u) ? rgba(255, 255, 255)
                                                : rgba(0, 0, 0);
            result.river.pixels[index] =
                (column.feature_flags & 0x20u) ? rgba(255, 255, 255)
                                                : rgba(0, 0, 0);
            result.lake.pixels[index] =
                (column.feature_flags & 0x40u) ? rgba(255, 255, 255)
                                                : rgba(0, 0, 0);
        }
    }

    return result;
}

}  // namespace ws::rgba_terrain_core
