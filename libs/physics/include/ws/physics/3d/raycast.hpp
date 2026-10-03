#pragma once

#include <ws/physics/3d/body.hpp>

namespace ws::physics3d {

struct RayHit {
  BodyId body{};
  float t = 0.0f;  // distance along the ray, [0, max_distance]
  Vec3 point{};    // world space
  Vec3 normal{};   // unit length, points back toward the ray origin
};

}  // namespace ws::physics3d
