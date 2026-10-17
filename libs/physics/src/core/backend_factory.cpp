// The backend factory: the ONE place that decides which backend a world
// gets (CONTRACT.md's "the lib owns semantics, backends only compute" needs
// a single switch point). World2D/World3D never name an engine - they call
// make_world_backend_2d/3d here - so swapping a default (or growing a
// runtime enum later) touches exactly this TU.
//
// WS_PHYSICS_DEFAULT_BACKEND_REFERENCE (the CMake option of the same name,
// default OFF) is a dev probe: it flips the 2D world onto the reference
// implementation - detection, events, ballistics and queries, but no solver
// response, CCD, joints or sleep onset (src/backend/reference2d). Machinery
// cases that need resting contact or joints fail against it honestly; the
// point is running the lib's machinery under a second implementation, not
// a "better" engine. 3D keeps Jolt under both settings for now - the 3D
// reference twin lands with the conformance suite (C7), which needs
// machinery-over-both-backends in one binary.

#include <memory>

#include "core/backend2d.hpp"
#include "core/backend3d.hpp"

namespace ws::physics::detail {

std::unique_ptr<IWorld2DBackend> make_world_backend_2d(
    const BackendDesc2D& desc) {
#if WS_PHYSICS_DEFAULT_BACKEND_REFERENCE
  return make_reference_world_2d(desc);
#else
  return make_box2d_world_2d(desc);
#endif
}

std::unique_ptr<IWorld3DBackend> make_world_backend_3d(
    const BackendDesc3D& desc) {
  return make_jolt_world_3d(desc);
}

}  // namespace ws::physics::detail
