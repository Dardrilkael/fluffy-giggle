#pragma once

// The backend-independent world machinery, shared by World2D and World3D
// (internal - never included by a public header). Everything that does not
// depend on the dimension's types lives HERE, once: generational handle
// tables, the pose mirror behind transforms(), the contact-event buffer
// with its drain/normalize/sort/cap discipline, the fixed-timestep
// accumulator with the spiral-of-death clamp, bulk set_transforms
// atomicity, and the destroy_body -> joint cascade.
//
// The dimension is a Traits type supplied by the per-dimension .cpp:
//   struct Dim2D;  // in world2d.cpp - aliases Vec2/float types + the 2D seam
//   struct Dim3D;  // in world3d.cpp
// The public World classes stay separate BY DESIGN (the user-visible API
// is two distinct mirrored types); only the tricky machinery is shared,
// so a fix lands in one place. The per-dim .cpp keeps what genuinely
// differs: Desc validation, shape validation, JointDesc translation
// (public variant with BodyIds -> flat BackendJoint with slots) and the
// query result mapping.
//
// Allocation discipline: EVERY vector is sized at construction. Steady
// state (create/destroy bodies + joints, step, advance, set_transforms,
// drain events) allocates nothing; only create() itself and the query
// APIs (documented as not-the-hot-path) may allocate.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <ws/core/log.hpp>
#include <ws/core/result.hpp>
#include <ws/physics/common.hpp>
#include <ws/physics/errors.hpp>

#include "handle_table.hpp"

namespace ws::physics::detail {

// The dimension-independent part of World2D/3D::Desc.
struct CoreDesc {
  float fixed_dt = 0.0f;
  std::uint32_t max_bodies = 0;
  std::uint32_t max_joints = 0;
  std::uint32_t max_contact_events = 0;
  std::uint32_t thread_count = 0;
  float hit_speed_threshold = 0.0f;
  std::uint32_t max_substeps = 8;
};

// Shared Desc validation (the gravity field is the only dimension-specific
// one - each world .cpp checks that and calls this for the rest).
[[nodiscard]] inline ws::core::Status validate_core_desc(const CoreDesc& d) {
  if (!std::isfinite(d.fixed_dt) || !(d.fixed_dt > 0.0f)) {
    return fail(PhysicsError::invalid_argument, "fixed_dt must be finite and > 0");
  }
  if (d.max_bodies > kMaxBodiesPerWorld) {
    return fail(PhysicsError::invalid_argument,
                "max_bodies exceeds ws::physics::kMaxBodiesPerWorld");
  }
  if (d.max_joints > kMaxJointsPerWorld) {
    return fail(PhysicsError::invalid_argument,
                "max_joints exceeds ws::physics::kMaxJointsPerWorld");
  }
  if (d.max_substeps < 1) {
    return fail(PhysicsError::invalid_argument, "max_substeps must be >= 1");
  }
  if (!std::isfinite(d.hit_speed_threshold) || d.hit_speed_threshold < 0.0f) {
    return fail(PhysicsError::invalid_argument,
                "hit_speed_threshold must be finite and >= 0");
  }
  return {};
}

template <typename D>
class WorldCore {
 public:
  using Vec = typename D::Vec;              // Vec2 / Vec3
  using Angular = typename D::Angular;      // float / Vec3
  using BodyIdT = typename D::BodyId;
  using JointIdT = typename D::JointId;
  using BodyDescT = typename D::BodyDesc;
  using BodyStateT = typename D::BodyState;
  using TransformT = typename D::Transform;
  using BodyMoveT = typename D::BodyMove;
  using ContactEventT = typename D::ContactEvent;
  using Backend = typename D::Backend;
  using BackendJoint = typename D::BackendJoint;
  using BackendEvent = typename D::BackendEvent;
  using BackendVelocity = typename D::BackendVelocity;

  WorldCore(const CoreDesc& desc, std::unique_ptr<Backend> backend)
      : desc_(desc),
        bodies_(desc.max_bodies),
        joints_(desc.max_joints),
        backend_(std::move(backend)) {
    transforms_.resize(desc.max_bodies);      // one allocation, final size
    events_.reserve(desc.max_contact_events);  // capacity only
    records_.resize(desc.max_joints);
  }

  WorldCore(const WorldCore&) = delete;
  WorldCore& operator=(const WorldCore&) = delete;

  // --- bodies -------------------------------------------------------------

  // Pre-validated desc; the dimension layer validates shapes/kinds first.
  [[nodiscard]] ws::core::Result<BodyIdT> create_body(const BodyDescT& desc) {
    const auto slot = bodies_.allocate();
    if (!slot) {
      return fail(PhysicsError::world_full, "body capacity exhausted");
    }
    if (auto st = backend_->create_body(*slot, desc); !st) {
      bodies_.free(*slot);
      return fail(PhysicsError::backend_error,
                  std::string{"backend create_body: "} + st.error().message);
    }
    transforms_[*slot] = desc.transform;
    return BodyIdT{*slot, bodies_.generation(*slot)};
  }

