#include "ai_terrain/terrain_analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ai_terrain {

TerrainFeatures TerrainAnalyzer::analyze(const Heightmap& map) {
  if (map.width < 2 || map.depth < 2 ||
      map.heights.size() != map.width * map.depth) {
    throw std::invalid_argument("heightmap must be at least 2x2 and fully populated");
  }

  double sum = 0.0;
  for (const float h : map.heights) {
    if (!std::isfinite(h) || h < 0.0f || h > 1.0f) {
      throw std::invalid_argument("height samples must be finite and in [0, 1]");
    }
    sum += h;
  }

  const double count = static_cast<double>(map.heights.size());
  const double mean = sum / count;
  double variance = 0.0;
  for (const float h : map.heights) {
    const double d = static_cast<double>(h) - mean;
    variance += d * d;
  }
  variance /= count;

  double gx = 0.0;
  double gz = 0.0;
  double slope = 0.0;
  double roughness = 0.0;
  std::size_t x_edges = 0;
  std::size_t z_edges = 0;
  std::size_t cells = 0;

  for (std::size_t z = 0; z < map.depth; ++z) {
    for (std::size_t x = 0; x < map.width; ++x) {
      const float h = map.at(x, z);
      if (x + 1 < map.width) {
        const float dx = std::abs(map.at(x + 1, z) - h);
        gx += dx;
        ++x_edges;
      }
      if (z + 1 < map.depth) {
        const float dz = std::abs(map.at(x, z + 1) - h);
        gz += dz;
        ++z_edges;
      }

      float neighbor_sum = 0.0f;
      int neighbors = 0;
      if (x > 0) { neighbor_sum += map.at(x - 1, z); ++neighbors; }
      if (x + 1 < map.width) { neighbor_sum += map.at(x + 1, z); ++neighbors; }
      if (z > 0) { neighbor_sum += map.at(x, z - 1); ++neighbors; }
      if (z + 1 < map.depth) { neighbor_sum += map.at(x, z + 1); ++neighbors; }
      roughness += std::abs(h - neighbor_sum / static_cast<float>(neighbors));
      ++cells;
    }
  }

  const double mean_gx = gx / static_cast<double>(x_edges);
  const double mean_gz = gz / static_cast<double>(z_edges);
  // Mean magnitude of the local forward-difference gradient.
  for (std::size_t z = 0; z + 1 < map.depth; ++z) {
    for (std::size_t x = 0; x + 1 < map.width; ++x) {
      const double dx = static_cast<double>(map.at(x + 1, z) - map.at(x, z));
      const double dz = static_cast<double>(map.at(x, z + 1) - map.at(x, z));
      slope += std::sqrt(dx * dx + dz * dz);
    }
  }
  const double slope_count =
      static_cast<double>((map.width - 1) * (map.depth - 1));

  return {
      static_cast<float>(mean),
      static_cast<float>(variance),
      static_cast<float>(slope / slope_count),
      static_cast<float>(roughness / static_cast<double>(cells)),
      static_cast<float>(mean_gx),
      static_cast<float>(mean_gz),
  };
}

}  // namespace ai_terrain
