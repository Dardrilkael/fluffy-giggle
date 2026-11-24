// apps/voxel_game - renderer.hpp: the game-space -> r3d draw bridge.
//
// One job: given the player's GAME-space pose and the world's chunk
// meshes, set the r3d scene (camera + light) and draw every mesh. The
// coordinate mirror lives here and in the mesher (world.cpp's note):
// game (x, y, z) -> render (x, -y, z), so the camera's eye/target are
// mirrored the same way, with the camera's up (0, -1, 0) - render +y
// is DOWN, so render-up is -y... spelled: the r2d::Camera3D defaults
// have up (0, -1, 0) which IS "game +y up" after the mirror.
//
// The camera's look direction: the player's look_direction() is GAME
// space; mirrored it is (dx, -dy, dz). The camera's eye looks at
// target = eye + mirrored direction.

#pragma once

#include <ws/math/vecmath.hpp>
#include <ws/ui/renderer2d.hpp>  // Camera3D
#include <ws/ui/renderer3d.hpp>  // draw_mesh, set_light

#include "player.hpp"
#include "world.hpp"

namespace voxel {

// The scene's fixed light: mid-afternoon, a bit overhead and from the
// east - the terrain's top faces catch the full term, north/south
// faces stay dimmer (the classic voxel read).
[[nodiscard]] ws::ui::r3d::Light3D scene_light();

// Sets the r3d scene for the frame: the camera from the player's pose
// (mirrored), the scene light. Call inside on_update, before the
// chunk draws.
void begin_scene(const Player& player, float aspect);

// Draws every chunk mesh at its tint, one draw_mesh per mesh, inside
// the CURRENT begin(Camera3D) block. Layer 0: the HUD rides higher.
void draw_world(const World& world);

}  // namespace voxel
