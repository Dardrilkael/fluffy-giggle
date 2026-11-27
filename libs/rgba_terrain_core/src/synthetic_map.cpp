#include <ws/rgba_terrain_core/rgba_terrain.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ws::rgba_terrain_core {
namespace {

std::uint32_t mix_bits(std::uint32_t value) {
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

float lattice(std::int32_t x, std::int32_t z, std::uint32_t seed) {
    std::uint32_t h = seed;
    h ^= mix_bits(static_cast<std::uint32_t>(x) + 0x9e3779b9u);
    h = mix_bits(h ^ (mix_bits(static_cast<std::uint32_t>(z)) + 0x85ebca6bu));
    return static_cast<float>(h & 0x00ffffffu) / 8388607.5f - 1.0f;
}

float smooth(float t) {
    return t * t * (3.0f - 2.0f * t);
}

float smoothstep(float edge0, float edge1, float value) {
    if (edge1 <= edge0) {
        return value < edge0 ? 0.0f : 1.0f;
    }
    return smooth(std::clamp((value - edge0) / (edge1 - edge0),
                             0.0f, 1.0f));
}

float value_noise(float x, float z, std::uint32_t seed) {
    const auto ix = static_cast<std::int32_t>(std::floor(x));
    const auto iz = static_cast<std::int32_t>(std::floor(z));
    const float fx = smooth(x - static_cast<float>(ix));
    const float fz = smooth(z - static_cast<float>(iz));
    const float a = lattice(ix, iz, seed);
    const float b = lattice(ix + 1, iz, seed);
    const float c = lattice(ix, iz + 1, seed);
    const float d = lattice(ix + 1, iz + 1, seed);
    const float north = a + (b - a) * fx;
    const float south = c + (d - c) * fx;
    return north + (south - north) * fz;
}

float fbm(float x, float z, std::uint32_t seed, int octaves) {
    float sum = 0.0f;
    float amplitude = 0.5f;
    float total = 0.0f;
    for (int octave = 0; octave < octaves; ++octave) {
        sum += value_noise(x, z, seed + static_cast<std::uint32_t>(octave) *
                                      0x9e3779b9u) * amplitude;
        total += amplitude;
        x *= 2.03f;
        z *= 2.03f;
        amplitude *= 0.5f;
    }
    return sum / total;
}

float remap01(float value) {
    return std::clamp(value * 0.5f + 0.5f, 0.0f, 1.0f);
}

}  // namespace

RgbaImage generate_synthetic_map(std::size_t width, std::size_t depth,
                                 std::uint32_t seed,
                                 const TerrainSettings& settings) {
    if (width == 0 || depth == 0 ||
        width > std::numeric_limits<std::size_t>::max() / depth) {
        throw std::invalid_argument("synthetic map dimensions are invalid");
    }
    if (!std::isfinite(settings.min_height) ||
        !std::isfinite(settings.max_height) ||
        settings.min_height < 0.0f ||
        settings.max_height < settings.min_height ||
        settings.volume_height == 0 ||
        settings.max_height >= static_cast<float>(settings.volume_height) ||
        settings.sea_level >= settings.volume_height) {
        throw std::invalid_argument("synthetic map terrain settings are invalid");
    }

    RgbaImage image;
    image.width = width;
    image.height = depth;
    image.pixels.resize(width * depth);

    const float scale = static_cast<float>(std::max(width, depth));
    const float world_scale = std::max(scale, 1.0f);
    const float sea = static_cast<float>(settings.sea_level);
    const float height_span = settings.max_height - settings.min_height;

    for (std::size_t z = 0; z < depth; ++z) {
        for (std::size_t x = 0; x < width; ++x) {
            const float nx = static_cast<float>(x) / world_scale;
            const float nz = static_cast<float>(z) / world_scale;

            // Low-frequency domain warping breaks up the smooth, blobby
            // contours produced by sampling the continental field directly.
            const float warp_x = fbm(nx * 1.7f, nz * 1.7f,
                                    seed ^ 0x7F4A7C15u, 3);
            const float warp_z = fbm(nx * 1.7f + 19.3f, nz * 1.7f - 7.1f,
                                    seed ^ 0x94D049BBu, 3);
            const float warped_x = nx + warp_x * 0.075f;
            const float warped_z = nz + warp_z * 0.075f;

            // Continentalness controls broad landmasses and ocean basins.
            // A soft shoreline transition avoids a binary island mask.
            const float continental = fbm(warped_x * 2.0f,
                                           warped_z * 2.0f,
                                           seed ^ 0x31415926u, 5);
            const float landmass = smoothstep(-0.28f, 0.34f, continental);
            const float hills = fbm(warped_x * 10.0f, warped_z * 10.0f,
                                    seed ^ 0x27182818u, 4);

            // Mountain ranges use a broad regional envelope plus elongated
            // ridged noise. The unequal axis scales create connected chains
            // rather than isolated, evenly distributed peaks.
            const float mountain_region = fbm(
                warped_x * 2.8f, warped_z * 2.8f,
                seed ^ 0xA341316Cu, 4);
            const float range_source = fbm(
                warped_x * 3.2f, warped_z * 8.5f,
                seed ^ 0xC8013EA4u, 4);
            const float ridge_detail = fbm(
                warped_x * 9.0f, warped_z * 12.0f,
                seed ^ 0xAD90777Du, 3);
            const float primary_ridge = std::pow(
                std::max(0.0f, 1.0f - std::abs(range_source)), 1.45f);
            const float secondary_ridge = std::pow(
                std::max(0.0f, 1.0f - std::abs(ridge_detail)), 2.0f);
            const float ridge = std::clamp(
                primary_ridge * 0.78f + secondary_ridge * 0.22f,
                0.0f, 1.0f);
            const float mountain = smoothstep(
                0.02f, 0.55f, mountain_region) *
                smoothstep(0.12f, 0.58f, continental);

            const float broad = smoothstep(0.0f, 1.0f, landmass);
            const float hill = remap01(hills);
            const float foothills = mountain * (0.10f + ridge * 0.18f);
            const float terrain = std::clamp(
                0.035f + broad * 0.47f + hill * 0.105f +
                    foothills + mountain * ridge * 0.30f,
                0.0f, 1.0f);
            float world_height = settings.min_height +
                                 terrain * height_span;

            const float temperature = fbm(warped_x * 3.7f, warped_z * 3.7f,
                                           seed ^ 0xB5297A4Du, 3);
            const float moisture = fbm(warped_x * 5.1f, warped_z * 5.1f,
                                        seed ^ 0x68E31DA4u, 3);
            const bool desert = temperature > 0.18f && moisture < -0.10f;
            const bool forest = moisture > 0.18f && temperature > -0.35f;
            const bool mountains = mountain > 0.42f && ridge > 0.30f;

            // A gently meandering synthetic river. The center channel is
            // lowered to a bed below sea level, while a smooth distance
            // profile blends that cut into the surrounding terrain. The
            // water mask remains narrower than the carved bank region.
            const float river_axis = 0.52f +
                0.10f * std::sin(nx * 8.0f + fbm(nx * 2.0f, nz * 2.0f,
                                                  seed ^ 0xC0FFEE11u, 2));
            const float river_distance = std::abs(nz - river_axis);
            constexpr float river_half_width = 0.012f;
            constexpr float river_bank_width = 0.040f;
            const bool river = river_distance <= river_half_width &&
                               !mountains;
            if (!mountains && river_distance < river_bank_width) {
                const float channel_strength =
                    1.0f - smoothstep(river_half_width, river_bank_width,
                                      river_distance);
                const float river_bed = std::max(
                    settings.min_height, sea - 2.0f);
                world_height += (river_bed - world_height) *
                                channel_strength;
            }
            const bool water = world_height <= sea || river;
            const float normalized_height = height_span > 0.0f
                ? (world_height - settings.min_height) / height_span
                : 0.0f;
            const auto packed_height = static_cast<std::uint16_t>(
                std::clamp(std::lround(normalized_height * 65535.0f),
                           0l, 65535l));

            std::uint8_t biome = 0;
            if (desert) biome = 2;
            else if (mountains) biome = 3;
            else if (forest) biome = 1;

            std::uint8_t material = 1;  // grass
            if (desert || water) material = 4;  // sand
            else if (mountains) material = 5;   // stone

            std::uint8_t flags = biome & 0x07u;
            if (forest) flags |= 0x08u;
            if (mountains) flags |= 0x10u;
            if (river) flags |= 0x20u;
            if (water) flags |= 0x80u;

            Rgba8& pixel = image.pixels[z * width + x];
            pixel.r = static_cast<std::uint8_t>(packed_height >> 8u);
            pixel.g = static_cast<std::uint8_t>(packed_height & 0xffu);
            pixel.b = material;
            pixel.a = flags;
        }
    }

    image.validate();
    return image;
}

}  // namespace ws::rgba_terrain_core