  ws::core::Status destroy_body(BodyIdT id) {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    // Joints die with their bodies (both engines do this too - the lib
    // mirrors it so its own tables stay true). Joints first: the engines
    // refuse joint destruction once the body is gone.
    for (std::uint32_t j = 0; j < records_.size(); ++j) {
      if (records_[j].live &&
          (records_[j].slot_a == *slot || records_[j].slot_b == *slot)) {
        backend_->destroy_joint(j);
        records_[j].live = false;
        joints_.free(j);
      }
    }
    backend_->destroy_body(*slot);
    bodies_.free(*slot);
    return {};
  }

  [[nodiscard]] ws::core::Result<BodyStateT> body_state(BodyIdT id) const {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    BodyStateT state{};
    state.position = transforms_[*slot].position;
    state.rotation = transforms_[*slot].rotation;
    const BackendVelocity v = backend_->get_velocity(*slot);
    state.linear_velocity = v.linear;
    state.angular_velocity = v.angular;
    return state;
  }

  ws::core::Status set_transform(BodyIdT id, const TransformT& t) {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    transforms_[*slot] = t;
    backend_->set_transform(*slot, t);
    return {};
  }

  ws::core::Status set_velocity(BodyIdT id, Vec linear, Angular angular) {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    backend_->set_velocity(*slot, linear, angular);
    return {};
  }

  ws::core::Status apply_linear_impulse(BodyIdT id, Vec impulse) {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    backend_->apply_linear_impulse(*slot, impulse);
    return {};
  }

  ws::core::Status apply_angular_impulse(BodyIdT id, Angular impulse) {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    backend_->apply_angular_impulse(*slot, impulse);
    return {};
  }

  ws::core::Status apply_force(BodyIdT id, Vec force) {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    backend_->apply_force(*slot, force);
    return {};
  }

  ws::core::Status apply_torque(BodyIdT id, Angular torque) {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    backend_->apply_torque(*slot, torque);
    return {};
  }

  // --- joints ---------------------------------------------------------------

  // `joint` is validated and translated by the dimension layer; slots are
  // the resolved live bodies. Returns the public handle.
  [[nodiscard]] ws::core::Result<JointIdT> add_joint(std::uint32_t slot_a,
                                                     std::uint32_t slot_b,
                                                     const BackendJoint& joint) {
    const auto slot = joints_.allocate();
    if (!slot) {
      return fail(PhysicsError::world_full, "joint capacity exhausted");
    }
    if (auto st = backend_->create_joint(*slot, joint); !st) {
      joints_.free(*slot);
      return fail(PhysicsError::backend_error,
                  std::string{"backend create_joint: "} + st.error().message);
    }
    records_[*slot] = JointRecord{slot_a, slot_b, true};
    return JointIdT{*slot, joints_.generation(*slot)};
  }

  ws::core::Status destroy_joint(JointIdT id) {
    if (!joints_.alive(id.index, id.generation)) {
      return fail(PhysicsError::invalid_handle, "stale or unknown joint");
    }
    backend_->destroy_joint(id.index);
    records_[id.index].live = false;
    joints_.free(id.index);
    return {};
  }

  // --- stepping ---------------------------------------------------------------

  ws::core::Status step() {
    tick();
    return {};
  }

  ws::core::Status advance(float frame_dt) {
    if (!(frame_dt >= 0.0f) || !std::isfinite(frame_dt)) {
      return fail(PhysicsError::invalid_argument, "frame_dt must be finite and >= 0");
    }
    accumulator_ += frame_dt;
    std::uint32_t steps = 0;
    while (accumulator_ >= desc_.fixed_dt && steps < desc_.max_substeps) {
      tick();
      accumulator_ -= desc_.fixed_dt;
      ++steps;
    }
    if (accumulator_ >= desc_.fixed_dt) {
      // Spiral-of-death protection: drop the whole backlog, keep only the
      // current tick's fraction for interpolation.
      accumulator_ = std::fmod(accumulator_, desc_.fixed_dt);
    }
    alpha_ = accumulator_ / desc_.fixed_dt;
    return {};
  }

  [[nodiscard]] float interpolation_alpha() const { return alpha_; }
  [[nodiscard]] float fixed_dt() const { return desc_.fixed_dt; }

  // --- events + bulk paths ------------------------------------------------------

  [[nodiscard]] std::span<const ContactEventT> contact_events() const {
    return std::span<const ContactEventT>{events_};
  }

