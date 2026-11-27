#include <ws/rgba_terrain_core/world_sampler.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <stdexcept>
#include <utility>

namespace ws::rgba_terrain_core {
namespace {

constexpr std::uint8_t mountain_flag = 0x10u;

std::uint64_t mix_bits(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

struct Gradient2D {
    double x;
    double z;
};

// Sixteen evenly spaced unit gradients reduce the directional bias of the
// four-corner value-noise lattice while keeping each sample allocation-free.
constexpr std::array<Gradient2D, 16> gradient_directions{{
    {1.0000000000000000, 0.0000000000000000},
    {0.9238795325112867, 0.3826834323650898},
    {0.7071067811865476, 0.7071067811865475},
    {0.3826834323650898, 0.9238795325112867},
    {0.0000000000000000, 1.0000000000000000},
    {-0.3826834323650898, 0.9238795325112867},
    {-0.7071067811865475, 0.7071067811865476},
    {-0.9238795325112867, 0.3826834323650899},
    {-1.0000000000000000, 0.0000000000000001},
    {-0.9238795325112868, -0.3826834323650897},
    {-0.7071067811865477, -0.7071067811865475},
    {-0.3826834323650903, -0.9238795325112865},
    {-0.0000000000000002, -1.0000000000000000},
    {0.3826834323650900, -0.9238795325112866},
    {0.7071067811865474, -0.7071067811865477},
    {0.9238795325112865, -0.3826834323650904},
}};

std::uint64_t lattice_hash(
    std::int64_t x, std::int64_t z, std::uint64_t seed) {
    return mix_bits(
        seed ^ mix_bits(static_cast<std::uint64_t>(x)) ^
        mix_bits(static_cast<std::uint64_t>(z) + 0x632be59bd9b4e019ull));
}

double fade_quintic(double value) {
    // Perlin's quintic fade has zero first and second derivatives at lattice
    // boundaries, avoiding the visible slope changes of cubic interpolation.
    return value * value * value *
           (value * (value * 6.0 - 15.0) + 10.0);
}

double gradient_noise(double x, double z, std::uint64_t seed) {
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
    const double fx = x - floor_x;
    const double fz = z - floor_z;

    const auto corner_dot = [&](std::int64_t dx, std::int64_t dz,
                                double offset_x, double offset_z) {
        const auto& gradient = gradient_directions[
            static_cast<std::size_t>(
                lattice_hash(ix + dx, iz + dz, seed) &
                (gradient_directions.size() - 1u))];
        return gradient.x * (fx - offset_x) +
               gradient.z * (fz - offset_z);
    };

    const double n00 = corner_dot(0, 0, 0.0, 0.0);
    const double n10 = corner_dot(1, 0, 1.0, 0.0);
    const double n01 = corner_dot(0, 1, 0.0, 1.0);
    const double n11 = corner_dot(1, 1, 1.0, 1.0);

    const double u = fade_quintic(fx);
    const double v = fade_quintic(fz);
    const double north = n00 + (n10 - n00) * u;
    const double south = n01 + (n11 - n01) * u;

    // The scale keeps the useful range close to [-1,1] for the existing
    // terrain-field amplitudes and climate thresholds.
    return (north + (south - north) * v) * 1.4142135623730951;
}

double fbm(double x, double z, std::uint64_t seed, unsigned octaves) {
    double sum = 0.0;
    double amplitude = 0.5;
    double amplitude_sum = 0.0;
    constexpr double lacunarity = 2.03;
    // Rotate each successive octave as well as scaling it. The lattice axes
    // therefore do not remain aligned across the full fractal spectrum.
    constexpr double rotation_cos = 0.9323273456060345;  // cos(0.37 rad)
    constexpr double rotation_sin = 0.3616154319649620;  // sin(0.37 rad)
    for (unsigned octave = 0; octave < octaves; ++octave) {
        sum += gradient_noise(
                   x, z, seed + octave * 0x9e3779b97f4a7c15ull) *
               amplitude;
        amplitude_sum += amplitude;

        const double rotated_x = x * rotation_cos - z * rotation_sin;
        const double rotated_z = x * rotation_sin + z * rotation_cos;
        x = rotated_x * lacunarity;
        z = rotated_z * lacunarity;
        amplitude *= 0.5;
    }
    return amplitude_sum > 0.0 ? sum / amplitude_sum : 0.0;
}

double smooth(double t) {
    return t * t * (3.0 - 2.0 * t);
}

double smoothstep(double edge0, double edge1, double value) {
    if (edge1 <= edge0) {
        return value < edge0 ? 0.0 : 1.0;
    }
    return smooth(std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0));
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

struct RegionalInfluences {
    double weight_sum = 0.0;
    double elevation_sum = 0.0;
    double temperature_sum = 0.0;
    double humidity_sum = 0.0;
    std::array<double, 256> ecosystem_affinity{};
};

RegionalInfluences evaluate_regions(const WorldPlan& plan,
                                    double x, double z) {
    RegionalInfluences result;
    for (const auto& region : plan.regions) {
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
        result.weight_sum += weight;
        result.elevation_sum += region.elevation_bias * weight;
        result.temperature_sum += region.temperature_bias_c * weight;
        result.humidity_sum += region.humidity_bias * weight;
        if (region.ecosystem_affinity > 0.0) {
            result.ecosystem_affinity[region.ecosystem_id] +=
                region.ecosystem_affinity * weight;
        }
    }
    return result;
}

struct TerrainField {
    double normalized_elevation = 0.0;
    double continentalness = 0.0;
    double mountain_strength = 0.0;

    [[nodiscard]] bool is_ocean(double sea_level) const noexcept {
        return normalized_elevation <= sea_level;
    }
};

// This is the shared terrain evaluator used both for the requested coordinate
// and for the deterministic upwind atmospheric fetch. It deliberately has no
// finite-world bounds check: air entering at a world edge still has a stable
// surrounding terrain profile, while actual world samples remain bounds-safe.
TerrainField evaluate_terrain(const WorldPlan& plan, double x, double z,
                              double regional_elevation_bias) {
    const double continent_scale = plan.scales.continent_scale;
    const double province_scale = plan.scales.province_scale;

    // Two independent warp bands bend continent outlines at both
    // super-regional and province scales. The second band prevents broad
    // warping from leaving smooth, repetitive nested blobs.
    const double continental_warp_scale = continent_scale * 0.85;
    const double regional_warp_scale = province_scale * 1.65;
    const double continental_warp_x = fbm(
        x / continental_warp_scale, z / continental_warp_scale,
        plan.seed ^ 0x7F4A7C159E3779B9ull, 3);
    const double continental_warp_z = fbm(
        x / continental_warp_scale + 17.3,
        z / continental_warp_scale - 9.1,
        plan.seed ^ 0x94D049BB133111EBull, 3);
    const double regional_warp_x = fbm(
        x / regional_warp_scale + 31.7,
        z / regional_warp_scale - 12.9,
        plan.seed ^ 0xD1B54A32D192ED03ull, 2);
    const double regional_warp_z = fbm(
        x / regional_warp_scale - 8.3,
        z / regional_warp_scale + 23.1,
        plan.seed ^ 0xABC98388FB8FAC03ull, 2);
    const double warped_x =
        x + continental_warp_x * continent_scale * 0.13 +
        regional_warp_x * province_scale * 0.10;
    const double warped_z =
        z + continental_warp_z * continent_scale * 0.13 +
        regional_warp_z * province_scale * 0.10;

    const double continentalness = fbm(
        warped_x / continent_scale, warped_z / continent_scale,
        plan.seed ^ 0x3141592653589793ull, 5);
    const double province = fbm(
        warped_x / province_scale, warped_z / province_scale,
        plan.seed ^ 0xA341316CC8013EA4ull, 4);
    const double hills = fbm(
        warped_x / (province_scale * 0.21),
        warped_z / (province_scale * 0.21),
        plan.seed ^ 0x2718281828459045ull, 3);
    const double ridge_source = fbm(
        warped_x / (province_scale * 0.13),
        warped_z / (province_scale * 0.13),
        plan.seed ^ 0xAD90777D5A17E3B1ull, 3);
    const double ridge = std::pow(std::max(0.0, 1.0 - std::abs(ridge_source)),
                                  1.45);
    const double mountain_region =
        smoothstep(0.08, 0.62, province) *
        smoothstep(-0.18, 0.48, continentalness);
    const double mountain_strength = clamp01(mountain_region * ridge);

    const double elevation = clamp01(
        plan.sea_level_fraction + continentalness * 0.30 +
        province * 0.06 + hills * 0.025 + mountain_strength * 0.25 +
        regional_elevation_bias);

    return {elevation, continentalness, mountain_strength};
}

bool terrain_is_ocean_at(const WorldPlan& plan, double x, double z) {
    const RegionalInfluences regions = evaluate_regions(plan, x, z);
    const double blend = std::min(1.0, regions.weight_sum);
    const double elevation_bias = regions.weight_sum > 0.0
        ? (regions.elevation_sum / regions.weight_sum) * blend
        : 0.0;
    return evaluate_terrain(plan, x, z, elevation_bias)
        .is_ocean(plan.sea_level_fraction);
}

struct ClimateField {
    double temperature_c = 0.0;
    double humidity = 0.0;
    double precipitation = 0.0;
    double orographic_lift = 0.0;
    double rain_shadow = 0.0;
    double ocean_moisture = 0.0;
};

ClimateField evaluate_climate(
    const WorldPlan& plan, double x, double z,
    const RegionalInfluences& regions, const TerrainField& terrain) {
    const double province_scale = plan.scales.province_scale;
    const double maximum_elevation_m = plan.climate.maximum_elevation_m;
    const double region_blend = std::min(1.0, regions.weight_sum);
    const double above_sea_fraction = clamp01(
        (terrain.normalized_elevation - plan.sea_level_fraction) /
        (1.0 - plan.sea_level_fraction));
    const double elevation_m = above_sea_fraction * maximum_elevation_m;
    const double latitude = plan.depth <= 1
        ? 0.0
        : std::abs((z / static_cast<double>(plan.depth - 1)) * 2.0 - 1.0);
    const double regional_temperature = fbm(
        x / (province_scale * 1.4), z / (province_scale * 1.4),
        plan.seed ^ 0xB5297A4D68E31DA4ull, 3) *
        plan.climate.regional_temperature_variation_c;
    const double sea_level_temperature =
        plan.climate.equator_temperature_c +
        (plan.climate.pole_temperature_c -
         plan.climate.equator_temperature_c) * std::pow(latitude, 0.90) +
        regional_temperature +
        (regions.weight_sum > 0.0
             ? (regions.temperature_sum / regions.weight_sum) * region_blend
             : 0.0);
    ClimateField result;
    result.temperature_c = sea_level_temperature -
        (elevation_m / 1000.0) *
            plan.climate.elevation_lapse_rate_c_per_km;

    constexpr std::array<double, 3> fetch_fractions{0.22, 0.55, 1.15};
    constexpr std::array<double, 3> ocean_weights{0.45, 0.35, 0.20};
    const double wind_length = std::hypot(
        plan.climate.prevailing_wind_x, plan.climate.prevailing_wind_z);
    const double wind_x = plan.climate.prevailing_wind_x / wind_length;
    const double wind_z = plan.climate.prevailing_wind_z / wind_length;
    std::array<TerrainField, fetch_fractions.size()> upwind{};
    double upwind_peak = terrain.normalized_elevation;
    for (std::size_t i = 0; i < fetch_fractions.size(); ++i) {
        const double distance = province_scale * fetch_fractions[i];
        const double upwind_x = x - wind_x * distance;
        const double upwind_z = z - wind_z * distance;
        const auto upwind_regions = evaluate_regions(plan, upwind_x, upwind_z);
        const double upwind_blend = std::min(1.0, upwind_regions.weight_sum);
        const double upwind_bias = upwind_regions.weight_sum > 0.0
            ? (upwind_regions.elevation_sum / upwind_regions.weight_sum) *
                  upwind_blend : 0.0;
        upwind[i] = evaluate_terrain(plan, upwind_x, upwind_z, upwind_bias);
        upwind_peak = std::max(upwind_peak, upwind[i].normalized_elevation);
        if (upwind[i].is_ocean(plan.sea_level_fraction)) {
            result.ocean_moisture += ocean_weights[i];
        }
    }
    const double windward_rise = std::max(
        0.0, terrain.normalized_elevation - upwind.front().normalized_elevation);
    result.orographic_lift = clamp01(
        windward_rise * maximum_elevation_m / 1200.0 +
        terrain.mountain_strength * 0.08);
    result.rain_shadow = clamp01(
        (upwind_peak - terrain.normalized_elevation) *
        maximum_elevation_m / 1800.0);
    const double coast = 1.0 - smoothstep(
        0.015, 0.18,
        std::abs(terrain.normalized_elevation - plan.sea_level_fraction));
    const double moisture_noise = fbm(
        x / (province_scale * 0.82), z / (province_scale * 0.82),
        plan.seed ^ 0x68E31DA4C2B2AE35ull, 3) *
        plan.climate.regional_humidity_variation;
    const double evaporation_factor = std::clamp(
        0.55 + (sea_level_temperature + 10.0) / 80.0, 0.20, 1.10);
    const double ocean_vapor = plan.climate.coastal_humidity_boost *
        (0.35 * coast + 0.65 * result.ocean_moisture) * evaporation_factor;
    const double moisture_supply = clamp01(
        plan.climate.base_humidity + moisture_noise + ocean_vapor -
        result.rain_shadow * plan.climate.rain_shadow_strength +
        (regions.weight_sum > 0.0
             ? (regions.humidity_sum / regions.weight_sum) * region_blend
             : 0.0));
    const double lift_cooling_c = windward_rise * maximum_elevation_m *
        plan.climate.elevation_lapse_rate_c_per_km / 1000.0;
    const double condensation_efficiency = std::clamp(
        0.70 + lift_cooling_c / 28.0, 0.70, 1.15);
    result.precipitation = clamp01(
        moisture_supply * (0.42 + 0.58 * result.orographic_lift) *
        condensation_efficiency + terrain.mountain_strength * 0.025);
    result.humidity = clamp01(moisture_supply - result.precipitation * 0.12);
    if (terrain.is_ocean(plan.sea_level_fraction)) {
        result.humidity = std::max(result.humidity, 0.68);
        result.precipitation = clamp01(result.precipitation + 0.04);
    }
    return result;
}

struct HydrologyField {
    DrainageDirection direction = DrainageDirection::Sink;
    double accumulation = 0.0;
    double river_strength = 0.0;
    double lake_depth_m = 0.0;
    double lake_surface_elevation = 0.0;
    bool river = false;
    bool lake = false;
};

constexpr std::array<int, 8> drainage_dx{0, 1, 1, 1, 0, -1, -1, -1};
constexpr std::array<int, 8> drainage_dz{-1, -1, 0, 1, 1, 1, 0, -1};

// Each query builds the same bounded, absolute-coordinate drainage window.
// Routing is evaluated once per cell, then runoff is accumulated downstream
// in descending elevation order. No world-sized or chunk-local state exists.
constexpr int watershed_radius = 5;
constexpr int watershed_width = watershed_radius * 2 + 1;
constexpr int watershed_cell_count = watershed_width * watershed_width;
constexpr int elevation_radius = watershed_radius + 1;
constexpr int elevation_width = elevation_radius * 2 + 1;
constexpr int elevation_cell_count = elevation_width * elevation_width;

double terrain_at_grid(const WorldPlan& plan, std::int64_t gx,
                       std::int64_t gz) {
    const double scale = plan.hydrology.routing_cell_scale;
    const double x = (static_cast<double>(gx) + 0.5) * scale;
    const double z = (static_cast<double>(gz) + 0.5) * scale;
    const auto regions = evaluate_regions(plan, x, z);
    const double blend = std::min(1.0, regions.weight_sum);
    const double bias = regions.weight_sum > 0.0
        ? (regions.elevation_sum / regions.weight_sum) * blend : 0.0;
    return evaluate_terrain(plan, x, z, bias).normalized_elevation;
}

HydrologyField evaluate_hydrology(const WorldPlan& plan, double x, double z,
                                  double normalized_elevation,
                                  [[maybe_unused]] double precipitation) {
    HydrologyField result;
    const double scale = plan.hydrology.routing_cell_scale;
    const double grid_x = std::floor(x / scale);
    const double grid_z = std::floor(z / scale);

    // Keep room for the fixed window's coordinate offsets before converting
    // floating-point grid coordinates to signed integers.
    constexpr double offset = static_cast<double>(elevation_radius + 1);
    const double min_grid = static_cast<double>(
        std::numeric_limits<std::int64_t>::min()) + offset;
    const double max_grid = static_cast<double>(
        std::numeric_limits<std::int64_t>::max()) - offset;
    if (!std::isfinite(grid_x) || !std::isfinite(grid_z) ||
        grid_x < min_grid || grid_x > max_grid ||
        grid_z < min_grid || grid_z > max_grid) {
        return result;
    }

    const auto gx = static_cast<std::int64_t>(grid_x);
    const auto gz = static_cast<std::int64_t>(grid_z);
    const auto elevation_index = [](int dx, int dz) {
        return static_cast<std::size_t>(
            (dz + elevation_radius) * elevation_width +
            (dx + elevation_radius));
    };
    const auto cell_index = [](int column, int row) {
        return static_cast<std::size_t>(row * watershed_width + column);
    };

    // The one-cell halo supplies all D8 neighbors for the routing window.
    std::array<double, elevation_cell_count> elevations{};
    for (int dz = -elevation_radius; dz <= elevation_radius; ++dz) {
        for (int dx = -elevation_radius; dx <= elevation_radius; ++dx) {
            elevations[elevation_index(dx, dz)] =
                terrain_at_grid(plan, gx + dx, gz + dz);
        }
    }

    std::array<double, watershed_cell_count> cell_elevation{};
    std::array<double, watershed_cell_count> cell_drop{};
    std::array<double, watershed_cell_count> accumulation{};
    std::array<DrainageDirection, watershed_cell_count> directions{};
    std::array<std::size_t, watershed_cell_count> order{};

    const double province_scale = plan.scales.province_scale;
    for (int row = 0; row < watershed_width; ++row) {
        for (int column = 0; column < watershed_width; ++column) {
            const int dx = column - watershed_radius;
            const int dz = row - watershed_radius;
            const std::size_t index = cell_index(column, row);
            const double elevation = elevations[elevation_index(dx, dz)];
            cell_elevation[index] = elevation;

            double best_drop = 0.0;
            DrainageDirection best_direction = DrainageDirection::Sink;
            for (std::size_t direction = 0;
                 direction < drainage_dx.size(); ++direction) {
                const double neighbor = elevations[elevation_index(
                    dx + drainage_dx[direction],
                    dz + drainage_dz[direction])];
                const double distance = (direction % 2 == 1)
                    ? 1.41421356237 : 1.0;
                const double drop = (elevation - neighbor) / distance;
                if (drop > best_drop) {
                    best_drop = drop;
                    best_direction = static_cast<DrainageDirection>(direction);
                }
            }
            directions[index] = best_direction;
            cell_drop[index] = best_drop;

            const std::int64_t source_x = gx + dx;
            const std::int64_t source_z = gz + dz;
            const double world_x = (static_cast<double>(source_x) + 0.5) * scale;
            const double world_z = (static_cast<double>(source_z) + 0.5) * scale;
            const auto source_regions = evaluate_regions(
                plan, world_x, world_z);
            const double source_blend =
                std::min(1.0, source_regions.weight_sum);
            const double source_bias = source_regions.weight_sum > 0.0
                ? (source_regions.elevation_sum /
                   source_regions.weight_sum) * source_blend : 0.0;
            const TerrainField source_terrain = evaluate_terrain(
                plan, world_x, world_z, source_bias);
            const ClimateField source_climate = evaluate_climate(
                plan, world_x, world_z, source_regions, source_terrain);
            accumulation[index] = source_terrain.is_ocean(
                plan.sea_level_fraction) ? 0.0 : source_climate.precipitation;
            order[index] = index;
        }
    }

    // Strictly downhill D8 edges form a directed acyclic graph. Propagating
    // from high cells to low cells produces local flow accumulation without
    // repeatedly tracing the same paths from every source.
    std::sort(order.begin(), order.end(),
        [&](std::size_t lhs, std::size_t rhs) {
            if (cell_elevation[lhs] != cell_elevation[rhs]) {
                return cell_elevation[lhs] > cell_elevation[rhs];
            }
            return lhs < rhs;
        });

    for (const std::size_t index : order) {
        const auto direction = directions[index];
        if (direction == DrainageDirection::Sink) {
            continue;
        }
        const int column = static_cast<int>(index % watershed_width);
        const int row = static_cast<int>(index / watershed_width);
        const auto direction_index = static_cast<std::size_t>(direction);
        const int next_column = column + drainage_dx[direction_index];
        const int next_row = row + drainage_dz[direction_index];
        if (next_column < 0 || next_column >= watershed_width ||
            next_row < 0 || next_row >= watershed_width) {
            continue;
        }
        accumulation[cell_index(next_column, next_row)] += accumulation[index];
    }

    constexpr std::size_t target_index =
        static_cast<std::size_t>(watershed_radius * watershed_width +
                                 watershed_radius);
    result.direction = directions[target_index];
    result.accumulation = accumulation[target_index];

    const double threshold = plan.hydrology.river_accumulation_threshold;
    result.river_strength = clamp01(
        (result.accumulation - threshold) / (threshold * 2.5));

    // Follow the target's globally deterministic D8 edges inside this local
    // window to find its terminal sink. If the route exits the window, lake
    // classification is deferred rather than inventing a local basin.
    std::size_t sink_index = target_index;
    bool found_sink = false;
    for (int step = 0; step < watershed_cell_count; ++step) {
        const auto direction = directions[sink_index];
        if (direction == DrainageDirection::Sink) {
            found_sink = true;
            break;
        }
        const int column = static_cast<int>(sink_index % watershed_width);
        const int row = static_cast<int>(sink_index / watershed_width);
        const auto direction_index = static_cast<std::size_t>(direction);
        const int next_column = column + drainage_dx[direction_index];
        const int next_row = row + drainage_dz[direction_index];
        if (next_column < 0 || next_column >= watershed_width ||
            next_row < 0 || next_row >= watershed_width) {
            break;
        }
        sink_index = cell_index(next_column, next_row);
    }

    bool sink_is_lake = false;
    double spill_elevation = 0.0;
    if (found_sink) {
        // Minimax flood from the sink to the window boundary finds the lowest
        // local escape level: the minimum possible maximum elevation along
        // any 8-connected path from the sink to the edge.
        std::array<double, watershed_cell_count> spill_cost{};
        std::array<bool, watershed_cell_count> visited{};
        spill_cost.fill(std::numeric_limits<double>::infinity());
        spill_cost[sink_index] = cell_elevation[sink_index];
        using SpillNode = std::pair<double, std::size_t>;
        std::priority_queue<SpillNode, std::vector<SpillNode>,
                            std::greater<SpillNode>> frontier;
        frontier.emplace(spill_cost[sink_index], sink_index);

        bool found_spill = false;
        while (!frontier.empty()) {
            const auto [current_cost, current] = frontier.top();
            frontier.pop();
            if (visited[current] || current_cost != spill_cost[current]) {
                continue;
            }
            visited[current] = true;

            const int column = static_cast<int>(current % watershed_width);
            const int row = static_cast<int>(current / watershed_width);
            if (column == 0 || row == 0 ||
                column == watershed_width - 1 ||
                row == watershed_width - 1) {
                spill_elevation = current_cost;
                found_spill = true;
                break;
            }

            for (std::size_t direction = 0;
                 direction < drainage_dx.size(); ++direction) {
                const int next_column = column + drainage_dx[direction];
                const int next_row = row + drainage_dz[direction];
                if (next_column < 0 || next_column >= watershed_width ||
                    next_row < 0 || next_row >= watershed_width) {
                    continue;
                }
                const std::size_t next = cell_index(next_column, next_row);
                if (visited[next]) {
                    continue;
                }
                const double candidate_cost = std::max(
                    current_cost, cell_elevation[next]);
                if (candidate_cost < spill_cost[next]) {
                    spill_cost[next] = candidate_cost;
                    frontier.emplace(candidate_cost, next);
                }
            }
        }

        const double depression_fraction = found_spill
            ? std::max(0.0, spill_elevation - cell_elevation[sink_index])
            : 0.0;
        result.lake_depth_m = depression_fraction *
                              plan.climate.maximum_elevation_m;

        const int sink_column =
            static_cast<int>(sink_index % watershed_width);
        const int sink_row =
            static_cast<int>(sink_index / watershed_width);
        const double sink_world_x =
            (static_cast<double>(gx + sink_column - watershed_radius) + 0.5) *
            scale;
        const double sink_world_z =
            (static_cast<double>(gz + sink_row - watershed_radius) + 0.5) *
            scale;
        const double sink_moisture_noise = fbm(
            sink_world_x / (province_scale * 0.82),
            sink_world_z / (province_scale * 0.82),
            plan.seed ^ 0x68E31DA4C2B2AE35ull, 2);
        const double sink_precipitation = clamp01(
            plan.climate.base_humidity * 0.70 + 0.10 +
            0.20 * (0.5 + 0.5 * sink_moisture_noise));

        sink_is_lake = found_spill &&
            result.lake_depth_m >= plan.hydrology.lake_minimum_depth_m &&
            accumulation[sink_index] >=
                plan.hydrology.lake_minimum_accumulation &&
            sink_precipitation >=
                plan.hydrology.lake_minimum_precipitation;
    }

    const bool target_inside_basin = sink_is_lake &&
        cell_elevation[target_index] <= spill_elevation + 1.0e-9 &&
        normalized_elevation <= spill_elevation + 1.0e-9;
    result.lake = target_inside_basin;
    if (result.lake) {
        // The current RGBA format stores one elevation value per column. Use
        // the shared spill level for all basin columns so the reconstructed
        // lake has a flat surface instead of water following the basin floor.
        result.lake_surface_elevation = spill_elevation;
    }

    const bool local_sink =
        result.direction == DrainageDirection::Sink || cell_drop[target_index] < 1.0e-7;
    result.river = !result.lake && !local_sink &&
                   result.accumulation >= threshold;
    return result;
}


// Evolve a deterministic finite-world elevation field before final D8
// routing. All passes read one immutable elevation snapshot and write to a
// separate buffer, so results do not depend on traversal or chunk order.
void erode_terrain(std::vector<double>& elevation,
                   const std::vector<double>& precipitation,
                   const std::vector<bool>& ocean,
                   std::size_t width, std::size_t height,
                   double sea_level) {
    const std::size_t count = elevation.size();
    if (count == 0 || width == 0 || height == 0 ||
        width > std::numeric_limits<std::size_t>::max() / height ||
        count != width * height) {
        return;
    }
    const auto index = [width](std::size_t x, std::size_t z) {
        return z * width + x;
    };
    const double land_floor = std::min(
        1.0, sea_level + std::min(0.001, (1.0 - sea_level) * 0.25));
    std::vector<double> next_elevation(count);
    std::vector<double> accumulation(count);
    std::vector<double> sediment(count);
    std::vector<double> delta(count);
    std::vector<std::size_t> order(count);
    std::vector<DrainageDirection> direction(count, DrainageDirection::Sink);

    constexpr int hydraulic_iterations = 4;
    constexpr int thermal_iterations = 2;
    constexpr double talus_threshold = 0.014;
    constexpr double thermal_rate = 0.10;
    for (int iteration = 0; iteration < hydraulic_iterations; ++iteration) {
        std::fill(accumulation.begin(), accumulation.end(), 0.0);
        std::fill(sediment.begin(), sediment.end(), 0.0);
        std::fill(direction.begin(), direction.end(), DrainageDirection::Sink);
        for (std::size_t i = 0; i < count; ++i) {
            accumulation[i] = ocean[i] ? 0.0 : precipitation[i];
            order[i] = i;
            if (ocean[i]) continue;
            const std::size_t x = i % width;
            const std::size_t z = i / width;
            double best_slope = 0.0;
            for (std::size_t d = 0; d < drainage_dx.size(); ++d) {
                const auto nx = static_cast<std::int64_t>(x) + drainage_dx[d];
                const auto nz = static_cast<std::int64_t>(z) + drainage_dz[d];
                if (nx < 0 || nz < 0 ||
                    nx >= static_cast<std::int64_t>(width) ||
                    nz >= static_cast<std::int64_t>(height)) continue;
                const std::size_t neighbor = index(
                    static_cast<std::size_t>(nx), static_cast<std::size_t>(nz));
                if (ocean[neighbor]) continue;
                const double distance = (d % 2 == 1) ? 1.41421356237 : 1.0;
                const double slope =
                    (elevation[i] - elevation[neighbor]) / distance;
                if (slope > best_slope) {
                    best_slope = slope;
                    direction[i] = static_cast<DrainageDirection>(d);
                }
            }
        }
        std::sort(order.begin(), order.end(),
            [&](std::size_t lhs, std::size_t rhs) {
                if (elevation[lhs] != elevation[rhs])
                    return elevation[lhs] > elevation[rhs];
                return lhs < rhs;
            });
        for (const std::size_t i : order) {
            const auto d = direction[i];
            if (d == DrainageDirection::Sink) continue;
            const std::size_t di = static_cast<std::size_t>(d);
            const auto nx = static_cast<std::int64_t>(i % width) + drainage_dx[di];
            const auto nz = static_cast<std::int64_t>(i / width) + drainage_dz[di];
            if (nx < 0 || nz < 0 ||
                nx >= static_cast<std::int64_t>(width) ||
                nz >= static_cast<std::int64_t>(height)) continue;
            accumulation[index(static_cast<std::size_t>(nx),
                               static_cast<std::size_t>(nz))] += accumulation[i];
        }

        next_elevation = elevation;
        for (const std::size_t i : order) {
            if (ocean[i]) continue;
            const auto d = direction[i];
            if (d == DrainageDirection::Sink) {
                next_elevation[i] = std::clamp(
                    elevation[i] + sediment[i],
                    land_floor, 1.0);
                continue;
            }
            const std::size_t di = static_cast<std::size_t>(d);
            const auto nx = static_cast<std::int64_t>(i % width) + drainage_dx[di];
            const auto nz = static_cast<std::int64_t>(i / width) + drainage_dz[di];
            if (nx < 0 || nz < 0 ||
                nx >= static_cast<std::int64_t>(width) ||
                nz >= static_cast<std::int64_t>(height)) continue;
            const std::size_t downstream = index(
                static_cast<std::size_t>(nx), static_cast<std::size_t>(nz));
            const double distance = (di % 2 == 1) ? 1.41421356237 : 1.0;
            const double slope = std::max(
                0.0, (elevation[i] - elevation[downstream]) / distance);
            const double stream_power =
                std::pow(std::max(0.0, accumulation[i]), 0.55) *
                std::pow(slope * 100.0, 0.65);
            const double incision = std::min(
                0.0018, 0.000055 * stream_power);
            const double carried_sediment = sediment[i] + incision * 0.70;

            // Transport most sediment downstream; deposit more where the
            // channel flattens, forming small deterministic valley fans.
            const double deposition_fraction =
                0.12 + 0.58 * (1.0 - smoothstep(0.006, 0.035, slope));
            const double deposited = carried_sediment * deposition_fraction;
            next_elevation[i] = std::clamp(
                elevation[i] - incision + deposited,
                land_floor, 1.0);
            sediment[downstream] += carried_sediment - deposited;
        }
        elevation.swap(next_elevation);

        // Thermal relaxation moves material only when a cardinal slope
        // exceeds the talus threshold. Pairwise transfers are accumulated
        // from the same snapshot to avoid directional scan-order artifacts.
        for (int thermal = 0; thermal < thermal_iterations; ++thermal) {
            std::fill(delta.begin(), delta.end(), 0.0);
            for (std::size_t z = 0; z < height; ++z) {
                for (std::size_t x = 0; x < width; ++x) {
                    const std::size_t high = index(x, z);
                    if (ocean[high]) continue;
                    constexpr std::array<std::array<int, 2>, 2> edges{{
                        {{1, 0}}, {{0, 1}}}};
                    for (const auto& edge : edges) {
                        const std::size_t nx = x + 1u == width && edge[0] != 0
                            ? width : x + static_cast<std::size_t>(edge[0]);
                        const std::size_t nz = z + 1u == height && edge[1] != 0
                            ? height : z + static_cast<std::size_t>(edge[1]);
                        if (nx >= width || nz >= height) continue;
                        const std::size_t low = index(nx, nz);
                        if (ocean[low]) continue;
                        const double difference =
                            elevation[high] - elevation[low];
                        const double excess =
                            std::abs(difference) - talus_threshold;
                        if (excess <= 0.0) continue;
                        const double transfer = excess * thermal_rate;
                        if (difference > 0.0) {
                            delta[high] -= transfer;
                            delta[low] += transfer;
                        } else {
                            delta[high] += transfer;
                            delta[low] -= transfer;
                        }
                    }
                }
            }
            next_elevation = elevation;
            for (std::size_t i = 0; i < count; ++i) {
                if (!ocean[i]) {
                    next_elevation[i] = std::clamp(
                        elevation[i] + delta[i], land_floor, 1.0);
                }
            }
            elevation.swap(next_elevation);
        }
    }
}

double sample_elevation_offset(const std::vector<double>& offsets,
                               std::size_t width, std::size_t height,
                               double scale, std::int64_t world_x,
                               std::int64_t world_z) {
    if (offsets.empty() || width == 0 || height == 0) return 0.0;
    const double gx = std::clamp(
        static_cast<double>(world_x) / scale - 0.5, 0.0,
        static_cast<double>(width - 1));
    const double gz = std::clamp(
        static_cast<double>(world_z) / scale - 0.5, 0.0,
        static_cast<double>(height - 1));
    const auto x0 = static_cast<std::size_t>(std::floor(gx));
    const auto z0 = static_cast<std::size_t>(std::floor(gz));
    const auto x1 = std::min(width - 1, x0 + 1u);
    const auto z1 = std::min(height - 1, z0 + 1u);
    const double tx = gx - static_cast<double>(x0);
    const double tz = gz - static_cast<double>(z0);
    const auto at = [&](std::size_t x, std::size_t z) {
        return offsets[z * width + x];
    };
    const double top = at(x0, z0) * (1.0 - tx) + at(x1, z0) * tx;
    const double bottom = at(x0, z1) * (1.0 - tx) + at(x1, z1) * tx;
    return top * (1.0 - tz) + bottom * tz;
}

// Whole-world coarse drainage context. Building this once per RGBA region keeps
// flow accumulation continuous across output chunks and avoids reconstructing
// a 13x13 terrain neighborhood for every output pixel.
struct HydrologyGrid {
    std::size_t width = 0;
    std::size_t height = 0;
    std::vector<HydrologyField> cells;
    std::vector<double> elevation_offsets;
    // Pre-erosion elevation at grid cell centers. Used to determine ocean
    // classification at pixel resolution by bilinear interpolation and
    // comparison with sea_level_fraction, matching the baseline terrain's
    // per-pixel ocean check.
    std::vector<double> original_elevation;
    // Pre-erosion hydrology at grid cell centers (rivers, lakes, etc.).
    // Used for ocean classification to match the baseline terrain's
    // grid-resolution hydrology (which includes river channel cuts).
    std::vector<HydrologyField> original_cells;
    // Pre-erosion ocean classification (original analytical terrain mask).
    // Kept for backwards compatibility and potential future use.
    // Stored as uint8_t (0 or 1) for efficient sampling.
    std::vector<std::uint8_t> original_ocean;
    double maximum_accumulation = 0.0;
    double maximum_lake_depth_m = 0.0;
    double maximum_erosion_offset = 0.0;
    bool valid = false;
};

HydrologyGrid build_hydrology_grid(const WorldPlan& plan) {
    HydrologyGrid grid;
    const double scale = plan.hydrology.routing_cell_scale;
    const auto grid_dimension = [scale](std::int64_t extent) {
        const double cells = std::ceil(static_cast<double>(extent) / scale);
        if (!std::isfinite(cells) || cells < 1.0 ||
            cells > static_cast<double>(
                std::numeric_limits<std::size_t>::max())) {
            return std::size_t{0};
        }
        return static_cast<std::size_t>(cells);
    };
    grid.width = grid_dimension(plan.width);
    grid.height = grid_dimension(plan.depth);
    if (grid.width == 0 || grid.height == 0 ||
        grid.width > std::numeric_limits<std::size_t>::max() / grid.height) {
        return grid;
    }
    const std::size_t count = grid.width * grid.height;
    // Bound temporary memory for unusually large finite worlds. Point sampling
    // retains its deterministic bounded-window fallback in that case.
    constexpr std::size_t maximum_routing_cells = 1'000'000;
    if (count > maximum_routing_cells) return grid;

    std::vector<double> elevation(count);
    std::vector<double> filled(count);
    std::vector<double> accumulation(count);
    std::vector<double> precipitation(count);
    std::vector<bool> ocean(count, false);
    std::vector<std::size_t> parent(count, count);
    std::vector<std::size_t> visit_order(count, count);
    std::vector<DrainageDirection> direction(count, DrainageDirection::Sink);
    const auto index = [&grid](std::size_t x, std::size_t z) {
        return z * grid.width + x;
    };
    for (std::size_t z = 0; z < grid.height; ++z) {
        for (std::size_t x = 0; x < grid.width; ++x) {
            const std::size_t i = index(x, z);
            const double world_x = (static_cast<double>(x) + 0.5) * scale;
            const double world_z = (static_cast<double>(z) + 0.5) * scale;
            const auto regions = evaluate_regions(plan, world_x, world_z);
            const double blend = std::min(1.0, regions.weight_sum);
            const double bias = regions.weight_sum > 0.0
                ? regions.elevation_sum / regions.weight_sum * blend : 0.0;
            const TerrainField terrain = evaluate_terrain(
                plan, world_x, world_z, bias);
            elevation[i] = terrain.normalized_elevation;
            ocean[i] = terrain.is_ocean(plan.sea_level_fraction);
            const ClimateField climate = evaluate_climate(
                plan, world_x, world_z, regions, terrain);
            precipitation[i] = climate.precipitation;
            // Each routing cell contributes its actual climate-derived rainfall.
            // Ocean cells terminate drainage instead of adding river runoff.
            accumulation[i] = ocean[i] ? 0.0 : precipitation[i];
        }
    }

    const std::vector<double> original_elevation = elevation;
    // Store pre-erosion elevation for pixel-level ocean classification.
    // This allows bilinear interpolation of elevation at output pixel resolution
    // and comparison with sea_level_fraction, matching the baseline terrain's
    // per-pixel ocean check.
    grid.original_elevation = original_elevation;
    // Store pre-erosion ocean classification (original analytical terrain mask).
    // Kept for backwards compatibility.
    grid.original_ocean.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        grid.original_ocean[i] = ocean[i] ? 1 : 0;
    }

    // River threshold is used for both pre- and post-erosion hydrology.
    const double threshold = plan.hydrology.river_accumulation_threshold;

    // Compute and store pre-erosion hydrology cells (rivers, lakes, etc.)
    // for ocean classification that matches the baseline terrain's
    // grid-resolution hydrology (which includes river channel cuts).
    // This mirrors the post-erosion computation below but uses pre-erosion data.
    std::vector<DrainageDirection> original_direction(count, DrainageDirection::Sink);
    std::vector<double> original_accumulation = accumulation;
    std::vector<double> original_filled(count);
    std::vector<std::size_t> original_parent(count, count);
    std::vector<std::size_t> original_visit_order(count, count);
    std::size_t original_sequence = 0;
    // We'll reuse the priority-flood approach for consistency
    using FloodNode = std::pair<double, std::size_t>;
    std::priority_queue<FloodNode, std::vector<FloodNode>,
                        std::greater<FloodNode>> original_frontier;
    std::vector<bool> original_visited(count, false);
    const auto original_seed_boundary = [&](std::size_t x, std::size_t z) {
        const std::size_t i = index(x, z);
        if (original_visit_order[i] != count) return;
        original_visit_order[i] = original_sequence++;
        original_filled[i] = original_elevation[i];
        original_frontier.emplace(original_filled[i], i);
    };
    for (std::size_t x = 0; x < grid.width; ++x) {
        original_seed_boundary(x, 0);
        original_seed_boundary(x, grid.height - 1);
    }
    for (std::size_t z = 0; z < grid.height; ++z) {
        original_seed_boundary(0, z);
        original_seed_boundary(grid.width - 1, z);
    }
    // Mark boundary as visited
    for (std::size_t x = 0; x < grid.width; ++x) {
        original_visited[index(x, 0)] = true;
        original_visited[index(x, grid.height - 1)] = true;
    }
    for (std::size_t z = 0; z < grid.height; ++z) {
        original_visited[index(0, z)] = true;
        original_visited[index(grid.width - 1, z)] = true;
    }
    while (!original_frontier.empty()) {
        const auto [level, current] = original_frontier.top();
        original_frontier.pop();
        const std::size_t x = current % grid.width;
        const std::size_t z = current / grid.width;
        for (std::size_t d = 0; d < drainage_dx.size(); ++d) {
            const auto nx_signed = static_cast<std::int64_t>(x) + drainage_dx[d];
            const auto nz_signed = static_cast<std::int64_t>(z) + drainage_dz[d];
            if (nx_signed < 0 || nz_signed < 0 ||
                nx_signed >= static_cast<std::int64_t>(grid.width) ||
                nz_signed >= static_cast<std::int64_t>(grid.height)) continue;
            const std::size_t next = index(
                static_cast<std::size_t>(nx_signed),
                static_cast<std::size_t>(nz_signed));
            if (original_visited[next]) continue;
            original_visited[next] = true;
            original_parent[next] = current;
            original_filled[next] = std::max(original_elevation[next], level);
            original_visit_order[next] = original_sequence++;
            original_frontier.emplace(original_filled[next], next);
        }
    }

    // D8 routing on pre-erosion elevation
    for (std::size_t z = 0; z < grid.height; ++z) {
        for (std::size_t x = 0; x < grid.width; ++x) {
            const std::size_t i = index(x, z);
            if (ocean[i]) {
                original_direction[i] = DrainageDirection::Sink;
                continue;
            }
            double best_drop = 0.0;
            for (std::size_t d = 0; d < drainage_dx.size(); ++d) {
                const auto nx = static_cast<std::int64_t>(x) + drainage_dx[d];
                const auto nz = static_cast<std::int64_t>(z) + drainage_dz[d];
                if (nx < 0 || nz < 0 ||
                    nx >= static_cast<std::int64_t>(grid.width) ||
                    nz >= static_cast<std::int64_t>(grid.height)) continue;
                const std::size_t next = index(
                    static_cast<std::size_t>(nx), static_cast<std::size_t>(nz));
                const double distance = (d % 2 == 1) ? 1.41421356237 : 1.0;
                const double drop = (original_elevation[i] - original_elevation[next]) / distance;
                if (drop > best_drop) {
                    best_drop = drop;
                    original_direction[i] = static_cast<DrainageDirection>(d);
                }
            }
            if (best_drop <= 0.0 && original_parent[i] != count) {
                const auto px = static_cast<std::int64_t>(original_parent[i] % grid.width);
                const auto pz = static_cast<std::int64_t>(original_parent[i] / grid.width);
                const auto dx = px - static_cast<std::int64_t>(x);
                const auto dz = pz - static_cast<std::int64_t>(z);
                for (std::size_t d = 0; d < drainage_dx.size(); ++d) {
                    if (drainage_dx[d] == dx && drainage_dz[d] == dz) {
                        original_direction[i] = static_cast<DrainageDirection>(d);
                        break;
                    }
                }
            }
        }
    }

    // Accumulation on pre-erosion elevation
    std::vector<std::size_t> original_order(count);
    for (std::size_t i = 0; i < count; ++i) original_order[i] = i;
    std::sort(original_order.begin(), original_order.end(), [&](std::size_t a, std::size_t b) {
        if (original_filled[a] != original_filled[b]) return original_filled[a] > original_filled[b];
        return original_visit_order[a] > original_visit_order[b];
    });
    for (const std::size_t i : original_order) {
        const auto d = original_direction[i];
        if (d == DrainageDirection::Sink) continue;
        const std::size_t di = static_cast<std::size_t>(d);
        const auto x = static_cast<std::int64_t>(i % grid.width) + drainage_dx[di];
        const auto z = static_cast<std::int64_t>(i / grid.width) + drainage_dz[di];
        if (x < 0 || z < 0 ||
            x >= static_cast<std::int64_t>(grid.width) ||
            z >= static_cast<std::int64_t>(grid.height)) continue;
        original_accumulation[index(static_cast<std::size_t>(x),
                           static_cast<std::size_t>(z))] += original_accumulation[i];
    }

    // Build pre-erosion hydrology cells
    grid.original_cells.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        HydrologyField& cell = grid.original_cells[i];
        cell.direction = original_direction[i];
        cell.accumulation = original_accumulation[i];
        cell.river_strength = clamp01(
            (original_accumulation[i] - threshold) / (threshold * 2.5));
        cell.lake_depth_m = std::max(0.0, original_filled[i] - original_elevation[i]) *
                            plan.climate.maximum_elevation_m;
        cell.lake = !ocean[i] &&
            cell.lake_depth_m >= plan.hydrology.lake_minimum_depth_m &&
            original_accumulation[i] >= plan.hydrology.lake_minimum_accumulation &&
            precipitation[i] >= plan.hydrology.lake_minimum_precipitation;
        cell.lake_surface_elevation = original_filled[i];
        cell.river = !cell.lake && original_direction[i] != DrainageDirection::Sink &&
                     original_accumulation[i] >= threshold;
    }

