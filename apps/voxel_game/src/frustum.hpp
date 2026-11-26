#pragma once

// Compatibility bridge: the implementation is reusable math, not voxel
// game logic. New consumers should include <ws/math/frustum.hpp> directly.
#include <ws/math/frustum.hpp>

namespace voxel {
namespace culling = ws::math::culling;
}  // namespace voxel
