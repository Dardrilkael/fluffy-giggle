#include <cstddef>
#include <iomanip>
#include <iostream>

#include "ai_terrain/fuzzy_controller.hpp"
#include "ai_terrain/region_simulation.hpp"
#include "ai_terrain/terrain_analyzer.hpp"
#include "ai_terrain/test_terrain.hpp"

namespace {

void print_features(const ai_terrain::TerrainFeatures& f) {
  std::cout << "  features: mean=" << f.meanHeight
            << " variance=" << f.heightVariance
            << " slope=" << f.averageSlope
            << " roughness=" << f.roughness
            << " gradient=(" << f.gradientX << ", " << f.gradientZ << ")\n";
}

void print_profile(const ai_terrain::ParameterProfile& p) {
  std::cout << "  parameters: amplitude=" << p.amplitude
            << " frequency=" << p.frequency
            << " mountains=" << p.mountainStrength
            << " hills=" << p.hillStrength
            << " flatness=" << p.flatness
            << " roughness=" << p.roughness
            << " variation=" << p.variation << '\n';
}

void inspect_sample(const char* name, const ai_terrain::Heightmap& map) {
  const auto features = ai_terrain::TerrainAnalyzer::analyze(map);
  const auto parameters = ai_terrain::FuzzyController{}.decide(features);
  std::cout << name << '\n';
  print_features(features);
  print_profile(parameters);
}

}  // namespace

int main() {
  std::cout << std::fixed << std::setprecision(3);
  std::cout << "AI terrain parameter POC\n"
            << "Synthetic inputs are analyzed; no game world or chunks are generated.\n\n";

  inspect_sample("Flat terrain", ai_terrain::test_terrain::flat());
  std::cout << '\n';
  inspect_sample("Rolling terrain", ai_terrain::test_terrain::rolling());
  std::cout << '\n';
  inspect_sample("Mountainous terrain", ai_terrain::test_terrain::mountainous());

  constexpr std::size_t region_count = 10;
  constexpr std::uint64_t seed = 123456u;
  const auto sequence = ai_terrain::RegionSimulation{}.run(
      ai_terrain::test_terrain::rolling(), region_count, seed);

  std::cout << "\nSequential region simulation (seed=" << seed << ")\n";
  std::cout << "region  amplitude  frequency  mountains  hills  flatness  roughness\n";
  for (const auto& region : sequence) {
    const auto& p = region.parameters;
    std::cout << std::setw(6) << region.regionIndex << "  "
              << std::setw(9) << p.amplitude << "  "
              << std::setw(9) << p.frequency << "  "
              << std::setw(9) << p.mountainStrength << "  "
              << std::setw(5) << p.hillStrength << "  "
              << std::setw(8) << p.flatness << "  "
              << std::setw(9) << p.roughness << '\n';
  }

  return 0;
}
