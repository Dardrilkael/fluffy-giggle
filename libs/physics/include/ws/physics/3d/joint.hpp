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

using JointDesc = std::variant<FixedJoint, HingeJoint, DistanceJoint>;

}  // namespace ws::physics3d
