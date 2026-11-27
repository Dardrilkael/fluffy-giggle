#include <ws/rgba_terrain_core/world_plan.hpp>

#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace ws::rgba_terrain_core {
namespace {

void require_finite(double value, const char* name) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument(std::string(name) + " must be finite");
    }
}

void require_positive(double value, const char* name) {
    require_finite(value, name);
    if (value <= 0.0) {
        throw std::invalid_argument(std::string(name) + " must be positive");
    }
}

void validate_climate_match(double temperature, double temperature_tolerance,
                            double humidity, double humidity_tolerance,
                            double elevation, double elevation_tolerance,
                            double weight, const char* label) {
    require_finite(temperature, label);
    require_positive(temperature_tolerance, label);
    require_finite(humidity, label);
    require_positive(humidity_tolerance, label);
    require_finite(elevation, label);
    require_positive(elevation_tolerance, label);
    require_positive(weight, label);

    if (humidity < 0.0 || humidity > 1.0 ||
        elevation < 0.0 || elevation > 1.0) {
        throw std::invalid_argument(std::string(label) +
                                    " humidity/elevation optimum must be in [0,1]");
    }
}

}  // namespace

void WorldPlan::validate() const {
    if (version != current_version) {
        throw std::invalid_argument("unsupported world-plan version");
    }
    if (width <= 0 || depth <= 0) {
        throw std::invalid_argument("world dimensions must be positive");
    }
    require_finite(sea_level_fraction, "sea_level_fraction");
    if (sea_level_fraction <= 0.0 || sea_level_fraction >= 1.0) {
        throw std::invalid_argument("sea_level_fraction must be inside (0,1)");
    }

    require_positive(scales.continent_scale, "continent_scale");
    require_positive(scales.province_scale, "province_scale");
    require_positive(scales.ecosystem_scale, "ecosystem_scale");
    require_positive(scales.sub_biome_scale, "sub_biome_scale");
    require_positive(scales.habitat_scale, "habitat_scale");
    require_finite(scales.ecosystem_spatial_bias, "ecosystem_spatial_bias");
    require_finite(scales.sub_biome_spatial_bias, "sub_biome_spatial_bias");
    if (scales.ecosystem_spatial_bias < 0.0 ||
        scales.sub_biome_spatial_bias < 0.0) {
        throw std::invalid_argument("spatial bias strengths cannot be negative");
    }

    require_finite(climate.equator_temperature_c, "equator_temperature_c");
    require_finite(climate.pole_temperature_c, "pole_temperature_c");
    require_finite(climate.elevation_lapse_rate_c_per_km,
                   "elevation_lapse_rate_c_per_km");
    require_positive(climate.maximum_elevation_m, "maximum_elevation_m");
    require_finite(climate.base_humidity, "base_humidity");
    require_finite(climate.coastal_humidity_boost, "coastal_humidity_boost");
    require_finite(climate.regional_temperature_variation_c,
                   "regional_temperature_variation_c");
    require_finite(climate.regional_humidity_variation,
                   "regional_humidity_variation");
    require_finite(climate.rain_shadow_strength, "rain_shadow_strength");
    require_finite(climate.prevailing_wind_x, "prevailing_wind_x");
    require_finite(climate.prevailing_wind_z, "prevailing_wind_z");
    if (climate.base_humidity < 0.0 || climate.base_humidity > 1.0 ||
        climate.coastal_humidity_boost < 0.0 ||
        climate.coastal_humidity_boost > 1.0 ||
        climate.elevation_lapse_rate_c_per_km < 0.0 ||
        climate.regional_temperature_variation_c < 0.0 ||
        climate.regional_humidity_variation < 0.0 ||
        climate.rain_shadow_strength < 0.0 ||
        climate.rain_shadow_strength > 1.0 ||
        (climate.prevailing_wind_x == 0.0 &&
         climate.prevailing_wind_z == 0.0)) {
        throw std::invalid_argument("world climate settings are outside valid ranges");
    }

    require_positive(hydrology.routing_cell_scale, "hydrology.routing_cell_scale");
    require_positive(hydrology.river_accumulation_threshold,
                     "hydrology.river_accumulation_threshold");
    require_finite(hydrology.river_channel_depth_fraction,
                   "hydrology.river_channel_depth_fraction");
    require_positive(hydrology.lake_minimum_depth_m,
                     "hydrology.lake_minimum_depth_m");
    require_finite(hydrology.lake_minimum_accumulation,
                   "hydrology.lake_minimum_accumulation");
    require_finite(hydrology.lake_minimum_precipitation,
                   "hydrology.lake_minimum_precipitation");
    if (hydrology.river_channel_depth_fraction > 0.25 ||
        hydrology.lake_minimum_accumulation < 0.0 ||
        hydrology.lake_minimum_precipitation < 0.0 ||
        hydrology.lake_minimum_precipitation > 1.0) {
        throw std::invalid_argument("world hydrology settings are outside valid ranges");
    }

    std::array<bool, 256> ecosystem_ids{};
    bool has_land_ecosystem = false;
    for (const auto& ecosystem : ecosystems) {
        if (ecosystem_ids[ecosystem.id]) {
            throw std::invalid_argument("duplicate ecosystem ID");
        }
        ecosystem_ids[ecosystem.id] = true;
        if (ecosystem.id != 0) {
            has_land_ecosystem = true;
        }
        validate_climate_match(
            ecosystem.temperature_optimum_c,
            ecosystem.temperature_tolerance_c,
            ecosystem.humidity_optimum, ecosystem.humidity_tolerance,
            ecosystem.elevation_optimum, ecosystem.elevation_tolerance,
            ecosystem.weight, "ecosystem definition");
    }
    if (!has_land_ecosystem) {
        throw std::invalid_argument("world plan needs at least one land ecosystem");
    }

    std::array<bool, 256> biome_ids{};
    std::array<bool, 256> ecosystem_has_biome{};
    for (const auto& biome : biomes) {
        if (biome_ids[biome.id]) {
            throw std::invalid_argument("duplicate biome ID");
        }
        biome_ids[biome.id] = true;
        if (!ecosystem_ids[biome.ecosystem_id]) {
            throw std::invalid_argument("biome references an unknown ecosystem");
        }
        if ((biome.feature_flags & 0x87u) != 0) {
            throw std::invalid_argument(
                "biome feature_flags may only use bits 3 through 6");
        }
        if (biome.material_id > 15) {
            throw std::invalid_argument(
                "v2 terrain material IDs must fit the four-bit material field (0..15)");
        }
        validate_climate_match(
            biome.temperature_optimum_c, biome.temperature_tolerance_c,
            biome.humidity_optimum, biome.humidity_tolerance,
            biome.elevation_optimum, biome.elevation_tolerance,
            biome.weight, "biome definition");
        ecosystem_has_biome[biome.ecosystem_id] = true;
    }
    for (const auto& ecosystem : ecosystems) {
        if (ecosystem.id != 0 && !ecosystem_has_biome[ecosystem.id]) {
            throw std::invalid_argument(
                "every land ecosystem needs at least one biome definition");
        }
    }

    std::unordered_set<std::uint32_t> region_ids;
    for (const auto& region : regions) {
        if (!region_ids.insert(region.id).second) {
            throw std::invalid_argument("duplicate world-region ID");
        }
        require_finite(region.center_x, "region.center_x");
        require_finite(region.center_z, "region.center_z");
        require_positive(region.radius_x, "region.radius_x");
        require_positive(region.radius_z, "region.radius_z");
        require_finite(region.rotation_radians, "region.rotation_radians");
        require_finite(region.elevation_bias, "region.elevation_bias");
        require_finite(region.temperature_bias_c, "region.temperature_bias_c");
        require_finite(region.humidity_bias, "region.humidity_bias");
        require_finite(region.ecosystem_affinity, "region.ecosystem_affinity");
        if (region.ecosystem_affinity < 0.0 ||
            region.ecosystem_affinity > 1.0 ||
            (region.ecosystem_affinity > 0.0 &&
             !ecosystem_ids[region.ecosystem_id])) {
            throw std::invalid_argument("world-region ecosystem affinity is invalid");
        }
    }
}

