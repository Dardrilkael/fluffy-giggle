#pragma once

#include <cstdint>

#include <ws/math/vecmath.hpp>
#include <ws/physics/3d/shape.hpp>
#include <ws/physics/common.hpp>

namespace ws::physics3d {

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
  Vec3 position{};
  ws::math::Quat rotation{};  // identity by default (w = 1)
};

// One entry of the bulk set_transforms path.
struct BodyMove {
  BodyId id{};
  Vec3 position{};
  ws::math::Quat rotation{};
};

// Full per-body readback (pose + velocities) from body_state().
struct BodyState {
  Vec3 position{};
  ws::math::Quat rotation{};
  Vec3 linear_velocity{};
  Vec3 angular_velocity{};  // rad/s
};

struct BodyDesc {
  ws::physics::BodyKind kind = ws::physics::BodyKind::dynamic;
  Shape shape{Box{}};  // value type; equal shapes share one backend shape
  Transform transform{};
  Vec3 linear_velocity{};
  Vec3 angular_velocity{};  // rad/s
  float density = 1.0f;    // kg/m^3; > 0 for dynamic bodies
  float friction = 0.2f;
  float restitution = 0.0f;
  float gravity_scale = 1.0f;
  bool is_sensor = false;  // contact events fire, no collision response
  // Opaque per-body payload the lib stores but never interprets - a game
  // object index, or a pointer as uintptr_t. Contact events carry BodyIds
  // only; body_user_data() is the id -> payload lookup for that loop.
  std::uintptr_t user_data = 0;
  // Collision filtering: a and b collide iff (a.collision_layer &
  // b.collision_mask) != 0 AND (b.collision_layer & a.collision_mask) != 0
  // - both sides must accept the other. Defaults keep v1 behavior (collide
  // with everything). Sensors follow the same rule; queries ignore it.
  // Change at runtime via World3D::set_collision_filter.
  std::uint32_t collision_layer = 1u;
  std::uint32_t collision_mask = 0xffffffffu;
};

}  // namespace ws::physics3d
