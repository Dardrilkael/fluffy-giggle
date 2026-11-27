#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ai_terrain/heightmap.hpp"
#include "ai_terrain/parameter_profile.hpp"
#include "ai_terrain/terrain_features.hpp"

namespace ai_terrain {

struct RegionDecision {
  std::size_t regionIndex{};
  TerrainFeatures observed{};
  ParameterProfile parameters{};
};

class RegionSimulation {
 public:
  // Analyze the current map, decide the next map's parameters, then feed
  // the synthetic next map back into the controller. The initial map is
  // supplied by the caller; generated maps are test fixtures only.
  [[nodiscard]] std::vector<RegionDecision> run(
      const Heightmap& initial, std::size_t transitions,
      std::uint64_t seed) const;
};

}  // namespace ai_terrain
