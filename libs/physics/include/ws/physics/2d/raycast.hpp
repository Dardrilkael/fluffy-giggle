#pragma once

#include <ws/physics/2d/body.hpp>

namespace ws::physics2d {

struct RayHit {
  BodyId body{};
  float t = 0.0f;   // distance along the ray, [0, max_distance]
  Vec2 point{};     // world space
  Vec2 normal{};    // unit length, points back toward the ray origin
};

}  // namespace ws::physics2d
