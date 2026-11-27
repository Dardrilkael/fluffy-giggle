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
    RgbaImage stream_order;
    RgbaImage watershed_basins;
    RgbaImage river_influence;
    RgbaImage mountain_peak_distance;
    // Red = incision, blue = deposition; intensity is normalized per world.
    RgbaImage erosion_delta;
    // Separate process diagnostics: hydraulic, thermal and fluvial terrain change.
    RgbaImage hydraulic_erosion_delta;
    RgbaImage thermal_erosion_delta;
    RgbaImage fluvial_terrain_delta;
};

// Hierarchical importance of a precipitation-fed river segment.
enum class RiverClass : std::uint8_t {
    None = 0,
    Stream = 1,     // Strahler order 1-2
    River = 2,      // Strahler order 3-4
    MajorRiver = 3, // Strahler order 5+
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

    // Hydrology diagnostics. Accumulation is precipitation-derived runoff,
    // including temperature-dependent rain and snowmelt, in source-cell
    // equivalents. Watershed IDs are populated by whole-world generation;
    // the bounded point-sample fallback may leave them zero.
    DrainageDirection drainage_direction = DrainageDirection::Sink;
    double watershed_accumulation = 0.0;
    double river_strength = 0.0;
    std::uint32_t watershed_id = 0;
    std::uint8_t stream_order = 0;
    RiverClass river_class = RiverClass::None;
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

    // Samples the analytical point field at one global X/Z coordinate.
    // This lightweight query uses bounded local hydrology; whole-world
    // erosion is applied by generate_rgba_region(), which has the finite
    // routing context needed to keep erosion continuous across chunks.
    // Coordinates outside the finite world are rejected.
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