  [[nodiscard]] std::span<const TransformT> transforms() const {
    return std::span<const TransformT>{transforms_};
  }

  ws::core::Status set_transforms(std::span<const BodyMoveT> moves) {
    // ATOMIC: validate every id first; the apply loop cannot fail.
    for (const BodyMoveT& m : moves) {
      if (!bodies_.alive(m.id.index, m.id.generation)) {
        return fail(PhysicsError::invalid_handle, "set_transforms: stale or unknown body");
      }
    }
    for (const BodyMoveT& m : moves) {
      const TransformT t{m.position, m.rotation};
      transforms_[m.id.index] = t;
      backend_->set_transform(m.id.index, t);
    }
    return {};
  }

  // Query helper: backend returns slots; the dimension layer maps them to
  // public handles through this. Sorted by creation index = slot order is
  // NOT guaranteed from the engine, so sort here (deterministic).
  [[nodiscard]] ws::core::Result<std::vector<BodyIdT>> ids_for_slots(
      std::vector<std::uint32_t>& slots) const {
    std::sort(slots.begin(), slots.end());
    std::vector<BodyIdT> out;
    out.reserve(slots.size());
    for (std::uint32_t s : slots) {
      if (bodies_.live(s)) {
        out.push_back(BodyIdT{s, bodies_.generation(s)});
      }
    }
    return out;
  }

  [[nodiscard]] std::optional<BodyIdT> id_for_slot(std::uint32_t slot) const {
    if (bodies_.live(slot)) {
      return BodyIdT{slot, bodies_.generation(slot)};
    }
    return std::nullopt;
  }

  // --- internals used by the dimension layer ------------------------------

  [[nodiscard]] ws::core::Result<std::uint32_t> resolve_body(BodyIdT id) const {
    if (bodies_.alive(id.index, id.generation)) {
      return id.index;
    }
    return fail(PhysicsError::invalid_handle, "stale or unknown body");
  }

  [[nodiscard]] const Backend& backend() const { return *backend_; }

 private:
  struct JointRecord {
    std::uint32_t slot_a = 0;
    std::uint32_t slot_b = 0;
    bool live = false;
  };

  // The per-tick engine drive, shared by step() and advance().
  void tick() {
    events_.clear();  // capacity kept: no reallocation ever
    EventCollector collector{*this};
    backend_->step(desc_.fixed_dt, collector);
    // Worker-thread arrival order is not deterministic (Jolt); the public
    // order is (a.index, b.index, phase) - sorted once, in place.
    std::sort(events_.begin(), events_.end(),
              [](const ContactEventT& l, const ContactEventT& r) {
                if (l.a.index != r.a.index) return l.a.index < r.a.index;
                if (l.b.index != r.b.index) return l.b.index < r.b.index;
                return static_cast<std::uint8_t>(l.phase) <
                       static_cast<std::uint8_t>(r.phase);
              });
    backend_->copy_transforms(transforms_);
  }

  // IEventCollector2D/3D implementation: resolves engine slots to public
  // handles, orders the pair by creation index (flipping the normal so it
  // keeps pointing a -> b), caps the buffer, warns once on overflow.
  class EventCollector final : public D::CollectorBase {
   public:
    explicit EventCollector(WorldCore& core) : core_(core) {}

    void add(const BackendEvent& event) override {
      if (core_.events_.size() >= core_.desc_.max_contact_events) {
        if (!core_.overflow_warned_) {
          core_.overflow_warned_ = true;
          ws::core::warn(
              "physics: contact-event buffer full ({}); dropping newest events",
              core_.events_.capacity());
        }
        return;
      }
      if (!core_.bodies_.live(event.slot_a) ||
          !core_.bodies_.live(event.slot_b)) {
        return;  // defensive: bodies retire between ticks, never mid-tick
      }
      BodyIdT a{event.slot_a, core_.bodies_.generation(event.slot_a)};
      BodyIdT b{event.slot_b, core_.bodies_.generation(event.slot_b)};
      typename D::Vec normal = event.normal;
      if (a.index > b.index) {
        std::swap(a, b);
        normal = -normal;
      }
      core_.events_.push_back(
          ContactEventT{event.phase, a, b, event.point, normal, event.speed});
    }

   private:
    WorldCore& core_;
  };

  CoreDesc desc_;
  HandleTable bodies_;
  HandleTable joints_;
  std::unique_ptr<Backend> backend_;

  std::vector<TransformT> transforms_;   // slot-indexed pose mirror
  std::vector<ContactEventT> events_;     // drained each tick, capacity fixed
  std::vector<JointRecord> records_;     // joint slot -> body slots
  float accumulator_ = 0.0f;
  float alpha_ = 0.0f;
  bool overflow_warned_ = false;
};

}  // namespace ws::physics::detail
