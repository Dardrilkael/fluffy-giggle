// World3D's pimpl: the exact mirror of world2d.cpp (field names match by
// design - that mirror discipline is what lets the machinery in
// world_core.hpp be shared). Differences from 2D: Vec3/Quat types, the
// Y-aligned capsule, hinge joints carry a world axis (normalized here -
// the backend receives a unit vector), and queries are overlap_sphere.
// The backend is Jolt (src/backend/jolt3d); the null placeholder was
// removed when it landed, same as the 2D path.

#include <cmath>
#include <memory>
#include <numbers>
#include <variant>

#include <ws/core/result.hpp>
#include <ws/math/vecmath.hpp>
#include <ws/physics/3d/world.hpp>

#include "core/backend3d.hpp"
#include "core/world_core.hpp"

namespace ws::physics::detail {

struct Dim3D {
  using Vec = ws::math::Vec3;
  using Angular = ws::math::Vec3;
  using Rotation = ws::math::Quat;
  using BodyId = physics3d::BodyId;
  using JointId = physics3d::JointId;
  using BodyDesc = physics3d::BodyDesc;
  using BodyState = physics3d::BodyState;
  using Transform = physics3d::Transform;
  using BodyMove = physics3d::BodyMove;
  using ContactEvent = physics3d::ContactEvent;
  using Backend = IWorld3DBackend;
  using BackendJoint = BackendJoint3D;
  using BackendEvent = BackendEvent3D;
  using BackendMove = BackendMove3D;
  using BackendVelocity = BackendVelocity3D;
  using CollectorBase = IEventCollector3D;
  // Lib-computed mass (CONTRACT.md 4): the trait routes WorldCore to the
  // dimension's closed-form. Same mirror discipline as every alias above.
  using MassData = MassData3D;

  [[nodiscard]] static MassData mass_data(const BodyDesc& d) {
    return mass_data_3d(d);
  }

  // Finiteness predicates for the runtime setters (world_core.hpp): a NaN
  // must fail the call, never reach Jolt (it asserts on contact).
  [[nodiscard]] static bool is_finite(Vec v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
  }
  [[nodiscard]] static bool is_finite(ws::math::Quat q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) &&
           std::isfinite(q.w);
  }
  // Jolt asserts on non-normalized rotations: zero-length fails here, and
  // everything else is normalized at the boundary (sanitize_rotation).
  [[nodiscard]] static bool is_valid_rotation(ws::math::Quat q) {
    return is_finite(q) && (q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w) > 0.0f;
  }
  // Pre-checked by is_valid_rotation - ws::math::normalize is deliberately
  // undefined on zero input (like GLM), the zero case never reaches it.
  static void sanitize_rotation(ws::math::Quat& q) { q = ws::math::normalize(q); }
};

}  // namespace ws::physics::detail

