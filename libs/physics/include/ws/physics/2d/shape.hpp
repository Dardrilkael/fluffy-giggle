#pragma once

#include <variant>

#include <ws/math/vecmath.hpp>

namespace ws::physics2d {

using ws::math::Vec2;

// Shapes are VALUE types (locked design): plain data, no backend resource.
// The lib materializes the backend's native shape on first use and caches it
// keyed by shape value, so identical shapes share one backend shape (no
// per-body allocation for instanced bodies). Only the lib visits the
// variant, so adding shapes later is non-breaking.
struct Circle {
  float radius = 0.5f;  // m, > 0
};

struct Box {
  Vec2 half_extents{0.5f, 0.5f};  // m, both > 0
};

// Capsule = the segment a->b in body-local space, inflated by radius.
struct Capsule {
  Vec2 a{0.0f, -0.5f};
  Vec2 b{0.0f, 0.5f};
  float radius = 0.5f;  // m, > 0
};

using Shape = std::variant<Circle, Box, Capsule>;

// Axis-aligned query box for overlap_aabb (min <= max component-wise).
struct Aabb {
  Vec2 min{};
  Vec2 max{};
};

}  // namespace ws::physics2d