    if (plan.hydrology.erosion_enabled) {
        erode_terrain(elevation, precipitation, ocean, grid.width, grid.height,
                      plan.sea_level_fraction);
    }
    grid.elevation_offsets.resize(count, 0.0);
    for (std::size_t i = 0; i < count; ++i) {
        grid.elevation_offsets[i] = elevation[i] - original_elevation[i];
        grid.maximum_erosion_offset = std::max(
            grid.maximum_erosion_offset,
            std::abs(grid.elevation_offsets[i]));
        ocean[i] = elevation[i] <= plan.sea_level_fraction;
        if (ocean[i]) {
            precipitation[i] = 0.0;
        } else if (plan.hydrology.erosion_enabled) {
            const std::size_t x = i % grid.width;
            const std::size_t z = i / grid.width;
            const double world_x = (static_cast<double>(x) + 0.5) * scale;
            const double world_z = (static_cast<double>(z) + 0.5) * scale;
            const auto regions = evaluate_regions(plan, world_x, world_z);
            const double blend = std::min(1.0, regions.weight_sum);
            const double bias = regions.weight_sum > 0.0
                ? regions.elevation_sum / regions.weight_sum * blend : 0.0;
            TerrainField eroded_terrain = evaluate_terrain(
                plan, world_x, world_z, bias);
            eroded_terrain.normalized_elevation = elevation[i];
            precipitation[i] = evaluate_climate(
                plan, world_x, world_z, regions, eroded_terrain).precipitation;
        }
        accumulation[i] = ocean[i] ? 0.0 : precipitation[i];
    }

