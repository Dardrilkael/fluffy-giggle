#include <ws/rgba_terrain_core/metadata.hpp>
#include <ws/rgba_terrain_core/map_analysis.hpp>
#include <ws/rgba_terrain_core/rgba_terrain.hpp>
#include <ws/rgba_terrain_core/voxel_file.hpp>
#include <ws/rgba_terrain_core/voxel_validation.hpp>
#include <ws/rgba_terrain_core/world_plan.hpp>
#include <ws/rgba_terrain_core/world_sampler.hpp>

#include <ws/image/image.hpp>
#include <ws/core/log.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <format>
#include <functional>
#include <utility>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::uint32_t checked_u32(std::size_t value, const char* field) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(std::string(field) +
                                 " exceeds the file format limit");
    }
    return static_cast<std::uint32_t>(value);
}

std::size_t parse_size(const std::string& str, const char* field) {
    std::size_t value = 0;
    for (char c : str) {
        if (c < '0' || c > '9') {
            throw std::runtime_error(std::string(field) + " must be a positive integer");
        }
        value = value * 10 + (c - '0');
    }
    if (value == 0) {
        throw std::runtime_error(std::string(field) + " must be greater than zero");
    }
    return value;
}

std::uint32_t parse_seed(const std::string& str) {
    std::uint32_t value = 0;
    std::size_t pos = 0;
    if (str.size() >= 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        pos = 2;
        for (std::size_t i = 2; i < str.size(); ++i) {
            char c = str[i];
            if (c >= '0' && c <= '9') value = value * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') value = value * 16 + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value = value * 16 + (c - 'A' + 10);
            else throw std::runtime_error("seed must be a valid integer (decimal or 0x hex)");
        }
    } else {
        for (char c : str) {
            if (c < '0' || c > '9') {
                throw std::runtime_error("seed must be a non-negative integer");
            }
            value = value * 10 + (c - '0');
        }
    }
    return value;
}

std::vector<std::uint8_t> flatten_rgba(const ws::rgba_terrain_core::RgbaImage& image) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(image.pixels.size() * 4);
    for (const auto& p : image.pixels) {
        bytes.push_back(p.r);
        bytes.push_back(p.g);
        bytes.push_back(p.b);
        bytes.push_back(p.a);
    }
    return bytes;
}

void print_map_report(const ws::rgba_terrain_core::RgbaImage& image,
                      const ws::rgba_terrain_core::VoxelVolume& volume) {
    std::uint32_t min_height = 65535;
    std::uint32_t max_height = 0;
    std::size_t water_pixels = 0;
    std::size_t forest_pixels = 0;
    std::size_t mountain_pixels = 0;
    std::size_t river_pixels = 0;
    std::size_t lake_pixels = 0;
    for (const auto& p : image.pixels) {
        const std::uint32_t height =
            (static_cast<std::uint32_t>(p.r) << 8u) | p.g;
        min_height = std::min(min_height, height);
        max_height = std::max(max_height, height);
        water_pixels += (p.a & 0x80u) != 0 ? 1u : 0u;
        forest_pixels += (p.a & 0x08u) != 0 ? 1u : 0u;
        mountain_pixels += (p.a & 0x10u) != 0 ? 1u : 0u;
        river_pixels += (p.a & 0x20u) != 0 ? 1u : 0u;
        lake_pixels += (p.a & 0x40u) != 0 ? 1u : 0u;
    }
    ws::core::info("rgba_terrain: data map {}x{}", image.width, image.height);
    ws::core::info("rgba_terrain: packed 16-bit height range {}..{}", min_height, max_height);
    ws::core::info("rgba_terrain: channels B=material ID, A=biome + feature flags");
    ws::core::info("rgba_terrain: feature columns water={}, forest={}, mountain={}, river={}, lake={}",
                   water_pixels, forest_pixels, mountain_pixels, river_pixels,
                   lake_pixels);
    ws::core::info("rgba_terrain: voxel volume {}x{}x{} ({} voxels, {} payload bytes)",
                   volume.width, volume.depth, volume.height, volume.voxels.size(),
                   volume.voxels.size() * 5u);
}

void print_distribution(const char* label,
                         const std::array<std::uint64_t, 256>& counts) {
    std::vector<std::pair<std::uint64_t, unsigned>> sorted;
    for (unsigned value = 0; value < counts.size(); ++value) {
        if (counts[value] != 0) {
            sorted.emplace_back(counts[value], value);
        }
    }
    std::sort(sorted.begin(), sorted.end(), std::greater<>());
    std::string summary = std::string(label) + ":";
    if (sorted.empty()) summary += " none";
    const std::size_t limit = std::min<std::size_t>(sorted.size(), 10);
    for (std::size_t i = 0; i < limit; ++i)
        summary += std::format(" {}={}", sorted[i].second, sorted[i].first);
    if (sorted.size() > limit)
        summary += std::format(" (+{} more)", sorted.size() - limit);
    ws::core::info("rgba_terrain: {}", summary);
}

