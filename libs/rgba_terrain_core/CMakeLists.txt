ws_add_library(rgba_terrain_core DEPENDS ws::image)

ws_add_test(rgba_terrain_core
  SOURCES
    tests/test_rgba_terrain_core.cpp
    tests/test_voxel_file.cpp
    tests/test_world_sampler.cpp
    tests/test_map_analysis.cpp)
