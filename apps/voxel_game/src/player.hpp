// apps/voxel_game - player.hpp: first-person voxel character controller.
// Movement and collision are driven by ws::physics3d::VoxelGrid3D. The
// general rigid-body world remains available to other game entities, but
// the player and static terrain do not allocate rigid bodies.

#pragma once

#include <ws/math/vecmath.hpp>
#include <ws/physics/3d/voxel_grid.hpp>

namespace voxel {

struct PlayerTuning {
  float half_width = 0.3f;
  float half_height = 0.9f;
  float gravity = -24.0f;
  float jump_speed = 7.75f;
  float walk_speed = 6.0f;
  float sprint_speed = 10.5f;
  float max_fall = 55.0f;
  float eye_height = 1.72f;
  float mouse_sensitivity = 0.0025f;
  float collision_skin = 0.001f;
};

class Player {
 public:
  [[nodiscard]] bool spawn(const ws::math::Vec3& center) noexcept;

  struct MoveInput {
    bool forward = false;
    bool back = false;
    bool left = false;
    bool right = false;
    bool jump = false;
    bool sprint = false;
    float look_dx = 0.0f;
    float look_dy = 0.0f;
  };

  // Integrates movement and resolves the player's AABB against nearby
  // occupied voxels. dt is the clamped frame delta supplied by the app.
  void tick(const MoveInput& input, float dt,
            const ws::physics3d::VoxelGrid3D& grid) noexcept;

  [[nodiscard]] ws::physics3d::Aabb bounds() const noexcept;
  [[nodiscard]] ws::math::Vec3 eye_position() const noexcept;
  [[nodiscard]] ws::math::Vec3 look_direction() const noexcept;
  [[nodiscard]] float tuning_gravity() const noexcept {
    return tuning_.gravity;
  }
  [[nodiscard]] float tuning_half_height() const noexcept {
    return tuning_.half_height;
  }
  [[nodiscard]] ws::math::Vec3 position() const noexcept { return position_; }
  [[nodiscard]] ws::math::Vec3 velocity() const noexcept { return velocity_; }
  [[nodiscard]] bool grounded() const noexcept { return grounded_; }

 private:
  ws::math::Vec3 position_{};
  ws::math::Vec3 velocity_{};
  float yaw_ = 0.0f;
  float pitch_ = 0.0f;
  bool grounded_ = false;
  float coyote_time_ = 0.0f;
  float jump_buffer_ = 0.0f;
  PlayerTuning tuning_{};
};

}  // namespace voxel