int inspect_command(int argc, char** argv) {
    if (argc != 4) {
        ws::core::warn("rgba_terrain: usage: rgba_terrain_poc inspect <input.png> <input.rgba-terrain>");
        return 2;
    }

    const auto metadata = ws::rgba_terrain_core::load_metadata(argv[3]);
    const auto image = ws::rgba_terrain_core::load_rgba_image(argv[2]);
    const auto analysis = ws::rgba_terrain_core::analyze_map(
        image, metadata.layout, metadata.settings);
    const double pixels = static_cast<double>(analysis.width) *
                          static_cast<double>(analysis.depth);
    const auto percentage = [pixels](std::uint64_t count) {
        return pixels > 0.0 ? 100.0 * static_cast<double>(count) / pixels : 0.0;
    };

    ws::core::info("rgba_terrain: terrain data-map inspection");
    ws::core::info("rgba_terrain: dimensions {}x{} ({} columns)",
                   analysis.width, analysis.depth, static_cast<std::uint64_t>(pixels));
    ws::core::info("rgba_terrain: encoded elevation (0..{}): min={} max={} distinct={} mean={:.3f} stddev={:.3f}",
                   analysis.height_encoding_max, analysis.min_height, analysis.max_height,
                   analysis.distinct_heights, analysis.mean_height, analysis.standard_deviation);
    ws::core::info("rgba_terrain: horizontal neighbor step: mean={:.3f} max={} across {} edges",
                   analysis.mean_horizontal_step, analysis.max_horizontal_step, analysis.horizontal_edges);
    ws::core::info("rgba_terrain: vertical neighbor step: mean={:.3f} max={} across {} edges",
                   analysis.mean_vertical_step, analysis.max_vertical_step, analysis.vertical_edges);
    print_distribution("surface materials", analysis.materials);
    print_distribution("biomes", analysis.biomes);
    ws::core::info("rgba_terrain: feature coverage water={} ({:.3f}%)",
                   analysis.water_columns, percentage(analysis.water_columns));
    ws::core::info("rgba_terrain: feature coverage forest={} ({:.3f}%)",
                   analysis.forest_columns, percentage(analysis.forest_columns));
    ws::core::info("rgba_terrain: feature coverage mountain={} ({:.3f}%)",
                   analysis.mountain_columns, percentage(analysis.mountain_columns));
    ws::core::info("rgba_terrain: feature coverage river={} ({:.3f}%)",
                   analysis.river_columns, percentage(analysis.river_columns));
    ws::core::info("rgba_terrain: reserved alpha bit={} ({:.3f}%)",
                   analysis.reserved_flag_columns, percentage(analysis.reserved_flag_columns));
    return 0;
}

int generate_command(int argc, char** argv) {
    if (argc != 8) {
        ws::core::warn("rgba_terrain: usage: rgba_terrain_poc generate <width> <depth> <seed> <output.png> <output.rgba-terrain> <output.rgba-voxels>");
        return 2;
    }

    const std::size_t width = parse_size(argv[2], "width");
    const std::size_t depth = parse_size(argv[3], "depth");
    constexpr std::size_t max_voxels = 64u * 1024u * 1024u;
    if (width > std::numeric_limits<std::size_t>::max() / depth ||
        width * depth > max_voxels / 64u) {
        throw std::runtime_error(
            "requested terrain volume exceeds the 64-million-voxel POC limit");
    }
    const std::uint32_t seed = parse_seed(argv[4]);

    ws::rgba_terrain_core::TerrainSettings settings;
    settings.min_height = 0.0f;
    settings.max_height = 56.0f;
    settings.volume_height = 64;
    settings.sea_level = 12;
    settings.subsurface_depth = 4;

    const auto image = ws::rgba_terrain_core::generate_synthetic_map(
        width, depth, seed, settings);

    const auto bytes = flatten_rgba(image);
    const auto written = ws::image::write_rgba8_png_file(
        argv[5], checked_u32(width, "width"), checked_u32(depth, "depth"),
        bytes);
    if (!written) {
        throw std::runtime_error("PNG write failed: " + written.error().message);
    }

    const std::string metadata =
        "# RGBA terrain data map v1\n"
        "channel.r=height16_msb\n"
        "channel.g=height16_lsb\n"
        "channel.b=material_id\n"
        "channel.a=biome_water_flags\n"
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
    {
        std::ofstream out(argv[6]);
        if (!out) {
            throw std::runtime_error("cannot create metadata output");
        }
        out << metadata;
        if (!out) {
            throw std::runtime_error("failed writing metadata output");
        }
    }

    // Read both artifacts back from disk. This exercises the metadata parser
    // and ensures voxelization consumes the exact persisted PNG channel data.
    const auto decoded_metadata = ws::rgba_terrain_core::load_metadata(argv[6]);
    const auto decoded_image = ws::rgba_terrain_core::load_rgba_image(argv[5]);
    const auto volume = ws::rgba_terrain_core::generate_voxel_volume(
        decoded_image, decoded_metadata.layout, decoded_metadata.settings);
    const auto voxel_write = ws::rgba_terrain_core::write_voxel_file(argv[7], volume);
    if (!voxel_write) {
        ws::core::log_error(voxel_write.error());
        return 1;
    }
    print_map_report(decoded_image, volume);
    ws::core::info("rgba_terrain: wrote data PNG '{}'", argv[5]);
    ws::core::info("rgba_terrain: wrote metadata '{}'", argv[6]);
    ws::core::info("rgba_terrain: wrote voxel volume '{}'", argv[7]);
    return 0;
}

