// apps/voxel_game - renderer.cpp: the scene bridge (see renderer.hpp).

#include "renderer.hpp"

namespace voxel {

ws::ui::r3d::Light3D scene_light() {
  // The direction TO the light (r3d::set_light normalizes): from the
  // east and overhead in GAME space, mapped to render space by the
  // same proper rotation the mesher uses (x, -y, -z - the 180-degree
  // turn about the x axis).
  ws::ui::r3d::Light3D light;
  light.dir[0] = 0.55f;   // east
  light.dir[1] = -0.82f;  // GAME up -> render -y (the rotation)
  light.dir[2] = -0.18f;  // GAME slightly-north -> render -z
  light.color[0] = light.color[1] = light.color[2] = 1.0f;
  light.ambient = 0.35f;  // the shadow-side floor: dark, not black
  return light;
}

void begin_scene(const Player& player, float aspect) {
  // The player's GAME-space eye and look, mapped to render space by
  // world.hpp's game_to_render - the SAME map the mesher baked the
  // geometry with, so the camera and the world can never disagree
  // about the frame (the first build's mirror-vs-rotation drift is
  // exactly the bug this centralization closes).
  const ws::math::Vec3 eye = player.eye_position();
  const ws::math::Vec3 look = player.look_direction();

  ws::ui::r2d::Camera3D camera;
  camera.eye[0] = game_to_render(eye.x, 0);
  camera.eye[1] = game_to_render(eye.y, 1);
  camera.eye[2] = game_to_render(eye.z, 2);
  camera.target[0] = game_to_render(eye.x + look.x, 0);
  camera.target[1] = game_to_render(eye.y + look.y, 1);
  camera.target[2] = game_to_render(eye.z + look.z, 2);
  // up (0, -1, 0) is the default already: render +y is DOWN, so
  // "up" on screen is -y in render space = +y in game space.
  camera.up[0] = 0.0f;
  camera.up[1] = -1.0f;
  camera.up[2] = 0.0f;
  camera.fov_y = 1.2217f;  // 70 degrees, the FPS default
  camera.near_z = 0.1f;
  camera.far_z = 260.0f;   // the whole 128-block world is in view
  (void)aspect;

  ws::ui::r2d::begin(camera);
  (void)ws::ui::r3d::set_light(scene_light());
}

void draw_world(const World& world) {
  // Every chunk mesh is built in RENDER space at world-grid positions
  // already (the mesher's own note) - the identity transform draws it
  // exactly where it was baked. One draw per (chunk, type) mesh; the
  // painter's order within layer 0 does not matter (the mesh path is
  // depth-tested - the GPU's depth buffer resolves the geometry).
  for (const World::ChunkMesh& chunk : world.chunk_meshes()) {
    ws::ui::r3d::Transform3D identity{};  // translation 0, scale 1
    ws::ui::r3d::MeshMaterial material;
    material.tint = tint_of(chunk.type);
    ws::ui::r3d::draw_mesh(chunk.mesh, identity, material);
  }
}

}  // namespace voxel
