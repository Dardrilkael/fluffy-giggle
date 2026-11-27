#pragma once

#include <cstdint>

namespace ai_terrain {

class DeterministicNoise {
 public:
  explicit DeterministicNoise(std::uint64_t seed) : seed_(seed) {}

  // Stable across platforms/builds: does not depend on std::hash or a
  // standard-library random distribution.
  [[nodiscard]] float signed_unit(std::uint64_t stream) const {
    std::uint64_t x = seed_ + 0x9E3779B97F4A7C15ull * (stream + 1ull);
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    const auto mantissa = static_cast<std::uint32_t>(x >> 40);
    return static_cast<float>(mantissa) / 8388607.5f - 1.0f;
  }

 private:
  std::uint64_t seed_;
};

}  // namespace ai_terrain
