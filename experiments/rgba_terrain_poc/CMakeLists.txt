# Included by the workspace's experiments/CMakeLists.txt.
if(NOT TARGET ws::image)
  message(FATAL_ERROR "RGBA terrain POC requires ws::image. Configure the workspace with WS_BUILD_LIBS=image (or build all libraries).")
endif()

option(RGBA_TERRAIN_BUILD_TESTS "Build RGBA terrain tests" ${WS_BUILD_TESTS})

add_library(rgba_terrain_core
  src/rgba_terrain.cpp
  src/metadata.cpp
  src/image_io.cpp
  src/synthetic_map.cpp
  src/map_analysis.cpp)
target_include_directories(rgba_terrain_core PUBLIC include)
target_compile_features(rgba_terrain_core PUBLIC cxx_std_20)
target_link_libraries(rgba_terrain_core PUBLIC ws::image)

ws_set_warnings(rgba_terrain_core)

# Use the workspace app helper so this executable inherits the same C++,
# warning, linking, and build-output conventions as apps under apps/.
# The root CMakeLists places the resulting .exe in build/<preset>/bin.
ws_add_app(rgba_terrain_poc
  SOURCES src/main.cpp
  DEPENDS rgba_terrain_core)

if(RGBA_TERRAIN_BUILD_TESTS)
  enable_testing()
  add_executable(rgba_terrain_tests tests/test_rgba_terrain.cpp)
  target_link_libraries(rgba_terrain_tests PRIVATE rgba_terrain_core)
  ws_set_warnings(rgba_terrain_tests)
  # The current terrain test uses assert(); keep its checks active in Release.
  if(MSVC)
    target_compile_options(rgba_terrain_tests PRIVATE /UNDEBUG)
  else()
    target_compile_options(rgba_terrain_tests PRIVATE -UNDEBUG)
  endif()
  add_test(NAME rgba_terrain_tests COMMAND rgba_terrain_tests)
endif()
