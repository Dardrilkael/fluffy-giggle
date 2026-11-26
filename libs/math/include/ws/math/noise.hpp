#pragma once

// Deterministic, allocation-free value-noise primitives. The seed and
// integer lattice coordinates fully determine the output, making these
// useful for terrain, procedural placement, and stable simulation fields.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ws::math::noise {

[[nodiscard]] inline float hash01(
    std::uint32_t x, std::uint32_t y, std::uint32_t seed) noexcept {
  std::uint32_t h = seed + 0x9E3779B9u;
  h ^= x * 0x85EBCA6Bu;
  h = (h << 13) | (h >> 19);
  h *= 0xC2B2AE35u;
  h ^= y * 0x27D4EB2Fu;
  h = (h << 13) | (h >> 19);
  h *= 0x165667B1u;
  h ^= h >> 15;
  return static_cast<float>(h & 0xFFFFFFu) / 16777216.0f;
}

[[nodiscard]] inline float hash01_3d(
    int x, int y, int z, std::uint32_t seed) noexcept {
  const auto ux = static_cast<std::uint32_t>(x);
  const auto uy = static_cast<std::uint32_t>(y);
  const auto uz = static_cast<std::uint32_t>(z);
  return hash01(ux ^ (uz * 0x9E3779B9u), uy ^ (uz * 0x85EBCA6Bu),
                seed ^ (uz * 0xC2B2AE35u));
}

[[nodiscard]] inline float smooth01(float t) noexcept {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

[[nodiscard]] inline float value_noise2d(
    float x, float y, std::uint32_t seed) noexcept {
  const int xi = static_cast<int>(std::floor(x));
  const int yi = static_cast<int>(std::floor(y));
  const float sx = smooth01(x - static_cast<float>(xi));
  const float sy = smooth01(y - static_cast<float>(yi));
  const float a = hash01(static_cast<std::uint32_t>(xi),
                         static_cast<std::uint32_t>(yi), seed);
  const float b = hash01(static_cast<std::uint32_t>(xi + 1),
                         static_cast<std::uint32_t>(yi), seed);
  const float c = hash01(static_cast<std::uint32_t>(xi),
                         static_cast<std::uint32_t>(yi + 1), seed);
  const float d = hash01(static_cast<std::uint32_t>(xi + 1),
                         static_cast<std::uint32_t>(yi + 1), seed);
  const float near_row = a + (b - a) * sx;
  const float far_row = c + (d - c) * sx;
  return near_row + (far_row - near_row) * sy;
}

[[nodiscard]] inline float fbm2d(
    float x, float y, std::uint32_t seed,
    int octaves = 4, float lacunarity = 2.03f,
    float persistence = 0.5f) noexcept {
  if (octaves <= 0 || !std::isfinite(lacunarity) ||
      !std::isfinite(persistence) || persistence <= 0.0f) {
    return 0.0f;
  }
  float sum = 0.0f;
  float amplitude = 0.5f;
  float total = 0.0f;
  for (int octave = 0; octave < octaves; ++octave) {
    sum += value_noise2d(
        x, y, seed + static_cast<std::uint32_t>(octave) * 0x9E3779B9u) *
        amplitude;
    total += amplitude;
    x *= lacunarity;
    y *= lacunarity;
    amplitude *= persistence;
  }
  return total > 0.0f ? sum / total : 0.0f;
}

[[nodiscard]] inline float value_noise3d(
    float x, float y, float z, std::uint32_t seed) noexcept {
  const int xi = static_cast<int>(std::floor(x));
  const int yi = static_cast<int>(std::floor(y));
  const int zi = static_cast<int>(std::floor(z));
  const float sx = smooth01(x - static_cast<float>(xi));
  const float sy = smooth01(y - static_cast<float>(yi));
  const float sz = smooth01(z - static_cast<float>(zi));
  float plane[2]{};
  for (int dy = 0; dy < 2; ++dy) {
    const float a = hash01_3d(xi, yi + dy, zi, seed);
    const float b = hash01_3d(xi + 1, yi + dy, zi, seed);
    const float c = hash01_3d(xi, yi + dy, zi + 1, seed);
    const float d = hash01_3d(xi + 1, yi + dy, zi + 1, seed);
    const float near_z = a + (b - a) * sx;
    const float far_z = c + (d - c) * sx;
    plane[dy] = near_z + (far_z - near_z) * sz;
  }
  return plane[0] + (plane[1] - plane[0]) * sy;
}

}  // namespace ws::math::noise