int generate_world_command(int argc, char** argv) {
    if (argc != 8) {
        ws::core::warn("rgba_terrain: usage: rgba_terrain_poc generate-world <width> <depth> <seed> <output.png> <output.rgba-terrain> <output.rgba-voxels>");
        return 2;
    }
    const std::size_t width = parse_size(argv[2], "width");
    const std::size_t depth = parse_size(argv[3], "depth");
    constexpr std::size_t max_voxels = 64u * 1024u * 1024u;
    if (width > std::numeric_limits<std::size_t>::max() / depth ||
        width * depth > max_voxels / 64u) {
        throw std::runtime_error("requested terrain volume exceeds the 64-million-voxel POC limit");
    }
    const std::uint32_t seed = parse_seed(argv[4]);
    ws::rgba_terrain_core::TerrainSettings settings;
    settings.min_height = 0.0f;
    settings.max_height = 56.0f;
    settings.volume_height = 64;
    settings.sea_level = 12;
    settings.subsurface_depth = 4;

    auto plan = ws::rgba_terrain_core::make_default_world_plan(
        static_cast<std::int64_t>(width), static_cast<std::int64_t>(depth), seed);
    const double span = static_cast<double>(std::max(width, depth));
    plan.scales.continent_scale = std::max(16.0, span * 0.52);
    plan.scales.province_scale = std::max(8.0, span * 0.23);
    plan.scales.ecosystem_scale = std::max(4.0, span * 0.12);
    plan.scales.sub_biome_scale = std::max(2.0, span * 0.045);
    plan.scales.habitat_scale = std::max(1.0, span * 0.015);
    plan.hydrology.routing_cell_scale = std::max(1.0, span / 32.0);
    plan.validate();

    const ws::rgba_terrain_core::WorldSampler sampler(std::move(plan));
    const auto image = sampler.generate_rgba_region(0, 0, width, depth, settings);
    const auto bytes = flatten_rgba(image);
    const auto written = ws::image::write_rgba8_png_file(
        argv[5], checked_u32(width, "width"), checked_u32(depth, "depth"), bytes);
    if (!written) throw std::runtime_error("PNG write failed: " + written.error().message);

    const std::string metadata =
        "# RGBA terrain data map v1\n"
        "channel.r=height16_msb\nchannel.g=height16_lsb\n"
        "channel.b=material_id\nchannel.a=biome_water_flags\n"
        "min_height=0\nmax_height=56\nvolume_height=64\nsea_level=12\n"
        "subsurface_depth=4\nwater_threshold=128\ndefault_material=1\n"
        "subsurface_material=2\ndeep_material=3\nwater_material=6\n"
        "default_biome=0\nsolid_density=255\n";
    {
        std::ofstream out(argv[6]);
        if (!out) throw std::runtime_error("cannot create metadata output");
        out << metadata;
        if (!out) throw std::runtime_error("failed writing metadata output");
    }
    const auto decoded_metadata = ws::rgba_terrain_core::load_metadata(argv[6]);
    const auto decoded_image = ws::rgba_terrain_core::load_rgba_image(argv[5]);
    const auto volume = ws::rgba_terrain_core::generate_voxel_volume(
        decoded_image, decoded_metadata.layout, decoded_metadata.settings);
    const auto voxel_write = ws::rgba_terrain_core::write_voxel_file(argv[7], volume);
    if (!voxel_write) {
        ws::core::log_error(voxel_write.error());
        return 1;
    }
    print_map_report(decoded_image, volume);
    ws::core::info("rgba_terrain: generated with WorldSampler + hydrology");
    ws::core::info("rgba_terrain: wrote data PNG '{}'", argv[5]);
    ws::core::info("rgba_terrain: wrote metadata '{}'", argv[6]);
    ws::core::info("rgba_terrain: wrote voxel volume '{}'", argv[7]);
    return 0;
}