    using FloodNode = std::pair<double, std::size_t>;
    std::priority_queue<FloodNode, std::vector<FloodNode>,
                        std::greater<FloodNode>> frontier;
    std::vector<bool> visited(count, false);
    std::size_t sequence = 0;
    const auto seed_boundary = [&](std::size_t x, std::size_t z) {
        const std::size_t i = index(x, z);
        if (visited[i]) return;
        visited[i] = true;
        filled[i] = elevation[i];
        visit_order[i] = sequence++;
        frontier.emplace(filled[i], i);
    };
    for (std::size_t x = 0; x < grid.width; ++x) {
        seed_boundary(x, 0);
        seed_boundary(x, grid.height - 1);
    }
    for (std::size_t z = 0; z < grid.height; ++z) {
        seed_boundary(0, z);
        seed_boundary(grid.width - 1, z);
    }

    while (!frontier.empty()) {
        const auto [level, current] = frontier.top();
        frontier.pop();
        const std::size_t x = current % grid.width;
        const std::size_t z = current / grid.width;
        for (std::size_t d = 0; d < drainage_dx.size(); ++d) {
            const auto nx_signed = static_cast<std::int64_t>(x) + drainage_dx[d];
            const auto nz_signed = static_cast<std::int64_t>(z) + drainage_dz[d];
            if (nx_signed < 0 || nz_signed < 0 ||
                nx_signed >= static_cast<std::int64_t>(grid.width) ||
                nz_signed >= static_cast<std::int64_t>(grid.height)) continue;
            const std::size_t next = index(
                static_cast<std::size_t>(nx_signed),
                static_cast<std::size_t>(nz_signed));
            if (visited[next]) continue;
            visited[next] = true;
            parent[next] = current;
            filled[next] = std::max(elevation[next], level);
            visit_order[next] = sequence++;
            frontier.emplace(filled[next], next);
        }
    }

