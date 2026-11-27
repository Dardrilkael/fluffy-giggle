#include "rgba_terrain/rgba_terrain.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace rgba_terrain {
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

std::uint8_t quantize_height(float value, float minimum, float maximum) {
    const float normalized = maximum > minimum
        ? std::clamp((value - minimum) / (maximum - minimum), 0.0f, 1.0f)
        : 0.0f;
    return static_cast<std::uint8_t>(
        std::clamp(std::lround(normalized * 65535.0f), 0l, 65535l));
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

            const float continental = fbm(nx * 2.4f, nz * 2.4f,
                                           seed ^ 0x31415926u, 5);
            const float hills = fbm(nx * 11.0f, nz * 11.0f,
                                    seed ^ 0x27182818u, 4);
            const float ridge_source = fbm(nx * 6.0f, nz * 6.0f,
                                           seed ^ 0xA341316Cu, 4);
            const float ridge = 1.0f - std::abs(ridge_source);
            const float mountain = smooth(std::clamp(
                (continental - 0.05f) * 2.8f, 0.0f, 1.0f));
            const float broad = remap01(continental);
            const float hill = remap01(hills);
            const float terrain = std::clamp(
                0.12f + broad * 0.40f + hill * 0.12f +
                    mountain * ridge * 0.36f,
                0.0f, 1.0f);
            const float world_height = settings.min_height +
                                       terrain * height_span;
            const auto packed_height = static_cast<std::uint16_t>(
                std::clamp(std::lround(terrain * 65535.0f), 0l, 65535l));

            const float temperature = fbm(nx * 3.7f, nz * 3.7f,
                                           seed ^ 0xB5297A4Du, 3);
            const float moisture = fbm(nx * 5.1f, nz * 5.1f,
                                        seed ^ 0x68E31DA4u, 3);
            const bool desert = temperature > 0.18f && moisture < -0.10f;
            const bool forest = moisture > 0.18f && temperature > -0.35f;
            const bool mountains = mountain > 0.45f && ridge > 0.28f;

            // A broad, gently meandering river corridor. This is a deliberately
            // simple synthetic mask, not a simulated drainage network.
            const float river_axis = 0.52f +
                0.10f * std::sin(nx * 8.0f + fbm(nx * 2.0f, nz * 2.0f,
                                                  seed ^ 0xC0FFEE11u, 2));
            const bool river = std::abs(nz - river_axis) < 0.018f &&
                               !mountains;
            const bool water = world_height <= sea || river;

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

}  // namespace rgba_terrain
