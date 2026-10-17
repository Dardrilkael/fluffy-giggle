#pragma once

// The 2D traits for WorldCore (world_core.hpp): the type aliases + the
// dimension-specific predicates the machinery needs. Internal like the
// rest of src/core - never under include/. It lives in a header, not the
// world .cpp, so the conformance suite (tests/test_conformance.cpp) can
// instantiate the full machinery over ANY backend in one binary - the
// same WorldCore<Dim2D> the public World2D runs, driven over Box2D and
// the reference backend side by side.

#include <cmath>

#include "core/backend2d.hpp"

namespace ws::physics::detail {

// Field names mirror dim_3d.hpp exactly - that mirror discipline is what
// lets the machinery be type-parameterized.
struct Dim2D {
  using Vec = ws::math::Vec2;
  using Angular = float;
  using Rotation = float;  // a 2D rotation is a bare angle
  using BodyId = physics2d::BodyId;
  using JointId = physics2d::JointId;
  using BodyDesc = physics2d::BodyDesc;
  using BodyState = physics2d::BodyState;
  using Transform = physics2d::Transform;
  using BodyMove = physics2d::BodyMove;
  using ContactEvent = physics2d::ContactEvent;
  using Backend = IWorld2DBackend;
  using BackendJoint = BackendJoint2D;
  using BackendEvent = BackendEvent2D;
  using BackendMove = BackendMove2D;
  using BackendVelocity = BackendVelocity2D;
  using CollectorBase = IEventCollector2D;
  // Lib-computed mass (CONTRACT.md 4): the trait routes WorldCore to the
  // dimension's closed-form. Same mirror discipline as every alias above.
  using MassData = MassData2D;

  [[nodiscard]] static MassData mass_data(const BodyDesc& d) {
    return mass_data_2d(d);
  }

  // Finiteness predicates for the runtime setters (world_core.hpp): a NaN
  // must fail the call, never reach the engine.
  [[nodiscard]] static bool is_finite(Vec v) {
    return std::isfinite(v.x) && std::isfinite(v.y);
  }
  [[nodiscard]] static bool is_finite(float v) { return std::isfinite(v); }
  // Any finite angle is already canonical - nothing to sanitize.
  [[nodiscard]] static bool is_valid_rotation(float angle) {
    return std::isfinite(angle);
  }
  static void sanitize_rotation(float&) {}
};

}  // namespace ws::physics::detail
