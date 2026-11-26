#include "ai_terrain/region_simulation.hpp"

#include <algorithm>

#include "ai_terrain/deterministic_noise.hpp"
#include "ai_terrain/heightmap.hpp"
#include "ai_terrain/test_terrain.hpp"
#include "ai_terrain/terrain_analyzer.hpp"
#include "ai_terrain/fuzzy_controller.hpp"

namespace ai_terrain {
namespace {

float blend(float previous, float target, float amount) {
  return previous + (target - previous) * amount;
}

ParameterProfile smooth(const ParameterProfile& previous,
                        const ParameterProfile& target) {
  constexpr float kResponse = 0.62f;
  return {
      blend(previous.amplitude, target.amplitude, kResponse),
      blend(previous.frequency, target.frequency, kResponse),
      blend(previous.mountainStrength, target.mountainStrength, kResponse),
      blend(previous.hillStrength, target.hillStrength, kResponse),
      blend(previous.flatness, target.flatness, kResponse),
      blend(previous.roughness, target.roughness, kResponse),
      blend(previous.variation, target.variation, kResponse),
  };
}

ParameterProfile perturb(const ParameterProfile& p, std::uint64_t seed,
                          std::uint64_t region) {
  const DeterministicNoise noise(seed);
  constexpr float kAlpha = 0.025f;
  const auto n = [&](std::uint64_t stream) { return kAlpha * noise.signed_unit(region * 8 + stream); };
  return {
      std::clamp(p.amplitude + n(0), 0.0f, 1.0f),
      std::clamp(p.frequency + n(1), 0.0f, 1.0f),
      std::clamp(p.mountainStrength + n(2), 0.0f, 1.0f),
      std::clamp(p.hillStrength + n(3), 0.0f, 1.0f),
      std::clamp(p.flatness + n(4), 0.0f, 1.0f),
      std::clamp(p.roughness + n(5), 0.0f, 1.0f),
      std::clamp(p.variation + n(6), 0.0f, 1.0f),
  };
}

}  // namespace

std::vector<RegionDecision> RegionSimulation::run(
    const Heightmap& initial, std::size_t transitions,
    std::uint64_t seed) const {
  TerrainAnalyzer analyzer;
  FuzzyController controller;
  Heightmap current = initial;
  std::vector<RegionDecision> decisions;
  decisions.reserve(transitions);
  ParameterProfile previous{};
  bool has_previous = false;

  for (std::size_t i = 0; i < transitions; ++i) {
    const TerrainFeatures features = analyzer.analyze(current);
    ParameterProfile parameters = controller.decide(features);
    if (has_previous) parameters = smooth(previous, parameters);
    parameters = perturb(parameters, seed, i);
    decisions.push_back({i, features, parameters});
    previous = parameters;
    has_previous = true;
    current = test_terrain::from_profile(parameters, seed, i, current.width, current.depth);
  }
  return decisions;
}

}  // namespace ai_terrain
