#pragma once

#include <cstdint>
#include <variant>

#include <ws/physics/2d/body.hpp>

namespace ws::physics2d {

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

// Welds a and b at their relative pose AT CREATION.
struct FixedJoint {
  BodyId a{};
  BodyId b{};
};

// Pins a and b together at world_anchor, leaving rotation free. Limits and
// motor act on the relative rotation about the anchor (radians, CCW).
struct RevoluteJoint {
  BodyId a{};
  BodyId b{};
  Vec2 world_anchor{};
  bool enable_limits = false;
  float lower_angle = 0.0f;      // rad; requires lower <= upper
  float upper_angle = 0.0f;       // rad
  bool enable_motor = false;
  float motor_speed = 0.0f;       // target relative angular velocity, rad/s
  float max_motor_torque = 0.0f;  // N*m; must be > 0 when the motor is on
};

// Holds |position(a) - position(b)| at the given length.
struct DistanceJoint {
  BodyId a{};
  BodyId b{};
  float length = 1.0f;  // m, >= 0
};

using JointDesc = std::variant<FixedJoint, RevoluteJoint, DistanceJoint>;

}  // namespace ws::physics2d
