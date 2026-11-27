#pragma once

#include "ai_terrain/parameter_profile.hpp"
#include "ai_terrain/terrain_features.hpp"

namespace ai_terrain {

class FuzzyController {
 public:
  [[nodiscard]] ParameterProfile decide(const TerrainFeatures& features) const;
};

}  // namespace ai_terrain
