// apps/voxel_game - player.cpp: backend-independent FPS character movement.

#include "player.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace voxel {
namespace {

[[nodiscard]] ws::math::Vec3 yaw_forward(float yaw) noexcept {
  return {std::sin(yaw), 0.0f, -std::cos(yaw)};
}
[[nodiscard]] ws::math::Vec3 yaw_right(float yaw) noexcept {
  return {std::cos(yaw), 0.0f, std::sin(yaw)};
}

}  // namespace

bool Player::spawn(const ws::math::Vec3& center) noexcept {
  position_ = center;
  velocity_ = {};
  grounded_ = false;
  coyote_time_ = 0.0f;
  jump_buffer_ = 0.0f;
  return true;
}

ws::physics3d::Aabb Player::bounds() const noexcept {
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
  if (yaw_ > std::numbers::pi_v<float>) {
    yaw_ -= 2.0f * std::numbers::pi_v<float>;
  } else if (yaw_ < -std::numbers::pi_v<float>) {
    yaw_ += 2.0f * std::numbers::pi_v<float>;
  }

  ws::math::Vec3 move{};
  if (input.forward) move = move + yaw_forward(yaw_);
  if (input.back) move = move - yaw_forward(yaw_);
  if (input.right) move = move + yaw_right(yaw_);
  if (input.left) move = move - yaw_right(yaw_);
  const float speed = input.sprint ? tuning_.sprint_speed : tuning_.walk_speed;
  const float move_len = std::sqrt(move.x * move.x + move.z * move.z);
  velocity_.x = move_len > 1.0e-6f ? move.x / move_len * speed : 0.0f;
  velocity_.z = move_len > 1.0e-6f ? move.z / move_len * speed : 0.0f;

  constexpr float kJumpBufferSeconds = 0.12f;
  constexpr float kCoyoteSeconds = 0.10f;
  jump_buffer_ = input.jump ? kJumpBufferSeconds
                            : std::max(0.0f, jump_buffer_ - dt);
  coyote_time_ = grounded_ ? kCoyoteSeconds
                           : std::max(0.0f, coyote_time_ - dt);

  if (grounded_ && velocity_.y < 0.0f) velocity_.y = 0.0f;
  if (jump_buffer_ > 0.0f && coyote_time_ > 0.0f) {
    velocity_.y = tuning_.jump_speed;
    jump_buffer_ = 0.0f;
    coyote_time_ = 0.0f;
    grounded_ = false;
  } else {
    velocity_.y = std::max(velocity_.y + tuning_.gravity * dt,
                           -tuning_.max_fall);
  }

  const ws::math::Vec3 displacement = velocity_ * dt;
  const auto moved = grid.sweep_aabb(bounds(), displacement,
                                     tuning_.collision_skin);
  position_ = (moved.bounds.min + moved.bounds.max) * 0.5f;
  if (moved.hit_x) velocity_.x = 0.0f;
  if (moved.hit_y) velocity_.y = 0.0f;
  if (moved.hit_z) velocity_.z = 0.0f;
  grounded_ = moved.grounded();

  // A short downward support probe keeps grounding stable over seams and
  // while standing still, without relying on contact-event timing.
  if (!grounded_ && velocity_.y <= 0.0f) {
    const auto probe = grid.sweep_aabb(
        bounds(), ws::math::Vec3{0.0f, -0.04f, 0.0f},
        tuning_.collision_skin);
    if (probe.grounded()) {
      position_ = (probe.bounds.min + probe.bounds.max) * 0.5f;
      velocity_.y = 0.0f;
      grounded_ = true;
    }
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

}  // namespace voxel
