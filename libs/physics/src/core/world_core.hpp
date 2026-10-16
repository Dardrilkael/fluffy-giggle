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
    // The touching table: headroom for 4 distinct pairs per body (>= 16
    // entries, capped at 65536 at construction). One allocation, final
    // size; growth beyond it is amortized scene-complexity growth, never
    // steady-state churn (the edge-flood precedent).
    std::uint64_t pair_cap = std::uint64_t{4} * desc.max_bodies;
    pair_cap = std::clamp(pair_cap, std::uint64_t{16}, std::uint64_t{65536});
    std::size_t pairs_pow2 = std::size_t{16};
    while (pairs_pow2 < pair_cap) {
      pairs_pow2 <<= 1;
    }
    pairs_.assign(pairs_pow2, TouchPair{});
    pairs_mask_ = pairs_pow2 - 1;
    sleeping_pairs_.reserve(32);  // frozen pairs: the pile-sleeping subset
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
    // The one per-create copy, kept on the stack: BodyDesc carries its parts
    // in a fixed array (not a vector), so this copy allocates nothing - even
    // a mid-tick compound spawn inside the zero-alloc window test is free.
    BodyDescT local = desc;
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
    // The touching table dies with the body, SILENTLY (destroy-silence):
    // no ended events for a body the consumer already gave up, and no key
    // a reused slot could ever collide with.
    purge_pairs_for_slot(*slot);
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

  ws::core::Status set_collision_filter(BodyIdT id, std::uint32_t layer,
                                        std::uint32_t mask) {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    // Any bit pattern is legal (the engines take the bits verbatim) and
    // the backend owns the truth, so there is no lib-side mirror to keep.
    backend_->set_filter(*slot, layer, mask);
    return {};
  }

  // CCD flag: a bool, so nothing to validate beyond the handle; the
  // backend owns the truth.
  ws::core::Status set_bullet(BodyIdT id, bool enabled) {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    backend_->set_bullet(*slot, enabled);
    return {};
  }

  // Sleep control: bools and a bool readback, so nothing to validate
  // beyond the handle. The wake RULES live here in the doc, the backend
  // makes its engine obey them (CONTRACT.md 6).
  ws::core::Status set_sleepable(BodyIdT id, bool allowed) {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    backend_->set_sleepable(*slot, allowed);
    return {};
  }

  ws::core::Status wake(BodyIdT id) {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    backend_->wake(*slot);
    return {};
  }

  ws::core::Result<bool> is_sleeping(BodyIdT id) const {
    const auto slot = resolve_body(id);
    if (!slot) {
      return fail(PhysicsError::invalid_handle, "stale or unknown body");
    }
    return backend_->is_sleeping(*slot);
  }

  // No handle, no validation: every vector is a legal gravity (zero-g
  // included). The backend owns the engine truth AND the wake-up the
  // engines themselves skip.
  void set_gravity(Vec gravity) { backend_->set_gravity(gravity); }

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

  // --- lib-owned touching table (CONTRACT.md 7) -------------------------------
  // The engine-independent "who is touching whom": backends report only raw
  // begin/end edges, and THIS table decides the sleep/wake semantics for both
  // engines - no began/ended is ever caused by a body falling asleep or
  // waking up, whichever engine runs underneath. Keyed on the full SHAPE
  // pair (slots + parts), because engines report contacts per shape pair and
  // a second part's began must never read as a duplicate of the first.
  //
  // Open addressing, linear probing, tombstones on erase (probe paths stay
  // intact). Sized at construction for 4x max_bodies pairs (capped), grown
  // x2 at 3/4 load like the edge-flood precedent: steady state allocates
  // nothing, and growth means genuine scene-complexity growth, never churn.
  struct TouchPair {
    std::uint32_t slot_a = 0;  // canonical: slot_a < slot_b (creation order)
    std::uint32_t slot_b = 0;
    std::uint8_t part_a = 0;   // rides slot_a
    std::uint8_t part_b = 0;   // rides slot_b
    static constexpr std::uint8_t kEmpty = 0;
    static constexpr std::uint8_t kAlive = 1;     // touching, both sides live
    static constexpr std::uint8_t kFrozen = 2;    // touching-asleep (see below)
    static constexpr std::uint8_t kTombstone = 3;
    std::uint8_t state = kEmpty;
  };

  // The frozen subset: pairs whose `ended` was suppressed because one side
  // was asleep when it arrived (the lib cannot tell a sleep-purge from a
  // genuine separation at event time). Reconciliation: once the sleeper
  // MOVES again (the moves feed is the awake set in both dims), the pair
  // gets a deadline; if the engine re-adds the touch before it lapses -
  // Jolt does, on wake - the began confirms the pair and the deadline dies
  // quietly; if not, the separation was real and the lib emits the ended
  // itself, a couple of ticks late. frozen_at guards the same-tick case: a
  // body settling to sleep appears in THAT tick's moves feed, and a pair
  // frozen that same tick must not start its window yet - the window opens
  // on the sleeper's next motion, i.e. its wake.
  struct SleepingPair {
    std::uint32_t slot_a = 0;
    std::uint32_t slot_b = 0;
    std::uint8_t part_a = 0;
    std::uint8_t part_b = 0;
    std::uint32_t frozen_at = 0;  // tick_count_ low bits at the freeze
    std::uint32_t deadline = 0;   // 0 = unstamped; else absolute tick to emit
  };

  [[nodiscard]] static std::uint64_t pair_hash(std::uint32_t a, std::uint32_t b,
                                               std::uint8_t pa,
                                               std::uint8_t pb) {
    std::uint64_t h = 0x9E3779B97F4A7C15ull ^ (std::uint64_t{a} << 32) ^
                      std::uint64_t{b} ^ (std::uint64_t{pa} << 40) ^
                      (std::uint64_t{pb} << 48);
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBull;
    h ^= h >> 31;
    return h;
  }

  [[nodiscard]] std::size_t pair_home(std::uint32_t a, std::uint32_t b,
                                      std::uint8_t pa, std::uint8_t pb) const {
    return static_cast<std::size_t>(pair_hash(a, b, pa, pb)) & pairs_mask_;
  }

  // Probe to the pair's entry, skipping tombstones; npos when absent.
  [[nodiscard]] std::size_t find_pair(std::uint32_t a, std::uint32_t b,
                                      std::uint8_t pa, std::uint8_t pb) const {
    if (pairs_.empty()) {
      return static_cast<std::size_t>(-1);
    }
    std::size_t i = pair_home(a, b, pa, pb);
    for (std::size_t probe = 0; probe < pairs_.size(); ++probe) {
      const TouchPair& p = pairs_[i];
      if (p.state == TouchPair::kEmpty) {
        return static_cast<std::size_t>(-1);
      }
      if (p.state == TouchPair::kAlive || p.state == TouchPair::kFrozen) {
        if (p.slot_a == a && p.slot_b == b && p.part_a == pa &&
            p.part_b == pb) {
          return i;
        }
      }
      i = (i + 1) & pairs_mask_;
    }
    return static_cast<std::size_t>(-1);  // table full of tombstones (grown)
  }

  // Place a fresh alive entry (or resurrect a tombstone seen while probing).
  // The caller has already checked absence via find_pair.
  void insert_pair(std::uint32_t a, std::uint32_t b, std::uint8_t pa,
                   std::uint8_t pb) {
    if ((pairs_used_ + 1) * 4 >= pairs_.size() * 3) {
      grow_pairs();
    }
    std::size_t i = pair_home(a, b, pa, pb);
    std::size_t first_tombstone = static_cast<std::size_t>(-1);
    // Post-growth the load is under 3/4, so this loop always finds an
    // empty slot; a tombstone seen on the way wins over it (probe chains
    // through a reused tombstone must still find this pair later).
    while (true) {
      TouchPair& p = pairs_[i];
      if (p.state == TouchPair::kEmpty) {
        if (first_tombstone == static_cast<std::size_t>(-1)) {
          ++pairs_used_;  // fresh slot; a tombstone reuse counts nothing
        } else {
          i = first_tombstone;
        }
        pairs_[i] = TouchPair{a, b, pa, pb, TouchPair::kAlive};
        return;
      }
      if (p.state == TouchPair::kTombstone &&
          first_tombstone == static_cast<std::size_t>(-1)) {
        first_tombstone = i;
      }
      i = (i + 1) & pairs_mask_;
    }
  }

  // Drop a live/frozen pair without a trace beyond the tombstone.
  void erase_pair(std::size_t i) {
    if (i < pairs_.size() && pairs_[i].state != TouchPair::kEmpty &&
        pairs_[i].state != TouchPair::kTombstone) {
      pairs_[i].state = TouchPair::kTombstone;
      // pairs_used_ counts tombstones too: load stays honest, the next
      // grow compacts them away.
    }
  }

  // Amortized x2 growth; alive/frozen pairs rehash, tombstones vanish.
  void grow_pairs() {
    std::vector<TouchPair> old;
    old.swap(pairs_);
    const std::size_t cap = old.empty() ? std::size_t{16} : old.size() * 2;
    pairs_.assign(cap, TouchPair{});
    pairs_mask_ = cap - 1;
    pairs_used_ = 0;
    for (const TouchPair& p : old) {
      if (p.state == TouchPair::kAlive || p.state == TouchPair::kFrozen) {
        std::size_t i = pair_home(p.slot_a, p.slot_b, p.part_a, p.part_b);
        while (pairs_[i].state != TouchPair::kEmpty) {
          i = (i + 1) & pairs_mask_;
        }
        pairs_[i] = p;
        ++pairs_used_;
      }
    }
  }

  // destroy_body's half of the destroy-silence contract: every pair that
  // names the slot dies SILENTLY (no ended events - the body is gone, the
  // consumer's per-body state dies with it), and slot reuse can never
  // collide a stale key. The full scan is O(capacity) like the joint scan
  // above it - fine at v1 scales.
  void purge_pairs_for_slot(std::uint32_t slot) {
    for (TouchPair& p : pairs_) {
      if ((p.state == TouchPair::kAlive || p.state == TouchPair::kFrozen) &&
          (p.slot_a == slot || p.slot_b == slot)) {
        p.state = TouchPair::kTombstone;
      }
    }
    for (std::size_t i = sleeping_pairs_.size(); i-- > 0;) {
      const SleepingPair& sp = sleeping_pairs_[i];
      if (sp.slot_a == slot || sp.slot_b == slot) {
        sleeping_pairs_[i] = sleeping_pairs_.back();
        sleeping_pairs_.pop_back();
      }
    }
  }

  // The began-confirmation path: a frozen pair's sleeper woke and the
  // engine re-added the touch, so the pair never broke - drop the
  // reconciliation deadline with it.
  void unfreeze_sleeping(std::uint32_t a, std::uint32_t b, std::uint8_t pa,
                         std::uint8_t pb) {
    for (std::size_t i = sleeping_pairs_.size(); i-- > 0;) {
      const SleepingPair& sp = sleeping_pairs_[i];
      if (sp.slot_a == a && sp.slot_b == b && sp.part_a == pa &&
          sp.part_b == pb) {
        sleeping_pairs_[i] = sleeping_pairs_.back();
        sleeping_pairs_.pop_back();
        return;
      }
    }
  }

  // The reconciliation half of the sleep policy (CONTRACT.md 7): runs at
  // the END of every tick, after the moves fold. A world with nothing
  // frozen costs one empty check - sleeping worlds stay free. Two passes:
  //
  // Stamp: every entry in THIS tick's raw moves feed is a body the engine
  // simulated - in both dims that is the awake set, so a moved slot in a
  // frozen pair is the sleeper waking (the feed is per TICK on purpose:
  // moved_raw_ is per CALL and would keep a long-woken slot looking fresh
  // on later ticks of one advance()). A pair frozen this same tick is
  // skipped - its sleeper settled into sleep INSIDE this feed, and the
  // window must open on the next motion, the wake. The deadline is two
  // ticks out: enough for the engine's began re-add of a surviving touch
  // to land and confirm the pair before anything is reported.
  //
  // Expire: a lapsed deadline means no began came - the separation was
  // real, so the lib emits the ended itself (zeroed geometry, exactly
  // Box2D's end events) and drops the pair. Wrap-safe compare; backwards
  // iteration with swap-pop (no order to preserve in this list).
  void reconcile_sleeping_pairs(std::uint32_t moved_count) {
    if (sleeping_pairs_.empty()) {
      return;
    }
    const std::uint32_t now = static_cast<std::uint32_t>(tick_count_);
    if (moved_count > 0) {
      for (std::uint32_t i = 0; i < moved_count; ++i) {
        const std::uint32_t slot = backend_moves_[i].slot;
        for (SleepingPair& sp : sleeping_pairs_) {
          if (sp.deadline == 0 && sp.frozen_at != now &&
              (sp.slot_a == slot || sp.slot_b == slot)) {
            sp.deadline = now + 2;
          }
        }
      }
    }
    for (std::size_t i = sleeping_pairs_.size(); i-- > 0;) {
      const SleepingPair sp = sleeping_pairs_[i];
      if (sp.deadline == 0 || std::int32_t(now - sp.deadline) < 0) {
        continue;  // unstamped, or the window is still open
      }
      const std::size_t idx =
          find_pair(sp.slot_a, sp.slot_b, sp.part_a, sp.part_b);
      if (idx != static_cast<std::size_t>(-1) &&
          pairs_[idx].state == TouchPair::kFrozen) {
        // Only a purge can retire a slot while its pair sits here, and
        // the purge drops the entry too - both ids resolve.
        erase_pair(idx);
        const BodyIdT a{sp.slot_a, bodies_.generation(sp.slot_a)};
        const BodyIdT b{sp.slot_b, bodies_.generation(sp.slot_b)};
        events_.push_back(ContactEventT{
            decltype(ContactEventT{}.phase)::ended, a, b, Vec{}, Vec{},
            0.0f});
      }
      sleeping_pairs_[i] = sleeping_pairs_.back();
      sleeping_pairs_.pop_back();
    }
  }

  // Opens one public step()/advance() call: reset the per-call buffers. The
  // prev mirror and the published moved list are NOT reset here - they are
  // per TICK, so zero-tick advance() calls leave both standing (see tick()
  // and end_call()). The seen_ reset touches only the slots the previous
  // call marked (O(previous moved), not O(max_bodies)).
  void begin_call() {
    events_.clear();  // capacity kept; only an edge-flood tick grows past it
    for (const RawMove& m : moved_raw_) {
      seen_[m.slot] = 0;
    }
    moved_raw_.clear();
    ticked_this_call_ = false;
    overflow_drops_ = 0;
  }

  // Closes the call: fix the public event order (determinism contract) and
  // publish the moved list with live public handles, sorted by index. The
  // moved list is rebuilt ONLY when a tick ran - a zero-tick advance()
  // keeps the last tick's list standing, so renderers driven off it keep
  // interpolating through frames that had no physics time.
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
    if (ticked_this_call_) {
      moved_.clear();
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
    }
    // Re-arm the overflow warning once a call completes without dropping
    // anything: one warning per overflow episode, not per world lifetime.
    if (overflow_drops_ == 0) {
      overflow_warned_ = false;
    }
  }

  // One fixed tick of the engine. Appends to the CALL's buffers; the sort,
  // cap reporting and handle resolution happen once, in end_call().
  void tick() {
    // Low 32 bits feed the sleeping-pair deadlines (wrap-safe compares);
    // the u64 never wraps in practice.
    ++tick_count_;
    // The prev mirror is per TICK, snapped only on the FIRST tick of the
    // call (a multi-tick advance() lerps across the whole arc). A zero-tick
    // advance() never touches it, so interpolation endpoints stay stable
    // while alpha grows across frames with no physics time.
    if (!ticked_this_call_) {
      prev_transforms_ = transforms_;  // same size since construction
      ticked_this_call_ = true;
    }
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
    // The drain (collector.add) has finished, so any began that confirms a
    // frozen pair already did its work; what is left is deadline stamping
    // (this tick's awake set) and expiry - suppressed separations whose
    // re-add window has lapsed.
    reconcile_sleeping_pairs(static_cast<std::uint32_t>(count));
  }

  // IEventCollector2D/3D implementation: resolves engine slots to public
  // handles, orders the pair by BodyId index (flipping the normal so it
  // keeps pointing a -> b), caps the hit stream at max_contact_events
  // (began/ended bypass the cap - see add()), warns once per overflow
  // episode. The backend must feed events in a deterministic order so that
  // WHICH hits overflow-drop is itself deterministic.
  class EventCollector final : public D::CollectorBase {
   public:
    explicit EventCollector(WorldCore& core) : core_(core) {}

    void add(const BackendEvent& event) override {
      // The budget guards the high-volume stream: hit events (impacts) are
      // dropped once max_contact_events is spent. began/ended are EDGE
      // events that consumers build touching-state from - dropping an
      // `ended` leaves a stuck "touching" pair forever - so they are NEVER
      // dropped: the buffer grows past the budget instead. That growth is
      // a pathological tick, not steady state (the zero-alloc contract
      // covers steady state), and it is amortized like any vector.
      const bool is_hit = event.phase == decltype(event.phase)::hit;
      if (is_hit && core_.events_.size() >= core_.desc_.max_contact_events) {
        if (!core_.overflow_warned_) {
          core_.overflow_warned_ = true;
          ws::core::warn(
              "physics: contact-event budget spent ({}); dropping hit "
              "events (began/ended are never dropped)",
              core_.desc_.max_contact_events);
        }
        ++core_.overflow_drops_;
        return;
      }
      if (!core_.bodies_.live(event.slot_a) ||
          !core_.bodies_.live(event.slot_b)) {
        return;  // defensive: bodies retire between calls, never mid-call
      }
      // One canonical order for the table key and the public event: pair
      // by BodyId index (== slot), parts riding their slots, normal
      // flipped to keep pointing a -> b.
      std::uint32_t sa = event.slot_a;
      std::uint32_t sb = event.slot_b;
      std::uint8_t pa = event.part_a;
      std::uint8_t pb = event.part_b;
      typename D::Vec normal = event.normal;
      if (sa > sb) {
        std::swap(sa, sb);
        std::swap(pa, pb);
        normal = -normal;
      }
      // The lib's sleep/contact-event policy (CONTRACT.md 7): edges flow
      // through the touching table so the SEMANTICS are engine-neutral.
      // began for a pair the table already knows is an engine re-fire of
      // an existing touch - Jolt re-adds purged contacts on wake - and is
      // suppressed (a frozen pair unfreezes instead: the touch outlived
      // the sleep). ended that arrives while one side sleeps is frozen
      // (suppressed, reconciled later): the lib cannot tell a sleep-purge
      // from a genuine separation at event time, and no ended caused by
      // sleep may reach the consumer. Hits never touch the table.
      if (event.phase == decltype(event.phase)::began) {
        const std::size_t idx = core_.find_pair(sa, sb, pa, pb);
        if (idx != static_cast<std::size_t>(-1)) {
          if (core_.pairs_[idx].state == TouchPair::kFrozen) {
            // The sleeper woke and the engine re-added the touch: the
            // pair never broke from the consumer's point of view.
            core_.pairs_[idx].state = TouchPair::kAlive;
            core_.unfreeze_sleeping(sa, sb, pa, pb);
          }
          return;  // duplicate began: the touch was reported when it began
        }
        core_.insert_pair(sa, sb, pa, pb);
      } else if (event.phase == decltype(event.phase)::ended) {
        const std::size_t idx = core_.find_pair(sa, sb, pa, pb);
        if (idx == static_cast<std::size_t>(-1)) {
          // Not tracked (raced a purge, or an engine double-ended): pass
          // through so a consumer that saw the began stays honest.
        } else if (core_.pairs_[idx].state == TouchPair::kFrozen) {
          return;  // already consumed at freeze time
        } else if (core_.backend_->is_sleeping(sa) ||
                   core_.backend_->is_sleeping(sb)) {
          core_.pairs_[idx].state = TouchPair::kFrozen;
          core_.sleeping_pairs_.push_back(
              SleepingPair{sa, sb, pa, pb,
                           static_cast<std::uint32_t>(core_.tick_count_), 0});
          return;  // suppressed: no ended is ever caused by falling asleep
        } else {
          core_.erase_pair(idx);
        }
      }
      BodyIdT a{sa, core_.bodies_.generation(sa)};
      BodyIdT b{sb, core_.bodies_.generation(sb)};
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
  // Mirror before the last TICK that ran (snapped on a call's first tick,
  // never on zero-tick calls - the interpolation contract).
  std::vector<TransformT> prev_transforms_;
  std::vector<ContactEventT> events_;   // drained per call; pre-reserved to
                                       // the budget, edge floods may grow it
  std::vector<JointRecord> records_;    // joint slot -> body slots
  std::vector<BackendMove> backend_moves_;  // step() scratch, slot-indexed
  std::vector<RawMove> moved_raw_;      // deduped moved slots this call
  std::vector<BodyMoveT> moved_;        // public moved list: the last tick's
  std::vector<std::uint32_t> seen_;     // slot -> moved_raw_ index+1 (0 = no)
  std::vector<ws::physics::BodyKind> kinds_;  // slot -> body kind
  std::vector<std::uintptr_t> user_data_;  // slot -> opaque payload (lib-owned)
  double accumulator_ = 0.0;  // double: a float one drifts over long sessions
  float alpha_ = 0.0f;
  std::uint32_t overflow_drops_ = 0;
  bool ticked_this_call_ = false;  // first-tick snapshot gate (see tick())
  bool overflow_warned_ = false;
  // The lib-owned touching state (CONTRACT.md 7) - see the TouchPair block
  // above for the layout and the policy comments.
  std::vector<TouchPair> pairs_;  // open-addressed, power-of-two capacity
  std::size_t pairs_mask_ = 0;    // capacity - 1
  std::size_t pairs_used_ = 0;    // non-empty entries, tombstones included
  std::vector<SleepingPair> sleeping_pairs_;  // the frozen subset, flat
  std::uint64_t tick_count_ = 0;  // deadlines use the low 32 bits
};

}  // namespace ws::physics::detail
