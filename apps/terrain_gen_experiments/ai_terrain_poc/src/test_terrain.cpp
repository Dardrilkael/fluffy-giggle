#include "ai_terrain/test_terrain.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "ai_terrain/deterministic_noise.hpp"

namespace ai_terrain::test_terrain {
namespace {

constexpr float kPi = 3.14159265358979323846f;

void validate_size(std::size_t width, std::size_t depth) {
  if (width < 2 || depth < 2) {
    throw std::invalid_argument("synthetic terrain dimensions must be at least 2x2");
  }
}

}  // namespace

Heightmap flat(std::size_t width, std::size_t depth) {
  validate_size(width, depth);
  return {width, depth, std::vector<float>(width * depth, 0.35f)};
}

Heightmap rolling(std::size_t width, std::size_t depth) {
  validate_size(width, depth);
  Heightmap map{width, depth, std::vector<float>(width * depth)};
  for (std::size_t z = 0; z < depth; ++z) {
    for (std::size_t x = 0; x < width; ++x) {
      const float u = static_cast<float>(x) / static_cast<float>(width - 1);
      const float v = static_cast<float>(z) / static_cast<float>(depth - 1);
      map.heights[z * width + x] =
          std::clamp(0.42f + 0.12f * std::sin(2.0f * kPi * u) *
                                      std::cos(2.0f * kPi * v),
                     0.0f, 1.0f);
    }
  }
  return map;
}

Heightmap mountainous(std::size_t width, std::size_t depth) {
  validate_size(width, depth);
  Heightmap map{width, depth, std::vector<float>(width * depth)};
  for (std::size_t z = 0; z < depth; ++z) {
    for (std::size_t x = 0; x < width; ++x) {
      const float u = static_cast<float>(x) / static_cast<float>(width - 1);
      const float v = static_cast<float>(z) / static_cast<float>(depth - 1);
      const float ridge = std::abs(std::sin(3.0f * kPi * u + 1.4f * std::cos(2.0f * kPi * v)));
      const float detail = 0.5f + 0.5f * std::sin(8.0f * kPi * v + 2.0f * std::sin(6.0f * kPi * u));
      map.heights[z * width + x] = std::clamp(0.18f + 0.64f * ridge + 0.12f * detail, 0.0f, 1.0f);
    }
  }
  return map;
}

Heightmap from_profile(const ParameterProfile& p, std::uint64_t seed,
                       std::uint64_t region_index, std::size_t width,
                       std::size_t depth) {
  validate_size(width, depth);
  const DeterministicNoise noise(seed ^ (region_index * 0xD6E8FEB86659FD93ull));
  Heightmap map{width, depth, std::vector<float>(width * depth)};
  const float frequency = 1.0f + 5.0f * p.frequency;
  for (std::size_t z = 0; z < depth; ++z) {
    for (std::size_t x = 0; x < width; ++x) {
      const float u = static_cast<float>(x) / static_cast<float>(width - 1);
      const float v = static_cast<float>(z) / static_cast<float>(depth - 1);
      const float broad = std::sin(2.0f * kPi * frequency * u + 0.3f) *
                          std::cos(2.0f * kPi * frequency * v - 0.2f);
      const float ridge = 1.0f - std::abs(std::sin(2.0f * kPi * frequency * u));
      const float detail = noise.signed_unit(z * width + x);
      const float mountain = p.mountainStrength * (0.5f * ridge + 0.5f);
      const float hill = p.hillStrength * broad;
      const float micro = p.roughness * 0.18f * detail;
      const float height = 0.38f + p.amplitude * (0.48f * mountain + 0.30f * hill) +
                           p.variation * 0.10f * broad + micro;
      map.heights[z * width + x] = std::clamp(height, 0.0f, 1.0f);
    }
  }
  return map;
}

}  // namespace ai_terrain::test_terrain
