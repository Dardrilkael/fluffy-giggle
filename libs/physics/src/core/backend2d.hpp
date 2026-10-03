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
// World2D::transforms() serves. The lib writes it on create/set and the
// backend refreshes it in bulk after every tick (copy_transforms); the
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
  // its accumulated forces/torques by return.
  virtual void step(float dt, IEventCollector2D& collector) = 0;

  // Refresh the lib's slot-indexed pose mirror for every slot holding a
  // live backend body (slots it does not hold are left untouched).
  virtual void copy_transforms(std::span<physics2d::Transform> out) const = 0;

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

// Backends (defined in src/backend/<name>): the null backend exists so the
// lib machinery builds and tests standalone; Box2D v3 is the real 2D engine.
[[nodiscard]] std::unique_ptr<IWorld2DBackend> make_null_world_2d(
    const BackendDesc2D& desc);

}  // namespace ws::physics::detail
