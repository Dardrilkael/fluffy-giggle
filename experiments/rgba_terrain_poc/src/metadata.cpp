#include "rgba_terrain/metadata.hpp"
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
namespace rgba_terrain {
namespace {
std::string trim(const std::string& s) {
  const auto first=s.find_first_not_of(" \t\r");
  if(first==std::string::npos) return {};
  const auto last=s.find_last_not_of(" \t\r");
  return s.substr(first,last-first+1);
}
ChannelRole parse_role(const std::string& s) {
  if(s=="unused") return ChannelRole::Unused;
  if(s=="height8") return ChannelRole::Height8;
  if(s=="height16_msb") return ChannelRole::Height16Msb;
  if(s=="height16_lsb") return ChannelRole::Height16Lsb;
  if(s=="material_id") return ChannelRole::MaterialId;
  if(s=="biome_id") return ChannelRole::BiomeId;
  if(s=="water_mask") return ChannelRole::WaterMask;
  if(s=="density") return ChannelRole::Density;
  if(s=="biome_water_flags") return ChannelRole::BiomeWaterFlags;
  throw std::runtime_error("unknown channel role: "+s);
}
}
TerrainMetadata load_metadata(const std::string& path) {
  std::ifstream in(path);
  if(!in) throw std::runtime_error("cannot open metadata: "+path);
  std::unordered_map<std::string,std::string> values;
  std::string line;
  std::size_t line_number=0;
  while(std::getline(in,line)) {
    ++line_number;
    line=trim(line);
    if(line.empty() || line[0]=='#') continue;
    const auto equal=line.find('=');
    if(equal==std::string::npos) throw std::runtime_error("metadata line "+std::to_string(line_number)+" needs key=value");
    const auto key=trim(line.substr(0,equal));
    const auto value=trim(line.substr(equal+1));
    if(key.empty() || value.empty()) throw std::runtime_error("empty metadata key or value on line "+std::to_string(line_number));
    if(!values.emplace(key,value).second) throw std::runtime_error("duplicate metadata key: "+key);
  }
  auto required=[&](const std::string& key)->const std::string& {
    const auto it=values.find(key);
    if(it==values.end()) throw std::runtime_error("missing metadata key: "+key);
    return it->second;
  };
  auto number=[&](const std::string& key,float& target) {
    const auto& v=required(key); std::size_t used=0;
    try { target=std::stof(v,&used); }
    catch(...) { throw std::runtime_error("invalid number for "+key); }
    if(used!=v.size()) throw std::runtime_error("invalid number for "+key);
  };
  TerrainMetadata result;
  result.layout.channels={parse_role(required("channel.r")),parse_role(required("channel.g")),
                          parse_role(required("channel.b")),parse_role(required("channel.a"))};
  number("min_height",result.settings.min_height);
  number("max_height",result.settings.max_height);
  auto optional_u32=[&](const std::string& key, std::uint32_t& target) {
    const auto it=values.find(key); if(it==values.end()) return;
    std::size_t used=0; unsigned long long parsed=0;
    try { parsed=std::stoull(it->second,&used); }
    catch(...) { throw std::runtime_error("invalid integer for "+key); }
    if(used!=it->second.size() || parsed>std::numeric_limits<std::uint32_t>::max())
      throw std::runtime_error("invalid integer for "+key);
    target=static_cast<std::uint32_t>(parsed);
  };
  auto optional_u8=[&](const std::string& key, std::uint8_t& target) {
    std::uint32_t parsed=target; optional_u32(key,parsed);
    if(parsed>255) throw std::runtime_error(key+" must be in [0,255]");
    target=static_cast<std::uint8_t>(parsed);
  };
  optional_u32("volume_height",result.settings.volume_height);
  optional_u32("sea_level",result.settings.sea_level);
  optional_u32("subsurface_depth",result.settings.subsurface_depth);
  optional_u8("water_threshold",result.settings.water_threshold);
  optional_u8("default_material",result.settings.default_material);
  optional_u8("subsurface_material",result.settings.subsurface_material);
  optional_u8("deep_material",result.settings.deep_material);
  optional_u8("water_material",result.settings.water_material);
  optional_u8("default_biome",result.settings.default_biome);
  optional_u8("solid_density",result.settings.solid_density);
  result.layout.validate();
  for(const auto& [key,value]:values) {
    (void)value;
    if(key!="channel.r" && key!="channel.g" && key!="channel.b" && key!="channel.a" &&
       key!="min_height" && key!="max_height" && key!="volume_height" &&
       key!="sea_level" && key!="subsurface_depth" && key!="water_threshold" &&
       key!="default_material" && key!="subsurface_material" &&
       key!="deep_material" && key!="water_material" && key!="default_biome" &&
       key!="solid_density")
      throw std::runtime_error("unknown metadata key: "+key);
  }
  return result;
}
} // namespace rgba_terrain
