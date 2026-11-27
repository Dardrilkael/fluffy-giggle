#include "rgba_terrain/rgba_terrain.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace rgba_terrain {
namespace {
std::uint8_t channel_value(const Rgba8& p, Channel c) {
  switch (c) { case Channel::R: return p.r; case Channel::G: return p.g; case Channel::B: return p.b; case Channel::A: return p.a; }
  throw std::logic_error("invalid channel");
}
}
const Rgba8& RgbaImage::at(std::size_t x, std::size_t z) const {
  if (x >= width || z >= height) throw std::out_of_range("RGBA pixel coordinate");
  return pixels[z * width + x];
}
void RgbaImage::validate() const {
  if (!width || !height) throw std::invalid_argument("image dimensions must be non-zero");
  if (width > std::numeric_limits<std::size_t>::max() / height || width * height != pixels.size())
    throw std::invalid_argument("RGBA pixel count does not match image dimensions");
}
void ChannelLayout::validate() const {
  unsigned h8=0, msb=0, lsb=0;
  for (auto r : channels) { h8 += r==ChannelRole::Height8; msb += r==ChannelRole::Height16Msb; lsb += r==ChannelRole::Height16Lsb; }
  if (!((h8==1 && msb==0 && lsb==0) || (h8==0 && msb==1 && lsb==1)))
    throw std::invalid_argument("layout needs Height8 or exactly one Height16Msb/Height16Lsb pair");
}
TerrainColumns generate_columns(const RgbaImage& image, const ChannelLayout& layout, const TerrainSettings& s) {
  image.validate(); layout.validate();
  if (!std::isfinite(s.min_height) || !std::isfinite(s.max_height) ||
      s.min_height < 0.0f || s.max_height < s.min_height ||
      static_cast<double>(s.max_height) > std::numeric_limits<std::uint32_t>::max())
    throw std::invalid_argument("terrain height range must be finite, non-negative, and fit uint32");
  auto find = [&](ChannelRole role) { for (int i=0;i<4;++i) if (layout.channels[static_cast<std::size_t>(i)]==role) return i; return -1; };
  const int h8=find(ChannelRole::Height8), hm=find(ChannelRole::Height16Msb), hl=find(ChannelRole::Height16Lsb);
  const int mat=find(ChannelRole::MaterialId), bio=find(ChannelRole::BiomeId), water=find(ChannelRole::WaterMask), den=find(ChannelRole::Density);
  TerrainColumns out{image.width,image.height,{}};
  out.columns.reserve(image.pixels.size());
  const double levels=h8>=0 ? 255.0 : 65535.0;
  const double span=static_cast<double>(s.max_height)-s.min_height;
  for (std::size_t z=0;z<image.height;++z) for (std::size_t x=0;x<image.width;++x) {
    const auto& p=image.at(x,z);
    auto value=[&](int i)->std::uint8_t { return i<0 ? 0 : channel_value(p,static_cast<Channel>(i)); };
    const std::uint32_t encoded=h8>=0 ? value(h8) : (static_cast<std::uint32_t>(value(hm))<<8u)|value(hl);
    const double normalized=static_cast<double>(encoded)/levels;
    const double world=s.min_height+normalized*span;
    const auto height=static_cast<std::uint32_t>(std::max(0.0,std::floor(world+0.5)));
    out.columns.push_back({x,z,height,mat>=0?value(mat):s.default_material,bio>=0?value(bio):s.default_biome,
      water>=0?value(water):std::uint8_t{0},den>=0?value(den):s.solid_density});
  }
  return out;
}
} // namespace rgba_terrain
