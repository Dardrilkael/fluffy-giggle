#include "rgba_terrain/metadata.hpp"
#include "rgba_terrain/map_analysis.hpp"
#include "rgba_terrain/rgba_terrain.hpp"

#include <ws/image/image.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <utility>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void write_u32(std::ostream& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.put(static_cast<char>((value >> shift) & 255u));
    }
}

std::uint32_t checked_u32(std::size_t value, const char* field) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(std::string(field) +
                                 " exceeds the file format limit");
    }
    return static_cast<std::uint32_t>(value);
}

std::size_t parse_size(const char* text, const char* field) {
    std::size_t used = 0;
    unsigned long long value = 0;
    try {
        value = std::stoull(text, &used);
    } catch (...) {
        throw std::runtime_error(std::string("invalid ") + field);
    }
    if (used != std::string(text).size() ||
        value == 0 ||
        value > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error(std::string("invalid ") + field);
    }
    return static_cast<std::size_t>(value);
}

std::uint32_t parse_seed(const char* text) {
    std::size_t used = 0;
    unsigned long long value = 0;
    try {
        value = std::stoull(text, &used, 0);
    } catch (...) {
        throw std::runtime_error("invalid seed");
    }
    if (used != std::string(text).size() ||
        value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("seed must fit uint32 (decimal or 0x hex)");
    }
    return static_cast<std::uint32_t>(value);
}

std::vector<std::uint8_t> flatten_rgba(
    const rgba_terrain::RgbaImage& image) {
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

void write_voxel_file(const rgba_terrain::VoxelVolume& volume,
                      const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot create voxel output: " + path);
    }

    // RGAVOX02: dimensions, then five bytes per voxel.
    out.write("RGAVOX02", 8);
    write_u32(out, checked_u32(volume.width, "width"));
    write_u32(out, checked_u32(volume.depth, "depth"));
    write_u32(out, checked_u32(volume.height, "height"));
    for (const auto& voxel : volume.voxels) {
        out.put(static_cast<char>(voxel.kind));
        out.put(static_cast<char>(voxel.material_id));
        out.put(static_cast<char>(voxel.biome_id));
        out.put(static_cast<char>(voxel.density));
        out.put(static_cast<char>(voxel.feature_flags));
    }
    if (!out) {
        throw std::runtime_error("failed writing voxel volume: " + path);
    }
}

void print_map_report(const rgba_terrain::RgbaImage& image,
                      const rgba_terrain::VoxelVolume& volume) {
    std::uint32_t min_height = 65535;
    std::uint32_t max_height = 0;
    std::size_t water_pixels = 0;
    std::size_t forest_pixels = 0;
    std::size_t mountain_pixels = 0;
    std::size_t river_pixels = 0;
    for (const auto& p : image.pixels) {
        const std::uint32_t height =
            (static_cast<std::uint32_t>(p.r) << 8u) | p.g;
        min_height = std::min(min_height, height);
        max_height = std::max(max_height, height);
        water_pixels += (p.a & 0x80u) != 0 ? 1u : 0u;
        forest_pixels += (p.a & 0x08u) != 0 ? 1u : 0u;
        mountain_pixels += (p.a & 0x10u) != 0 ? 1u : 0u;
        river_pixels += (p.a & 0x20u) != 0 ? 1u : 0u;
    }
    std::cout << "RGBA data map: " << image.width << "x" << image.height
              << "\n"
              << "  R/G: packed 16-bit height range " << min_height
              << ".." << max_height << "\n"
              << "  B: material ID; A: biome + feature flags\n"
              << "  water: " << water_pixels << ", forest: " << forest_pixels
              << ", mountain: " << mountain_pixels << ", river: "
              << river_pixels << "\n"
              << "Voxel volume: " << volume.width << "x" << volume.depth
              << "x" << volume.height << ", " << volume.voxels.size()
              << " voxels, " << volume.voxels.size() * 5u
              << " bytes of voxel payload\n";
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
    std::cout << "  " << label << ":";
    if (sorted.empty()) {
        std::cout << " none";
    }
    const std::size_t limit = std::min<std::size_t>(sorted.size(), 10);
    for (std::size_t i = 0; i < limit; ++i) {
        std::cout << " " << sorted[i].second << "=" << sorted[i].first;
    }
    if (sorted.size() > limit) {
        std::cout << " (+" << sorted.size() - limit << " more)";
    }
    std::cout << "\n";
}

