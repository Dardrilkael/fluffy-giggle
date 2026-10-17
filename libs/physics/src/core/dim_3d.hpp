#pragma once

// The 3D traits for WorldCore (world_core.hpp) - the exact mirror of
// dim_2d.hpp (field names match by design, so the machinery stays
// type-parameterized). Internal like the rest of src/core. It lives in a
// header, not the world .cpp, so the conformance suite can instantiate
// the machinery over any backend in one binary.

#include <cmath>

#include "core/backend3d.hpp"

namespace ws::physics::detail {

struct Dim3D {
  using Vec = ws::math::Vec3;
  using Angular = ws::math::Vec3;
  using Rotation = ws::math::Quat;
  using BodyId = physics3d::BodyId;
  using JointId = physics3d::JointId;
  using BodyDesc = physics3d::BodyDesc;
  using BodyState = physics3d::BodyState;
  using Transform = physics3d::Transform;
  using BodyMove = physics3d::BodyMove;
  using ContactEvent = physics3d::ContactEvent;
  using Backend = IWorld3DBackend;
  using BackendJoint = BackendJoint3D;
  using BackendEvent = BackendEvent3D;
  using BackendMove = BackendMove3D;
  using BackendVelocity = BackendVelocity3D;
  using CollectorBase = IEventCollector3D;
  // Lib-computed mass (CONTRACT.md 4): the trait routes WorldCore to the
  // dimension's closed-form. Same mirror discipline as every alias above.
  using MassData = MassData3D;

  [[nodiscard]] static MassData mass_data(const BodyDesc& d) {
    return mass_data_3d(d);
  }

  // Finiteness predicates for the runtime setters (world_core.hpp): a NaN
  // must fail the call, never reach Jolt (it asserts on contact).
  [[nodiscard]] static bool is_finite(Vec v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
  }
  [[nodiscard]] static bool is_finite(ws::math::Quat q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) &&
           std::isfinite(q.w);
  }
  // Jolt asserts on non-normalized rotations: zero-length fails here, and
  // everything else is normalized at the boundary (sanitize_rotation).
  [[nodiscard]] static bool is_valid_rotation(ws::math::Quat q) {
    return is_finite(q) && (q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w) > 0.0f;
  }
  // Pre-checked by is_valid_rotation - ws::math::normalize is deliberately
  // undefined on zero input (like GLM), the zero case never reaches it.
  static void sanitize_rotation(ws::math::Quat& q) { q = ws::math::normalize(q); }
};

}  // namespace ws::physics::detail