namespace ws::physics3d {
namespace {

namespace detail = ::ws::physics::detail;
using ::ws::physics::PhysicsError;
using ::ws::physics::fail;

// Thin delegates: one source of truth for finiteness (the Dim3D hooks).
[[nodiscard]] bool is_finite(ws::math::Vec3 v) {
  return detail::Dim3D::is_finite(v);
}

[[nodiscard]] bool is_finite(ws::math::Quat q) {
  return detail::Dim3D::is_finite(q);
}

// Hinge limits are measured against the UNWOUND relative angle, same as the
// 2D revolute (see world2d.cpp): a limit beyond one turn has no meaning any
// engine can honor.
constexpr float kMaxJointLimitAngle = 0.99f * std::numbers::pi_v<float>;

[[nodiscard]] ws::core::Status validate_shape(const Shape& shape) {
  if (std::holds_alternative<Sphere>(shape)) {
    const float r = std::get<Sphere>(shape).radius;
    if (!std::isfinite(r) || !(r > 0.0f)) {
      return fail(PhysicsError::invalid_argument, "Sphere: radius must be finite and > 0");
    }
  } else if (std::holds_alternative<Box>(shape)) {
    const Vec3 h = std::get<Box>(shape).half_extents;
    if (!is_finite(h) || !(h.x > 0.0f) || !(h.y > 0.0f) || !(h.z > 0.0f)) {
      return fail(PhysicsError::invalid_argument,
                  "Box: half_extents must be finite and > 0");
    }
  } else {
    const Capsule& c = std::get<Capsule>(shape);
    // half_height == 0 degenerates to a sphere - legal (Jolt accepts it).
    if (!std::isfinite(c.half_height) || c.half_height < 0.0f ||
        !std::isfinite(c.radius) || !(c.radius > 0.0f)) {
      return fail(PhysicsError::invalid_argument,
                  "Capsule: needs half_height >= 0 and radius > 0, both finite");
    }
  }
  return {};
}

[[nodiscard]] ws::core::Status validate_body_desc(const BodyDesc& d) {
  if (auto st = validate_shape(d.shape); !st) {
    return st;
  }
  if (!is_finite(d.transform.position) || !is_finite(d.transform.rotation) ||
      !is_finite(d.linear_velocity) || !is_finite(d.angular_velocity)) {
    return fail(PhysicsError::invalid_argument, "body transform/velocity must be finite");
  }
  if (d.kind == ws::physics::BodyKind::dynamic &&
      (!std::isfinite(d.density) || !(d.density > 0.0f))) {
    return fail(PhysicsError::invalid_argument,
                "dynamic body: density must be finite and > 0");
  }
  if (!std::isfinite(d.density) || !std::isfinite(d.friction) ||
      !std::isfinite(d.restitution) || !std::isfinite(d.gravity_scale)) {
    return fail(PhysicsError::invalid_argument,
                "body material values must be finite");
  }
  if (d.friction < 0.0f || d.restitution < 0.0f) {
    // Engines mix materials via square roots (sqrt(f1 * f2)); a negative
    // value would poison every contact it touches with NaN.
    return fail(PhysicsError::invalid_argument,
                "friction and restitution must be >= 0");
  }
  if (d.part_count != 0 && d.is_sensor) {
    // Sensors are body-wide in this backend (Jolt) and per-shape in Box2D;
    // the lib keeps one contract: a sensor body is a single shape.
    return fail(PhysicsError::invalid_argument,
                "parts are solid-only: is_sensor + parts is not supported");
  }
  if (d.part_count > ws::physics::kMaxPartsPerBody) {
    return fail(PhysicsError::invalid_argument,
                "part_count exceeds ws::physics::kMaxPartsPerBody");
  }
  for (std::uint32_t p = 0; p < d.part_count; ++p) {
    const Part& part = d.parts[p];
    if (auto st = validate_shape(part.shape); !st) {
      return st;
    }
    if (!is_finite(part.offset) || !std::isfinite(part.friction) ||
        !std::isfinite(part.restitution)) {
      return fail(PhysicsError::invalid_argument,
                  "part offset and material values must be finite");
    }
    if (part.friction < 0.0f || part.restitution < 0.0f) {
      return fail(PhysicsError::invalid_argument,
                  "part friction and restitution must be >= 0");
    }
  }
  return {};
}

}  // namespace

struct World3D::Impl {
  Impl(detail::CoreDesc core, std::unique_ptr<detail::IWorld3DBackend> backend)
      : core_(core, std::move(backend)) {}

  detail::WorldCore<detail::Dim3D> core_;
};

// --- special members (pimpl discipline: defined where Impl is complete) -----

World3D::World3D() = default;
World3D::~World3D() = default;
World3D::World3D(World3D&&) noexcept = default;
World3D& World3D::operator=(World3D&&) noexcept = default;

[[nodiscard]] ws::core::Result<World3D> World3D::create(Desc desc) {
  if (!is_finite(desc.gravity)) {
    return fail(PhysicsError::invalid_argument, "gravity must be finite");
  }
  detail::CoreDesc core{};
  core.fixed_dt = desc.fixed_dt;
  core.max_bodies = desc.max_bodies;
  core.max_joints = desc.max_joints;
  core.max_contact_events = desc.max_contact_events;
  core.thread_count = desc.thread_count;
  core.hit_speed_threshold = desc.hit_speed_threshold;
  core.max_substeps = desc.max_substeps;
  if (auto st = detail::validate_core_desc(core); !st) {
    return ws::core::unexpected<ws::core::Error>(std::move(st.error()));
  }
  auto backend = detail::make_jolt_world_3d(detail::BackendDesc3D{
      desc.gravity, desc.thread_count, desc.hit_speed_threshold,
      desc.max_bodies, desc.max_joints, desc.max_contact_events});
  if (!backend) {
    return fail(PhysicsError::backend_error, "Jolt backend failed to start");
  }
  World3D world;
  world.impl_ = std::make_unique<Impl>(core, std::move(backend));
  return world;
}

// Defined here (not as an in-class default argument) for the same reason the
// header notes: Desc's member initializers are only odr-usable once the
// class is complete, and here it is.
[[nodiscard]] ws::core::Result<World3D> World3D::create() {
  return create(Desc{});
}

// --- bodies -------------------------------------------------------------------

[[nodiscard]] ws::core::Result<BodyId> World3D::create_body(const BodyDesc& desc) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  if (auto st = validate_body_desc(desc); !st) {
    return ws::core::unexpected<ws::core::Error>(std::move(st.error()));
  }
  return impl_->core_.create_body(desc);
}

