# apps/voxel_game - the first-person voxel sandbox on the ws stack:
# ws::ui renders chunk meshes, ws::physics3d drives player collision,
# ws::audio synthesizes place/break one-shots, and ws::rgba_terrain_core
# supplies the generic RGBA voxel format. The app-owned terrain adapter
# translates generic voxel IDs into this game's block and biome vocabulary.
find_package(Threads REQUIRED)
ws_add_app(voxel_game DEPENDS ws::ui ws::physics ws::audio ws::gameplay ws::rgba_terrain_core)
target_sources(voxel_game PRIVATE src/terrain_import.cpp)
target_link_libraries(voxel_game PRIVATE Threads::Threads)

# The headless logic suite: worldgen determinism, independent frustum
# oracles, DDA raycasts, direct-grid movement, deterministic player replay,
# and menu/settings geometry. A separate terrain-import suite checks the
# RGBA-to-game adapter, preflight rejection and chunk-coordinate mapping. LINK
# names the default-link override (ws::ui - the suite's TUs include
# world.cpp, which includes ws/ui headers), and DEPENDS carries the rest
# (ws_add_test's LINK is SINGLE-value).
ws_add_test(voxel_logic
  SOURCES src/world.cpp src/terrain_import.cpp tests/test_voxel_logic.cpp
  LINK ws::ui
  DEPENDS ws::physics ws::audio ws::gameplay ws::rgba_terrain_core)
if(TARGET test_voxel_logic)
  # The suite uses the public VoxelGrid3D API; add the physics include
  # path explicitly because LINK is link-only for non-library consumers.
  target_include_directories(test_voxel_logic PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/libs/physics/include")
  target_link_libraries(test_voxel_logic PRIVATE Threads::Threads)
endif()

ws_add_test(voxel_terrain_import
  SOURCES src/world.cpp src/terrain_import.cpp tests/test_terrain_import.cpp
  LINK ws::ui
  DEPENDS ws::physics ws::gameplay ws::rgba_terrain_core)
if(TARGET test_voxel_terrain_import)
  target_include_directories(test_voxel_terrain_import PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/libs/physics/include")
endif()

# The frame harness: the app's own frame rendered headless on a real
# Vulkan device (lavapipe is enough) with the pixels READ BACK - the
# terrain/horizon/displacement pins behind the two field reports.
# Opt-in like ui_mesh_gpu (WS_VOXEL_GPU_TESTS), skips with a message
# when no device comes up.
option(WS_VOXEL_GPU_TESTS
  "Build the headless-GPU frame harness (needs a Vulkan device)" OFF)
if(WS_VOXEL_GPU_TESTS)
  # LINK is single-value (WsHelpers.cmake) - ws::ui carries the
  # default-link override (the TUs include renderer.cpp/world.cpp),
  # DEPENDS carries gfx/physics the same way the logic suite carries
  # its extras.
  ws_add_test(voxel_frame_gpu
    SOURCES src/renderer.cpp src/world.cpp
            tests/test_voxel_frame_gpu.cpp
    LINK ws::ui
    DEPENDS ws::gfx ws::physics ws::gameplay)
  if(TARGET test_voxel_frame_gpu)
    target_include_directories(test_voxel_frame_gpu PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${CMAKE_SOURCE_DIR}/libs/physics/include"
      "${CMAKE_SOURCE_DIR}/libs/ui/src")
    target_link_libraries(test_voxel_frame_gpu PRIVATE volk stb Threads::Threads)
  endif()
endif()
