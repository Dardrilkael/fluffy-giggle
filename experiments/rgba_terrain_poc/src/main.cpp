#include "rgba_terrain/metadata.hpp"
#include "rgba_terrain/rgba_terrain.hpp"
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace {
void write_u32(std::ostream& out,std::uint32_t v) {
  for(unsigned s=0;s<32;s+=8) out.put(static_cast<char>((v>>s)&255u));
}
}
int main(int argc,char** argv) {
#ifndef RGBA_TERRAIN_HAS_PNG
  (void)argc; (void)argv;
  std::cerr<<"PNG support was not found at configure time. Install libpng and reconfigure.\n";
  return 2;
#else
  if(argc!=4) {
    std::cerr<<"Usage: rgba_terrain_poc <input.png> <input.rgba-terrain> <output.voxcols>\n";
    return 2;
  }
  try {
    const auto metadata=rgba_terrain::load_metadata(argv[2]);
    const auto image=rgba_terrain::load_png_rgba(argv[1]);
    const auto terrain=rgba_terrain::generate_columns(image,metadata.layout,metadata.settings);
    std::ofstream out(argv[3],std::ios::binary);
    if(!out) throw std::runtime_error("cannot create output file");
    out.write("RGBAVX01",8);
    write_u32(out,static_cast<std::uint32_t>(terrain.width));
    write_u32(out,static_cast<std::uint32_t>(terrain.depth));
    for(const auto& c:terrain.columns) {
      write_u32(out,static_cast<std::uint32_t>(c.x));
      write_u32(out,static_cast<std::uint32_t>(c.z));
      write_u32(out,c.height);
      out.put(static_cast<char>(c.material_id)); out.put(static_cast<char>(c.biome_id));
      out.put(static_cast<char>(c.water_mask)); out.put(static_cast<char>(c.density));
    }
    if(!out) throw std::runtime_error("failed writing voxel-column output");
    std::cout<<"Converted "<<terrain.width<<"x"<<terrain.depth<<" RGBA pixels into "<<terrain.columns.size()<<" spatial voxel columns.\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"RGBA terrain conversion failed: "<<e.what()<<"\n"; return 1;
  }
#endif
}
