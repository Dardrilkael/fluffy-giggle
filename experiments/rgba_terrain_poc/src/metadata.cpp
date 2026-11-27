#include "rgba_terrain/metadata.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
namespace rgba_terrain {
namespace {
ChannelRole parse_role(const std::string& s) {
  if(s=="unused") return ChannelRole::Unused;
  if(s=="height8") return ChannelRole::Height8;
  if(s=="height16_msb") return ChannelRole::Height16Msb;
  if(s=="height16_lsb") return ChannelRole::Height16Lsb;
  if(s=="material_id") return ChannelRole::MaterialId;
  if(s=="biome_id") return ChannelRole::BiomeId;
  if(s=="water_mask") return ChannelRole::WaterMask;
  if(s=="density") return ChannelRole::Density;
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
    const auto first=line.find_first_not_of(" \t\r");
    if(first==std::string::npos || line[first]=='#') continue;
    const auto equal=line.find('=',first);
    if(equal==std::string::npos) throw std::runtime_error("metadata line "+std::to_string(line_number)+" needs key=value");
    const auto key=line.substr(first,equal-first);
    const auto value=line.substr(equal+1);
    if(!values.emplace(key,value).second) throw std::runtime_error("duplicate metadata key: "+key);
  }
  auto required=[&](const std::string& key)->const std::string& {
    const auto it=values.find(key);
    if(it==values.end()) throw std::runtime_error("missing metadata key: "+key);
    return it->second;
  };
  auto number=[&](const std::string& key,float& target) {
    const auto& v=required(key); std::size_t used=0;
    target=std::stof(v,&used);
    if(used!=v.size()) throw std::runtime_error("invalid number for "+key);
  };
  TerrainMetadata result;
  result.layout.channels={parse_role(required("channel.r")),parse_role(required("channel.g")),
                          parse_role(required("channel.b")),parse_role(required("channel.a"))};
  number("min_height",result.settings.min_height);
  number("max_height",result.settings.max_height);
  result.layout.validate();
  for(const auto& [key,value]:values) {
    (void)value;
    if(key!="channel.r" && key!="channel.g" && key!="channel.b" && key!="channel.a" &&
       key!="min_height" && key!="max_height")
      throw std::runtime_error("unknown metadata key: "+key);
  }
  return result;
}
} // namespace rgba_terrain