ws::core::Status World3D::destroy_body(BodyId id) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.destroy_body(id);
}

[[nodiscard]] ws::core::Result<BodyState> World3D::body_state(BodyId id) const {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.body_state(id);
}

[[nodiscard]] ws::core::Result<std::uintptr_t> World3D::body_user_data(
    BodyId id) const {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.body_user_data(id);
}

ws::core::Status World3D::set_body_user_data(BodyId id,
                                              std::uintptr_t value) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.set_body_user_data(id, value);
}

ws::core::Status World3D::set_transform(BodyId id, Vec3 position,
                                        ws::math::Quat rotation) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.set_transform(id, Transform{position, rotation});
}

ws::core::Status World3D::set_velocity(BodyId id, Vec3 linear, Vec3 angular) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.set_velocity(id, linear, angular);
}

ws::core::Status World3D::set_collision_filter(BodyId id,
                                               std::uint32_t layer,
                                               std::uint32_t mask) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.set_collision_filter(id, layer, mask);
}

ws::core::Status World3D::set_bullet(BodyId id, bool enabled) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.set_bullet(id, enabled);
}

ws::core::Status World3D::set_sleepable(BodyId id, bool allowed) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.set_sleepable(id, allowed);
}

ws::core::Status World3D::wake(BodyId id) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.wake(id);
}

[[nodiscard]] ws::core::Result<bool> World3D::is_sleeping(BodyId id) const {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.is_sleeping(id);
}

ws::core::Status World3D::set_gravity(Vec3 gravity) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  impl_->core_.set_gravity(gravity);
  return {};
}

ws::core::Status World3D::apply_linear_impulse(BodyId id, Vec3 impulse) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.apply_linear_impulse(id, impulse);
}

ws::core::Status World3D::apply_angular_impulse(BodyId id, Vec3 impulse) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.apply_angular_impulse(id, impulse);
}

ws::core::Status World3D::apply_force(BodyId id, Vec3 force) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.apply_force(id, force);
}

ws::core::Status World3D::apply_torque(BodyId id, Vec3 torque) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.apply_torque(id, torque);
}

// --- joints ---------------------------------------------------------------------

[[nodiscard]] ws::core::Result<JointId> World3D::create_joint(const JointDesc& desc) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  using detail::BackendJoint3D;
  return std::visit(
      [this](const auto& j) -> ws::core::Result<JointId> {
        using J = std::decay_t<decltype(j)>;
        if (j.a == j.b) {
          return fail(PhysicsError::invalid_argument, "create_joint: a and b must differ");
        }
        WS_TRY_ASSIGN(std::uint32_t slot_a, impl_->core_.resolve_body(j.a));
        WS_TRY_ASSIGN(std::uint32_t slot_b, impl_->core_.resolve_body(j.b));
        BackendJoint3D bj{};
        bj.slot_a = slot_a;
        bj.slot_b = slot_b;
        if constexpr (std::is_same_v<J, FixedJoint>) {
          bj.type = BackendJoint3D::Type::fixed;
        } else if constexpr (std::is_same_v<J, HingeJoint>) {
          bj.type = BackendJoint3D::Type::hinge;
          if (!is_finite(j.world_anchor)) {
            return fail(PhysicsError::invalid_argument,
                        "HingeJoint: world_anchor must be finite");
          }
          if (!is_finite(j.world_axis) || j.world_axis == Vec3{}) {
            return fail(PhysicsError::invalid_argument,
                        "HingeJoint: world_axis must be finite and non-zero");
          }
          if (j.enable_limits) {
            if (!std::isfinite(j.lower_angle) || !std::isfinite(j.upper_angle) ||
                !(j.lower_angle <= j.upper_angle)) {
              return fail(PhysicsError::invalid_argument,
                          "HingeJoint: limits need lower <= upper, both finite");
            }
            if (j.lower_angle < -kMaxJointLimitAngle ||
                j.upper_angle > kMaxJointLimitAngle) {
              return fail(PhysicsError::invalid_argument,
                          "HingeJoint: limits must stay within one turn "
                          "(+-0.99*pi)");
            }
          }
          if (j.enable_motor) {
            if (!std::isfinite(j.motor_speed) || !std::isfinite(j.max_motor_torque) ||
                !(j.max_motor_torque > 0.0f)) {
              return fail(PhysicsError::invalid_argument,
                          "HingeJoint: enabled motor needs motor_speed finite "
                          "and max_motor_torque > 0");
            }
          }
          bj.world_anchor = j.world_anchor;
          bj.world_axis = ws::math::normalize(j.world_axis);  // backend gets a unit vector
          bj.enable_limits = j.enable_limits;
          bj.lower_angle = j.lower_angle;
          bj.upper_angle = j.upper_angle;
          bj.enable_motor = j.enable_motor;
          bj.motor_speed = j.motor_speed;
          bj.max_motor_torque = j.max_motor_torque;
        } else {  // DistanceJoint
          bj.type = BackendJoint3D::Type::distance;
          // > 0, not >= 0: Box2D hard-asserts length > 0 (joint.c) and a
          // zero-length rod is degenerate anyway - that is what a weld is
          // for. Both backends get the same contract from the lib.
          if (!std::isfinite(j.length) || !(j.length > 0.0f)) {
            return fail(PhysicsError::invalid_argument,
                        "DistanceJoint: length must be finite and > 0");
          }
          bj.length = j.length;
        }
        return impl_->core_.add_joint(slot_a, slot_b, bj);
      },
      desc);
}

