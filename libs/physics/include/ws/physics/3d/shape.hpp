#pragma once

#include <variant>

#include <ws/math/vecmath.hpp>

namespace ws::physics3d {

using ws::math::Vec3;

// Shapes are VALUE types (locked design): plain data, no backend resource.
// The lib materializes the backend's native shape on first use and caches it
// keyed by shape value, so identical shapes share one backend shape (no
// per-body allocation for instanced bodies). Only the lib visits the
// variant, so adding shapes later is non-breaking.
struct Sphere {
  float radius = 0.5f;  // m, > 0
};

struct Box {
  Vec3 half_extents{0.5f, 0.5f, 0.5f};  // m, all > 0
};

// Capsule axis-aligned along the local Y axis: the segment
// (0, -half_height, 0) - (0, +half_height, 0) inflated by radius. This is the
// 3D backend's native capsule form; the 2D lib keeps the general segment
// capsule, each dimension matching its backend's native shape.
struct Capsule {
  float half_height = 0.5f;  // half the inner segment's length, m, > 0
  float radius = 0.5f;       // m, > 0
};

using Shape = std::variant<Sphere, Box, Capsule>;

// Axis-aligned query box for overlap_aabb (min <= max component-wise).
struct Aabb {
  Vec3 min{};
  Vec3 max{};
};

}  // namespace ws::physics3d