    for (std::size_t z = 0; z < grid.height; ++z) {
        for (std::size_t x = 0; x < grid.width; ++x) {
            const std::size_t i = index(x, z);
            if (ocean[i]) {
                direction[i] = DrainageDirection::Sink;
                continue;
            }
            double best_drop = 0.0;
            for (std::size_t d = 0; d < drainage_dx.size(); ++d) {
                const auto nx = static_cast<std::int64_t>(x) + drainage_dx[d];
                const auto nz = static_cast<std::int64_t>(z) + drainage_dz[d];
                if (nx < 0 || nz < 0 ||
                    nx >= static_cast<std::int64_t>(grid.width) ||
                    nz >= static_cast<std::int64_t>(grid.height)) continue;
                const std::size_t next = index(
                    static_cast<std::size_t>(nx), static_cast<std::size_t>(nz));
                const double distance = (d % 2 == 1) ? 1.41421356237 : 1.0;
                const double drop = (elevation[i] - elevation[next]) / distance;
                if (drop > best_drop) {
                    best_drop = drop;
                    direction[i] = static_cast<DrainageDirection>(d);
                }
            }
            // In flats/depressions, follow the priority-flood parent. Parent
            // links always point to an earlier visited cell, so cannot cycle.
            if (best_drop <= 0.0 && parent[i] != count) {
                const auto px = static_cast<std::int64_t>(parent[i] % grid.width);
                const auto pz = static_cast<std::int64_t>(parent[i] / grid.width);
                const auto dx = px - static_cast<std::int64_t>(x);
                const auto dz = pz - static_cast<std::int64_t>(z);
                for (std::size_t d = 0; d < drainage_dx.size(); ++d) {
                    if (drainage_dx[d] == dx && drainage_dz[d] == dz) {
                        direction[i] = static_cast<DrainageDirection>(d);
                        break;
                    }
                }
            }
        }
    }

