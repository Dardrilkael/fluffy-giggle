// World2D's pimpl: dimension glue over the shared machinery (world_core.hpp)
// - Desc/shape validation, JointDesc translation (public variant with
// BodyIds -> flat BackendJoint2D with slots), query result mapping and the
// moved-from guards. Box2D v3 lives behind the seam (src/backend/box2d);
// swapping engines touches only the factory line below.

#include <cmath>
#include <memory>
#include <numbers>
#include <variant>

#include <ws/core/result.hpp>
#include <ws/math/vecmath.hpp>
#include <ws/physics/2d/world.hpp>

#include "core/backend2d.hpp"
#include "core/world_core.hpp"

namespace ws::physics::detail {

// The 2D traits for WorldCore. Field names mirror physics3d's exactly - that
// mirror discipline is what lets the machinery be type-parameterized.
struct Dim2D {
  using Vec = ws::math::Vec2;
  using Angular = float;
  using Rotation = float;  // a 2D rotation is a bare angle
  using BodyId = physics2d::BodyId;
  using JointId = physics2d::JointId;
  using BodyDesc = physics2d::BodyDesc;
  using BodyState = physics2d::BodyState;
  using Transform = physics2d::Transform;
  using BodyMove = physics2d::BodyMove;
  using ContactEvent = physics2d::ContactEvent;
  using Backend = IWorld2DBackend;
  using BackendJoint = BackendJoint2D;
  using BackendEvent = BackendEvent2D;
  using BackendMove = BackendMove2D;
  using BackendVelocity = BackendVelocity2D;
  using CollectorBase = IEventCollector2D;
  // Lib-computed mass (CONTRACT.md 4): the trait routes WorldCore to the
  // dimension's closed-form. Same mirror discipline as every alias above.
  using MassData = MassData2D;

  [[nodiscard]] static MassData mass_data(const BodyDesc& d) {
    return mass_data_2d(d);
  }

  // Finiteness predicates for the runtime setters (world_core.hpp): a NaN
  // must fail the call, never reach the engine.
  [[nodiscard]] static bool is_finite(Vec v) {
    return std::isfinite(v.x) && std::isfinite(v.y);
  }
  [[nodiscard]] static bool is_finite(float v) { return std::isfinite(v); }
  // Any finite angle is already canonical - nothing to sanitize.
  [[nodiscard]] static bool is_valid_rotation(float angle) {
    return std::isfinite(angle);
  }
  static void sanitize_rotation(float&) {}
};

}  // namespace ws::physics::detail

namespace ws::physics2d {
namespace {

namespace detail = ::ws::physics::detail;
using ::ws::physics::PhysicsError;
using ::ws::physics::fail;

[[nodiscard]] bool is_finite(ws::math::Vec2 v) {
  return std::isfinite(v.x) && std::isfinite(v.y);
}

// Revolute/hinge limits are measured against the UNWOUND relative angle:
// Box2D asserts values inside +-0.99*pi and Jolt's hinge normalizes the
// same way, so a limit beyond one turn has no meaning either engine can
// honor. The lib rejects it instead of letting the engines disagree.
constexpr float kMaxJointLimitAngle = 0.99f * std::numbers::pi_v<float>;

[[nodiscard]] ws::core::Status validate_shape(const Shape& shape) {
  if (std::holds_alternative<Circle>(shape)) {
    const float r = std::get<Circle>(shape).radius;
    if (!std::isfinite(r) || !(r > 0.0f)) {
      return fail(PhysicsError::invalid_argument, "Circle: radius must be finite and > 0");
    }
  } else if (std::holds_alternative<Box>(shape)) {
    const Vec2 h = std::get<Box>(shape).half_extents;
    if (!is_finite(h) || !(h.x > 0.0f) || !(h.y > 0.0f)) {
      return fail(PhysicsError::invalid_argument,
                  "Box: half_extents must be finite and > 0");
    }
  } else {
    const Capsule& c = std::get<Capsule>(shape);
    if (!is_finite(c.a) || !is_finite(c.b) || !std::isfinite(c.radius) ||
        !(c.radius > 0.0f) || c.a == c.b) {
      return fail(PhysicsError::invalid_argument,
                  "Capsule: needs a finite a != b and radius > 0");
    }
  }
  return {};
}

[[nodiscard]] ws::core::Status validate_body_desc(const BodyDesc& d) {
  if (auto st = validate_shape(d.shape); !st) {
    return st;
  }
  if (!is_finite(d.transform.position) || !std::isfinite(d.transform.rotation) ||
      !is_finite(d.linear_velocity) || !std::isfinite(d.angular_velocity)) {
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
    // Sensors are body-wide in the 3D backend and per-shape in Box2D; the
    // lib keeps one contract: a sensor body is a single shape.
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

struct World2D::Impl {
  Impl(detail::CoreDesc core, std::unique_ptr<detail::IWorld2DBackend> backend)
      : core_(core, std::move(backend)) {}

  detail::WorldCore<detail::Dim2D> core_;
};

// --- special members (pimpl discipline: defined where Impl is complete) -----

World2D::World2D() = default;
World2D::~World2D() = default;
World2D::World2D(World2D&&) noexcept = default;
World2D& World2D::operator=(World2D&&) noexcept = default;

[[nodiscard]] ws::core::Result<World2D> World2D::create(Desc desc) {
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
  auto backend = detail::make_box2d_world_2d(detail::BackendDesc2D{
      desc.gravity, desc.thread_count, desc.hit_speed_threshold,
      desc.max_bodies, desc.max_joints});
  if (!backend) {
    return fail(PhysicsError::backend_error, "Box2D backend failed to start");
  }
  World2D world;
  world.impl_ = std::make_unique<Impl>(core, std::move(backend));
  return world;
}

// Defined here (not as an in-class default argument) for the same reason the
// header notes: Desc's member initializers are only odr-usable once the
// class is complete, and here it is.
[[nodiscard]] ws::core::Result<World2D> World2D::create() {
  return create(Desc{});
}

// --- bodies -------------------------------------------------------------------

[[nodiscard]] ws::core::Result<BodyId> World2D::create_body(const BodyDesc& desc) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  if (auto st = validate_body_desc(desc); !st) {
    return ws::core::unexpected<ws::core::Error>(std::move(st.error()));
  }
  return impl_->core_.create_body(desc);
}

ws::core::Status World2D::destroy_body(BodyId id) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.destroy_body(id);
}

[[nodiscard]] ws::core::Result<BodyState> World2D::body_state(BodyId id) const {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.body_state(id);
}

[[nodiscard]] ws::core::Result<std::uintptr_t> World2D::body_user_data(
    BodyId id) const {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.body_user_data(id);
}

ws::core::Status World2D::set_body_user_data(BodyId id,
                                              std::uintptr_t value) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.set_body_user_data(id, value);
}

ws::core::Status World2D::set_transform(BodyId id, Vec2 position, float rotation) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.set_transform(id, Transform{position, rotation});
}

