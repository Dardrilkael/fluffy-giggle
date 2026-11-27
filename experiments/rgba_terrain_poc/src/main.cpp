#include "rgba_terrain/rgba_terrain.hpp"
#include <iostream>
int main() {
#ifndef RGBA_TERRAIN_HAS_PNG
  std::cout << "RGBA terrain core is available; install libpng and reconfigure to enable PNG input.\n";
#else
  std::cout << "RGBA PNG input is enabled. Use the rgba_terrain library API to supply a ChannelLayout and generate voxel columns.\n";
#endif
  return 0;
}