    std::vector<std::size_t> order(count);
    for (std::size_t i = 0; i < count; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (filled[a] != filled[b]) return filled[a] > filled[b];
        return visit_order[a] > visit_order[b];
    });
    for (const std::size_t i : order) {
        const auto d = direction[i];
        if (d == DrainageDirection::Sink) continue;
        const std::size_t di = static_cast<std::size_t>(d);
        const auto x = static_cast<std::int64_t>(i % grid.width) + drainage_dx[di];
        const auto z = static_cast<std::int64_t>(i / grid.width) + drainage_dz[di];
        if (x < 0 || z < 0 ||
            x >= static_cast<std::int64_t>(grid.width) ||
            z >= static_cast<std::int64_t>(grid.height)) continue;
        accumulation[index(static_cast<std::size_t>(x),
                           static_cast<std::size_t>(z))] += accumulation[i];
    }

    grid.cells.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        HydrologyField& cell = grid.cells[i];
        cell.direction = direction[i];
        cell.accumulation = accumulation[i];
        grid.maximum_accumulation = std::max(
            grid.maximum_accumulation, accumulation[i]);
        cell.river_strength = clamp01(
            (accumulation[i] - threshold) / (threshold * 2.5));
        cell.lake_depth_m = std::max(0.0, filled[i] - elevation[i]) *
                            plan.climate.maximum_elevation_m;
        cell.lake = !ocean[i] &&
            cell.lake_depth_m >= plan.hydrology.lake_minimum_depth_m &&
            accumulation[i] >= plan.hydrology.lake_minimum_accumulation &&
            precipitation[i] >= plan.hydrology.lake_minimum_precipitation;
        cell.lake_surface_elevation = filled[i];
        grid.maximum_lake_depth_m = std::max(
            grid.maximum_lake_depth_m, cell.lake_depth_m);
        cell.river = !cell.lake && direction[i] != DrainageDirection::Sink &&
                     accumulation[i] >= threshold;
    }
    grid.valid = true;
    return grid;
}

