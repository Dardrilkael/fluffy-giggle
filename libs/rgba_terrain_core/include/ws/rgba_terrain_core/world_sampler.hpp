#pragma once

#include <ws/rgba_terrain_core/rgba_terrain.hpp>
#include <ws/rgba_terrain_core/world_plan.hpp>

#include <cstdint>
#include <vector>

namespace ws::rgba_terrain_core {

// Optional climate-only preview layers. These images are diagnostic products;
// they never replace or alter the authoritative RGBA terrain map.
struct WorldClimateDiagnosticImages {
    RgbaImage temperature;
    RgbaImage humidity;
    RgbaImage precipitation;
    RgbaImage orographic_lift;
    RgbaImage rain_shadow;
    RgbaImage ocean_moisture;
};

// Optional hydrology preview layers. These are independent diagnostic images;
// they never replace or modify the authoritative RGBA terrain map.
struct WorldHydrologyDiagnosticImages {
    RgbaImage flow_accumulation;
    RgbaImage river_strength;
    RgbaImage drainage_direction;
    RgbaImage lake_depth;
    RgbaImage lake_basins;
};

// D8 direction on the hydrology routing grid. North is negative world Z.
enum class DrainageDirection : std::uint8_t {
    North = 0,
    NorthEast = 1,
    East = 2,
    SouthEast = 3,
    South = 4,
    SouthWest = 5,
    West = 6,
    NorthWest = 7,
    Sink = 255,
};

struct WorldSample {
    double normalized_elevation = 0.0;
    double elevation_m = 0.0;
    double continentalness = 0.0;
    double temperature_c = 0.0;
    // Normalized moisture-availability index in [0,1], not relative humidity.
    double humidity = 0.0;
    double precipitation = 0.0;

    // Climate diagnostics from the deterministic prevailing-wind fetch.
    double orographic_lift = 0.0;
    double rain_shadow = 0.0;
    double ocean_moisture = 0.0;

    // Hydrology diagnostics. Accumulation is a bounded, precipitation-weighted
    // contributing-area estimate in source-cell equivalents.
    DrainageDirection drainage_direction = DrainageDirection::Sink;
    double watershed_accumulation = 0.0;
    double river_strength = 0.0;
    double lake_depth_m = 0.0;
    // Shared normalized spill elevation for cells classified inside a lake basin.
    double lake_surface_elevation = 0.0;

    std::uint8_t ecosystem_id = 0;
    std::uint8_t biome_id = 0;
    std::uint8_t material_id = 1;
    std::uint8_t feature_flags = 0;

    // Transition values near one indicate close competing candidates;
    // habitat_variation is a stable local field in [0,1].
    double ecosystem_transition = 0.0;
    double biome_transition = 0.0;
    double habitat_variation = 0.0;

    bool is_ocean = false;
    bool is_mountain = false;
    bool is_river = false;
    bool is_lake = false;

    friend bool operator==(const WorldSample&, const WorldSample&) = default;
};

// Authoritative deterministic 2D world generator. Production terrain maps
// should be generated through this sampler; generate_synthetic_map() is only
// a legacy v1 channel-layout adapter retained for source compatibility.
class WorldSampler {
public:
    // Owns a copy of the compact plan and validates it once at construction.
    explicit WorldSampler(WorldPlan plan);

    [[nodiscard]] const WorldPlan& plan() const noexcept { return plan_; }

    // Samples one global X/Z coordinate. Coordinates outside the finite world
    // are rejected; adjacent chunks must pass their true global coordinates.
    [[nodiscard]] WorldSample sample(
        std::int64_t world_x, std::int64_t world_z) const;

    // Produces an RGBA8 v2 data-map region at its global world origin:
    // R/G = 16-bit normalized elevation, B = material/feature nibbles,
    // A = biome ID. The returned image can be passed through the existing
    // PNG writer and voxel reconstruction path. The region must fit entirely
    // inside the finite world. This is the production map-generation API.
    [[nodiscard]] RgbaImage generate_rgba_region(
        std::int64_t origin_x, std::int64_t origin_z,
        std::size_t width, std::size_t depth,
        const TerrainSettings& settings,
        WorldClimateDiagnosticImages* climate_diagnostics = nullptr,
        WorldHydrologyDiagnosticImages* hydrology_diagnostics = nullptr) const;

private:
    [[nodiscard]] WorldSample sample_impl(
        std::int64_t world_x, std::int64_t world_z,
        bool include_local_hydrology,
        double terrain_elevation_offset = 0.0) const;

    WorldPlan plan_;
};

}  // namespace ws::rgba_terrain_core
