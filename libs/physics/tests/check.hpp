#pragma once

// Shared assertion helpers for the physics test suite (internal to tests/).

#include <cstdint>
#include <string_view>

#include <ws/core/result.hpp>
#include <ws/physics/errors.hpp>

namespace wsphys::test {

// True when `r` failed with exactly this PhysicsError and category.
// Category check pins the boundary conversion (root CLAUDE.md: core never
// knows lib codes - it must see "physics", never a raw int).
template <typename T>
[[nodiscard]] bool has_code(const ws::core::Result<T>& r,
                            ws::physics::PhysicsError e) {
  return !r.has_value() &&
         r.error().code == static_cast<std::int32_t>(e) &&
         std::string_view{r.error().category} == "physics";
}

}  // namespace wsphys::test
