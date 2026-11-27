#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ws::rgba_terrain_core {

enum class ChannelRole : std::uint8_t {
    Unused,
    Height8,
    Height16Msb,
    Height16Lsb,
    MaterialId,
    BiomeId,
    WaterMask,
    Density,
    // Legacy v1 alpha packing: biome bits 0..2; flags bits 3..7.
    BiomeWaterFlags,
    // v2 B channel: material bits 0..3, feature flags bits 4..7.
    MaterialFeatureFlags,
};

enum class Channel : std::uint8_t { R = 0, G = 1, B = 2, A = 3 };

struct Rgba8 {
    std::uint8_t r{}, g{}, b{}, a{};
};

struct RgbaImage {
    std::size_t width{}, height{};
    std::vector<Rgba8> pixels;

    [[nodiscard]] const Rgba8& at(std::size_t x, std::size_t z) const;
    void validate() const;
};

struct ChannelLayout {
    std::array<ChannelRole, 4> channels{
        ChannelRole::Height8,
        ChannelRole::MaterialId,
        ChannelRole::BiomeId,
        ChannelRole::WaterMask,
    };

    void validate() const;
};

struct TerrainSettings {
    // The top surface height is mapped into [min_height, max_height].
    float min_height = 0.0f;
    float max_height = 46.0f;

    // Vertical world volume. Heights are inclusive: a height of N fills
    // solid voxels through y=N.
    std::uint32_t volume_height = 64;

    // Water is filled above qualifying columns up to and including sea_level.
    std::uint32_t sea_level = 12;
    std::uint8_t water_threshold = 128;

    // Material layering, from the surface downwards.
    std::uint32_t subsurface_depth = 4;
    std::uint8_t default_material = 1;
    std::uint8_t subsurface_material = 2;
    std::uint8_t deep_material = 3;
    std::uint8_t water_material = 6;

    std::uint8_t default_biome = 0;
    std::uint8_t solid_density = 255;
};

struct VoxelColumn {
    std::size_t x{}, z{};
    std::uint32_t height{};
    std::uint8_t material_id{}, biome_id{}, water_mask{}, density{}, feature_flags{};
};

struct TerrainColumns {
    std::size_t width{}, depth{};
    std::vector<VoxelColumn> columns;
};

enum class VoxelKind : std::uint8_t {
    Air = 0,
    Solid = 1,
    Water = 2,
};

// Five bytes per voxel. feature_flags are preserved independently from biome ID.
struct Voxel {
    VoxelKind kind = VoxelKind::Air;
    std::uint8_t material_id = 0;
    std::uint8_t biome_id = 0;
    std::uint8_t density = 0;
    std::uint8_t feature_flags = 0;
};

struct VoxelVolume {
    std::size_t width{}, depth{}, height{};
    // X is fastest, then Z, then Y: x + z*width + y*width*depth.
    std::vector<Voxel> voxels;

    [[nodiscard]] const Voxel& at(std::size_t x, std::size_t y,
                                  std::size_t z) const;
    [[nodiscard]] Voxel& at(std::size_t x, std::size_t y, std::size_t z);
    void validate() const;
};

[[nodiscard]] TerrainColumns generate_columns(
    const RgbaImage&, const ChannelLayout&, const TerrainSettings&);
[[nodiscard]] VoxelVolume generate_voxel_volume(
    const TerrainColumns&, const TerrainSettings&);
[[nodiscard]] VoxelVolume generate_voxel_volume(
    const RgbaImage&, const ChannelLayout&, const TerrainSettings&);
[[nodiscard]] RgbaImage load_rgba_image(const std::string& path);
// Legacy v1 API. Preserves the original RGBA channel contract but delegates
// all terrain, climate, biome and hydrology sampling to WorldSampler.
// New production code should use WorldSampler::generate_rgba_region().
[[nodiscard]] RgbaImage generate_synthetic_map(
    std::size_t width, std::size_t depth, std::uint32_t seed,
    const TerrainSettings& settings);

}  // namespace ws::rgba_terrain_core
