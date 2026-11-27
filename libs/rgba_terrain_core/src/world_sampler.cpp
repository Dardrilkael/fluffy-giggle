#include <ws/rgba_terrain_core/world_sampler.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ws::rgba_terrain_core {
namespace {

constexpr std::uint8_t mountain_flag = 0x10u;
constexpr std::uint8_t water_flag = 0x80u;

std::uint64_t mix_bits(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

double lattice(std::int64_t x, std::int64_t z, std::uint64_t seed) {
    const std::uint64_t hash = mix_bits(
        seed ^ mix_bits(static_cast<std::uint64_t>(x)) ^
        mix_bits(static_cast<std::uint64_t>(z) + 0x632be59bd9b4e019ull));
    const double unit = static_cast<double>(hash >> 11u) *
                        (1.0 / 9007199254740992.0);
    return unit * 2.0 - 1.0;
}

double smooth(double value) {
    return value * value * (3.0 - 2.0 * value);
}

double smoothstep(double edge0, double edge1, double value) {
    if (edge1 <= edge0) {
        return value < edge0 ? 0.0 : 1.0;
    }
    return smooth(std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0));
}

double value_noise(double x, double z, std::uint64_t seed) {
    const double floor_x = std::floor(x);
    const double floor_z = std::floor(z);
    constexpr double safe_lattice_limit = 9.0e18;
    // Refuse unrepresentable lattice coordinates rather than invoking an
    // out-of-range floating-point-to-integer conversion at extreme world sizes.
    if (!std::isfinite(floor_x) || !std::isfinite(floor_z) ||
        floor_x < -safe_lattice_limit || floor_x > safe_lattice_limit ||
        floor_z < -safe_lattice_limit || floor_z > safe_lattice_limit) {
        return 0.0;
    }

    const auto ix = static_cast<std::int64_t>(floor_x);
    const auto iz = static_cast<std::int64_t>(floor_z);
    const double fx = smooth(x - floor_x);
    const double fz = smooth(z - floor_z);

    const double a = lattice(ix, iz, seed);
    const double b = lattice(ix + 1, iz, seed);
    const double c = lattice(ix, iz + 1, seed);
    const double d = lattice(ix + 1, iz + 1, seed);
    const double north = a + (b - a) * fx;
    const double south = c + (d - c) * fx;
    return north + (south - north) * fz;
}

double fbm(double x, double z, std::uint64_t seed, unsigned octaves) {
    double sum = 0.0;
    double amplitude = 0.5;
    double amplitude_sum = 0.0;
    for (unsigned octave = 0; octave < octaves; ++octave) {
        sum += value_noise(x, z, seed + octave * 0x9e3779b97f4a7c15ull) *
               amplitude;
        amplitude_sum += amplitude;
        x *= 2.03;
        z *= 2.03;
        amplitude *= 0.5;
    }
    return amplitude_sum > 0.0 ? sum / amplitude_sum : 0.0;
}

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

double climate_distance(double temperature, double humidity,
                        double elevation, double temperature_optimum,
                        double temperature_tolerance, double humidity_optimum,
                        double humidity_tolerance, double elevation_optimum,
                        double elevation_tolerance) {
    const double dt = (temperature - temperature_optimum) / temperature_tolerance;
    const double dh = (humidity - humidity_optimum) / humidity_tolerance;
    const double de = (elevation - elevation_optimum) / elevation_tolerance;
    return dt * dt + dh * dh + de * de;
}

struct ScoredId {
    std::uint8_t id = 0;
    double score = -std::numeric_limits<double>::infinity();
};

void insert_top_two(ScoredId candidate, ScoredId& best, ScoredId& second) {
    if (candidate.score > best.score) {
        second = best;
        best = candidate;
    } else if (candidate.score > second.score) {
        second = candidate;
    }
}

// A high transition value means the two strongest candidates are close.
double transition_from_scores(const ScoredId& best, const ScoredId& second) {
    if (!std::isfinite(second.score)) {
        return 0.0;
    }
    return clamp01(1.0 - (best.score - second.score) / 2.5);
}

void validate_terrain_settings(const TerrainSettings& settings) {
    if (!std::isfinite(settings.min_height) ||
        !std::isfinite(settings.max_height) ||
        settings.min_height < 0.0f ||
        settings.max_height <= settings.min_height ||
        settings.volume_height == 0 ||
        settings.max_height >= static_cast<float>(settings.volume_height) ||
        settings.sea_level >= settings.volume_height) {
        throw std::invalid_argument("RGBA region terrain settings are invalid");
    }
}

}  // namespace

