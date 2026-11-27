#include "ai_terrain/experiment_output.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <vector>

namespace ai_terrain {
namespace {

void write_u32_le(std::ostream& out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    out.put(static_cast<char>((value >> shift) & 0xffu));
}

void write_heightmap_binary(const std::filesystem::path& path,
                            const Heightmap& map) {
  std::ofstream out(path, std::ios::binary);
  if (!out) throw std::runtime_error("cannot create " + path.string());
  out.write("AITHMAP1", 8);
  write_u32_le(out, static_cast<std::uint32_t>(map.width));
  write_u32_le(out, static_cast<std::uint32_t>(map.depth));
  for (float value : map.heights) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    write_u32_le(out, bits);
  }
  if (!out) throw std::runtime_error("failed writing " + path.string());
}

// Dependency-free 24-bit BMP preview. The binary heightmap remains the
// authoritative lossless source; this image is only for quick inspection.
void write_heightmap_bmp(const std::filesystem::path& path,
                         const Heightmap& map) {
  const std::uint32_t row_bytes = static_cast<std::uint32_t>(map.width * 3);
  const std::uint32_t stride = (row_bytes + 3u) & ~3u;
  const std::uint32_t image_bytes = stride * static_cast<std::uint32_t>(map.depth);
  const std::uint32_t file_bytes = 54u + image_bytes;
  std::ofstream out(path, std::ios::binary);
  if (!out) throw std::runtime_error("cannot create " + path.string());

  out.put('B'); out.put('M');
  write_u32_le(out, file_bytes);
  write_u32_le(out, 0u);
  write_u32_le(out, 54u);
  write_u32_le(out, 40u);
  write_u32_le(out, static_cast<std::uint32_t>(map.width));
  write_u32_le(out, static_cast<std::uint32_t>(map.depth));
  out.put(1); out.put(0);
  out.put(24); out.put(0);
  write_u32_le(out, 0u);
  write_u32_le(out, image_bytes);
  write_u32_le(out, 2835u); write_u32_le(out, 2835u);
  write_u32_le(out, 0u); write_u32_le(out, 0u);

  const std::array<char, 3> padding{};
  for (std::size_t z = map.depth; z-- > 0;) {
    for (std::size_t x = 0; x < map.width; ++x) {
      const auto gray = static_cast<unsigned char>(
          map.heights[z * map.width + x] * 255.0f + 0.5f);
      out.put(static_cast<char>(gray));
      out.put(static_cast<char>(gray));
      out.put(static_cast<char>(gray));
    }
    const auto pad = stride - row_bytes;
    out.write(padding.data(), static_cast<std::streamsize>(pad));
  }
  if (!out) throw std::runtime_error("failed writing " + path.string());
}

void write_json(const std::filesystem::path& path, const std::string& body) {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("cannot create " + path.string());
  out << body << '\n';
  if (!out) throw std::runtime_error("failed writing " + path.string());
}

}  // namespace

ExperimentOutput::ExperimentOutput(std::filesystem::path root)
    : root_(std::move(root)) {
  std::filesystem::create_directories(root_);
}

void ExperimentOutput::write_sample(const std::string& name,
                                    const Heightmap& map,
                                    const TerrainFeatures& f,
                                    const ParameterProfile& p) const {
  const auto dir = root_ / name;
  std::filesystem::create_directories(dir);
  write_heightmap_binary(dir / "heightmap.bin", map);
  write_heightmap_bmp(dir / "heightmap.bmp", map);

  std::ostringstream analysis;
  analysis << std::setprecision(9)
           << "{\n  \"width\": " << map.width
           << ",\n  \"depth\": " << map.depth
           << ",\n  \"mean_height\": " << f.meanHeight
           << ",\n  \"height_variance\": " << f.heightVariance
           << ",\n  \"average_slope\": " << f.averageSlope
           << ",\n  \"roughness\": " << f.roughness
           << ",\n  \"gradient_x\": " << f.gradientX
           << ",\n  \"gradient_z\": " << f.gradientZ << "\n}";
  write_json(dir / "analysis.json", analysis.str());

  std::ostringstream parameters;
  parameters << std::setprecision(9)
             << "{\n  \"amplitude\": " << p.amplitude
             << ",\n  \"frequency\": " << p.frequency
             << ",\n  \"mountain_strength\": " << p.mountainStrength
             << ",\n  \"hill_strength\": " << p.hillStrength
             << ",\n  \"flatness\": " << p.flatness
             << ",\n  \"roughness\": " << p.roughness
             << ",\n  \"variation\": " << p.variation << "\n}";
  write_json(dir / "parameters.json", parameters.str());
}

void ExperimentOutput::write_manifest(std::uint64_t seed,
                                       std::size_t region_count) const {
  std::ostringstream manifest;
  manifest << "{\n"
           << "  \"format_version\": 1,\n"
           << "  \"experiment\": \"ai_terrain_poc\",\n"
           << "  \"seed\": " << seed << ",\n"
           << "  \"region_count\": " << region_count << ",\n"
           << "  \"heightmap_format\": \"AITHMAP1: uint32 width, uint32 depth, row-major float32 little-endian\",\n"
           << "  \"preview_format\": \"24-bit grayscale BMP\",\n"
           << "  \"samples\": [\"flat\", \"rolling\", \"mountainous\"]\n"
           << "}";
  write_json(root_ / "manifest.json", manifest.str());
}

}  // namespace ai_terrain
