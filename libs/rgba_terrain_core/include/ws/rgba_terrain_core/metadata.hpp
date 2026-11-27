#pragma once
#include <ws/rgba_terrain_core/rgba_terrain.hpp>
#include <string>
namespace ws::rgba_terrain_core {
// Small strict key=value sidecar format. Unknown keys and invalid roles are errors.
struct TerrainMetadata {
  ChannelLayout layout;
  TerrainSettings settings;
};
[[nodiscard]] TerrainMetadata load_metadata(const std::string& path);
} // namespace ws::rgba_terrain_core