int inspect_command(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: rgba_terrain_poc inspect <input.png> <input.rgba-terrain>\n";
        return 2;
    }

    const auto metadata = rgba_terrain::load_metadata(argv[3]);
    const auto image = rgba_terrain::load_rgba_image(argv[2]);
    const auto analysis = rgba_terrain::analyze_map(
        image, metadata.layout, metadata.settings);
    const double pixels = static_cast<double>(analysis.width) *
                          static_cast<double>(analysis.depth);
    const auto percentage = [pixels](std::uint64_t count) {
        return pixels > 0.0 ? 100.0 * static_cast<double>(count) / pixels : 0.0;
    };

    std::cout << "Terrain data-map inspection\n"
              << "  dimensions: " << analysis.width << "x" << analysis.depth
              << " (" << static_cast<std::uint64_t>(pixels) << " columns)\n"
              << "  encoded elevation (0..65535): min=" << analysis.min_height
              << " max=" << analysis.max_height
              << " distinct=" << analysis.distinct_heights
              << " mean=" << analysis.mean_height
              << " stddev=" << analysis.standard_deviation << "\n"
              << "  horizontal neighbor step: mean="
              << analysis.mean_horizontal_step
              << " max=" << analysis.max_horizontal_step
              << " across " << analysis.horizontal_edges << " edges\n"
              << "  vertical neighbor step: mean="
              << analysis.mean_vertical_step
              << " max=" << analysis.max_vertical_step
              << " across " << analysis.vertical_edges << " edges\n";
    print_distribution("surface materials", analysis.materials);
    print_distribution("biomes", analysis.biomes);
    std::cout << "  feature coverage:\n"
              << "    water: " << analysis.water_columns << " ("
              << percentage(analysis.water_columns) << "%)\n"
              << "    forest: " << analysis.forest_columns << " ("
              << percentage(analysis.forest_columns) << "%)\n"
              << "    mountain: " << analysis.mountain_columns << " ("
              << percentage(analysis.mountain_columns) << "%)\n"
              << "    river: " << analysis.river_columns << " ("
              << percentage(analysis.river_columns) << "%)\n"
              << "    reserved alpha bit: " << analysis.reserved_flag_columns
              << " ("
              << percentage(analysis.reserved_flag_columns) << "%)\n";
    return 0;
}

int generate_command(int argc, char** argv) {
    if (argc != 8) {
        std::cerr
            << "Usage: rgba_terrain_poc generate <width> <depth> <seed> "
               "<output.png> <output.rgba-terrain> <output.rgba-voxels>\n";
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

    rgba_terrain::TerrainSettings settings;
    settings.min_height = 0.0f;
    settings.max_height = 56.0f;
    settings.volume_height = 64;
    settings.sea_level = 12;
    settings.subsurface_depth = 4;

    const auto image = rgba_terrain::generate_synthetic_map(
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
    const auto decoded_metadata = rgba_terrain::load_metadata(argv[6]);
    const auto decoded_image = rgba_terrain::load_rgba_image(argv[5]);
    const auto volume = rgba_terrain::generate_voxel_volume(
        decoded_image, decoded_metadata.layout, decoded_metadata.settings);
    write_voxel_file(volume, argv[7]);
    print_map_report(decoded_image, volume);
    std::cout << "Wrote data PNG: " << argv[5] << "\n"
              << "Wrote metadata: " << argv[6] << "\n"
              << "Wrote voxels: " << argv[7] << "\n";
    return 0;
}

int convert_command(int argc, char** argv) {
    if (argc != 5) {
        std::cerr
            << "Usage: rgba_terrain_poc convert <input.png> "
               "<input.rgba-terrain> <output.rgba-voxels>\n";
        return 2;
    }
    const auto metadata = rgba_terrain::load_metadata(argv[3]);
    const auto image = rgba_terrain::load_rgba_image(argv[2]);
    const auto volume = rgba_terrain::generate_voxel_volume(
        image, metadata.layout, metadata.settings);
    write_voxel_file(volume, argv[4]);
    print_map_report(image, volume);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc >= 2 && std::string(argv[1]) == "generate") {
            return generate_command(argc, argv);
        }
        if (argc >= 2 && std::string(argv[1]) == "convert") {
            return convert_command(argc, argv);
        }
        if (argc >= 2 && std::string(argv[1]) == "inspect") {
            return inspect_command(argc, argv);
        }
        std::cerr
            << "RGBA terrain generator\n"
            << "  generate <width> <depth> <seed> <out.png> <out.meta> <out.voxels>\n"
            << "  convert <input.png> <input.meta> <out.voxels>\n"
            << "  inspect <input.png> <input.meta>\n";
        return 2;
    } catch (const std::exception& exception) {
        std::cerr << "RGBA terrain generation failed: " << exception.what()
                  << "\n";
        return 1;
    }
}
