cmake_minimum_required(VERSION 3.20)
project(ai_terrain_poc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(ai_terrain
  src/terrain_analyzer.cpp
  src/fuzzy_controller.cpp
  src/test_terrain.cpp
  src/region_simulation.cpp
)
target_include_directories(ai_terrain PUBLIC include)
if(MSVC)
  target_compile_options(ai_terrain PRIVATE /W4 /permissive-)
else()
  target_compile_options(ai_terrain PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(ai_terrain_demo src/main.cpp)
target_link_libraries(ai_terrain_demo PRIVATE ai_terrain)

include(CTest)
if(BUILD_TESTING)
  add_executable(ai_terrain_tests tests/test_ai_terrain.cpp)
  target_link_libraries(ai_terrain_tests PRIVATE ai_terrain)
  add_test(NAME ai_terrain_tests COMMAND ai_terrain_tests)
endif()
