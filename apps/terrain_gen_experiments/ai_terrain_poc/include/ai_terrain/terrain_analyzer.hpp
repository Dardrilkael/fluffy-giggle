#pragma once

#include "ai_terrain/heightmap.hpp"
#include "ai_terrain/terrain_features.hpp"

namespace ai_terrain {

class TerrainAnalyzer {
 public:
  // Returns normalized statistics. Invalid dimensions, sample counts, or
  // non-finite/out-of-range heights throw std::invalid_argument.
  [[nodiscard]] static TerrainFeatures analyze(const Heightmap& map);
};

}  // namespace ai_terrain