WorldSampler::WorldSampler(WorldPlan plan) : plan_(std::move(plan)) {
    plan_.validate();
}

WorldSample WorldSampler::sample(std::int64_t world_x,
                                 std::int64_t world_z) const {
    if (world_x < 0 || world_x >= plan_.width ||
        world_z < 0 || world_z >= plan_.depth) {
        throw std::out_of_range("world sample coordinate is outside the finite world");
    }

    const double x = static_cast<double>(world_x);
    const double z = static_cast<double>(world_z);
    const double continent_scale = plan_.scales.continent_scale;
    const double province_scale = plan_.scales.province_scale;

    // Low-frequency warping creates irregular continental and provincial
    // outlines while keeping the field tied to global world coordinates.
    const double warp_x = fbm(x / (continent_scale * 0.72),
                              z / (continent_scale * 0.72),
                              plan_.seed ^ 0x7F4A7C159E3779B9ull, 3);
    const double warp_z = fbm(x / (continent_scale * 0.72) + 17.3,
                              z / (continent_scale * 0.72) - 9.1,
                              plan_.seed ^ 0x94D049BB133111EBull, 3);
    const double warped_x = x + warp_x * continent_scale * 0.075;
    const double warped_z = z + warp_z * continent_scale * 0.075;

    const double continentalness = fbm(
        warped_x / continent_scale, warped_z / continent_scale,
        plan_.seed ^ 0x3141592653589793ull, 5);
    const double province = fbm(
        warped_x / province_scale, warped_z / province_scale,
        plan_.seed ^ 0xA341316CC8013EA4ull, 4);
    const double hills = fbm(
        warped_x / (province_scale * 0.21),
        warped_z / (province_scale * 0.21),
        plan_.seed ^ 0x2718281828459045ull, 3);
    const double ridge_source = fbm(
        warped_x / (province_scale * 0.13),
        warped_z / (province_scale * 0.13),
        plan_.seed ^ 0xAD90777D5A17E3B1ull, 3);
    const double ridge = std::pow(std::max(0.0, 1.0 - std::abs(ridge_source)),
                                  1.45);
    const double mountain_region =
        smoothstep(0.08, 0.62, province) *
        smoothstep(-0.18, 0.48, continentalness);
    const double mountain_strength = clamp01(mountain_region * ridge);

    double elevation = clamp01(
        plan_.sea_level_fraction + continentalness * 0.30 +
        province * 0.06 + hills * 0.025 + mountain_strength * 0.25);

    // Compact AI-authored regions apply soft elliptical influences. Overlaps
    // blend instead of producing hard borders or discontinuities.
    double region_weight_sum = 0.0;
    double region_elevation_sum = 0.0;
    double region_temperature_sum = 0.0;
    double region_humidity_sum = 0.0;
    std::array<double, 256> ecosystem_affinity{};
    for (const auto& region : plan_.regions) {
        const double dx = x - region.center_x;
        const double dz = z - region.center_z;
        const double cosine = std::cos(region.rotation_radians);
        const double sine = std::sin(region.rotation_radians);
        const double local_x = dx * cosine + dz * sine;
        const double local_z = -dx * sine + dz * cosine;
        const double nx = local_x / region.radius_x;
        const double nz = local_z / region.radius_z;
        const double distance = std::sqrt(nx * nx + nz * nz);
        if (distance >= 1.0) {
            continue;
        }

        const double weight = 1.0 - smoothstep(0.68, 1.0, distance);
        region_weight_sum += weight;
        region_elevation_sum += region.elevation_bias * weight;
        region_temperature_sum += region.temperature_bias_c * weight;
        region_humidity_sum += region.humidity_bias * weight;
        if (region.ecosystem_affinity > 0.0) {
            ecosystem_affinity[region.ecosystem_id] +=
                region.ecosystem_affinity * weight;
        }
    }

    if (region_weight_sum > 0.0) {
        const double blend = std::min(1.0, region_weight_sum);
        elevation = clamp01(
            elevation + (region_elevation_sum / region_weight_sum) * blend);
    }

    const double maximum_elevation_m = plan_.climate.maximum_elevation_m;
    const double above_sea_fraction = clamp01(
        (elevation - plan_.sea_level_fraction) /
        (1.0 - plan_.sea_level_fraction));
    const double elevation_m = above_sea_fraction * maximum_elevation_m;
    const double latitude = plan_.depth <= 1
        ? 0.0
        : std::abs((z / static_cast<double>(plan_.depth - 1)) * 2.0 - 1.0);
    const double regional_temperature = fbm(
        warped_x / (province_scale * 1.4),
        warped_z / (province_scale * 1.4),
        plan_.seed ^ 0xB5297A4D68E31DA4ull, 3) *
        plan_.climate.regional_temperature_variation_c;
    double temperature =
        plan_.climate.equator_temperature_c +
        (plan_.climate.pole_temperature_c -
         plan_.climate.equator_temperature_c) * std::pow(latitude, 0.90) -
        (elevation_m / 1000.0) *
            plan_.climate.elevation_lapse_rate_c_per_km +
        regional_temperature;

    const double coast = 1.0 - smoothstep(
        0.015, 0.18, std::abs(elevation - plan_.sea_level_fraction));
    const double moisture_noise = fbm(
        warped_x / (province_scale * 0.82),
        warped_z / (province_scale * 0.82),
        plan_.seed ^ 0x68E31DA4C2B2AE35ull, 3) *
        plan_.climate.regional_humidity_variation;

    const double wind_length = std::hypot(
        plan_.climate.prevailing_wind_x,
        plan_.climate.prevailing_wind_z);
    const double wind_x = plan_.climate.prevailing_wind_x / wind_length;
    const double wind_z = plan_.climate.prevailing_wind_z / wind_length;
    const double upwind_continentalness = fbm(
        (warped_x - wind_x * province_scale) / continent_scale,
        (warped_z - wind_z * province_scale) / continent_scale,
        plan_.seed ^ 0x3141592653589793ull, 5);
    const double windward_lift = clamp01(
        (continentalness - upwind_continentalness) * 1.8 +
        mountain_strength * 0.25);
    const double rain_shadow = clamp01(
        (upwind_continentalness - continentalness) * 1.8);

    double humidity = clamp01(
        plan_.climate.base_humidity +
        coast * plan_.climate.coastal_humidity_boost +
        moisture_noise -
        rain_shadow * plan_.climate.rain_shadow_strength);
    double precipitation = clamp01(
        humidity * (0.55 + windward_lift * 0.45) +
        mountain_strength * 0.06);

    if (region_weight_sum > 0.0) {
        const double blend = std::min(1.0, region_weight_sum);
        temperature += (region_temperature_sum / region_weight_sum) * blend;
        humidity = clamp01(
            humidity + (region_humidity_sum / region_weight_sum) * blend);
        precipitation = clamp01(
            precipitation + (region_humidity_sum / region_weight_sum) *
                                blend * 0.65);
    }

    WorldSample result;
    result.normalized_elevation = elevation;
    result.elevation_m = elevation_m;
    result.continentalness = continentalness;
    result.temperature_c = temperature;
    result.humidity = humidity;
    result.precipitation = precipitation;
    result.is_ocean = elevation <= plan_.sea_level_fraction;
    result.is_mountain = !result.is_ocean && mountain_strength > 0.34;
    result.habitat_variation = clamp01(
        0.5 + 0.5 * fbm(
            warped_x / plan_.scales.habitat_scale,
            warped_z / plan_.scales.habitat_scale,
            plan_.seed ^ 0xC0FFEE11E7037ED1ull, 3));

    if (result.is_ocean) {
        result.ecosystem_id = 0;
        result.biome_id = 0;
        result.material_id = 4;
        return result;
    }

    ScoredId best_ecosystem;
    ScoredId second_ecosystem;
    for (const auto& ecosystem : plan_.ecosystems) {
        if (ecosystem.id == 0) {
            continue;
        }
        const double distance = climate_distance(
            temperature, humidity, elevation,
            ecosystem.temperature_optimum_c,
            ecosystem.temperature_tolerance_c,
            ecosystem.humidity_optimum, ecosystem.humidity_tolerance,
            ecosystem.elevation_optimum, ecosystem.elevation_tolerance);
        const double spatial = fbm(
            warped_x / plan_.scales.ecosystem_scale,
            warped_z / plan_.scales.ecosystem_scale,
            plan_.seed ^ (0x9E3779B97F4A7C15ull *
                          (static_cast<std::uint64_t>(ecosystem.id) + 1u)), 2);
        const double affinity = std::min(
            1.0, ecosystem_affinity[ecosystem.id]);
        const double score = -distance + std::log(ecosystem.weight) +
            spatial * plan_.scales.ecosystem_spatial_bias + affinity * 2.0;
        insert_top_two({ecosystem.id, score},
                       best_ecosystem, second_ecosystem);
    }
    result.ecosystem_id = best_ecosystem.id;
    result.ecosystem_transition = transition_from_scores(
        best_ecosystem, second_ecosystem);

    ScoredId best_biome;
    ScoredId second_biome;
    for (const auto& biome : plan_.biomes) {
        if (biome.id == 0 || biome.ecosystem_id != result.ecosystem_id) {
            continue;
        }
        const double distance = climate_distance(
            temperature, humidity, elevation,
            biome.temperature_optimum_c, biome.temperature_tolerance_c,
            biome.humidity_optimum, biome.humidity_tolerance,
            biome.elevation_optimum, biome.elevation_tolerance);
        const std::uint64_t biome_seed =
            plan_.seed ^ (0xD6E8FEB86659FD93ull *
                          (static_cast<std::uint64_t>(biome.id) + 1u));
        const double patch_noise = fbm(
            warped_x / plan_.scales.sub_biome_scale,
            warped_z / plan_.scales.sub_biome_scale,
            biome_seed, 2);
        const double habitat_noise = fbm(
            warped_x / plan_.scales.habitat_scale,
            warped_z / plan_.scales.habitat_scale,
            biome_seed ^ 0xA5A3564E27F8862Bull, 2);
        const double score = -distance + std::log(biome.weight) +
            patch_noise * plan_.scales.sub_biome_spatial_bias +
            habitat_noise * 0.035;
        insert_top_two({biome.id, score}, best_biome, second_biome);
    }

    if (!std::isfinite(best_biome.score)) {
        throw std::logic_error("selected ecosystem has no matching biome");
    }
    const BiomeDefinition* selected_biome = nullptr;
    for (const auto& biome : plan_.biomes) {
        if (biome.id == best_biome.id) {
            selected_biome = &biome;
            break;
        }
    }
    if (selected_biome == nullptr) {
        throw std::logic_error("selected biome definition disappeared");
    }

    result.biome_id = selected_biome->id;
    result.material_id = selected_biome->material_id;
    result.feature_flags = selected_biome->feature_flags;
    if (result.is_mountain) {
        result.feature_flags |= mountain_flag;
    }
    result.biome_transition = transition_from_scores(
        best_biome, second_biome);
    return result;
}

