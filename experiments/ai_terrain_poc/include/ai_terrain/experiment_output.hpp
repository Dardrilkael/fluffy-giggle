#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "ai_terrain/heightmap.hpp"
#include "ai_terrain/parameter_profile.hpp"
#include "ai_terrain/terrain_features.hpp"

namespace ai_terrain {

class ExperimentOutput {
 public:
  explicit ExperimentOutput(std::filesystem::path root);
  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

  void write_sample(const std::string& name, const Heightmap& map,
                    const TerrainFeatures& features,
                    const ParameterProfile& parameters) const;
  void write_manifest(std::uint64_t seed, std::size_t region_count) const;

 private:
  std::filesystem::path root_;
};

}  // namespace ai_terrain
