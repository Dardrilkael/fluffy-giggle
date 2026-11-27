#pragma once

#include <cstdint>
#include <vector>

namespace ws::rgba_terrain_core {

// Absolute world-unit scales. They do not change when the finite world grows.
struct WorldScales {
    double continent_scale = 300000.0;
    double province_scale = 70000.0;
    double ecosystem_scale = 38000.0;
    double sub_biome_scale = 8500.0;
    double habitat_scale = 1500.0;

    // Small deterministic spatial biases break up climate-only classifications.
    double ecosystem_spatial_bias = 0.18;
    double sub_biome_spatial_bias = 0.28;
};

struct WorldClimateSettings {
    double equator_temperature_c = 29.0;
    double pole_temperature_c = -18.0;
    double elevation_lapse_rate_c_per_km = 6.5;
    double maximum_elevation_m = 7000.0;

    double base_humidity = 0.46;
    double coastal_humidity_boost = 0.28;
    double regional_temperature_variation_c = 3.5;
    double regional_humidity_variation = 0.16;
    double rain_shadow_strength = 0.20;

    // Direction from which the prevailing wind arrives.
    double prevailing_wind_x = 1.0;
    double prevailing_wind_z = 0.0;
};

struct WorldHydrologySettings {
    // The local D8 routing grid is evaluated at this absolute world-unit
    // spacing. It is independent of world dimensions and chunk boundaries.
    double routing_cell_scale = 1200.0;

    // Accumulation is precipitation-weighted contributing source cells in a
    // bounded routing neighborhood, not a global cell-count raster.
    double river_accumulation_threshold = 2.6;
    double river_channel_depth_fraction = 0.018;

    double lake_minimum_depth_m = 40.0;
    double lake_minimum_accumulation = 1.2;
    double lake_minimum_precipitation = 0.18;
};

struct EcosystemDefinition {
    std::uint8_t id = 0;
    double temperature_optimum_c = 15.0;
    double temperature_tolerance_c = 20.0;
    double humidity_optimum = 0.5;
    double humidity_tolerance = 0.5;
    double elevation_optimum = 0.5;
    double elevation_tolerance = 0.5;
    double weight = 1.0;
};

struct BiomeDefinition {
    // The current RGBA alpha packing reserves three bits for biome IDs (0..7).
    std::uint8_t id = 0;
    std::uint8_t ecosystem_id = 0;
    double temperature_optimum_c = 15.0;
    double temperature_tolerance_c = 20.0;
    double humidity_optimum = 0.5;
    double humidity_tolerance = 0.5;
    double elevation_optimum = 0.5;
    double elevation_tolerance = 0.5;
    double weight = 1.0;
    std::uint8_t material_id = 1;

    // Feature bits use the packed-alpha convention: forest=0x08,
    // mountain=0x10, river=0x20, lake=0x40. The water bit (0x80)
    // is generated separately for ocean and inland water columns.
    std::uint8_t feature_flags = 0;
};

// A compact AI-authored regional influence. Elliptical regions can describe
// mountain provinces, wet basins, warm pockets, or an ecosystem preference.
struct WorldRegion {
    std::uint32_t id = 0;
    double center_x = 0.0;
    double center_z = 0.0;
    double radius_x = 1.0;
    double radius_z = 1.0;
    double rotation_radians = 0.0;

    // Additive influence on normalized elevation, temperature and humidity.
    double elevation_bias = 0.0;
    double temperature_bias_c = 0.0;
    double humidity_bias = 0.0;

    // Zero means no ecosystem preference. Otherwise this nudges the sampler
    // toward ecosystem_id inside the region without forcing a hard border.
    std::uint8_t ecosystem_id = 0;
    double ecosystem_affinity = 0.0;
};

struct WorldPlan {
    static constexpr std::uint32_t current_version = 1;

    std::uint32_t version = current_version;
    std::int64_t width = 1;
    std::int64_t depth = 1;
    std::uint64_t seed = 0;

    // Normalized [0,1] elevation threshold for ocean columns. Keep this
    // consistent with TerrainSettings::sea_level when exporting RGBA maps.
    double sea_level_fraction = 12.0 / 56.0;

    WorldScales scales;
    WorldClimateSettings climate;
    WorldHydrologySettings hydrology;
    std::vector<EcosystemDefinition> ecosystems;
    std::vector<BiomeDefinition> biomes;
    std::vector<WorldRegion> regions;

    // Throws std::invalid_argument when the plan cannot be sampled safely.
    void validate() const;
};

// A compact, useful starting plan. AI-authored plans can replace or extend
// its definitions and regional influences without changing the sampler API.
[[nodiscard]] WorldPlan make_default_world_plan(
    std::int64_t width, std::int64_t depth, std::uint64_t seed);

}  // namespace ws::rgba_terrain_core
