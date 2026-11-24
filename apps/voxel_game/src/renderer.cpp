// apps/voxel_game - renderer.cpp: the scene bridge (see renderer.hpp).

#include "renderer.hpp"

namespace voxel {

ws::ui::r3d::Light3D scene_light() {
  // The direction TO the light (r3d::set_light normalizes): from the
  // east and overhead in GAME space -> mirrored for the render scene
  // the same way everything else is (y negated).
  ws::ui::r3d::Light3D light;
  light.dir[0] = 0.55f;   // east
  light.dir[1] = -0.82f;  // GAME up -> render -y (the mirror)
  light.dir[2] = 0.18f;
  light.color[0] = light.color[1] = light.color[2] = 1.0f;
  light.ambient = 0.35f;  // the shadow-side floor: dark, not black
  return light;
}

void begin_scene(const Player& player, float aspect) {
  // The player's GAME-space eye and look, mirrored to render space.
  const ws::math::Vec3 eye = player.eye_position();
  const ws::math::Vec3 look = player.look_direction();

  ws::ui::r2d::Camera3D camera;
  camera.eye[0] = eye.x;
  camera.eye[1] = -eye.y;  // the mirror
  camera.eye[2] = eye.z;
  camera.target[0] = eye.x + look.x;
  camera.target[1] = -(eye.y + look.y);
  camera.target[2] = eye.z + look.z;
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
