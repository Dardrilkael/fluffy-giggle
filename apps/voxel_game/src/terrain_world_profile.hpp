#pragma once

#include <cstdint>

#include <ws/rgba_terrain_core/rgba_terrain.hpp>
#include <ws/rgba_terrain_core/world_plan.hpp>

// Shared configuration for the authoritative voxel-game terrain pipeline.
// Feature scales are absolute game-space units (one block = one unit), not
// ratios recomputed from requested PNG dimensions.
namespace voxel::terrain_world_profile {

inline constexpr float kMinimumHeight = 0.0f;
inline constexpr float kMaximumHeight = 56.0f;
inline constexpr std::uint32_t kVolumeHeight = 64;
inline constexpr std::uint32_t kSeaLevel = 12;

inline constexpr double kContinentScale = 133.12;
inline constexpr double kProvinceScale = 58.88;
inline constexpr double kEcosystemScale = 30.72;
inline constexpr double kSubBiomeScale = 11.52;
inline constexpr double kHabitatScale = 3.84;
inline constexpr double kRoutingCellScale = 8.0;

[[nodiscard]] inline ws::rgba_terrain_core::WorldPlan make_world_plan(
    std::int64_t width, std::int64_t depth, std::uint64_t seed) {
    auto plan = ws::rgba_terrain_core::make_default_world_plan(
        width, depth, seed);
    plan.scales.continent_scale = kContinentScale;
    plan.scales.province_scale = kProvinceScale;
    plan.scales.ecosystem_scale = kEcosystemScale;
    plan.scales.sub_biome_scale = kSubBiomeScale;
    plan.scales.habitat_scale = kHabitatScale;
    plan.hydrology.routing_cell_scale = kRoutingCellScale;
    plan.validate();
    return plan;
}

[[nodiscard]] inline ws::rgba_terrain_core::TerrainSettings settings() {
    ws::rgba_terrain_core::TerrainSettings result;
    result.min_height = kMinimumHeight;
    result.max_height = kMaximumHeight;
    result.volume_height = kVolumeHeight;
    result.sea_level = kSeaLevel;
    result.subsurface_depth = 4;
    return result;
}

[[nodiscard]] inline ws::rgba_terrain_core::ChannelLayout layout() {
    ws::rgba_terrain_core::ChannelLayout result;
    result.channels = {
        ws::rgba_terrain_core::ChannelRole::Height16Msb,
        ws::rgba_terrain_core::ChannelRole::Height16Lsb,
        ws::rgba_terrain_core::ChannelRole::MaterialFeatureFlags,
        ws::rgba_terrain_core::ChannelRole::BiomeId,
    };
    return result;
}

}  // namespace voxel::terrain_world_profile
