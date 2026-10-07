#pragma once

#include <array>
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
  // Continuous collision for a fast body. Box2D already sweeps every
  // fast-moving body against STATIC geometry on its own (free CCD vs
  // walls); this flag extends the sweep to the kinematic and DYNAMIC
  // trees, so the body also cannot tunnel through moving or resting
  // non-static targets. Off by default - the extra sweep costs solver
  // time, give it to the few bodies that earn it (projectile speeds).
  // Change at runtime via World2D::set_bullet.
  bool is_bullet = false;
  // Opaque per-body payload the lib stores but never interprets - a game
  // object index, or a pointer as uintptr_t. Contact events carry BodyIds
  // only; body_user_data() is the id -> payload lookup for that loop.
  std::uintptr_t user_data = 0;
  // Collision filtering: a and b collide iff (a.collision_layer &
  // b.collision_mask) != 0 AND (b.collision_layer & a.collision_mask) != 0
  // - both sides must accept the other. Defaults keep v1 behavior (collide
  // with everything). Sensors follow the same rule; queries ignore it.
  // Change at runtime via World2D::set_collision_filter.
  std::uint32_t collision_layer = 1u;
  std::uint32_t collision_mask = 0xffffffffu;
  // Multi-shape body: parts[0 .. part_count-1] define ALL shapes of the
  // body. part_count != 0 means the desc's shape/friction/restitution are
  // IGNORED for physics (still validated), density stays body-level, and
  // parts are SOLID-ONLY (is_sensor + parts fails create; sensors are
  // body-wide). A body pair may now report more than one contact event per
  // step - one per touching shape pair, each with its own phase transitions.
  //
  // Fixed array, not a std::vector: a desc copy (create_body sanitizes the
  // rotation through one) must stay allocation-free - a vector's first
  // iterator use heap-allocates MSVC's debug proxy and an empty vector
  // copy-assign RELEASES capacity, both of which break the zero-alloc
  // contract at every create.
  std::array<Part, ws::physics::kMaxPartsPerBody> parts{};
  std::uint32_t part_count = 0;
};

}  // namespace ws::physics2d
