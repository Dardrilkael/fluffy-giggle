#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace rgba_terrain {
enum class ChannelRole : std::uint8_t { Unused, Height8, Height16Msb, Height16Lsb, MaterialId, BiomeId, WaterMask, Density };
enum class Channel : std::uint8_t { R = 0, G = 1, B = 2, A = 3 };
struct Rgba8 { std::uint8_t r{}, g{}, b{}, a{}; };
struct RgbaImage {
  std::size_t width{}, height{};
  std::vector<Rgba8> pixels;
  [[nodiscard]] const Rgba8& at(std::size_t x, std::size_t z) const;
  void validate() const;
};
struct ChannelLayout {
  std::array<ChannelRole, 4> channels{ChannelRole::Height8, ChannelRole::MaterialId, ChannelRole::BiomeId, ChannelRole::WaterMask};
  void validate() const;
};
struct TerrainSettings {
  float min_height{};
  float max_height{128.0f};
  std::uint8_t default_material{1};
  std::uint8_t default_biome{};
  std::uint8_t solid_density{255};
};
struct VoxelColumn {
  std::size_t x{}, z{};
  std::uint32_t height{};
  std::uint8_t material_id{}, biome_id{}, water_mask{}, density{};
};
struct TerrainColumns {
  std::size_t width{}, depth{};
  std::vector<VoxelColumn> columns;
};
[[nodiscard]] TerrainColumns generate_columns(const RgbaImage&, const ChannelLayout&, const TerrainSettings&);
#ifdef RGBA_TERRAIN_HAS_PNG
[[nodiscard]] RgbaImage load_png_rgba(const char* path);
#endif
} // namespace rgba_terrain