// Compute the base terrain elevation at the exact world coordinate,
// matching the logic in sample_impl for the baseline terrain (no erosion).
// This includes regional influences, climate (for precipitation), lake
// surface elevation, and river channel cuts. Returns true if the final
// elevation <= sea_level_fraction.
//
// The comparison accounts for the uint16 packing precision used in the
// baseline terrain's pixel output (packed_height / 65535.0), which rounds
// to nearest. We simulate the exact pack/unpack cycle to match the test.
bool sample_original_ocean(const WorldPlan& plan,
                           double world_x, double world_z) {
    const double x = world_x;
    const double z = world_z;

    // Evaluate regional influences (same as sample_impl).
    const RegionalInfluences regions = evaluate_regions(plan, x, z);
    const double region_blend = std::min(1.0, regions.weight_sum);
    const double regional_elevation_bias = regions.weight_sum > 0.0
        ? (regions.elevation_sum / regions.weight_sum) * region_blend
        : 0.0;

    // Evaluate base terrain (same as sample_impl, without erosion offset).
    const TerrainField terrain = evaluate_terrain(
        plan, x, z, regional_elevation_bias);
    const double normalized_elevation = terrain.normalized_elevation;

    // Evaluate climate for precipitation (same as sample_impl).
    const ClimateField climate = evaluate_climate(
        plan, x, z, regions, terrain);

    // Evaluate hydrology at this coordinate for river/lake info
    // (same as sample_impl with include_local_hydrology = true).
    const HydrologyField hydrology = evaluate_hydrology(
        plan, x, z, normalized_elevation, climate.precipitation);

    // Compute surface elevation (lake surface or terrain elevation).
    const double surface_elevation = hydrology.lake
        ? hydrology.lake_surface_elevation
        : normalized_elevation;

    // Apply river channel cut (same as pixel packing code).
    const double channel_cut = hydrology.river
        ? plan.hydrology.river_channel_depth_fraction * hydrology.river_strength
        : 0.0;

    const double final_elevation = std::max(0.0, surface_elevation - channel_cut);

    // Simulate the exact pack/unpack cycle used in the baseline terrain:
    // packed_height = lround(final_elevation * 65535.0), clamped to [0, 65535]
    // unpacked = packed_height / 65535.0
    // Ocean if unpacked <= sea_level_fraction
    const long packed_height = std::lround(final_elevation * 65535.0);
    const std::uint16_t clamped = static_cast<std::uint16_t>(
        std::clamp(packed_height, 0l, 65535l));
    const double unpacked_elevation = static_cast<double>(clamped) / 65535.0;
    return unpacked_elevation <= plan.sea_level_fraction;
}

const HydrologyField& hydrology_at(
    const HydrologyGrid& grid, const WorldPlan& plan,
    std::int64_t world_x, std::int64_t world_z) {
    const double scale = plan.hydrology.routing_cell_scale;
    const auto x = std::min(
        grid.width - 1, static_cast<std::size_t>(world_x / scale));
    const auto z = std::min(
        grid.height - 1, static_cast<std::size_t>(world_z / scale));
    return grid.cells[z * grid.width + x];
}

