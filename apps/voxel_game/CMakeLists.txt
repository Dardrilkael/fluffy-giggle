# apps/voxel_game - the first-person voxel sandbox on the ws stack:
# ws::ui's r3d mesh path renders the chunk meshes, ws::physics3d drives
# the player's collision against a sparse static grid of surface blocks,
# and ws::audio synthesizes the place/break one-shots. One dependency
# set: ui, physics, audio - the bounce app's composition.
ws_add_app(voxel_game DEPENDS ws::ui ws::physics ws::audio)

# The headless logic suite: worldgen determinism, the culling rule,
# the DDA raycast - pure CPU, plus the capacity regression's World3D
# acceptance case. LINK names the default-link override (ws::ui - the
# suite's TUs include world.cpp, which includes ws/ui headers), and
# DEPENDS carries the rest (ws_add_test's LINK is SINGLE-value - putting
# the whole list there would parse just the first and silently drop
# physics and audio, which is exactly what the first registration did).
ws_add_test(voxel_logic
  SOURCES src/world.cpp tests/test_voxel_logic.cpp
  LINK ws::ui
  DEPENDS ws::physics ws::audio)
if(TARGET test_voxel_logic)
  # The suite grew a World3D acceptance case (the capacity regression:
  # every shell body + the player's own, in a real physics world), so
  # its TUs include ws/physics/... directly - the LINK line's link-only
  # contract does not hand the include path to a non-lib consumer, the
  # spell apps get from ws_add_app's DEPENDS. Added here the same way.
  target_include_directories(test_voxel_logic PRIVATE
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
    SOURCES src/renderer.cpp src/world.cpp src/player.cpp
            tests/test_voxel_frame_gpu.cpp
    LINK ws::ui
    DEPENDS ws::gfx ws::physics)
  if(TARGET test_voxel_frame_gpu)
    target_include_directories(test_voxel_frame_gpu PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${CMAKE_SOURCE_DIR}/libs/physics/include"
      "${CMAKE_SOURCE_DIR}/libs/ui/src")
    target_link_libraries(test_voxel_frame_gpu PRIVATE volk stb)
  endif()
endif()