ws::core::Status World2D::set_velocity(BodyId id, Vec2 linear, float angular) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.set_velocity(id, linear, angular);
}

ws::core::Status World2D::set_collision_filter(BodyId id,
                                               std::uint32_t layer,
                                               std::uint32_t mask) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.set_collision_filter(id, layer, mask);
}

ws::core::Status World2D::set_bullet(BodyId id, bool enabled) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.set_bullet(id, enabled);
}

ws::core::Status World2D::set_sleepable(BodyId id, bool allowed) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.set_sleepable(id, allowed);
}

ws::core::Status World2D::wake(BodyId id) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.wake(id);
}

[[nodiscard]] ws::core::Result<bool> World2D::is_sleeping(BodyId id) const {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.is_sleeping(id);
}

ws::core::Status World2D::set_gravity(Vec2 gravity) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  impl_->core_.set_gravity(gravity);
  return {};
}

ws::core::Status World2D::apply_linear_impulse(BodyId id, Vec2 impulse) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.apply_linear_impulse(id, impulse);
}

ws::core::Status World2D::apply_angular_impulse(BodyId id, float impulse) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.apply_angular_impulse(id, impulse);
}

ws::core::Status World2D::apply_force(BodyId id, Vec2 force) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.apply_force(id, force);
}

ws::core::Status World2D::apply_torque(BodyId id, float torque) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.apply_torque(id, torque);
}

// --- joints ---------------------------------------------------------------------