int convert_command(int argc, char** argv) {
    if (argc != 5) {
        ws::core::warn("rgba_terrain: usage: rgba_terrain_poc convert <input.png> <input.rgba-terrain> <output.rgba-voxels>");
        return 2;
    }
    const auto metadata = ws::rgba_terrain_core::load_metadata(argv[3]);
    const auto image = ws::rgba_terrain_core::load_rgba_image(argv[2]);
    const auto volume = ws::rgba_terrain_core::generate_voxel_volume(
        image, metadata.layout, metadata.settings);
    const auto voxel_write = ws::rgba_terrain_core::write_voxel_file(argv[4], volume);
    if (!voxel_write) {
        ws::core::log_error(voxel_write.error());
        return 1;
    }
    print_map_report(image, volume);
    return 0;
}


int validate_command(int argc, char** argv) {
    if (argc != 5) {
        ws::core::warn("rgba_terrain: usage: rgba_terrain_poc validate <input.png> <input.rgba-terrain> <input.rgba-voxels>");
        return 2;
    }
    const auto metadata = ws::rgba_terrain_core::load_metadata(argv[3]);
    const auto image = ws::rgba_terrain_core::load_rgba_image(argv[2]);
    const auto expected = ws::rgba_terrain_core::generate_voxel_volume(
        image, metadata.layout, metadata.settings);
    const auto loaded = ws::rgba_terrain_core::read_voxel_file(argv[4]);
    if (!loaded) {
        ws::core::log_error(loaded.error());
        return 1;
    }

    const auto report = ws::rgba_terrain_core::compare_voxel_volumes(
        expected, *loaded);
    ws::core::info("rgba_terrain: voxel validation");
    ws::core::info("rgba_terrain: expected volume {}x{}x{}",
                   expected.width, expected.depth, expected.height);
    ws::core::info("rgba_terrain: actual volume {}x{}x{}",
                   loaded->width, loaded->depth, loaded->height);
    if (!report.dimensions_match) {
        ws::core::error("rgba_terrain: voxel volume dimensions do not match");
        return 3;
    }
    if (!report.expected_storage_valid || !report.actual_storage_valid) {
        ws::core::error("rgba_terrain: invalid voxel storage");
        return 3;
    }

    ws::core::info("rgba_terrain: compared {} voxels; mismatched {}",
                   report.compared_voxels, report.mismatched_voxels);
    constexpr std::array<const char*, 5> names{
        "kind", "material", "biome", "density", "feature_flags"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        ws::core::info("rgba_terrain: {} mismatches={}",
                       names[i], report.field_mismatches[i]);
    }
    if (report.exact_match()) {
        ws::core::info("rgba_terrain: VALID — voxel file exactly matches PNG + metadata reconstruction");
        return 0;
    }
    if (report.has_first_mismatch) {
        const auto& m = report.first_mismatch;
        ws::core::error(
            "rgba_terrain: first mismatch at x={} y={} z={} "
            "(expected kind/material/biome/density/flags={}/{}/{}/{}/{}, "
            "actual={}/{}/{}/{}/{})",
            m.x, m.y, m.z,
            static_cast<unsigned>(m.expected.kind), m.expected.material_id,
            m.expected.biome_id, m.expected.density, m.expected.feature_flags,
            static_cast<unsigned>(m.actual.kind), m.actual.material_id,
            m.actual.biome_id, m.actual.density, m.actual.feature_flags);
    }
    return 3;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc >= 2 && std::string(argv[1]) == "generate") {
            return generate_command(argc, argv);
        }
        if (argc >= 2 && std::string(argv[1]) == "generate-world") {
            return generate_world_command(argc, argv);
        }
        if (argc >= 2 && std::string(argv[1]) == "convert") {
            return convert_command(argc, argv);
        }
        if (argc >= 2 && std::string(argv[1]) == "inspect") {
            return inspect_command(argc, argv);
        }
        if (argc >= 2 && std::string(argv[1]) == "validate") {
            return validate_command(argc, argv);
        }
        ws::core::info("rgba_terrain: RGBA terrain generator");
        ws::core::info("rgba_terrain: commands: generate <width> <depth> <seed> <out.png> <out.meta> <out.voxels>");
        ws::core::info("rgba_terrain:          generate-world <width> <depth> <seed> <out.png> <out.meta> <out.voxels>");
        ws::core::info("rgba_terrain:          convert <input.png> <input.meta> <out.voxels>");
        ws::core::info("rgba_terrain:          inspect <input.png> <input.meta>");
        ws::core::info("rgba_terrain:          validate <input.png> <input.meta> <input.voxels>");
        return 2;
    } catch (const std::exception& exception) {
        ws::core::error("rgba_terrain: {}", exception.what());
        return 1;
    }
}
