#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <ws/core/result.hpp>

namespace ws::physics {

// ws::physics's error codes (root CLAUDE.md, "Error handling"): every public
// failure in ws::physics2d / ws::physics3d returns a ws::core::Error with
// category "physics" and one of these as its code. The enum stays this lib's -
// core never knows these values. Codes are append-only: never renumber, only
// add.
enum class PhysicsError : std::int32_t {
  invalid_argument = 1,  // NaN/degenerate argument (bad shape dims, zero ray direction, ...)
  invalid_handle = 2,    // unknown, stale or never-issued BodyId/JointId
  world_full = 3,        // the world's body/joint capacity is exhausted
  backend_error = 4,     // a backend call failed; Result messages carry detail
  not_supported = 5,     // feature this backend lacks (reserved for future backends)
  invalid_state = 6,     // object not in a state that allows the call (moved-from world, ...)
};

// One-line description per code, from a static table. This is the ONLY
// message surface that never allocates (the workspace rule): the
// string_view points at this table and stays valid forever.
[[nodiscard]] constexpr std::string_view describe(PhysicsError code) noexcept {
  switch (code) {
    case PhysicsError::invalid_argument: return "invalid argument";
    case PhysicsError::invalid_handle: return "invalid handle";
    case PhysicsError::world_full: return "world is full";
    case PhysicsError::backend_error: return "backend error";
    case PhysicsError::not_supported: return "not supported";
    case PhysicsError::invalid_state: return "invalid state";
  }
  return "unknown physics error";
}

[[nodiscard]] inline ws::core::Error make_physics_error(PhysicsError code,
                                                        std::string message) {
  return ws::core::make_error(static_cast<std::int32_t>(code), "physics",
                              std::move(message));
}

// unexpected<Error> that converts into any Result<T>:
//   return fail(PhysicsError::invalid_handle, "body retired");
// RESULT-PATH ONLY: this allocates (Error::message is a std::string). Event
// and callback surfaces carry PhysicsError + describe() instead, never a
// dynamically built message. The name hides ws::core::fail inside
// ws::physics on purpose: physics errors go through the enum, never raw codes.
[[nodiscard]] inline ws::core::unexpected<ws::core::Error> fail(
    PhysicsError code, std::string message = {}) {
  if (message.empty()) {
    message = std::string{describe(code)};
  }
  return ws::core::unexpected<ws::core::Error>(
      make_physics_error(code, std::move(message)));
}

}  // namespace ws::physics
