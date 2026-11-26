#pragma once

namespace ai_terrain {

// Every field is dimensionless and clamped to [0, 1]. These are controls for
// a future terrain synthesizer, not world/chunk data.
struct ParameterProfile {
  float amplitude{};
  float frequency{};
  float mountainStrength{};
  float hillStrength{};
  float flatness{};
  float roughness{};
  float variation{};
};

}  // namespace ai_terrain
