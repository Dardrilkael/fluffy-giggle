#pragma once

namespace ai_terrain {

struct TerrainFeatures {
  float meanHeight{};
  float heightVariance{};
  float averageSlope{};
  float roughness{};
  float gradientX{};
  float gradientZ{};
};

}  // namespace ai_terrain
