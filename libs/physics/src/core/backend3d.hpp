#pragma once

// The 3D backend seam (internal - never included by a public header), the
// exact mirror of backend2d.hpp: World3D's pimpl talks ONLY to this
// interface; Jolt Physics v5 lives behind it in src/backend/jolt3d, so a
// backend swap touches no public header and no user code.
//
// Keying, pose ownership and force-clearing follow the 2D seam contract -
// see backend2d.hpp; only the types differ (Vec3 + Quat). One extra wrinkle
// specific to Jolt: contact events fire on solver worker threads during
// step(), so the backend buffers them per thread (pre-reserved) and feeds
// the collector in deterministic order after the tick completes. That is a
// REQUIREMENT, not a nicety: which events the lib's overflow cap drops must
// be deterministic. The lib's sort then fixes the public order.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/math/vecmath.hpp>
#include <ws/physics/3d/body.hpp>
#include <ws/physics/3d/contact.hpp>
#include <ws/physics/3d/joint.hpp>
#include <ws/physics/3d/raycast.hpp>
#include <ws/physics/3d/world.hpp>

namespace ws::physics::detail {

struct BackendDesc3D {
  ws::math::Vec3 gravity{};
  std::uint32_t thread_count = 0;    // 0 = single-threaded; sizes the JobSystem
  float hit_speed_threshold = 0.0f;
  std::uint32_t max_bodies = 0;
  std::uint32_t max_joints = 0;
  // The 2D seam needs no event cap (Box2D owns the event arrays and the
  // adapter drains them post-step); Jolt fires contact callbacks on
  // worker threads DURING the step, so the 3D backend pre-reserves its
  // raw buffer as a multiple of the lib's cap.
  std::uint32_t max_contact_events = 0;
};

struct BackendJoint3D {
  enum class Type : std::uint8_t { fixed, hinge, distance };
  Type type = Type::fixed;
  std::uint32_t slot_a = 0;
  std::uint32_t slot_b = 0;
  // hinge:
  ws::math::Vec3 world_anchor{};
  ws::math::Vec3 world_axis{0.0f, 1.0f, 0.0f};  // pre-normalized by the lib
  bool enable_limits = false;
  float lower_angle = 0.0f;
  float upper_angle = 0.0f;
  bool enable_motor = false;
  float motor_speed = 0.0f;
  float max_motor_torque = 0.0f;
  // distance:
  float length = 0.0f;
};

struct BackendEvent3D {
  std::uint32_t slot_a = 0;
  std::uint32_t slot_b = 0;
  physics3d::ContactPhase phase{};
  ws::math::Vec3 point{};
  ws::math::Vec3 normal{};
  float speed = 0.0f;
};

class IEventCollector3D {
 public:
  virtual ~IEventCollector3D() = default;
  virtual void add(const BackendEvent3D& event) = 0;
};

struct BackendRayHit3D {
  std::uint32_t slot = 0;
  float t = 0.0f;
  ws::math::Vec3 point{};
  ws::math::Vec3 normal{};
};

struct BackendVelocity3D {
  ws::math::Vec3 linear{};
  ws::math::Vec3 angular{};
};

// One body the backend simulated during a tick: the slot the lib assigned,
// plus the pose it ended the tick at (see BackendMove2D for the contract).
struct BackendMove3D {
  std::uint32_t slot = 0;
  physics3d::Transform transform{};
};

class IWorld3DBackend {
 public:
  virtual ~IWorld3DBackend() = default;

  [[nodiscard]] virtual ws::core::Status create_body(
      std::uint32_t slot, const physics3d::BodyDesc& desc) = 0;
  virtual void destroy_body(std::uint32_t slot) = 0;
  virtual void set_transform(std::uint32_t slot,
                             const physics3d::Transform& t) = 0;
  virtual void set_velocity(std::uint32_t slot, ws::math::Vec3 linear,
                            ws::math::Vec3 angular) = 0;
  // Collision filter (BodyDesc::collision_layer/mask at create). The
  // backend owns the truth - the lib keeps no copy.
  virtual void set_filter(std::uint32_t slot, std::uint32_t layer,
                          std::uint32_t mask) = 0;
  virtual void apply_linear_impulse(std::uint32_t slot,
                                    ws::math::Vec3 impulse) = 0;
  virtual void apply_angular_impulse(std::uint32_t slot,
                                     ws::math::Vec3 impulse) = 0;
  virtual void apply_force(std::uint32_t slot, ws::math::Vec3 force) = 0;
  virtual void apply_torque(std::uint32_t slot, ws::math::Vec3 torque) = 0;
  [[nodiscard]] virtual BackendVelocity3D get_velocity(
      std::uint32_t slot) const = 0;

  [[nodiscard]] virtual ws::core::Status create_joint(
      std::uint32_t slot, const BackendJoint3D& joint) = 0;
  virtual void destroy_joint(std::uint32_t slot) = 0;

  // Deterministic event order + moved-bodies contract: see the 2D seam.
  virtual std::size_t step(float dt, IEventCollector3D& collector,
                           std::span<BackendMove3D> moves_out) = 0;

  [[nodiscard]] virtual std::optional<BackendRayHit3D> raycast(
      ws::math::Vec3 origin, ws::math::Vec3 direction,
      float max_distance) const = 0;
  virtual void overlap_sphere(ws::math::Vec3 center, float radius,
                              std::vector<std::uint32_t>& out) const = 0;
  virtual void overlap_aabb(const physics3d::Aabb& box,
                            std::vector<std::uint32_t>& out) const = 0;
};

// The 3D backend: Jolt Physics v5.6 (src/backend/jolt3d/jolt3d_3d.cpp).
// Returns nullptr only when engine/pool initialization failed;
// World3D::create turns that into a backend_error Result.
[[nodiscard]] std::unique_ptr<IWorld3DBackend> make_jolt_world_3d(
    const BackendDesc3D& desc);

}  // namespace ws::physics::detail
