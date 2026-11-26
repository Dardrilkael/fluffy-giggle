#pragma once

// The voxel app uses the shared physics controller without a wrapper
// object, vtable, or per-frame conversion.
#include <ws/physics/3d/character_controller.hpp>

namespace voxel {
using Player = ws::physics3d::CharacterController;
using PlayerTuning = ws::physics3d::CharacterTuning;
}  // namespace voxel
