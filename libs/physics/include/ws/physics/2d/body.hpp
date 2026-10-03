#pragma once

#include <cstdint>

#include <ws/physics/2d/shape.hpp>
#include <ws/physics/common.hpp>

namespace ws::physics2d {

// Generational handle: O(1) lookup into the world's dense body arrays (no
// maps). A retired slot's generation advances, so stale handles error
// instead of aliasing. BodyId{} is never issued.
struct BodyId {
  std::uint32_t index = 0;
  std::uint32_t generation = 0;
};
[[nodiscard]] constexpr bool operator==(BodyId l, BodyId r) {
  return l.index == r.index && l.generation == r.generation;
}
[[nodiscard]] constexpr bool operator!=(BodyId l, BodyId r) {
  return !(l == r);
}

struct Transform {
  Vec2 position{};
  float rotation = 0.0f;  // radians, CCW about +Z
};

// One entry of the bulk set_transforms path.
struct BodyMove {
  BodyId id{};
  Vec2 position{};
  float rotation = 0.0f;
};

// Full per-body readback (pose + velocities) from body_state().
struct BodyState {
  Vec2 position{};
  float rotation = 0.0f;
  Vec2 linear_velocity{};
  float angular_velocity = 0.0f;  // rad/s
};

struct BodyDesc {
  ws::physics::BodyKind kind = ws::physics::BodyKind::dynamic;
  Shape shape{Box{}};  // value type; equal shapes share one backend shape
  Transform transform{};
  Vec2 linear_velocity{};
  float angular_velocity = 0.0f;  // rad/s
  float density = 1.0f;           // kg/m^2; > 0 for dynamic bodies
  float friction = 0.2f;
  float restitution = 0.0f;
  float gravity_scale = 1.0f;
  bool is_sensor = false;  // contact events fire, no collision response
};

}  // namespace ws::physics2d
