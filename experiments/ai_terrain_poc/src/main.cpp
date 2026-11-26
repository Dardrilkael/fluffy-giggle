#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>

#include "ai_terrain/region_simulation.hpp"
#include "ai_terrain/test_terrain.hpp"

namespace {

void print_header() {
  std::cout << "region  meanH   variance  slope   rough   amplitude frequency mountain hills flatness variation\n";
}

void print(const ai_terrain::RegionDecision& d) {
  const auto& f = d.observed;
  const auto& p = d.parameters;
  std::cout << std::setw(6) << d.regionIndex << "  "
            << std::fixed << std::setprecision(3)
            << f.meanHeight << "   " << f.heightVariance << "    "
            << f.averageSlope << "  " << f.roughness << "   "
            << p.amplitude << "    " << p.frequency << "    "
            << p.mountainStrength << "   " << p.hillStrength << "  "
            << p.flatness << "   " << p.variation << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  std::string_view fixture = argc > 1 ? argv[1] : "rolling";
  ai_terrain::Heightmap initial;
  if (fixture == "flat") initial = ai_terrain::test_terrain::flat();
  else if (fixture == "mountain") initial = ai_terrain::test_terrain::mountainous();
  else if (fixture == "rolling") initial = ai_terrain::test_terrain::rolling();
  else {
    std::cerr << "Usage: ai_terrain_demo [flat|rolling|mountain]\n";
    return 2;
  }

  constexpr std::uint64_t kSeed = 0xA17E'2026ull;
  const auto decisions = ai_terrain::RegionSimulation{}.run(initial, 12, kSeed);
  print_header();
  for (const auto& decision : decisions) print(decision);
  return 0;
}
