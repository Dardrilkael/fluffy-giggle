#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include "ai_terrain/experiment_output.hpp"
#include "ai_terrain/fuzzy_controller.hpp"
#include "ai_terrain/region_simulation.hpp"
#include "ai_terrain/terrain_analyzer.hpp"
#include "ai_terrain/test_terrain.hpp"

namespace {
void print_features(const ai_terrain::TerrainFeatures& f) {
  std::cout << "  features: mean=" << f.meanHeight << " variance=" << f.heightVariance
            << " slope=" << f.averageSlope << " roughness=" << f.roughness
            << " gradient=(" << f.gradientX << ", " << f.gradientZ << ")\n";
}
void print_profile(const ai_terrain::ParameterProfile& p) {
  std::cout << "  parameters: amplitude=" << p.amplitude << " frequency=" << p.frequency
            << " mountains=" << p.mountainStrength << " hills=" << p.hillStrength
            << " flatness=" << p.flatness << " roughness=" << p.roughness
            << " variation=" << p.variation << '\n';
}
void inspect_and_save(const char* name, const std::string& folder,
                      const ai_terrain::Heightmap& map,
                      const ai_terrain::ExperimentOutput& output) {
  const auto features = ai_terrain::TerrainAnalyzer::analyze(map);
  const auto parameters = ai_terrain::FuzzyController{}.decide(features);
  std::cout << name << '\n';
  print_features(features);
  print_profile(parameters);
  output.write_sample(folder, map, features, parameters);
}
}  // namespace

int main() {
  constexpr std::size_t region_count = 10;
  constexpr std::uint64_t seed = 123456u;
  try {
    std::ostringstream run_name;
    run_name << "run_seed_" << seed;
    const std::filesystem::path output_root =
        std::filesystem::path("ai_terrain_output") / run_name.str();
    const ai_terrain::ExperimentOutput output(output_root);

    std::cout << std::fixed << std::setprecision(3)
              << "AI terrain parameter POC\n"
              << "Synthetic inputs are analyzed; no game world or chunks are generated.\n"
              << "Seed: " << seed << "\n\n";
    inspect_and_save("Flat terrain", "flat", ai_terrain::test_terrain::flat(), output);
    std::cout << '\n';
    inspect_and_save("Rolling terrain", "rolling", ai_terrain::test_terrain::rolling(), output);
    std::cout << '\n';
    inspect_and_save("Mountainous terrain", "mountainous",
                     ai_terrain::test_terrain::mountainous(), output);

    const auto sequence = ai_terrain::RegionSimulation{}.run(
        ai_terrain::test_terrain::rolling(), region_count, seed);
    std::cout << "\nSequential region simulation (seed=" << seed << ")\n"
              << "region  amplitude  frequency  mountains  hills  flatness  roughness\n";
    for (const auto& region : sequence) {
      const auto& p = region.parameters;
      std::cout << std::setw(6) << region.regionIndex << "  "
                << std::setw(9) << p.amplitude << "  " << std::setw(9) << p.frequency
                << "  " << std::setw(9) << p.mountainStrength << "  "
                << std::setw(5) << p.hillStrength << "  " << std::setw(8) << p.flatness
                << "  " << std::setw(9) << p.roughness << '\n';
      const auto map = ai_terrain::test_terrain::from_profile(p, seed, region.regionIndex);
      std::ostringstream folder;
      folder << "regions/region_" << std::setw(4) << std::setfill('0') << region.regionIndex;
      output.write_sample(folder.str(), map, region.observed, p);
    }
    output.write_manifest(seed, region_count);
    std::cout << "\nSaved reproducible experiment to: " << output_root.string() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Failed to save terrain experiment: " << error.what() << '\n';
    return 1;
  }
}
