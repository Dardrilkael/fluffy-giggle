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
#
# RTTI off on non-MSVC, matching Jolt's own build (its CMake defaults
# CPP_RTTI_ENABLED OFF -> -fno-rtti on clang/gcc). The adapter subclasses
# Jolt types (jolt3d_3d.cpp's LayerMaskFilter : JPH::GroupFilter), and
# under the Itanium ABI an RTTI-enabled TU that subclasses a -fno-rtti
# base references a typeinfo that is never emitted (the key-function TU
# skips it) - undefined at the final .so link. MSVC's typeinfo emission
# links fine, so only the NDK leg needs the flag. The lib itself uses no
# typeid/dynamic_cast. NOTE: a static-lib compile-check cannot catch
# this - since backend_factory.cpp references every backend, even a
# 2D-only app pulls jolt3d_3d.cpp.o at link time; only linking an actual
# image (e.g. libbounce.so) resolves the archive.
if(NOT MSVC)
  target_compile_options(ws_physics PRIVATE -fvisibility=hidden -fno-rtti)
endif()

# Dev probe (default OFF): default BOTH dimensions onto their reference
# backends (src/backend/reference2d, src/backend/reference3d) instead of
# Box2D/Jolt - flips make_world_backend_2d/3d in src/core/
# backend_factory.cpp. The references compute detection, events,
# ballistics and queries with NO solver response, CCD, joints or sleep
# onset, so machinery cases that need resting contact or joints fail
# against them honestly; the point is running the lib's machinery under a
# second implementation, not a "better" engine.
option(WS_PHYSICS_DEFAULT_BACKEND_REFERENCE
       "ws::physics: default both worlds to their reference backends (dev probe)"
       OFF)
if(WS_PHYSICS_DEFAULT_BACKEND_REFERENCE)
  target_compile_definitions(ws_physics PRIVATE
                              WS_PHYSICS_DEFAULT_BACKEND_REFERENCE=1)
endif()

ws_add_test(physics
  SOURCES tests/test_common.cpp tests/test_world_2d.cpp tests/test_world_3d.cpp
          tests/test_world_2d_box2d.cpp tests/test_world_3d_jolt.cpp
          tests/test_mass_properties.cpp tests/test_material.cpp
          tests/test_reference_2d.cpp tests/test_conformance.cpp
          tests/test_reference_3d.cpp tests/test_conformance_3d.cpp)
# tests drive the internal machinery directly (the net pattern: src/ never
# reaches apps, but tests reach it through this PRIVATE path).
if(TARGET test_physics)
  target_include_directories(test_physics PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
