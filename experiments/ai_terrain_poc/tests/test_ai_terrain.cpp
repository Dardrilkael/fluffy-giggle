#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

#include "ai_terrain/region_simulation.hpp"
#include "ai_terrain/terrain_analyzer.hpp"
#include "ai_terrain/test_terrain.hpp"

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

bool near(float a, float b, float epsilon = 1.0e-5f) {
  return std::abs(a - b) <= epsilon;
}

void test_flat_analysis() {
  const auto f = ai_terrain::TerrainAnalyzer::analyze(ai_terrain::test_terrain::flat());
  require(near(f.meanHeight, 0.35f), "flat mean height");
  require(near(f.heightVariance, 0.0f), "flat variance");
  require(near(f.averageSlope, 0.0f), "flat slope");
  require(near(f.roughness, 0.0f), "flat roughness");
}

void test_inputs_change_decisions() {
  const auto flat = ai_terrain::TerrainAnalyzer::analyze(ai_terrain::test_terrain::flat());
  const auto mountain = ai_terrain::TerrainAnalyzer::analyze(ai_terrain::test_terrain::mountainous());
  const auto a = ai_terrain::FuzzyController{}.decide(flat);
  const auto b = ai_terrain::FuzzyController{}.decide(mountain);
  require(b.mountainStrength > a.mountainStrength, "mountain input raises mountain strength");
  require(a.flatness > b.flatness, "flat input raises flatness");
}

void test_deterministic_sequence_and_bounded_noise() {
  const auto initial = ai_terrain::test_terrain::rolling();
  const auto a = ai_terrain::RegionSimulation{}.run(initial, 10, 123456u);
  const auto b = ai_terrain::RegionSimulation{}.run(initial, 10, 123456u);
  require(a.size() == 10 && b.size() == 10, "sequence length");
  for (std::size_t i = 0; i < a.size(); ++i) {
    require(near(a[i].parameters.amplitude, b[i].parameters.amplitude),
            "same seed reproduces amplitude");
    require(near(a[i].parameters.roughness, b[i].parameters.roughness),
            "same seed reproduces roughness");
    const auto& p = a[i].parameters;
    require(p.amplitude >= 0.0f && p.amplitude <= 1.0f, "amplitude bounded");
    require(p.frequency >= 0.0f && p.frequency <= 1.0f, "frequency bounded");
    require(p.mountainStrength >= 0.0f && p.mountainStrength <= 1.0f,
            "mountain strength bounded");
    if (i > 0) {
      require(std::abs(p.amplitude - a[i - 1].parameters.amplitude) < 0.5f,
              "adjacent amplitude changes are controlled");
    }
  }
  const auto other_seed = ai_terrain::RegionSimulation{}.run(initial, 10, 999u);
  bool differs = false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    differs = differs || !near(a[i].parameters.amplitude,
                               other_seed[i].parameters.amplitude, 1.0e-6f);
  }
  require(differs, "different seed changes deterministic variation");
}

void test_invalid_heightmap_rejected() {
  bool threw = false;
  try {
    ai_terrain::Heightmap bad{2, 2, {0.1f, 0.2f, 0.3f}};
    (void)ai_terrain::TerrainAnalyzer::analyze(bad);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "malformed map rejected");
}

}  // namespace

int main() {
  test_flat_analysis();
  test_inputs_change_decisions();
  test_deterministic_sequence_and_bounded_noise();
  test_invalid_heightmap_rejected();
  std::cout << "All AI terrain POC tests passed.\n";
  return 0;
}
