# Included by the workspace's experiments/CMakeLists.txt.
if(NOT TARGET ws::image)
  message(FATAL_ERROR "RGBA terrain POC requires ws::image. Configure the workspace with WS_BUILD_LIBS=image (or build all libraries).")
endif()

option(RGBA_TERRAIN_BUILD_TESTS "Build RGBA terrain tests" ON)

add_library(rgba_terrain_core
  src/rgba_terrain.cpp
  src/metadata.cpp
  src/image_io.cpp)
target_include_directories(rgba_terrain_core PUBLIC include)
target_compile_features(rgba_terrain_core PUBLIC cxx_std_20)
target_link_libraries(rgba_terrain_core PUBLIC ws::image)

if(MSVC)
  target_compile_options(rgba_terrain_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(rgba_terrain_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(rgba_terrain_poc src/main.cpp)
target_link_libraries(rgba_terrain_poc PRIVATE rgba_terrain_core)

if(RGBA_TERRAIN_BUILD_TESTS)
  enable_testing()
  add_executable(rgba_terrain_tests tests/test_rgba_terrain.cpp)
  target_link_libraries(rgba_terrain_tests PRIVATE rgba_terrain_core)
  add_test(NAME rgba_terrain_tests COMMAND rgba_terrain_tests)
endif()