std::uint8_t diagnostic_gray(double value) {
    return static_cast<std::uint8_t>(
        std::clamp(std::lround(clamp01(value) * 255.0), 0l, 255l));
}

Rgba8 diagnostic_pixel(double value) {
    const std::uint8_t channel = diagnostic_gray(value);
    return {channel, channel, channel, 255};
}

void initialize_diagnostic_image(RgbaImage& image,
                                 std::size_t width, std::size_t depth) {
    image.width = width;
    image.height = depth;
    image.pixels.resize(width * depth);
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
    return sample_impl(world_x, world_z, true);
}

WorldSample WorldSampler::sample_impl(std::int64_t world_x,
                                      std::int64_t world_z,
                                      bool include_local_hydrology,
                                      double terrain_elevation_offset) const {
    if (world_x < 0 || world_x >= plan_.width ||
        world_z < 0 || world_z >= plan_.depth) {
        throw std::out_of_range("world sample coordinate is outside the finite world");
    }

    const double x = static_cast<double>(world_x);
    const double z = static_cast<double>(world_z);
    const RegionalInfluences regions = evaluate_regions(plan_, x, z);
    const double region_blend = std::min(1.0, regions.weight_sum);
    const double regional_elevation_bias = regions.weight_sum > 0.0
        ? (regions.elevation_sum / regions.weight_sum) * region_blend
        : 0.0;
    TerrainField terrain = evaluate_terrain(
        plan_, x, z, regional_elevation_bias);
    terrain.normalized_elevation = clamp01(
        terrain.normalized_elevation + terrain_elevation_offset);

    const ClimateField climate = evaluate_climate(
        plan_, x, z, regions, terrain);
    const double elevation_m = clamp01(
        (terrain.normalized_elevation - plan_.sea_level_fraction) /
        (1.0 - plan_.sea_level_fraction)) *
        plan_.climate.maximum_elevation_m;
    const double temperature = climate.temperature_c;
    const double humidity = climate.humidity;
    const double precipitation = climate.precipitation;
    const bool is_ocean = terrain.is_ocean(plan_.sea_level_fraction);

    WorldSample result;
    result.normalized_elevation = terrain.normalized_elevation;
    result.elevation_m = elevation_m;
    result.continentalness = terrain.continentalness;
    result.temperature_c = temperature;
    result.humidity = humidity;
    result.precipitation = precipitation;
    result.orographic_lift = climate.orographic_lift;
    result.rain_shadow = climate.rain_shadow;
    result.ocean_moisture = climate.ocean_moisture;
    result.is_ocean = is_ocean;
    result.is_mountain = !is_ocean && terrain.mountain_strength > 0.34;
    const HydrologyField hydrology = include_local_hydrology
        ? evaluate_hydrology(plan_, x, z, terrain.normalized_elevation, precipitation)
        : HydrologyField{};
    result.drainage_direction = hydrology.direction;
    result.watershed_accumulation = hydrology.accumulation;
    result.river_strength = hydrology.river_strength;
    result.lake_depth_m = hydrology.lake_depth_m;
    result.lake_surface_elevation = hydrology.lake_surface_elevation;
    result.is_river = hydrology.river;
    result.is_lake = hydrology.lake;
    result.habitat_variation = clamp01(
        0.5 + 0.5 * fbm(
            x / plan_.scales.habitat_scale,
            z / plan_.scales.habitat_scale,
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
            temperature, humidity, terrain.normalized_elevation,
            ecosystem.temperature_optimum_c,
            ecosystem.temperature_tolerance_c,
            ecosystem.humidity_optimum, ecosystem.humidity_tolerance,
            ecosystem.elevation_optimum, ecosystem.elevation_tolerance);
        const double spatial = fbm(
            x / plan_.scales.ecosystem_scale,
            z / plan_.scales.ecosystem_scale,
            plan_.seed ^ (0x9E3779B97F4A7C15ull *
                          (static_cast<std::uint64_t>(ecosystem.id) + 1u)), 2);
        const double affinity = std::min(
            1.0, regions.ecosystem_affinity[ecosystem.id]);
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
            temperature, humidity, terrain.normalized_elevation,
            biome.temperature_optimum_c, biome.temperature_tolerance_c,
            biome.humidity_optimum, biome.humidity_tolerance,
            biome.elevation_optimum, biome.elevation_tolerance);
        const std::uint64_t biome_seed =
            plan_.seed ^ (0xD6E8FEB86659FD93ull *
                          (static_cast<std::uint64_t>(biome.id) + 1u));
        const double patch_noise = fbm(
            x / plan_.scales.sub_biome_scale,
            z / plan_.scales.sub_biome_scale,
            biome_seed, 2);
        const double habitat_noise = fbm(
            x / plan_.scales.habitat_scale,
            z / plan_.scales.habitat_scale,
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
    if (result.is_river) {
        result.feature_flags |= 0x20u;
    }
    if (result.is_lake) {
        result.feature_flags |= 0x40u;
    }
    result.biome_transition = transition_from_scores(
        best_biome, second_biome);
    return result;
}

RgbaImage WorldSampler::generate_rgba_region(
    std::int64_t origin_x, std::int64_t origin_z,
    std::size_t width, std::size_t depth,
    const TerrainSettings& settings,
    WorldClimateDiagnosticImages* climate_diagnostics,
    WorldHydrologyDiagnosticImages* hydrology_diagnostics) const {
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

    // Build a hydrology grid with erosion_enabled = false to exactly match
    // the baseline terrain's hydrology for ocean classification.
    // This ensures the erosion_delta diagnostic has zero values at ocean pixels.
    WorldPlan baseline_plan = plan_;
    baseline_plan.hydrology.erosion_enabled = false;
    const HydrologyGrid baseline_hydrology = build_hydrology_grid(baseline_plan);

    // Build the eroded terrain's hydrology grid (with erosion enabled)
    const HydrologyGrid hydrology_grid = build_hydrology_grid(plan_);

    if (hydrology_diagnostics != nullptr) {
        initialize_diagnostic_image(
            hydrology_diagnostics->flow_accumulation, width, depth);
        initialize_diagnostic_image(
            hydrology_diagnostics->river_strength, width, depth);
        initialize_diagnostic_image(
            hydrology_diagnostics->drainage_direction, width, depth);
        initialize_diagnostic_image(
            hydrology_diagnostics->lake_depth, width, depth);
        initialize_diagnostic_image(
            hydrology_diagnostics->lake_basins, width, depth);
        initialize_diagnostic_image(
            hydrology_diagnostics->erosion_delta, width, depth);
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

    double regional_temperature_min = 0.0;
    double regional_temperature_max = 0.0;
    for (const auto& region : plan_.regions) {
        regional_temperature_min = std::min(
            regional_temperature_min, region.temperature_bias_c);
        regional_temperature_max = std::max(
            regional_temperature_max, region.temperature_bias_c);
    }
    double temperature_min = std::min(
        plan_.climate.equator_temperature_c,
        plan_.climate.pole_temperature_c) -
        plan_.climate.regional_temperature_variation_c +
        regional_temperature_min -
        plan_.climate.maximum_elevation_m *
            plan_.climate.elevation_lapse_rate_c_per_km / 1000.0;
    double temperature_max = std::max(
        plan_.climate.equator_temperature_c,
        plan_.climate.pole_temperature_c) +
        plan_.climate.regional_temperature_variation_c +
        regional_temperature_max;
    if (temperature_max <= temperature_min) {
        temperature_max = temperature_min + 1.0;
    }

    if (climate_diagnostics != nullptr) {
        initialize_diagnostic_image(climate_diagnostics->temperature, width, depth);
        initialize_diagnostic_image(climate_diagnostics->humidity, width, depth);
        initialize_diagnostic_image(climate_diagnostics->precipitation, width, depth);
        initialize_diagnostic_image(climate_diagnostics->orographic_lift, width, depth);
        initialize_diagnostic_image(climate_diagnostics->rain_shadow, width, depth);
        initialize_diagnostic_image(climate_diagnostics->ocean_moisture, width, depth);
    }

    for (std::size_t z = 0; z < depth; ++z) {
        for (std::size_t x = 0; x < width; ++x) {
            const auto world_x = origin_x + static_cast<std::int64_t>(x);
            const auto world_z = origin_z + static_cast<std::int64_t>(z);

            // Erosion is simulated only on land routing cells. Do not let
            // bilinear interpolation blend neighboring land offsets into
            // ocean pixels. Use the baseline terrain's hydrology grid (built
            // with erosion_enabled = false) for river/lake classification,
            // and compute the exact per-pixel base terrain elevation to match
            // the baseline terrain's per-pixel ocean check.
            const bool base_terrain_is_ocean =
                baseline_hydrology.valid &&
                [&]() {
                    const double scale = plan_.hydrology.routing_cell_scale;
                    const auto gx = std::min(
                        baseline_hydrology.width - 1,
                        static_cast<std::size_t>(world_x / scale));
                    const auto gz = std::min(
                        baseline_hydrology.height - 1,
                        static_cast<std::size_t>(world_z / scale));
                    const auto& hydro = baseline_hydrology.cells[gz * baseline_hydrology.width + gx];

                    // Compute base terrain elevation at exact world coordinate (same as sample_impl
                    // with erosion_offset = 0). This matches the baseline terrain's per-pixel
                    // elevation computation which uses evaluate_terrain at the exact coordinate.
                    const double x = static_cast<double>(world_x);
                    const double z = static_cast<double>(world_z);
                    const RegionalInfluences regions = evaluate_regions(plan_, x, z);
                    const double region_blend = std::min(1.0, regions.weight_sum);
                    const double regional_elevation_bias = regions.weight_sum > 0.0
                        ? (regions.elevation_sum / regions.weight_sum) * region_blend
                        : 0.0;
                    const TerrainField terrain = evaluate_terrain(
                        plan_, x, z, regional_elevation_bias);
                    const double normalized_elevation = terrain.normalized_elevation;

                    // Compute surface elevation (lake surface or terrain elevation)
                    // using river/lake info from baseline hydrology grid (grid resolution)
                    const double surface_elevation = hydro.lake
                        ? hydro.lake_surface_elevation
                        : normalized_elevation;

                    // Apply river channel cut (same as pixel packing code)
                    const double channel_cut = hydro.river
                        ? plan_.hydrology.river_channel_depth_fraction * hydro.river_strength
                        : 0.0;

                    const double final_elevation = std::max(0.0, surface_elevation - channel_cut);

                    // Simulate the exact pack/unpack cycle used in the baseline terrain:
                    // packed_height = lround(final_elevation * 65535.0), clamped to [0, 65535]
                    // unpacked = packed_height / 65535.0
                    // Ocean if unpacked <= sea_level_fraction
                    const long packed_height = std::lround(final_elevation * 65535.0);
                    const std::uint16_t clamped = static_cast<std::uint16_t>(
                        std::clamp(packed_height, 0l, 65535l));
                    const double unpacked_elevation = static_cast<double>(clamped) / 65535.0;
                    return unpacked_elevation <= plan_.sea_level_fraction;
                }();

            const double erosion_offset =
                hydrology_grid.valid && !base_terrain_is_ocean
                ? sample_elevation_offset(
                    hydrology_grid.elevation_offsets,
                    hydrology_grid.width, hydrology_grid.height,
                    plan_.hydrology.routing_cell_scale, world_x, world_z)
                : 0.0;

            WorldSample sample_at = sample_impl(
                world_x, world_z, !hydrology_grid.valid, erosion_offset);
            if (hydrology_grid.valid && !sample_at.is_ocean) {
                const auto& hydro = hydrology_at(
                    hydrology_grid, plan_,
                    origin_x + static_cast<std::int64_t>(x),
                    origin_z + static_cast<std::int64_t>(z));
                sample_at.drainage_direction = hydro.direction;
                sample_at.watershed_accumulation = hydro.accumulation;
                sample_at.river_strength = hydro.river_strength;
                sample_at.lake_depth_m = hydro.lake_depth_m;
                sample_at.lake_surface_elevation = hydro.lake_surface_elevation;
                sample_at.is_river = hydro.river;
                sample_at.is_lake = hydro.lake;
                sample_at.feature_flags = static_cast<std::uint8_t>(
                    sample_at.feature_flags & static_cast<std::uint8_t>(~0x60u));
                if (hydro.river) sample_at.feature_flags |= 0x20u;
                if (hydro.lake) sample_at.feature_flags |= 0x40u;
            }
            if (climate_diagnostics != nullptr) {
                const std::size_t index = z * width + x;
                climate_diagnostics->temperature.pixels[index] = diagnostic_pixel(
                    (sample_at.temperature_c - temperature_min) /
                    (temperature_max - temperature_min));
                climate_diagnostics->humidity.pixels[index] =
                    diagnostic_pixel(sample_at.humidity);
                climate_diagnostics->precipitation.pixels[index] =
                    diagnostic_pixel(sample_at.precipitation);
                climate_diagnostics->orographic_lift.pixels[index] =
                    diagnostic_pixel(sample_at.orographic_lift);
                climate_diagnostics->rain_shadow.pixels[index] =
                    diagnostic_pixel(sample_at.rain_shadow);
                climate_diagnostics->ocean_moisture.pixels[index] =
                    diagnostic_pixel(sample_at.ocean_moisture);
            }
            if (hydrology_diagnostics != nullptr) {
                const std::size_t index = z * width + x;
                HydrologyField hydro;
                if (hydrology_grid.valid && !sample_at.is_ocean) {
                    hydro = hydrology_at(
                        hydrology_grid, plan_,
                        origin_x + static_cast<std::int64_t>(x),
                        origin_z + static_cast<std::int64_t>(z));
                } else if (!hydrology_grid.valid) {
                    hydro.direction = sample_at.drainage_direction;
                    hydro.accumulation = sample_at.watershed_accumulation;
                    hydro.river_strength = sample_at.river_strength;
                    hydro.lake_depth_m = sample_at.lake_depth_m;
                    hydro.river = sample_at.is_river;
                    hydro.lake = sample_at.is_lake;
                }

                const double accumulation_normalized =
                    hydrology_grid.valid &&
                    hydrology_grid.maximum_accumulation > 0.0
                    ? std::log1p(hydro.accumulation) /
                      std::log1p(hydrology_grid.maximum_accumulation)
                    : clamp01(hydro.accumulation /
                        std::max(1.0, plan_.hydrology.river_accumulation_threshold * 8.0));
                hydrology_diagnostics->flow_accumulation.pixels[index] =
                    diagnostic_pixel(accumulation_normalized);
                hydrology_diagnostics->river_strength.pixels[index] =
                    diagnostic_pixel(hydro.river_strength);

                Rgba8 direction_pixel{0, 0, 0, 255};
                constexpr std::array<Rgba8, 8> direction_colors{{
                    {40, 110, 255, 255}, {40, 210, 255, 255},
                    {40, 230, 120, 255}, {180, 240, 50, 255},
                    {255, 190, 40, 255}, {255, 110, 40, 255},
                    {230, 50, 80, 255}, {180, 60, 230, 255},
                }};
                const auto direction_value =
                    static_cast<std::uint8_t>(hydro.direction);
                if (direction_value < direction_colors.size()) {
                    direction_pixel = direction_colors[direction_value];
                }
                hydrology_diagnostics->drainage_direction.pixels[index] =
                    direction_pixel;

                const double lake_depth_normalized =
                    hydrology_grid.valid &&
                    hydrology_grid.maximum_lake_depth_m > 0.0
                    ? hydro.lake_depth_m / hydrology_grid.maximum_lake_depth_m
                    : clamp01(hydro.lake_depth_m /
                        std::max(1.0, plan_.hydrology.lake_minimum_depth_m * 4.0));
                hydrology_diagnostics->lake_depth.pixels[index] =
                    diagnostic_pixel(lake_depth_normalized);
                hydrology_diagnostics->lake_basins.pixels[index] =
                    hydro.lake ? Rgba8{40, 120, 255, 255}
                               : Rgba8{0, 0, 0, 255};

                const double erosion_intensity =
                    hydrology_grid.valid &&
                    hydrology_grid.maximum_erosion_offset > 0.0
                    ? clamp01(std::abs(erosion_offset) /
                              hydrology_grid.maximum_erosion_offset)
                    : 0.0;
                const std::uint8_t erosion_channel =
                    diagnostic_gray(erosion_intensity);
                hydrology_diagnostics->erosion_delta.pixels[index] =
                    erosion_offset < 0.0
                        ? Rgba8{erosion_channel, 0, 0, 255}
                        : (erosion_offset > 0.0
                            ? Rgba8{0, 0, erosion_channel, 255}
                            : Rgba8{0, 0, 0, 255});
            }

            const double surface_elevation = sample_at.is_lake
                ? sample_at.lake_surface_elevation
                : sample_at.normalized_elevation;
            const double channel_cut = sample_at.is_river
                ? plan_.hydrology.river_channel_depth_fraction *
                      sample_at.river_strength
                : 0.0;
            const auto packed_height = static_cast<std::uint16_t>(
                std::clamp(std::lround(
                    std::max(0.0, surface_elevation - channel_cut) *
                    65535.0), 0l, 65535l));

            // v2: full biome ID in A; B low nibble material, high
            // nibble forest/mountain/river/lake feature bits.
            const std::uint8_t feature_nibble = static_cast<std::uint8_t>(
                (sample_at.feature_flags & 0x78u) >> 3u);
            const std::uint8_t material_features = static_cast<std::uint8_t>(
                (sample_at.material_id & 0x0Fu) | (feature_nibble << 4u));
            Rgba8& pixel = image.pixels[z * width + x];
            pixel.r = static_cast<std::uint8_t>(packed_height >> 8u);
            pixel.g = static_cast<std::uint8_t>(packed_height & 0xffu);
            pixel.b = material_features;
            pixel.a = sample_at.biome_id;
        }
    }

    image.validate();
    if (climate_diagnostics != nullptr) {
        climate_diagnostics->temperature.validate();
        climate_diagnostics->humidity.validate();
        climate_diagnostics->precipitation.validate();
        climate_diagnostics->orographic_lift.validate();
        climate_diagnostics->rain_shadow.validate();
        climate_diagnostics->ocean_moisture.validate();
    }
    if (hydrology_diagnostics != nullptr) {
        hydrology_diagnostics->flow_accumulation.validate();
        hydrology_diagnostics->river_strength.validate();
        hydrology_diagnostics->drainage_direction.validate();
        hydrology_diagnostics->lake_depth.validate();
        hydrology_diagnostics->lake_basins.validate();
        hydrology_diagnostics->erosion_delta.validate();
    }
    return image;
}

}  // namespace ws::rgba_terrain_core