RgbaImage WorldSampler::generate_rgba_region(
    std::int64_t origin_x, std::int64_t origin_z,
    std::size_t width, std::size_t depth,
    const TerrainSettings& settings) const {
    validate_terrain_settings(settings);
    if (origin_x < 0 || origin_z < 0 || width == 0 || depth == 0 ||
        width > std::numeric_limits<std::size_t>::max() / depth ||
        width > static_cast<std::size_t>(
                    std::numeric_limits<std::int64_t>::max()) ||
        depth > static_cast<std::size_t>(
                    std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("RGBA region dimensions or origin are invalid");
    }
    const auto signed_width = static_cast<std::int64_t>(width);
    const auto signed_depth = static_cast<std::int64_t>(depth);
    if (origin_x > plan_.width - signed_width ||
        origin_z > plan_.depth - signed_depth) {
        throw std::out_of_range("RGBA region lies outside the finite world");
    }

    const double height_span =
        static_cast<double>(settings.max_height) - settings.min_height;
    const double encoded_sea_level =
        static_cast<double>(settings.min_height) +
        plan_.sea_level_fraction * height_span;
    if (std::abs(encoded_sea_level -
                 static_cast<double>(settings.sea_level)) > 1.0) {
        throw std::invalid_argument(
            "TerrainSettings sea_level does not match the world-plan sea level");
    }

    RgbaImage image;
    image.width = width;
    image.height = depth;
    image.pixels.resize(width * depth);

    for (std::size_t z = 0; z < depth; ++z) {
        for (std::size_t x = 0; x < width; ++x) {
            const WorldSample sample_at = sample(
                origin_x + static_cast<std::int64_t>(x),
                origin_z + static_cast<std::int64_t>(z));
            const auto packed_height = static_cast<std::uint16_t>(
                std::clamp(std::lround(
                    sample_at.normalized_elevation * 65535.0), 0l, 65535l));

            std::uint8_t flags = static_cast<std::uint8_t>(
                sample_at.biome_id & 0x07u);
            flags |= static_cast<std::uint8_t>(
                sample_at.feature_flags & 0x78u);
            if (sample_at.is_ocean) {
                flags |= water_flag;
            }

            Rgba8& pixel = image.pixels[z * width + x];
            pixel.r = static_cast<std::uint8_t>(packed_height >> 8u);
            pixel.g = static_cast<std::uint8_t>(packed_height & 0xffu);
            pixel.b = sample_at.material_id;
            pixel.a = flags;
        }
    }

    image.validate();
    return image;
}

}  // namespace ws::rgba_terrain_core
