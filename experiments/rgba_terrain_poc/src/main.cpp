#include "rgba_terrain/metadata.hpp"
#include "rgba_terrain/rgba_terrain.hpp"

#include <ws/image/image.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
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
    std::uint8_t min_height_hi = 255, max_height_hi = 0;
    std::uint8_t min_height_lo = 255, max_height_lo = 0;
    std::size_t water_pixels = 0;
    std::size_t forest_pixels = 0;
    std::size_t mountain_pixels = 0;
    for (const auto& p : image.pixels) {
        min_height_hi = std::min(min_height_hi, p.r);
        max_height_hi = std::max(max_height_hi, p.r);
        min_height_lo = std::min(min_height_lo, p.g);
        max_height_lo = std::max(max_height_lo, p.g);
        water_pixels += (p.a & 0x80u) != 0 ? 1u : 0u;
        forest_pixels += (p.a & 0x08u) != 0 ? 1u : 0u;
        mountain_pixels += (p.a & 0x10u) != 0 ? 1u : 0u;
    }
    std::cout << "RGBA data map: " << image.width << "x" << image.height
              << "\n"
              << "  R/G: packed 16-bit height (high-byte range "
              << static_cast<unsigned>(min_height_hi) << ".."
              << static_cast<unsigned>(max_height_hi) << ", low-byte range "
              << static_cast<unsigned>(min_height_lo) << ".."
              << static_cast<unsigned>(max_height_lo) << ")\n"
              << "  B: material ID; A: biome + feature flags\n"
              << "  water pixels: " << water_pixels
              << ", forest: " << forest_pixels
              << ", mountain: " << mountain_pixels << "\n"
              << "Voxel volume: " << volume.width << "x" << volume.depth
              << "x" << volume.height << ", " << volume.voxels.size()
              << " voxels, " << volume.voxels.size() * 5u
              << " bytes of voxel payload\n";
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
        std::cerr
            << "RGBA terrain generator\n"
            << "  generate <width> <depth> <seed> <out.png> <out.meta> <out.voxels>\n"
            << "  convert <input.png> <input.meta> <out.voxels>\n";
        return 2;
    } catch (const std::exception& exception) {
        std::cerr << "RGBA terrain generation failed: " << exception.what()
                  << "\n";
        return 1;
    }
}
