#pragma once

#include <ws/rgba_terrain_core/rgba_terrain.hpp>
#include <ws/rgba_terrain_core/world_plan.hpp>

#include <cstdint>
#include <vector>

namespace ws::rgba_terrain_core {

struct WorldSample {
    double normalized_elevation = 0.0;
    double elevation_m = 0.0;
    double continentalness = 0.0;
    double temperature_c = 0.0;
    double humidity = 0.0;
    double precipitation = 0.0;

    std::uint8_t ecosystem_id = 0;
    std::uint8_t biome_id = 0;
    std::uint8_t material_id = 1;
    std::uint8_t feature_flags = 0;

    // Values near one indicate a strong distinction from the next candidate;
    // habitat_variation is a stable local field in [0,1].
    double ecosystem_transition = 0.0;
    double biome_transition = 0.0;
    double habitat_variation = 0.0;

    bool is_ocean = false;
    bool is_mountain = false;

    friend bool operator==(const WorldSample&, const WorldSample&) = default;
};

class WorldSampler {
public:
    // Owns a copy of the compact plan and validates it once at construction.
    explicit WorldSampler(WorldPlan plan);

    [[nodiscard]] const WorldPlan& plan() const noexcept { return plan_; }

    // Samples one global X/Z coordinate. Coordinates outside the finite world
    // are rejected; adjacent chunks must pass their true global coordinates.
    [[nodiscard]] WorldSample sample(
        std::int64_t world_x, std::int64_t world_z) const;

    // Produces a small RGBA8 data-map region at its global world origin. The
    // returned image can be passed through the existing PNG writer and voxel
    // reconstruction path. The region must fit entirely inside the world.
    [[nodiscard]] RgbaImage generate_rgba_region(
        std::int64_t origin_x, std::int64_t origin_z,
        std::size_t width, std::size_t depth,
        const TerrainSettings& settings) const;

private:
    WorldPlan plan_;
};

}  // namespace ws::rgba_terrain_core
