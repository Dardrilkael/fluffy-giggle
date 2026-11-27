#include <ws/rgba_terrain_core/rgba_terrain.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ws::rgba_terrain_core {
namespace {

std::uint8_t channel_value(const Rgba8& pixel, Channel channel) {
    switch (channel) {
        case Channel::R: return pixel.r;
        case Channel::G: return pixel.g;
        case Channel::B: return pixel.b;
        case Channel::A: return pixel.a;
    }
    throw std::logic_error("invalid RGBA channel");
}

int find_role(const ChannelLayout& layout, ChannelRole role) {
    for (std::size_t i = 0; i < layout.channels.size(); ++i) {
        if (layout.channels[i] == role) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::uint8_t value_at(const Rgba8& pixel, int channel) {
    return channel < 0
        ? std::uint8_t{0}
        : channel_value(pixel, static_cast<Channel>(channel));
}

void validate_height_range(const TerrainSettings& settings) {
    if (!std::isfinite(settings.min_height) ||
        !std::isfinite(settings.max_height) ||
        settings.min_height < 0.0f ||
        settings.max_height < settings.min_height ||
        static_cast<double>(settings.max_height) >
            static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::invalid_argument(
            "terrain height range must be finite, non-negative, ordered, "
            "and fit uint32");
    }
}

void validate_volume_settings(const TerrainSettings& settings) {
    validate_height_range(settings);
    if (settings.volume_height == 0) {
        throw std::invalid_argument("voxel volume height must be non-zero");
    }
    if (settings.max_height >= static_cast<float>(settings.volume_height)) {
        throw std::invalid_argument(
            "max_height must be smaller than volume_height");
    }
    if (settings.sea_level >= settings.volume_height) {
        throw std::invalid_argument(
            "sea_level must be inside the voxel volume");
    }
}

std::size_t checked_volume_size(std::size_t width, std::size_t depth,
                                std::size_t height) {
    if (width == 0 || depth == 0 || height == 0) {
        throw std::invalid_argument("voxel volume dimensions must be non-zero");
    }
    const auto limit = std::numeric_limits<std::size_t>::max();
    if (width > limit / depth) {
        throw std::length_error("voxel volume dimensions overflow size_t");
    }
    const std::size_t plane = width * depth;
    if (plane > limit / height) {
        throw std::length_error("voxel volume dimensions overflow size_t");
    }
    return plane * height;
}

}  // namespace

const Rgba8& RgbaImage::at(std::size_t x, std::size_t z) const {
    if (x >= width || z >= height) {
        throw std::out_of_range("RGBA pixel coordinate");
    }
    return pixels[z * width + x];
}

void RgbaImage::validate() const {
    if (width == 0 || height == 0) {
        throw std::invalid_argument("image dimensions must be non-zero");
    }
    if (width > std::numeric_limits<std::size_t>::max() / height ||
        width * height != pixels.size()) {
        throw std::invalid_argument(
            "RGBA pixel count does not match image dimensions");
    }
}

void ChannelLayout::validate() const {
    std::array<unsigned, 9> counts{};
    for (const ChannelRole role : channels) {
        const auto index = static_cast<std::size_t>(role);
        if (index >= counts.size()) {
            throw std::invalid_argument("channel layout contains an invalid role");
        }
        ++counts[index];
    }

    const bool height8 = counts[static_cast<std::size_t>(ChannelRole::Height8)] == 1;
    const bool height16 =
        counts[static_cast<std::size_t>(ChannelRole::Height16Msb)] == 1 &&
        counts[static_cast<std::size_t>(ChannelRole::Height16Lsb)] == 1;
    if (height8 == height16 ||
        counts[static_cast<std::size_t>(ChannelRole::Height8)] > 1 ||
        counts[static_cast<std::size_t>(ChannelRole::Height16Msb)] > 1 ||
        counts[static_cast<std::size_t>(ChannelRole::Height16Lsb)] > 1) {
        throw std::invalid_argument(
            "layout needs either one Height8 channel or exactly one "
            "Height16Msb/Height16Lsb pair");
    }

    for (std::size_t i = static_cast<std::size_t>(ChannelRole::MaterialId);
         i < counts.size(); ++i) {
        if (counts[i] > 1) {
            throw std::invalid_argument(
                "a channel role may only be assigned once");
        }
    }
}

const Voxel& VoxelVolume::at(std::size_t x, std::size_t y,
                             std::size_t z) const {
    if (x >= width || y >= height || z >= depth) {
        throw std::out_of_range("voxel coordinate");
    }
    return voxels[x + z * width + y * width * depth];
}

Voxel& VoxelVolume::at(std::size_t x, std::size_t y, std::size_t z) {
    if (x >= width || y >= height || z >= depth) {
        throw std::out_of_range("voxel coordinate");
    }
    return voxels[x + z * width + y * width * depth];
}

void VoxelVolume::validate() const {
    const std::size_t expected = checked_volume_size(width, depth, height);
    if (voxels.size() != expected) {
        throw std::invalid_argument(
            "voxel count does not match volume dimensions");
    }
}

TerrainColumns generate_columns(const RgbaImage& image,
                                const ChannelLayout& layout,
                                const TerrainSettings& settings) {
    image.validate();
    layout.validate();
    validate_height_range(settings);

    const int height8 = find_role(layout, ChannelRole::Height8);
    const int height_msb = find_role(layout, ChannelRole::Height16Msb);
    const int height_lsb = find_role(layout, ChannelRole::Height16Lsb);
    const int material = find_role(layout, ChannelRole::MaterialId);
    const int biome = find_role(layout, ChannelRole::BiomeId);
    const int water = find_role(layout, ChannelRole::WaterMask);
    const int density = find_role(layout, ChannelRole::Density);
    const int packed_flags = find_role(layout, ChannelRole::BiomeWaterFlags);
    if (packed_flags >= 0 && (biome >= 0 || water >= 0)) {
        throw std::invalid_argument(
            "biome_water_flags cannot be combined with separate biome_id or water_mask channels");
    }

    TerrainColumns result{image.width, image.height, {}};
    result.columns.reserve(image.pixels.size());

    const double levels = height8 >= 0 ? 255.0 : 65535.0;
    const double height_span =
        static_cast<double>(settings.max_height) - settings.min_height;

    for (std::size_t z = 0; z < image.height; ++z) {
        for (std::size_t x = 0; x < image.width; ++x) {
            const Rgba8& pixel = image.at(x, z);
            const std::uint32_t encoded = height8 >= 0
                ? value_at(pixel, height8)
                : (static_cast<std::uint32_t>(value_at(pixel, height_msb)) << 8u) |
                      value_at(pixel, height_lsb);
            const double normalized = static_cast<double>(encoded) / levels;
            const double world_height =
                static_cast<double>(settings.min_height) +
                normalized * height_span;
            const auto height = static_cast<std::uint32_t>(
                std::floor(world_height + 0.5));

            result.columns.push_back(VoxelColumn{
                x,
                z,
                height,
                material >= 0 ? value_at(pixel, material)
                              : settings.default_material,
                packed_flags >= 0
                    ? static_cast<std::uint8_t>(value_at(pixel, packed_flags) & 0x07u)
                    : (biome >= 0 ? value_at(pixel, biome) : settings.default_biome),
                water >= 0 ? value_at(pixel, water) : std::uint8_t{0},
                density >= 0 ? value_at(pixel, density)
                             : settings.solid_density,
                packed_flags >= 0
                    ? static_cast<std::uint8_t>(value_at(pixel, packed_flags) & 0xF8u)
                    : std::uint8_t{0},
            });
        }
    }
    return result;
}

VoxelVolume generate_voxel_volume(const TerrainColumns& columns,
                                  const TerrainSettings& settings) {
    validate_volume_settings(settings);
    if (columns.width == 0 || columns.depth == 0) {
        throw std::invalid_argument("terrain column dimensions must be non-zero");
    }
    if (columns.width > std::numeric_limits<std::size_t>::max() /
                            columns.depth ||
        columns.width * columns.depth != columns.columns.size()) {
        throw std::invalid_argument(
            "terrain column count does not match its dimensions");
    }

    VoxelVolume volume;
    volume.width = columns.width;
    volume.depth = columns.depth;
    volume.height = settings.volume_height;
    volume.voxels.resize(checked_volume_size(
        volume.width, volume.depth, volume.height));

    for (std::size_t z = 0; z < volume.depth; ++z) {
        for (std::size_t x = 0; x < volume.width; ++x) {
            const VoxelColumn& column =
                columns.columns[z * volume.width + x];
            if (column.x != x || column.z != z) {
                throw std::invalid_argument(
                    "terrain columns must be in row-major x/z order");
            }
            if (column.height >= volume.height) {
                throw std::invalid_argument(
                    "terrain column height lies outside the voxel volume");
            }
        }
    }

    // Construct in Y/Z/X order: X is contiguous in the chosen volume layout.
    // This also makes each vertical layer a sequence of adjacent image pixels.
    for (std::size_t y = 0; y < volume.height; ++y) {
        for (std::size_t z = 0; z < volume.depth; ++z) {
            for (std::size_t x = 0; x < volume.width; ++x) {
                const VoxelColumn& column =
                    columns.columns[z * volume.width + x];
                Voxel voxel{};
                voxel.biome_id = column.biome_id;
                voxel.feature_flags = column.feature_flags;

                if (y <= column.height) {
                    voxel.kind = VoxelKind::Solid;
                    voxel.density = column.density;
                    if (y == column.height) {
                        voxel.material_id = column.material_id;
                    } else if (y + settings.subsurface_depth >= column.height) {
                        voxel.material_id = settings.subsurface_material;
                    } else {
                        voxel.material_id = settings.deep_material;
                    }
                } else if (y <= settings.sea_level &&
                           (column.water_mask >= settings.water_threshold ||
                            (column.feature_flags & 0x80u) != 0)) {
                    voxel.kind = VoxelKind::Water;
                    voxel.material_id = settings.water_material;
                    voxel.density = 255;
                }

                volume.at(x, y, z) = voxel;
            }
        }
    }

    volume.validate();
    return volume;
}

VoxelVolume generate_voxel_volume(const RgbaImage& image,
                                  const ChannelLayout& layout,
                                  const TerrainSettings& settings) {
    const TerrainColumns columns = generate_columns(image, layout, settings);
    return generate_voxel_volume(columns, settings);
}

}  // namespace ws::rgba_terrain_core