[[nodiscard]] ws::core::Result<JointId> World2D::create_joint(const JointDesc& desc) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  using detail::BackendJoint2D;
  return std::visit(
      [this](const auto& j) -> ws::core::Result<JointId> {
        using J = std::decay_t<decltype(j)>;
        if (j.a == j.b) {
          return fail(PhysicsError::invalid_argument, "create_joint: a and b must differ");
        }
        WS_TRY_ASSIGN(std::uint32_t slot_a, impl_->core_.resolve_body(j.a));
        WS_TRY_ASSIGN(std::uint32_t slot_b, impl_->core_.resolve_body(j.b));
        BackendJoint2D bj{};
        bj.slot_a = slot_a;
        bj.slot_b = slot_b;
        if constexpr (std::is_same_v<J, FixedJoint>) {
          bj.type = BackendJoint2D::Type::fixed;
        } else if constexpr (std::is_same_v<J, RevoluteJoint>) {
          bj.type = BackendJoint2D::Type::revolute;
          if (!is_finite(j.world_anchor)) {
            return fail(PhysicsError::invalid_argument,
                        "RevoluteJoint: world_anchor must be finite");
          }
          bj.world_anchor = j.world_anchor;
          bj.enable_limits = j.enable_limits;
          bj.lower_angle = j.lower_angle;
          bj.upper_angle = j.upper_angle;
          bj.enable_motor = j.enable_motor;
          bj.motor_speed = j.motor_speed;
          bj.max_motor_torque = j.max_motor_torque;
          if (j.enable_limits) {
            if (!std::isfinite(j.lower_angle) || !std::isfinite(j.upper_angle) ||
                !(j.lower_angle <= j.upper_angle)) {
              return fail(PhysicsError::invalid_argument,
                          "RevoluteJoint: limits need lower <= upper, both finite");
            }
            if (j.lower_angle < -kMaxJointLimitAngle ||
                j.upper_angle > kMaxJointLimitAngle) {
              return fail(PhysicsError::invalid_argument,
                          "RevoluteJoint: limits must stay within one turn "
                          "(+-0.99*pi)");
            }
          }
          if (j.enable_motor) {
            if (!std::isfinite(j.motor_speed) || !std::isfinite(j.max_motor_torque) ||
                !(j.max_motor_torque > 0.0f)) {
              return fail(PhysicsError::invalid_argument,
                          "RevoluteJoint: enabled motor needs motor_speed finite "
                          "and max_motor_torque > 0");
            }
          }
        } else {  // DistanceJoint
          bj.type = BackendJoint2D::Type::distance;
          // > 0, not >= 0: Box2D hard-asserts length > 0 (joint.c), and a
          // zero-length rod is degenerate anyway - that is what a weld is
          // for. The check must happen here, before the engine ever sees it.
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

ws::core::Status World2D::destroy_joint(JointId id) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.destroy_joint(id);
}

// --- stepping ---------------------------------------------------------------------

ws::core::Status World2D::step() {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.step();
}

ws::core::Status World2D::advance(float frame_dt) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.advance(frame_dt);
}

[[nodiscard]] float World2D::interpolation_alpha() const {
  return impl_ ? impl_->core_.interpolation_alpha() : 0.0f;
}

[[nodiscard]] float World2D::fixed_dt() const {
  return impl_ ? impl_->core_.fixed_dt() : 0.0f;
}

// --- events + bulk -------------------------------------------------------------------

[[nodiscard]] std::span<const ContactEvent> World2D::contact_events() const {
  return impl_ ? impl_->core_.contact_events() : std::span<const ContactEvent>{};
}

[[nodiscard]] std::span<const Transform> World2D::transforms() const {
  return impl_ ? impl_->core_.transforms() : std::span<const Transform>{};
}

[[nodiscard]] std::span<const Transform> World2D::prev_transforms() const {
  return impl_ ? impl_->core_.prev_transforms()
               : std::span<const Transform>{};
}

[[nodiscard]] std::span<const BodyMove> World2D::moved_bodies() const {
  return impl_ ? impl_->core_.moved_bodies() : std::span<const BodyMove>{};
}

ws::core::Status World2D::set_transforms(std::span<const BodyMove> moves) {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  return impl_->core_.set_transforms(moves);
}

// --- queries --------------------------------------------------------------------------

[[nodiscard]] ws::core::Result<std::optional<RayHit>> World2D::raycast(
    Vec2 origin, Vec2 direction, float max_distance) const {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  if (!is_finite(origin) || !is_finite(direction)) {
    return fail(PhysicsError::invalid_argument, "raycast: origin/direction must be finite");
  }
  if (direction == Vec2{}) {
    return fail(PhysicsError::invalid_argument, "raycast: direction must be non-zero");
  }
  if (!std::isfinite(max_distance) || max_distance < 0.0f) {
    return fail(PhysicsError::invalid_argument, "raycast: max_distance must be finite and >= 0");
  }
  const Vec2 unit = ws::math::normalize(direction);
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

[[nodiscard]] ws::core::Result<std::vector<BodyId>> World2D::overlap_circle(
    Vec2 center, float radius) const {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  if (!is_finite(center) || !std::isfinite(radius) || radius < 0.0f) {
    return fail(PhysicsError::invalid_argument,
                "overlap_circle: needs finite center and radius >= 0");
  }
  std::vector<std::uint32_t> slots;
  impl_->core_.backend().overlap_circle(center, radius, slots);
  return impl_->core_.ids_for_slots(slots);
}

[[nodiscard]] ws::core::Result<std::vector<BodyId>> World2D::overlap_aabb(
    Aabb box) const {
  if (!impl_) return fail(PhysicsError::invalid_state, "world is moved-from");
  if (!is_finite(box.min) || !is_finite(box.max) || box.min.x > box.max.x ||
      box.min.y > box.max.y) {
    return fail(PhysicsError::invalid_argument,
                "overlap_aabb: needs finite min <= max");
  }
  std::vector<std::uint32_t> slots;
  impl_->core_.backend().overlap_aabb(box, slots);
  return impl_->core_.ids_for_slots(slots);
}

}  // namespace ws::physics2d
