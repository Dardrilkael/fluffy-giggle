#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/math/vecmath.hpp>
#include <ws/physics/2d/body.hpp>
#include <ws/physics/2d/contact.hpp>
#include <ws/physics/2d/joint.hpp>
#include <ws/physics/2d/raycast.hpp>
#include <ws/physics/common.hpp>
#include <ws/physics/errors.hpp>
#include <ws/physics/visibility.hpp>

namespace ws::physics2d {

// The 2D simulation. Box2D v3 lives behind the pimpl - no Box2D type appears
// here, and a backend swap touches no public header. Movable, not copyable;
// created via create(). All fallible calls return ws::core::Result/Status.
//
// Units: SI (m, kg, s). Bodies and joints are addressed by generational
// handles - stale handles return invalid_handle, never UB.
//
// Determinism: same binary, same thread_count, same call sequence produces
// identical results (tested). Cross-platform determinism is NOT promised.
//
// Threading: the world itself is NOT thread-safe - drive it from one thread;
// Desc::thread_count only sizes the backend's internal solver worker pool.
class WS_PHYSICS_API World2D {
 public:
  struct Desc {
    Vec2 gravity{0.0f, -9.81f};
    float fixed_dt = 1.0f / 60.0f;  // simulation tick, > 0
    std::uint32_t max_bodies = 4096;  // <= ws::physics::kMaxBodiesPerWorld
    std::uint32_t max_joints = 1024;  // <= ws::physics::kMaxJointsPerWorld
    std::uint32_t max_contact_events = 1024;  // buffer pre-reserved to this
    std::uint32_t thread_count =
        ws::physics::default_thread_count();  // 0 = single-threaded
    float hit_speed_threshold = 0.0f;  // m/s; 0 disables hit events
    std::uint32_t max_substeps = 8;    // advance() clamp (spiral-of-death)
  };

  // Creates the world and PRE-RESERVES every per-step buffer: steady-state
  // step()/advance() allocate nothing (verified by allocation counters in
  // the test suite).
  [[nodiscard]] static ws::core::Result<World2D> create(Desc desc);

  // Desc's documented defaults. (A defaulted parameter cannot do this
  // in-class: odr-using a default member initializer there needs the
  // enclosing class complete, which clang enforces.)
  [[nodiscard]] static ws::core::Result<World2D> create();

  // --- bodies -------------------------------------------------------------
  [[nodiscard]] ws::core::Result<BodyId> create_body(const BodyDesc& desc);
  ws::core::Status destroy_body(BodyId id);
  [[nodiscard]] ws::core::Result<BodyState> body_state(BodyId id) const;

  ws::core::Status set_transform(BodyId id, Vec2 position, float rotation);
  ws::core::Status set_velocity(BodyId id, Vec2 linear, float angular);

  // World-space impulse at the center of mass; wakes the body.
  ws::core::Status apply_linear_impulse(BodyId id, Vec2 impulse);
  ws::core::Status apply_angular_impulse(BodyId id, float impulse);

  // Accumulated forces/torques are cleared at the END of every tick -
  // guaranteed by this lib whichever backend is in use.
  ws::core::Status apply_force(BodyId id, Vec2 force);
  ws::core::Status apply_torque(BodyId id, float torque);

  // --- joints ---------------------------------------------------------------
  [[nodiscard]] ws::core::Result<JointId> create_joint(const JointDesc& desc);
  ws::core::Status destroy_joint(JointId id);

  // --- stepping ---------------------------------------------------------------
  // Advances EXACTLY one fixed_dt tick. The contact-event buffer is cleared
  // at the start of the tick, then filled by it.
  ws::core::Status step();

  // Accumulator form: consumes frame_dt, running at most Desc::max_substeps
  // ticks; leftover time is dropped (spiral-of-death protection). After
  // return, interpolation_alpha() is the fixed_dt fraction for render
  // lerping (lerp between your copy of last frame's transforms() and the
  // current one).
  ws::core::Status advance(float frame_dt);
  [[nodiscard]] float interpolation_alpha() const;  // [0, 1]
  [[nodiscard]] float fixed_dt() const;

  // --- events ---------------------------------------------------------------
  // The last tick's contact events. The span is valid until the next
  // step()/advance(). Events are sorted by (a.index, b.index, phase) -
  // deterministic regardless of which worker thread reported them. Overflow
  // beyond max_contact_events drops the newest events and warns once.
  [[nodiscard]] std::span<const ContactEvent> contact_events() const;

  // --- bulk renderer path ------------------------------------------------------
  // Slot-indexed by BodyId::index (one contiguous array, no per-body call in
  // the renderer's hot loop): transforms()[i] is the transform of the body
  // whose id.index == i. Entries for retired slots are stale - the renderer
  // indexes with the ids it holds. Copied out by the backend in bulk after
  // each tick.
  [[nodiscard]] std::span<const Transform> transforms() const;

  // Bulk teleport (kinematic targets etc.). ATOMIC: every entry is validated
  // before anything is applied; one invalid id fails the whole call and
  // nothing moves.
  ws::core::Status set_transforms(std::span<const BodyMove> moves);

  // --- queries -------------------------------------------------------------------
  // direction: any non-zero vector (normalized internally). Returns the
  // CLOSEST hit; a miss is std::nullopt, not an error.
  [[nodiscard]] ws::core::Result<std::optional<RayHit>>
  raycast(Vec2 origin, Vec2 direction, float max_distance) const;

  // Bodies whose shapes overlap the query region; sorted by creation index
  // (deterministic). One allocation per call - queries are not the hot path.
  [[nodiscard]] ws::core::Result<std::vector<BodyId>>
  overlap_circle(Vec2 center, float radius) const;
  [[nodiscard]] ws::core::Result<std::vector<BodyId>>
  overlap_aabb(Aabb box) const;

 public:
  // Movable (Result<World2D> must move the world out of create()), never
  // copyable; create() is the only constructor.
  World2D(World2D&&) noexcept;
  World2D& operator=(World2D&&) noexcept;
  ~World2D();

 private:
  World2D();
  World2D(const World2D&) = delete;
  World2D& operator=(const World2D&) = delete;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ws::physics2d
