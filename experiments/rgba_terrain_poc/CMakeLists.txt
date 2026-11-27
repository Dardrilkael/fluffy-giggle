# Included by the workspace's experiments/CMakeLists.txt.
if(NOT TARGET ws::image OR NOT TARGET ws::core)
  message(FATAL_ERROR "RGBA terrain POC requires ws::image and ws::core. Configure with WS_BUILD_LIBS=image (or build all libraries).")
endif()

option(RGBA_TERRAIN_BUILD_TESTS "Build RGBA terrain tests" ${WS_BUILD_TESTS})

# Keep the experiment-local implementation out of the workspace library
# registry until its API is mature enough to become a reusable ws:: library.
# The executable and tests compile the same implementation sources directly;
# the executable itself links only workspace-owned ws:: targets.
set(RGBA_TERRAIN_SOURCES
  src/rgba_terrain.cpp
  src/metadata.cpp
  src/image_io.cpp
  src/synthetic_map.cpp
  src/map_analysis.cpp)

ws_add_app(rgba_terrain_poc
  SOURCES src/main.cpp ${RGBA_TERRAIN_SOURCES}
  DEPENDS ws::core ws::image)
target_include_directories(rgba_terrain_poc PRIVATE include)

if(RGBA_TERRAIN_BUILD_TESTS)
  enable_testing()
  add_executable(rgba_terrain_tests
    tests/test_rgba_terrain.cpp
    ${RGBA_TERRAIN_SOURCES})
  target_include_directories(rgba_terrain_tests PRIVATE include)
  target_link_libraries(rgba_terrain_tests PRIVATE ws::image)
  ws_set_warnings(rgba_terrain_tests)
  # The current terrain test uses assert(); keep its checks active in Release.
  if(MSVC)
    target_compile_options(rgba_terrain_tests PRIVATE /UNDEBUG)
  else()
    target_compile_options(rgba_terrain_tests PRIVATE -UNDEBUG)
  endif()
  add_test(NAME rgba_terrain_tests COMMAND rgba_terrain_tests)
endif()