WorldPlan make_default_world_plan(std::int64_t width, std::int64_t depth,
                                  std::uint64_t seed) {
    WorldPlan plan;
    plan.width = width;
    plan.depth = depth;
    plan.seed = seed;

    // Ecosystem zero and biome zero are reserved for ocean columns.
    plan.ecosystems = {
        {0, 15.0, 30.0, 0.85, 0.8, 0.12, 0.8, 1.0},
        {1, 26.0, 13.0, 0.82, 0.34, 0.52, 0.42, 1.0},
        {2, 11.0, 15.0, 0.64, 0.34, 0.48, 0.45, 1.0},
        {3, 28.0, 16.0, 0.18, 0.30, 0.44, 0.48, 1.0},
        {4, -2.0, 14.0, 0.55, 0.38, 0.50, 0.48, 1.0},
        {5, -12.0, 12.0, 0.34, 0.35, 0.38, 0.45, 1.0},
        {6, 15.0, 16.0, 0.38, 0.28, 0.47, 0.42, 1.0},
        {7, 27.0, 14.0, 0.32, 0.27, 0.44, 0.42, 1.0},
        {8, 21.0, 13.0, 0.92, 0.18, 0.22, 0.30, 1.0},
        {9, -2.0, 18.0, 0.50, 0.34, 0.86, 0.18, 1.0},
        {10, 19.0, 15.0, 0.38, 0.30, 0.49, 0.42, 1.0},
    };
    constexpr std::uint8_t forest = 0x08u;
    plan.biomes = {
        {0, 0, 15.0, 30.0, 0.85, 0.8, 0.12, 0.8, 1.0, 4, 0},
        {1, 1, 26.0, 12.0, 0.90, 0.22, 0.52, 0.42, 1.0, 1, forest},
        {2, 1, 25.0, 13.0, 0.68, 0.28, 0.50, 0.42, 1.0, 1, forest},
        {3, 2, 13.0, 14.0, 0.72, 0.28, 0.47, 0.42, 1.0, 1, forest},
        {4, 6, 15.0, 16.0, 0.40, 0.24, 0.46, 0.42, 1.0, 1, 0},
        {5, 3, 29.0, 15.0, 0.11, 0.22, 0.48, 0.42, 1.0, 4, 0},
        {6, 4, 0.0, 14.0, 0.60, 0.28, 0.48, 0.42, 1.0, 15, forest},
        {7, 5, -12.0, 12.0, 0.32, 0.27, 0.50, 0.42, 1.0, 8, 0},
        {8, 7, 27.0, 14.0, 0.31, 0.23, 0.45, 0.42, 1.0, 1, 0},
        {9, 9, -2.0, 17.0, 0.52, 0.28, 0.84, 0.17, 1.0, 8, 0},
        {10, 9, -4.0, 17.0, 0.44, 0.30, 0.94, 0.12, 0.9, 3, 0},
        {11, 8, 21.0, 13.0, 0.94, 0.18, 0.22, 0.28, 1.0, 11, 0},
        {12, 1, 26.0, 12.0, 0.96, 0.16, 0.18, 0.24, 1.0, 11, forest},
        {13, 10, 19.0, 14.0, 0.38, 0.25, 0.50, 0.42, 1.0, 1, forest},
        {14, 6, 14.0, 16.0, 0.30, 0.23, 0.47, 0.42, 1.0, 6, 0},
        {15, 5, -5.0, 14.0, 0.28, 0.24, 0.52, 0.42, 1.0, 6, 0},
        {16, 3, 27.0, 15.0, 0.13, 0.22, 0.57, 0.38, 1.0, 13, 0},
        {17, 9, -8.0, 14.0, 0.55, 0.30, 0.92, 0.10, 1.0, 8, 0},
        {18, 3, 21.0, 16.0, 0.25, 0.24, 0.82, 0.18, 1.0, 10, 0},
        {19, 2, 12.0, 14.0, 0.94, 0.17, 0.60, 0.34, 1.0, 12, forest},
        {20, 8, 20.0, 14.0, 0.90, 0.18, 0.13, 0.20, 1.0, 11, 0},
    };

    plan.validate();
    return plan;
}

}  // namespace ws::rgba_terrain_core
