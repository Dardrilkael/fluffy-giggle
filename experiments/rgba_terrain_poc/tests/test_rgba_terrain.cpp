#include "rgba_terrain/rgba_terrain.hpp"
#include <cassert>
#include <stdexcept>
using namespace rgba_terrain;
int main() {
  RgbaImage image{2,1,{{255,7,3,0},{0,9,4,255}}};
  ChannelLayout layout; layout.channels={ChannelRole::Height8,ChannelRole::MaterialId,ChannelRole::BiomeId,ChannelRole::WaterMask};
  TerrainSettings settings; settings.max_height=100.0f;
  const auto terrain=generate_columns(image,layout,settings);
  assert(terrain.width==2 && terrain.depth==1 && terrain.columns.size()==2);
  assert(terrain.columns[0].height==100 && terrain.columns[0].material_id==7 && terrain.columns[0].biome_id==3);
  assert(terrain.columns[1].height==0 && terrain.columns[1].water_mask==255);
  ChannelLayout precise_layout; precise_layout.channels={ChannelRole::Height16Msb,ChannelRole::Height16Lsb,ChannelRole::MaterialId,ChannelRole::Unused};
  RgbaImage precise{1,1,{{0x80,0x00,12,255}}}; settings.max_height=65535.0f;
  const auto precise_out=generate_columns(precise,precise_layout,settings);
  assert(precise_out.columns[0].height==32768 && precise_out.columns[0].material_id==12);
  bool rejected=false;
  try { ChannelLayout invalid; invalid.channels={ChannelRole::Height16Msb,ChannelRole::MaterialId,ChannelRole::BiomeId,ChannelRole::Unused}; invalid.validate(); }
  catch (const std::invalid_argument&) { rejected=true; }
  assert(rejected); return 0;
}
