// apps/voxel_game - player.cpp: backend-independent FPS character movement.

#include "player.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ws::physics3d {
namespace {

[[nodiscard]] ws::math::Vec3 yaw_forward(float yaw) noexcept {
  return {std::sin(yaw), 0.0f, -std::cos(yaw)};
}
[[nodiscard]] ws::math::Vec3 yaw_right(float yaw) noexcept {
  return {std::cos(yaw), 0.0f, std::sin(yaw)};
}

// Move horizontal velocity toward a target without changing its direction
// abruptly. The Y component is preserved for independent jump/gravity logic.
[[nodiscard]] ws::math::Vec3 approach_horizontal(
    ws::math::Vec3 current, ws::math::Vec3 target, float max_delta) noexcept {
  const float dx = target.x - current.x;
  const float dz = target.z - current.z;
  const float distance = std::sqrt(dx * dx + dz * dz);
  if (distance < 1.0e-6f || distance <= max_delta) {
    current.x = target.x;
    current.z = target.z;
  } else {
    const float scale = max_delta / distance;
    current.x += dx * scale;
    current.z += dz * scale;
  }
  return current;
}

}  // namespace

bool CharacterController::spawn(const ws::math::Vec3& center) noexcept {
  position_ = center;
  velocity_ = {};
  grounded_ = false;
  coyote_time_ = 0.0f;
  jump_buffer_ = 0.0f;
  // A spawn starts a fresh run facing the default north direction.
  yaw_ = 0.0f;
  pitch_ = 0.0f;
  return true;
}

ws::physics3d::Aabb CharacterController::bounds() const noexcept {
  const ws::math::Vec3 half{tuning_.half_width, tuning_.half_height,
                            tuning_.half_width};
  return {position_ - half, position_ + half};
}

void Player::tick(const MoveInput& input, float dt,
                  const ws::physics3d::VoxelGrid3D& grid) noexcept {
  if (!std::isfinite(dt) || dt <= 0.0f) return;
  dt = std::min(dt, 0.05f);

  yaw_ += input.look_dx * tuning_.mouse_sensitivity;
  pitch_ -= input.look_dy * tuning_.mouse_sensitivity;
  constexpr float kPitchLimit = 1.5533f;
  pitch_ = std::clamp(pitch_, -kPitchLimit, kPitchLimit);
  constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
  yaw_ = std::remainder(yaw_, kTwoPi);

  ws::math::Vec3 move{};
  if (input.forward) move = move + yaw_forward(yaw_);
  if (input.back) move = move - yaw_forward(yaw_);
  if (input.right) move = move + yaw_right(yaw_);
  if (input.left) move = move - yaw_right(yaw_);
  const float speed = input.sprint ? tuning_.sprint_speed : tuning_.walk_speed;
  const float move_len = std::sqrt(move.x * move.x + move.z * move.z);
  if (move_len > 1.0e-6f) {
    move.x = move.x / move_len * speed;
    move.z = move.z / move_len * speed;
  }

  constexpr float kJumpBufferSeconds = 0.12f;
  constexpr float kCoyoteSeconds = 0.10f;
  constexpr float kGroundProbeDistance = 0.06f;

  // Resolve support before integrating gravity. This treats every solid
  // voxel as a valid floor (including placed blocks) and removes tiny gaps
  // caused by collision skin / floating-point drift.
  bool supported = false;
  if (velocity_.y <= 0.0f) {
    const auto support = grid.sweep_aabb(
        bounds(), ws::math::Vec3{0.0f, -kGroundProbeDistance, 0.0f},
        tuning_.collision_skin);
    if (support.grounded()) {
      position_ = (support.bounds.min + support.bounds.max) * 0.5f;
      velocity_.y = 0.0f;
      supported = true;
    }
  }
  grounded_ = supported;

  // Ground movement builds speed quickly; in the air, steering is weaker
  // and existing horizontal momentum is retained. Releasing movement applies
  // friction rather than stopping the player instantly.
  if (move_len > 1.0e-6f) {
    const float acceleration =
        grounded_ ? tuning_.ground_acceleration : tuning_.air_acceleration;
    velocity_ = approach_horizontal(velocity_, move, acceleration * dt);
  } else {
    const float friction =
        grounded_ ? tuning_.ground_friction : tuning_.air_drag;
    velocity_ = approach_horizontal(velocity_, ws::math::Vec3{}, friction * dt);
  }

  jump_buffer_ = input.jump ? kJumpBufferSeconds
                            : std::max(0.0f, jump_buffer_ - dt);
  coyote_time_ = grounded_ ? kCoyoteSeconds
                           : std::max(0.0f, coyote_time_ - dt);

  const bool jumping = jump_buffer_ > 0.0f && coyote_time_ > 0.0f;
  if (jumping) {
    velocity_.y = tuning_.jump_speed;
    jump_buffer_ = 0.0f;
    coyote_time_ = 0.0f;
    grounded_ = false;
  } else if (!grounded_) {
    // Gravity is only integrated while airborne. A supported player has
    // zero vertical velocity and does not accumulate downward acceleration.
    velocity_.y = std::max(velocity_.y + tuning_.gravity * dt,
                           -tuning_.max_fall);
  } else {
    velocity_.y = 0.0f;
  }

  const ws::math::Vec3 displacement = velocity_ * dt;
  const auto moved = grid.sweep_aabb(bounds(), displacement,
                                     tuning_.collision_skin);
  position_ = (moved.bounds.min + moved.bounds.max) * 0.5f;
  if (moved.hit_x) velocity_.x = 0.0f;
  if (moved.hit_y) velocity_.y = 0.0f;
  if (moved.hit_z) velocity_.z = 0.0f;

  // A downward collision is a floor; an upward collision is a ceiling.
  // Never mark the player grounded when their head hits a block.
  if (moved.grounded()) {
    grounded_ = true;
    velocity_.y = 0.0f;
    coyote_time_ = kCoyoteSeconds;
  } else if (jumping || velocity_.y > 0.0f) {
    grounded_ = false;
  } else {
    grounded_ = supported;
  }
}

ws::math::Vec3 Player::eye_position() const noexcept {
  return {position_.x,
          position_.y + (tuning_.eye_height - tuning_.half_height),
          position_.z};
}

ws::math::Vec3 Player::look_direction() const noexcept {
  const float cp = std::cos(pitch_);
  return {std::sin(yaw_) * cp, std::sin(pitch_), -std::cos(yaw_) * cp};
}

}  // namespace ws::physics3d
