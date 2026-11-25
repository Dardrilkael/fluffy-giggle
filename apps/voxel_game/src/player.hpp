// apps/voxel_game - player.hpp: the first-person controller.
//
// A physics3d dynamic capsule (0.6 wide x 1.8 tall) for collision
// against the static voxel grid. The controller owns horizontal velocity;
// the physics world alone integrates vertical gravity. A short coyote-time
// and jump-buffer window make grounding forgiving at voxel edges.
//
// Look: yaw/pitch from the mouse (the desktop path; Android gets a
// drag-look on the right half of the screen, a later-life toggle -
// this build is desktop-first, the Android entry still compiles and
// runs with the HUD and a tap-to-place).
//
// The look -> direction math is GAME space (yaw about +y, pitch about
// the horizontal). The forward vector: (sin(yaw)cos(pitch),
// sin(pitch), -cos(yaw)cos(pitch)) - yaw 0 looks north (-z), yaw
// +pi/2 east (+x), the standard FPS compass.
//
// Grounding is contact-driven, with short coyote-time and jump buffering
// to tolerate a one-frame contact gap at voxel seams.

#pragma once

#include <cstdint>

#include <ws/math/vecmath.hpp>
#include <ws/physics/3d/body.hpp>
#include <ws/physics/3d/contact.hpp>
#include <ws/physics/3d/shape.hpp>
#include <ws/physics/3d/world.hpp>

namespace voxel {

// The controller's tunables (the prototype's validated numbers).
struct PlayerTuning {
  float half_width = 0.3f;   // the box's x/z half extent (0.6 wide)
  float half_height = 0.9f;  // the box's y half extent (1.8 tall)
  float gravity = -32.0f;    // m/s^2 - 5x earth, the snappy arcade fall
  float jump_speed = 9.8f;   // m/s - ~1.5 m apex (sqrt(2 g h)): clears a
                             // full block with margin (the stronger jump)
  float walk_speed = 6.0f;   // m/s
  float sprint_speed = 10.5f;  // m/s (shift)
  float max_fall = 55.0f;     // m/s terminal clamp
  float eye_height = 1.72f;   // above the feet; slightly above classic eye level
  float mouse_sensitivity = 0.0025f;  // radians per pixel
};

class Player {
 public:
  // Creates the body in the world at the game-space spawn position
  // (the box's center - the caller places it half_height above the
  // surface). Returns false when the world refused the body.
  [[nodiscard]] bool spawn(ws::physics3d::World3D& world,
                           const ws::math::Vec3& center);

  // The controller tick: reads the InputFrame's key state (the GLFW
  // codes), integrates the look from the mouse deltas, drives the
  // body's velocity, then steps the world. dt is the frame's clamped
  // seconds (the caller advances the world; this only sets state).
  // moved/pressed come as plain arrays so this header needs no input
  // types - main.cpp passes the frame's fields.
  struct MoveInput {
    bool forward = false;
    bool back = false;
    bool left = false;
    bool right = false;
    bool jump = false;
    bool sprint = false;
    float look_dx = 0.0f;  // pixels this frame (the raw mouse delta)
    float look_dy = 0.0f;
  };
  void tick(const MoveInput& input, float dt,
            ws::physics3d::World3D& world);

  // Grounding bookkeeping from the world's contact events: call once
  // after world.advance() with the events span.
  void on_contacts(std::span<const ws::physics3d::ContactEvent> events);

  // The eye's game-space position (where the camera sits) and the look
  // direction (unit, GAME space).
  [[nodiscard]] ws::math::Vec3 eye_position() const;
  [[nodiscard]] ws::math::Vec3 look_direction() const;

  // The two tunables the app spells outside (the spawn height and the
  // physics world's gravity).
  [[nodiscard]] float tuning_gravity() const {
    return tuning_.gravity;
  }
  [[nodiscard]] float tuning_half_height() const {
    return tuning_.half_height;
  }

  // The body's game-space center + velocity (the HUD's readout).
  [[nodiscard]] ws::math::Vec3 position() const { return position_; }
  [[nodiscard]] ws::math::Vec3 velocity() const { return velocity_; }
  [[nodiscard]] bool grounded() const { return grounded_; }
  [[nodiscard]] ws::physics3d::BodyId body() const { return body_; }

 private:
  ws::physics3d::BodyId body_{};
  ws::math::Vec3 position_{};
  ws::math::Vec3 velocity_{};
  float yaw_ = 0.0f;   // about +y, 0 = north (-z)
  float pitch_ = 0.0f;  // about the horizontal, clamped +-89 deg
  bool grounded_ = false;
  float coyote_time_ = 0.0f;  // brief grace after leaving a floor edge
  float jump_buffer_ = 0.0f;  // brief grace when jump is pressed before landing
  PlayerTuning tuning_;
};

}  // namespace voxel
