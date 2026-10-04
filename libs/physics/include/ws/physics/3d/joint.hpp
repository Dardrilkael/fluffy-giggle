#pragma once

#include <cstdint>
#include <variant>

#include <ws/physics/3d/body.hpp>

namespace ws::physics3d {

// Generational handle, the BodyId discipline. JointId{} is never issued.
struct JointId {
  std::uint32_t index = 0;
  std::uint32_t generation = 0;
};
[[nodiscard]] constexpr bool operator==(JointId l, JointId r) {
  return l.index == r.index && l.generation == r.generation;
}
[[nodiscard]] constexpr bool operator!=(JointId l, JointId r) {
  return !(l == r);
}

// Every joint requires a != b and AT LEAST ONE DYNAMIC body: a joint
// anchoring two static bodies would be a permanent no-op the engines
// disagree about, so the lib rejects it with invalid_argument up front.
// (A dynamic-to-static joint is the normal anchor case and works.)

// Welds a and b at their relative pose AT CREATION.
struct FixedJoint {
  BodyId a{};
  BodyId b{};
};

// Hinge: pins a and b on the line (world_anchor, world_axis), leaving
// rotation about that axis free. Limits and motor act on the relative
// rotation about the axis (radians, right-handed around world_axis).
struct HingeJoint {
  BodyId a{};
  BodyId b{};
  Vec3 world_anchor{};
  Vec3 world_axis{0.0f, 1.0f, 0.0f};  // any non-zero vector, normalized internally
  bool enable_limits = false;
  float lower_angle = 0.0f;      // rad; requires lower <= upper
  float upper_angle = 0.0f;       // rad
  // Limits are measured against the UNWOUND relative angle: both must stay
  // within one turn (+-0.99*pi, checked by the lib - same as the 2D
  // revolute, and what Jolt's hinge expects).
  bool enable_motor = false;
  float motor_speed = 0.0f;       // target relative angular velocity, rad/s
  float max_motor_torque = 0.0f;  // N*m; must be > 0 when the motor is on
};

// Holds |position(a) - position(b)| at the given length.
struct DistanceJoint {
  BodyId a{};
  BodyId b{};
  // m, > 0: a zero-length rod is degenerate (that is a weld's job) and
  // Box2D hard-asserts it, so the lib rejects it up front.
  float length = 1.0f;
};

using JointDesc = std::variant<FixedJoint, HingeJoint, DistanceJoint>;

}  // namespace ws::physics3d