ws::core::Status World3D::destroy_joint(JointId id) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.destroy_joint(id);
}

// --- stepping ---------------------------------------------------------------------

ws::core::Status World3D::step() {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.step();
}

ws::core::Status World3D::advance(float frame_dt) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.advance(frame_dt);
}

[[nodiscard]] float World3D::interpolation_alpha() const {
  return impl_ ? impl_->core_.interpolation_alpha() : 0.0f;
}

[[nodiscard]] float World3D::fixed_dt() const {
  return impl_ ? impl_->core_.fixed_dt() : 0.0f;
}

// --- events + bulk -------------------------------------------------------------------

[[nodiscard]] std::span<const ContactEvent> World3D::contact_events() const {
  return impl_ ? impl_->core_.contact_events() : std::span<const ContactEvent>{};
}

[[nodiscard]] std::span<const Transform> World3D::transforms() const {
  return impl_ ? impl_->core_.transforms() : std::span<const Transform>{};
}

[[nodiscard]] std::span<const Transform> World3D::prev_transforms() const {
  return impl_ ? impl_->core_.prev_transforms()
               : std::span<const Transform>{};
}

[[nodiscard]] std::span<const BodyMove> World3D::moved_bodies() const {
  return impl_ ? impl_->core_.moved_bodies() : std::span<const BodyMove>{};
}

ws::core::Status World3D::set_transforms(std::span<const BodyMove> moves) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.set_transforms(moves);
}

// --- queries --------------------------------------------------------------------------

[[nodiscard]] ws::core::Result<std::optional<RayHit>> World3D::raycast(
    Vec3 origin, Vec3 direction, float max_distance) const {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  if (!is_finite(origin) || !is_finite(direction)) {
    return fail(PhysicsError::invalid_argument, "raycast: origin/direction must be finite");
  }
  if (direction == Vec3{}) {
    return fail(PhysicsError::invalid_argument, "raycast: direction must be non-zero");
  }
  if (!std::isfinite(max_distance) || max_distance < 0.0f) {
    return fail(PhysicsError::invalid_argument, "raycast: max_distance must be finite and >= 0");
  }
  const Vec3 unit = ws::math::normalize(direction);
  const auto hit = impl_->core_.backend().raycast(origin, unit, max_distance);
  if (!hit) {
    return std::optional<RayHit>{};
  }
  const auto body = impl_->core_.id_for_slot(hit->slot);
  if (!body) {
    return std::optional<RayHit>{};  // defensive: hit body retired mid-query
  }
  return std::optional<RayHit>{RayHit{*body, hit->t, hit->point, hit->normal}};
}

[[nodiscard]] ws::core::Result<std::vector<BodyId>> World3D::overlap_sphere(
    Vec3 center, float radius) const {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  if (!is_finite(center) || !std::isfinite(radius) || radius < 0.0f) {
    return fail(PhysicsError::invalid_argument,
                "overlap_sphere: needs finite center and radius >= 0");
  }
  std::vector<std::uint32_t> slots;
  impl_->core_.backend().overlap_sphere(center, radius, slots);
  return impl_->core_.ids_for_slots(slots);
}

[[nodiscard]] ws::core::Result<std::vector<BodyId>> World3D::overlap_aabb(
    Aabb box) const {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  if (!is_finite(box.min) || !is_finite(box.max) || box.min.x > box.max.x ||
      box.min.y > box.max.y || box.min.z > box.max.z) {
    return fail(PhysicsError::invalid_argument,
                "overlap_aabb: needs finite min <= max");
  }
  std::vector<std::uint32_t> slots;
  impl_->core_.backend().overlap_aabb(box, slots);
  return impl_->core_.ids_for_slots(slots);
}

}  // namespace ws::physics3d
