// apps/voxel_game - player.cpp: the controller the headless prototype
// validated (see libs/physics/tests/test_voxel_proto.cpp - the same
// numbers, the same velocity-exact FPS pattern, the same jump apex).
//
// The mouse-look integration happens in main.cpp (it owns the raw
// mouse deltas from the InputFrame); this tick takes the deltas as
// look_dx/dy and folds them into yaw/pitch here, keeping every
// game-space convention in one file.

#include "player.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <ws/core/log.hpp>
#include <ws/math/vecmath.hpp>

namespace voxel {

namespace {

// The horizontal move basis from the yaw: forward/back/right/left as
// unit GAME-space vectors. Yaw 0 looks north (-z); +yaw turns east.
[[nodiscard]] ws::math::Vec3 yaw_forward(float yaw) {
  return {std::sin(yaw), 0.0f, -std::cos(yaw)};
}
[[nodiscard]] ws::math::Vec3 yaw_right(float yaw) {
  return {std::cos(yaw), 0.0f, std::sin(yaw)};
}

}  // namespace

bool Player::spawn(ws::physics3d::World3D& world,
                   const ws::math::Vec3& center) {
  ws::physics3d::BodyDesc desc{};
  desc.kind = ws::physics::BodyKind::dynamic;
  // A capsule rolls over voxel corners instead of catching its box edges.
  // half_height is the inner segment; radius makes the total half-height
  // exactly tuning_.half_height (0.6 + 0.3 = 0.9).
  desc.shape = ws::physics3d::Capsule{
      tuning_.half_height - tuning_.half_width, tuning_.half_width};
  desc.transform = ws::physics3d::Transform{center, {}};
  // Zero friction: the controller owns the horizontal velocity
  // exactly every tick, so the solver's friction would only fight it
  // (the prototype's finding - walking was exact at friction 0).
  desc.friction = 0.0f;
  desc.restitution = 0.0f;
  desc.allow_sleep = false;  // a player never sleeps
  desc.gravity_scale = 1.0f;
  const auto id = world.create_body(desc);
  if (!id.has_value()) {
    ws::core::log_error(id.error());
    return false;
  }
  body_ = *id;
  position_ = center;
  return true;
}

void Player::tick(const MoveInput& input, float dt,
                  ws::physics3d::World3D& world) {
  // --- the look ----------------------------------------------------------------
  yaw_ += input.look_dx * tuning_.mouse_sensitivity;
  pitch_ -= input.look_dy * tuning_.mouse_sensitivity;
  constexpr float kPitchLimit = 1.5533f;  // ~89 degrees
  pitch_ = std::clamp(pitch_, -kPitchLimit, kPitchLimit);
  // Keep yaw in [-pi, pi): one wrap, no accumulation.
  if (yaw_ > std::numbers::pi_v<float>) {
    yaw_ -= 2.0f * std::numbers::pi_v<float>;
  } else if (yaw_ < -std::numbers::pi_v<float>) {
    yaw_ += 2.0f * std::numbers::pi_v<float>;
  }

  // --- the horizontal velocity (the FPS pattern) -----------------------------
  ws::math::Vec3 move{0.0f, 0.0f, 0.0f};
  if (input.forward) {
    move = move + yaw_forward(yaw_);
  }
  if (input.back) {
    move = move - yaw_forward(yaw_);
  }
  if (input.right) {
    move = move + yaw_right(yaw_);
  }
  if (input.left) {
    move = move - yaw_right(yaw_);
  }
  const float speed =
      input.sprint ? tuning_.sprint_speed : tuning_.walk_speed;
  ws::math::Vec3 horizontal{0.0f, 0.0f, 0.0f};
  const float len =
      std::sqrt(move.x * move.x + move.z * move.z);
  if (len > 1e-6f) {
    horizontal = ws::math::Vec3{move.x / len * speed, 0.0f,
                                move.z / len * speed};
  }

  // --- vertical motion: the physics world owns gravity ------------------------
  // Applying gravity here as well would double the world acceleration.
  // The controller only injects jump velocity and caps the fall speed.
  const auto state = world.body_state(body_);
  float vy = state.has_value() ? state->linear_velocity.y : 0.0f;

  constexpr float kJumpBufferSeconds = 0.12f;
  constexpr float kCoyoteSeconds = 0.10f;
  if (input.jump) {
    jump_buffer_ = kJumpBufferSeconds;
  } else {
    jump_buffer_ = std::max(0.0f, jump_buffer_ - dt);
  }
  if (grounded_) {
    coyote_time_ = kCoyoteSeconds;
  } else {
    coyote_time_ = std::max(0.0f, coyote_time_ - dt);
  }
  if (jump_buffer_ > 0.0f && coyote_time_ > 0.0f) {
    vy = tuning_.jump_speed;
    jump_buffer_ = 0.0f;
    coyote_time_ = 0.0f;
    grounded_ = false;
  }
  vy = std::max(vy, -tuning_.max_fall);

  (void)world.set_velocity(
      body_, ws::math::Vec3{horizontal.x, vy, horizontal.z},
      ws::math::Vec3{});

  // Cache the last known pose from the single state read. The world
  // advances after this controller tick; the camera/HUD are updated
  // again on the next frame.
  if (state.has_value()) {
    position_ = state->position;
    velocity_ = state->linear_velocity;
    velocity_.x = horizontal.x;
    velocity_.z = horizontal.z;
    velocity_.y = vy;
  }
}

void Player::on_contacts(
    std::span<const ws::physics3d::ContactEvent> events) {
  // Grounded while any touching pair's normal points up-ish (the
  // contact's normal points a -> b; the player is one side, so the
  // sign depends on which slot it occupies).
  bool grounded = false;
  for (const auto& event : events) {
    const bool player_is_a = event.a == body_;
    const bool player_is_b = event.b == body_;
    if (!player_is_a && !player_is_b) {
      continue;
    }
    if (event.phase == ws::physics3d::ContactPhase::ended) {
      continue;  // a lifting pair does not ground (the flag recomputes)
    }
    // The normal points a -> b. When the player is a, a normal with
    // +y points AWAY from the player's floor... the world's floor is
    // BELOW the player, so the contact's surface below pushes the
    // player UP only when the normal (a->b) has -y with the player as
    // b, or +y with the player as a. In practice: the floor body is
    // static, created EARLIER (lower slot index), so the player is
    // b and the normal points a -> b = up.
    const float ny = player_is_b ? event.normal.y : -event.normal.y;
    if (ny > 0.5f) {
      grounded = true;
    }
  }
  // An ended-only frame with no standing pair un-grounds. The decay is
  // one frame late at worst (the next frame's events restate it).
  grounded_ = grounded;
}

ws::math::Vec3 Player::eye_position() const {
  return {position_.x, position_.y + (tuning_.eye_height -
                                      tuning_.half_height),
          position_.z};
}

ws::math::Vec3 Player::look_direction() const {
  const float cp = std::cos(pitch_);
  return {std::sin(yaw_) * cp, std::sin(pitch_), -std::cos(yaw_) * cp};
}

}  // namespace voxel
