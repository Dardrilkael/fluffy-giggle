#pragma once

// The 2D backend seam (internal - never included by a public header, the
// net `ILoopBackend` pattern). World2D's pimpl talks ONLY to this
// interface; Box2D v3 lives behind it in src/backend/box2d, so a backend
// swap touches no public header and no user code.
//
// Keying: the lib owns the generational handle tables; the backend never
// sees a BodyId/JointId. Bodies and joints are addressed by their lib
// SLOT index (dense uint32). The backend keeps its own slot -> engine
// id maps and pre-sizes them from BackendDesc (no allocation once the
// world is created).
//
// Poses: the lib owns the slot-indexed transform mirror that
// World2D::transforms() serves. The lib writes it on create/set; the
// backend reports every body it simulated each tick (step()'s moves_out),
// and the lib folds those into the mirror - a body that did not move
// already holds its current pose there, so nothing else is needed. The
// backend owns simulation truth, the lib owns the renderer-facing copy.
// Velocities are backend-owned and read per body (get_velocity).
//
// Forces: apply_force/apply_torque accumulate backend-side; the backend
// MUST clear the accumulation at the end of every step() - the public
// contract ("forces are cleared at the end of every tick") is enforced
// here, whichever engine is underneath.

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
#include <ws/physics/2d/world.hpp>

namespace ws::physics::detail {

// What the backend needs from World2D::Desc, distilled (fixed_dt is passed
// per step() instead - advance() runs several sizes).
struct BackendDesc2D {
  ws::math::Vec2 gravity{};
  std::uint32_t thread_count = 0;      // 0 = single-threaded
  float hit_speed_threshold = 0.0f;    // 0 = no hit events
  std::uint32_t max_bodies = 0;       // pre-size backend slot maps
  std::uint32_t max_joints = 0;
};

// Joint description with lib slots in place of the public BodyIds, flattened
// into one struct the backend switches on. Validated by the lib BEFORE the
// backend sees it.
struct BackendJoint2D {
  enum class Type : std::uint8_t { fixed, revolute, distance };
  Type type = Type::fixed;
  std::uint32_t slot_a = 0;
  std::uint32_t slot_b = 0;
  // revolute:
  ws::math::Vec2 world_anchor{};
  bool enable_limits = false;
  float lower_angle = 0.0f;
  float upper_angle = 0.0f;
  bool enable_motor = false;
  float motor_speed = 0.0f;
  float max_motor_torque = 0.0f;
  // distance:
  float length = 0.0f;
};

// One raw contact occurrence, in engine order. The lib's collector resolves
// slots to BodyIds, orders the pair by creation index and flips the normal
// accordingly - the backend reports in whatever order its engine produces.
struct BackendEvent2D {
  std::uint32_t slot_a = 0;
  std::uint32_t slot_b = 0;
  physics2d::ContactPhase phase{};
  ws::math::Vec2 point{};
  ws::math::Vec2 normal{};  // engine convention; the lib normalizes to a->b
  float speed = 0.0f;
};

// Where the backend pushes events during step(). Written straight into the
// lib's pre-reserved buffer - zero allocation per tick.
class IEventCollector2D {
 public:
  virtual ~IEventCollector2D() = default;
  virtual void add(const BackendEvent2D& event) = 0;
};

struct BackendRayHit2D {
  std::uint32_t slot = 0;
  float t = 0.0f;
  ws::math::Vec2 point{};
  ws::math::Vec2 normal{};
};

struct BackendVelocity2D {
  ws::math::Vec2 linear{};
  float angular = 0.0f;
};

// One body the backend simulated during a tick: the slot the lib assigned,
// plus the pose it ended the tick at. Asleep bodies never appear (they did
// not move); the lib folds these into its pose mirror.
struct BackendMove2D {
  std::uint32_t slot = 0;
  physics2d::Transform transform{};
};

class IWorld2DBackend {
 public:
  virtual ~IWorld2DBackend() = default;

  // Bodies. `slot` was just allocated by the lib; `desc` is pre-validated.
  [[nodiscard]] virtual ws::core::Status create_body(
      std::uint32_t slot, const physics2d::BodyDesc& desc) = 0;
  virtual void destroy_body(std::uint32_t slot) = 0;
  virtual void set_transform(std::uint32_t slot,
                             const physics2d::Transform& t) = 0;
  virtual void set_velocity(std::uint32_t slot, ws::math::Vec2 linear,
                            float angular) = 0;
  // Collision filter (BodyDesc::collision_layer/mask at create). The
  // backend owns the truth - the lib keeps no copy.
  virtual void set_filter(std::uint32_t slot, std::uint32_t layer,
                          std::uint32_t mask) = 0;
  virtual void apply_linear_impulse(std::uint32_t slot,
                                    ws::math::Vec2 impulse) = 0;
  virtual void apply_angular_impulse(std::uint32_t slot, float impulse) = 0;
  virtual void apply_force(std::uint32_t slot, ws::math::Vec2 force) = 0;
  virtual void apply_torque(std::uint32_t slot, float torque) = 0;
  [[nodiscard]] virtual BackendVelocity2D get_velocity(
      std::uint32_t slot) const = 0;

  // Joints. `slot` was just allocated by the lib; `joint` is pre-validated.
  [[nodiscard]] virtual ws::core::Status create_joint(
      std::uint32_t slot, const BackendJoint2D& joint) = 0;
  virtual void destroy_joint(std::uint32_t slot) = 0;

  // One fixed tick. The backend calls collector.add() for every began/ended/
  // hit event it produced during the tick, then returns; it must have cleared
  // its accumulated forces/torques by return. Events must be fed in
  // DETERMINISTIC order (engine array order per tick): which events the
  // lib's overflow cap drops must itself be deterministic, so a
  // multithreaded engine buffers per thread and merges in a fixed order.
  //
  // Moves: the backend writes one entry per body it simulated this tick
  // (awake bodies - asleep ones did not move and cost nothing) into
  // moves_out and returns the count. The span is pre-sized to max_bodies,
  // which bounds the per-tick awake count; writing past it is a backend
  // bug. Slots must be ones the lib assigned. Returns the entry count.
  virtual std::size_t step(float dt, IEventCollector2D& collector,
                           std::span<BackendMove2D> moves_out) = 0;

  // Queries (allocating by design - not the hot path). Ray direction is
  // non-zero; normalize it first. Miss = nullopt.
  [[nodiscard]] virtual std::optional<BackendRayHit2D> raycast(
      ws::math::Vec2 origin, ws::math::Vec2 direction,
      float max_distance) const = 0;
  virtual void overlap_circle(ws::math::Vec2 center, float radius,
                              std::vector<std::uint32_t>& out) const = 0;
  virtual void overlap_aabb(const physics2d::Aabb& box,
                            std::vector<std::uint32_t>& out) const = 0;
};

// The 2D backend: Box2D v3 (src/backend/box2d/box2d_2d.cpp). Returns
// nullptr only when engine/pool initialization failed; World2D::create
// turns that into a backend_error Result.
[[nodiscard]] std::unique_ptr<IWorld2DBackend> make_box2d_world_2d(
    const BackendDesc2D& desc);

}  // namespace ws::physics::detail
