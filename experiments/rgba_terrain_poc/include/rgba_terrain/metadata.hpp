#pragma once
#include "rgba_terrain/rgba_terrain.hpp"
#include <string>
namespace rgba_terrain {
// Small strict key=value sidecar format. Unknown keys and invalid roles are errors.
struct TerrainMetadata {
  ChannelLayout layout;
  TerrainSettings settings;
};
[[nodiscard]] TerrainMetadata load_metadata(const std::string& path);
} // namespace rgba_terrain
