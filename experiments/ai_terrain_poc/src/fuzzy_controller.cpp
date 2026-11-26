#include "ai_terrain/fuzzy_controller.hpp"

#include <algorithm>
#include <cmath>

namespace ai_terrain {
namespace {

float clamp01(float value) {
  return std::clamp(value, 0.0f, 1.0f);
}

// Smooth membership ramp. The overlap between neighboring terms avoids
// abrupt category changes at a single threshold.
float ramp(float value, float low, float high) {
  const float t = clamp01((value - low) / (high - low));
  return t * t * (3.0f - 2.0f * t);
}

}  // namespace

ParameterProfile FuzzyController::decide(const TerrainFeatures& f) const {
  const float highland = ramp(f.meanHeight, 0.48f, 0.82f);
  const float steep = ramp(f.averageSlope, 0.035f, 0.18f);
  const float varied = ramp(f.heightVariance, 0.012f, 0.12f);
  const float jagged = ramp(f.roughness, 0.012f, 0.075f);
  const float directional = clamp01(std::max(f.gradientX, f.gradientZ) * 3.0f);

  // Interpretable fuzzy-style rule activations:
  // - high + steep terrain encourages mountains;
  // - moderate slope with lower elevation encourages hills;
  // - low slope and low variance encourages plains;
  // - rough/variable terrain increases detail and variation.
  const float mountain = clamp01(0.58f * highland + 0.32f * steep +
                                  0.10f * varied);
  const float hills = clamp01((1.0f - highland) * 0.55f * steep +
                               0.25f * varied + 0.20f * directional);
  const float plains = clamp01((1.0f - steep) * (1.0f - varied));

  return {
      clamp01(0.10f + 0.62f * mountain + 0.22f * hills),
      clamp01(0.08f + 0.34f * jagged + 0.22f * steep + 0.12f * directional),
      mountain,
      hills,
      plains,
      clamp01(0.08f + 0.72f * jagged + 0.12f * steep),
      clamp01(0.10f + 0.48f * varied + 0.24f * jagged + 0.12f * mountain),
  };
}

}  // namespace ai_terrain
