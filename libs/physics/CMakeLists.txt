ws_add_library(physics DEPENDS ws::core ws::math)

# Box2D (2D) and Jolt Physics (3D) are the simulation backends - private,
# like every third-party here: box2d.h and Jolt headers never appear under
# include/ (only src/backend/box2d and src/backend/jolt3d include them),
# and apps link only ws::physics, never the engines. The fetches in
# cmake/ThirdParty.cmake are unconditional: both backends build on
# Windows/MSVC and Android/NDK alike - never NOT-ANDROID-gated (locked
# design decision).
target_link_libraries(ws_physics PRIVATE box2d Jolt)

# Hidden by default on GCC/Clang; WS_PHYSICS_API (visibility.hpp) is the
# only exported surface - the ws_net pattern. MSVC needs no flag - its
# DLLs export nothing without __declspec anyway.
if(NOT MSVC)
  target_compile_options(ws_physics PRIVATE -fvisibility=hidden)
endif()

# Dev probe (default OFF): default the 2D world onto the reference backend
# (src/backend/reference2d) instead of Box2D - flips make_world_backend_2d
# in src/core/backend_factory.cpp. The reference computes detection, events,
# ballistics and queries with NO solver response, CCD, joints or sleep
# onset, so machinery cases that need resting contact or joints fail
# against it honestly; the point is running the lib's machinery under a
# second implementation, not a "better" engine.
option(WS_PHYSICS_DEFAULT_BACKEND_REFERENCE
       "ws::physics: default the 2D world to the reference backend (dev probe)"
       OFF)
if(WS_PHYSICS_DEFAULT_BACKEND_REFERENCE)
  target_compile_definitions(ws_physics PRIVATE
                              WS_PHYSICS_DEFAULT_BACKEND_REFERENCE=1)
endif()

ws_add_test(physics
  SOURCES tests/test_common.cpp tests/test_world_2d.cpp tests/test_world_3d.cpp
          tests/test_world_2d_box2d.cpp tests/test_world_3d_jolt.cpp
          tests/test_mass_properties.cpp tests/test_material.cpp
          tests/test_reference_2d.cpp)
# tests drive the internal machinery directly (the net pattern: src/ never
# reaches apps, but tests reach it through this PRIVATE path).
if(TARGET test_physics)
  target_include_directories(test_physics PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
