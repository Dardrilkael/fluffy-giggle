# apps/voxel_game - the first-person voxel sandbox on the ws stack:
# ws::ui's r3d mesh path renders the chunk meshes, ws::physics3d drives
# the player's collision against a sparse static grid of surface blocks,
# and ws::audio synthesizes the place/break one-shots. One dependency
# set: ui, physics, audio - the bounce app's composition.
ws_add_app(voxel_game DEPENDS ws::ui ws::physics ws::audio)

# The headless logic suite: worldgen determinism, the culling rule,
# the DDA raycast - all pure CPU, no GPU context needed. LINK spells
# the real targets (the default ws::voxel_logic does not exist) - the
# three libs the app's modules sit on; the app's src/ dir is the
# include path (the tests include "world.hpp").
ws_add_test(voxel_logic
  SOURCES src/world.cpp tests/test_voxel_logic.cpp
  LINK ws::ui ws::physics ws::audio)
if(TARGET test_voxel_logic)
  target_include_directories(test_voxel_logic PRIVATE
                              "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
