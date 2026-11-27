#pragma once

#include <cstddef>
#include <vector>

namespace ai_terrain {

// Heights are normalized to [0, 1]. Samples are row-major: z * width + x.
struct Heightmap {
  std::size_t width{};
  std::size_t depth{};
  std::vector<float> heights;

  [[nodiscard]] float at(std::size_t x, std::size_t z) const {
    return heights[z * width + x];
  }
};

}  // namespace ai_terrain
