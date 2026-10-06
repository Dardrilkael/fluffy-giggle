#pragma once

// The backend-independent world machinery, shared by World2D and World3D
// (internal - never included by a public header). Everything that does not
// depend on the dimension's types lives HERE, once: generational handle
// tables, the per-body user-data table, the pose mirror behind transforms()
// and its previous-call copy,
// the moved-bodies list fed by the backend each tick, the contact-event
// buffer with its normalize/sort/cap discipline, the fixed-timestep
// accumulator with the spiral-of-death clamp, bulk set_transforms
// atomicity, and the destroy_body -> joint cascade.
//
// The dimension is a Traits type supplied by the per-dimension .cpp:
//   struct Dim2D;  // in world2d.cpp - aliases Vec2/float types + the 2D seam
//   struct Dim3D;  // in world3d.cpp
// Beyond the type aliases the traits provide the dimension-specific
// predicates the machinery needs:
//   is_finite(Vec) / is_finite(Angular)  - finiteness for runtime setters
//   is_valid_rotation(Rotation)          - finite AND non-degenerate
//   sanitize_rotation(Rotation&)         - canonicalize (3D: normalize the Quat)
// The public World classes stay separate BY DESIGN (the user-visible API
// is two distinct mirrored types); only the tricky machinery is shared,
// so a fix lands in one place. The per-dim .cpp keeps what genuinely
// differs: Desc validation, shape validation, JointDesc translation
// (public variant with BodyIds -> flat BackendJoint with slots) and the
// query result mapping.
//
// Event + move semantics: contact_events(), moved_bodies() and
// prev_transforms() are all PER step()/advance() CALL - an advance() that
// runs several fixed ticks unions the ticks' events and moved bodies and
// reports them once, which is how a game consumes them (once per frame).
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
#include <string>
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
  using Rotation = typename D::Rotation;    // float / Quat
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
  using BackendMove = typename D::BackendMove;
  using BackendVelocity = typename D::BackendVelocity;

  WorldCore(const CoreDesc& desc, std::unique_ptr<Backend> backend)
      : desc_(desc),
        bodies_(desc.max_bodies),
        joints_(desc.max_joints),
        backend_(std::move(backend)) {
    transforms_.resize(desc.max_bodies);       // one allocation, final size
    prev_transforms_.resize(desc.max_bodies);
    events_.reserve(desc.max_contact_events);  // capacity only
    records_.resize(desc.max_joints);
    backend_moves_.resize(desc.max_bodies);   // step() scratch
    seen_.assign(desc.max_bodies, 0);          // moved dedupe, 0 = unseen
    moved_raw_.reserve(desc.max_bodies);
    moved_.reserve(desc.max_bodies);
    kinds_.assign(desc.max_bodies, ws::physics::BodyKind::static_);
    user_data_.assign(desc.max_bodies, 0);
  }

  WorldCore(const WorldCore&) = delete;
  WorldCore& operator=(const WorldCore&) = delete;

  // --- bodies -------------------------------------------------------------

  // Pre-validated desc (the dimension layer checks shapes/kinds/material);
  // the rotation is re-checked and canonicalized here because every path
  // that writes a pose goes through the same rule.
  [[nodiscard]] ws::core::Result<BodyIdT> create_body(const BodyDescT& desc) {
    if (!D::is_valid_rotation(desc.transform.rotation)) {
      return fail(PhysicsError::invalid_argument,
                  "body rotation must be finite and non-degenerate");
    }
    BodyDescT local = desc;  // creation is cold: one copy is fine
    D::sanitize_rotation(local.transform.rotation);
    const auto slot = bodies_.allocate();
    if (!slot) {
      return fail(PhysicsError::world_full, "body capacity exhausted");
    }
    if (auto st = backend_->create_body(*slot, local); !st) {
      bodies_.free(*slot);
      return fail(PhysicsError::backend_error,
                  std::string{"backend create_body: "} + st.error().message);
    }
    transforms_[*slot] = local.transform;
    // New bodies spawn at their pose in BOTH mirrors: interpolating a body
    // from a stale pre-birth pose would teleport it across the screen.
    prev_transforms_[*slot] = local.transform;
    kinds_[*slot] = local.kind;
    user_data_[*slot] = local.user_data;
    return BodyIdT{*slot, bodies_.generation(*slot)};
  }

  ws::core::Status destroy_body(BodyIdT id) {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    // Joints die with their bodies (both engines do this too - the lib
    // mirrors it so its own tables stay true). Joints first: the engines
    // refuse joint destruction once the body is gone. The scan is
    // O(max_joints) per destroy - fine for v1 scales; a per-body joint
    // list is the upgrade path if mass despawns ever profile hot.
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
    kinds_[*slot] = ws::physics::BodyKind::static_;
    user_data_[*slot] = 0;  // retired slots never leak a stale payload
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

  // The lib-owned payload table: user data never crosses the backend seam
  // (no engine needs it - every public surface speaks BodyIds), so both are
  // pure slot lookups with the same generational guard as every body path.
  [[nodiscard]] ws::core::Result<std::uintptr_t> body_user_data(BodyIdT id) const {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    return user_data_[*slot];
  }

  ws::core::Status set_body_user_data(BodyIdT id, std::uintptr_t value) {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    user_data_[*slot] = value;
    return {};
  }

  ws::core::Status set_transform(BodyIdT id, TransformT t) {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    if (!D::is_finite(t.position) || !D::is_valid_rotation(t.rotation)) {
      return fail(PhysicsError::invalid_argument,
                  "set_transform: position/rotation must be finite "
                  "(and the rotation non-degenerate)");
    }
    D::sanitize_rotation(t.rotation);
    transforms_[*slot] = t;
    backend_->set_transform(*slot, t);
    return {};
  }

  ws::core::Status set_velocity(BodyIdT id, Vec linear, Angular angular) {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    if (!D::is_finite(linear) || !D::is_finite(angular)) {
      return fail(PhysicsError::invalid_argument,
                  "set_velocity: values must be finite");
    }
    backend_->set_velocity(*slot, linear, angular);
    return {};
  }

  ws::core::Status apply_linear_impulse(BodyIdT id, Vec impulse) {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    if (!D::is_finite(impulse)) {
      return fail(PhysicsError::invalid_argument, "impulse must be finite");
    }
    backend_->apply_linear_impulse(*slot, impulse);
    return {};
  }

  ws::core::Status apply_angular_impulse(BodyIdT id, Angular impulse) {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    if (!D::is_finite(impulse)) {
      return fail(PhysicsError::invalid_argument, "impulse must be finite");
    }
    backend_->apply_angular_impulse(*slot, impulse);
    return {};
  }

  ws::core::Status apply_force(BodyIdT id, Vec force) {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    if (!D::is_finite(force)) {
      return fail(PhysicsError::invalid_argument, "force must be finite");
    }
    backend_->apply_force(*slot, force);
    return {};
  }

  ws::core::Status apply_torque(BodyIdT id, Angular torque) {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    if (!D::is_finite(torque)) {
      return fail(PhysicsError::invalid_argument, "torque must be finite");
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
    if (kinds_[slot_a] != ws::physics::BodyKind::dynamic &&
        kinds_[slot_b] != ws::physics::BodyKind::dynamic) {
      // Solver islands grow from dynamic bodies: a joint with none on
      // either side never simulates and silently does nothing.
      return fail(PhysicsError::invalid_argument,
                  "create_joint: at least one body must be dynamic");
    }
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

  // Events, moved bodies and the prev mirror are all PER CALL (see the
  // file header): step() and advance() both open and close one "call".
  // v1 backends cannot fail mid-step; the Status is reserved for engines
  // that can report step failure.
  ws::core::Status step() {
    begin_call();
    tick();
    end_call();
    return {};
  }

  ws::core::Status advance(float frame_dt) {
    if (!(frame_dt >= 0.0f) || !std::isfinite(frame_dt)) {
      return fail(PhysicsError::invalid_argument, "frame_dt must be finite and >= 0");
    }
    begin_call();
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
      accumulator_ = std::fmod(accumulator_, static_cast<double>(desc_.fixed_dt));
    }
    alpha_ = static_cast<float>(accumulator_ / desc_.fixed_dt);
    end_call();
    return {};
  }

  [[nodiscard]] float interpolation_alpha() const { return alpha_; }
  [[nodiscard]] float fixed_dt() const { return desc_.fixed_dt; }

  // --- events + bulk paths ------------------------------------------------------

  // This CALL's contact events (a multi-tick advance() unions its ticks).
  [[nodiscard]] std::span<const ContactEventT> contact_events() const {
    return std::span<const ContactEventT>{events_};
  }

  [[nodiscard]] std::span<const TransformT> transforms() const {
    return std::span<const TransformT>{transforms_};
  }

  // The pose mirror as it was before this CALL's step()/advance() - the
  // "previous" endpoint for interpolation_alpha() lerping. Index-aligned
  // with transforms(); retired slots hold stale data in both.
  [[nodiscard]] std::span<const TransformT> prev_transforms() const {
    return std::span<const TransformT>{prev_transforms_};
  }

  // Bodies the backend reported as simulated/moved during this CALL, sorted
  // by BodyId index. Engines report awake bodies (Box2D reports every awake
  // body each step, displacement or not); sleeping bodies never appear, so
  // large sleeping worlds cost nothing to sync.
  [[nodiscard]] std::span<const BodyMoveT> moved_bodies() const {
    return std::span<const BodyMoveT>{moved_};
  }

  ws::core::Status set_transforms(std::span<const BodyMoveT> moves) {
    // ATOMIC: every id AND every value is validated first; the apply loop
    // cannot fail. A NaN in entry 50 leaves the world untouched.
    for (const BodyMoveT& m : moves) {
      if (!bodies_.alive(m.id.index, m.id.generation)) {
        return fail(PhysicsError::invalid_handle, "set_transforms: stale or unknown body");
      }
      if (!D::is_finite(m.position) || !D::is_valid_rotation(m.rotation)) {
        return fail(PhysicsError::invalid_argument,
                    "set_transforms: position/rotation must be finite "
                    "(and the rotation non-degenerate)");
      }
    }
    for (const BodyMoveT& m : moves) {
      TransformT t{m.position, m.rotation};
      D::sanitize_rotation(t.rotation);
      transforms_[m.id.index] = t;
      backend_->set_transform(m.id.index, t);
    }
    return {};
  }

  // Query helper: backend returns slots; the dimension layer maps them to
  // public handles through this. Engines return arbitrary order, so sorting
  // by BodyId index makes the result deterministic. Note slot reuse means
  // slot order is NOT creation order - the public contract promises a
  // deterministic order, not a creation order.
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

  // A moved entry before its public handle is resolved.
  struct RawMove {
    std::uint32_t slot;
    TransformT transform;
  };

  // Opens one public step()/advance() call: snapshot the previous pose,
  // reset the per-call buffers. The seen_ reset touches only the slots the
  // previous call marked (O(previous moved), not O(max_bodies)).
  void begin_call() {
    prev_transforms_ = transforms_;  // same size since construction: no realloc
    events_.clear();                 // capacity kept: no reallocation ever
    for (const RawMove& m : moved_raw_) {
      seen_[m.slot] = 0;
    }
    moved_raw_.clear();
    moved_.clear();
    overflow_drops_ = 0;
  }

  // Closes the call: fix the public event order (determinism contract) and
  // publish the moved list with live public handles, sorted by index.
  void end_call() {
    // Same-pair events with the same phase (multi-point contacts, repeated
    // hits) would come out in arbitrary order - std::sort is not stable, so
    // the comparison continues through point, normal and speed.
    std::sort(events_.begin(), events_.end(),
              [](const ContactEventT& l, const ContactEventT& r) {
                if (l.a.index != r.a.index) return l.a.index < r.a.index;
                if (l.b.index != r.b.index) return l.b.index < r.b.index;
                const auto lp = static_cast<std::uint8_t>(l.phase);
                const auto rp = static_cast<std::uint8_t>(r.phase);
                if (lp != rp) return lp < rp;
                if (l.point.x != r.point.x) return l.point.x < r.point.x;
                if (l.point.y != r.point.y) return l.point.y < r.point.y;
                if constexpr (requires { l.point.z; }) {
                  if (l.point.z != r.point.z) return l.point.z < r.point.z;
                }
                if (l.normal.x != r.normal.x) return l.normal.x < r.normal.x;
                if (l.normal.y != r.normal.y) return l.normal.y < r.normal.y;
                if constexpr (requires { l.normal.z; }) {
                  if (l.normal.z != r.normal.z) return l.normal.z < r.normal.z;
                }
                if (l.speed != r.speed) return l.speed < r.speed;
                return false;
              });
    for (const RawMove& m : moved_raw_) {
      if (const auto id = id_for_slot(m.slot)) {
        moved_.push_back(
            BodyMoveT{*id, m.transform.position, m.transform.rotation});
      }
    }
    std::sort(moved_.begin(), moved_.end(),
              [](const BodyMoveT& l, const BodyMoveT& r) {
                return l.id.index < r.id.index;
              });
    // Re-arm the overflow warning once a call completes without dropping
    // anything: one warning per overflow episode, not per world lifetime.
    if (overflow_drops_ == 0) {
      overflow_warned_ = false;
    }
  }

  // One fixed tick of the engine. Appends to the CALL's buffers; the sort,
  // cap reporting and handle resolution happen once, in end_call().
  void tick() {
    EventCollector collector{*this};
    const std::size_t count =
        backend_->step(desc_.fixed_dt, collector, backend_moves_);
    for (std::size_t i = 0; i < count; ++i) {
      const auto& mv = backend_moves_[i];
      if (mv.slot >= transforms_.size()) {
        continue;  // defensive: lib/backend slot maps are built in lockstep
      }
      transforms_[mv.slot] = mv.transform;
      if (seen_[mv.slot] == 0) {
        seen_[mv.slot] = static_cast<std::uint32_t>(moved_raw_.size()) + 1;
        moved_raw_.push_back(RawMove{mv.slot, mv.transform});
      } else {
        moved_raw_[seen_[mv.slot] - 1].transform = mv.transform;
      }
    }
  }

  // IEventCollector2D/3D implementation: resolves engine slots to public
  // handles, orders the pair by BodyId index (flipping the normal so it
  // keeps pointing a -> b), caps the buffer, warns once per overflow
  // episode. The backend must feed events in a deterministic order so that
  // WHICH events overflow-drop is itself deterministic.
  class EventCollector final : public D::CollectorBase {
   public:
    explicit EventCollector(WorldCore& core) : core_(core) {}

    void add(const BackendEvent& event) override {
      if (core_.events_.size() >= core_.desc_.max_contact_events) {
        if (!core_.overflow_warned_) {
          core_.overflow_warned_ = true;
          ws::core::warn(
              "physics: contact-event buffer full ({}); dropping newest events",
              core_.desc_.max_contact_events);
        }
        ++core_.overflow_drops_;
        return;
      }
      if (!core_.bodies_.live(event.slot_a) ||
          !core_.bodies_.live(event.slot_b)) {
        return;  // defensive: bodies retire between calls, never mid-call
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
  std::vector<TransformT> prev_transforms_;  // mirror before this CALL
  std::vector<ContactEventT> events_;   // drained per call, capacity fixed
  std::vector<JointRecord> records_;    // joint slot -> body slots
  std::vector<BackendMove> backend_moves_;  // step() scratch, slot-indexed
  std::vector<RawMove> moved_raw_;      // deduped moved slots this call
  std::vector<BodyMoveT> moved_;        // public moved list this call
  std::vector<std::uint32_t> seen_;     // slot -> moved_raw_ index+1 (0 = no)
  std::vector<ws::physics::BodyKind> kinds_;  // slot -> body kind
  std::vector<std::uintptr_t> user_data_;  // slot -> opaque payload (lib-owned)
  double accumulator_ = 0.0;  // double: a float one drifts over long sessions
  float alpha_ = 0.0f;
  std::uint32_t overflow_drops_ = 0;
  bool overflow_warned_ = false;
};

}  // namespace ws::physics::detail
