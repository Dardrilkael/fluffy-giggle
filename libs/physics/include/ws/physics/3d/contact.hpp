#pragma once

#include <cstdint>

#include <ws/physics/3d/body.hpp>

namespace ws::physics3d {

enum class ContactPhase : std::uint8_t {
  began,  // the pair started touching during this tick
  ended,  // the pair stopped touching during this tick
  hit,    // an impact whose approach speed >= the world's hit_speed_threshold
};

// One buffered contact event. Events accumulate during step()/advance() and
// are drained via World3D::contact_events(). begin/end fire exactly once per
// overlap (no per-tick repeats while a pair rests); hit fires for impacts
// above the world's threshold. Sensors report began/ended only (CONTRACT.md
// 7: never hit) and carry no manifold geometry - zero point, zero normal,
// zero speed. Multi-shape bodies report per SHAPE pair: a body pair
// touching through two shape pairs can yield two began/ended cycles per
// step, each with its own transitions.
struct ContactEvent {
  ContactPhase phase{};
  BodyId a{};     // the body with the lower slot index (a.index < b.index)
  BodyId b{};
  Vec3 point{};   // one witness point, world space
  Vec3 normal{};  // unit length, points a -> b
  float speed = 0.0f;  // relative approach speed (hit events only)
};

}  // namespace ws::physics3d
