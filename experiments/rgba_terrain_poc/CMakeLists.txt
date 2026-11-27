# Included by the workspace's experiments/CMakeLists.txt.
if(NOT TARGET ws::rgba_terrain_core)
  message(FATAL_ERROR "RGBA terrain POC requires ws::rgba_terrain_core. Configure with WS_BUILD_LIBS=rgba_terrain_core (or build all libraries).")
endif()

ws_add_app(rgba_terrain_poc
  SOURCES src/main.cpp
  DEPENDS ws::rgba_terrain_core)
