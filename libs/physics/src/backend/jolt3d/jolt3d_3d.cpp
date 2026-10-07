// The Jolt Physics v5.6.0 backend behind the 3D seam (core/backend3d.hpp).
// This is the only 3D file that sees Jolt headers - the workspace no-leak
// rule; apps see ws::physics3d and nothing else.
//
// Slot mapping: the lib owns the generational handles and addresses this
// backend by dense slot. slot -> BodyID lives in a pre-sized vector (one
// allocation at create, none after). The reverse direction is two-headed:
//   - callbacks that carry Body refs read mUserData (slot + 1; 0 = never
//     set);
//   - OnContactRemoved carries only a SubShapeIDPair and its bodies may
//     already be destroyed (destroy cascades fire removed at the NEXT
//     Update - same shape as Box2D's stale end events), so it resolves
//     through a flat BodyID-index -> slot table maintained on the
//     control thread at create/destroy time (FLAT, not a hash map: node
//     inserts allocate mid-tick and the zero-alloc window spawns bodies
//     while stepping). A dead BodyID misses the table (its entry was
//     cleared at destroy, and a recycled Jolt index is seq-checked
//     away) and the event is dropped - exactly the 2D stale-shape
//     contract: the lib's destroy path owns that cleanup silently.
//
// Events: Jolt fires ContactListener callbacks on solver worker threads
// DURING Update, with every body locked (only the passed refs may be
// read; a locking interface would deadlock). The backend buffers raw
// events into one mutex-protected pre-reserved vector and, AFTER the
// tick, folds in the resolved removed events, sorts everything by the
// same tiebreak chain the lib re-sorts by, and feeds the collector in
// that order. The lib's cap guards only the hit stream and drops the
// newest: began/ended bypass it (a dropped `ended` would stick a
// consumer's touching state forever), so with a value-sorted feed
// WHICH hits survive a spent cap is the same every run even though
// callback arrival order is not. (Box2D drains engine arrays in engine
// order - deterministic for a different reason.) The raw buffer holds
// 4x the cap and cannot be edge-lossless: growing it inside the tick
// would allocate mid-step and break the zero-alloc contract, so a tick
// with more contact-lifecycle changes than 4x drops nondeterministically,
// edges included - Box2D's engine-side event arrays have the same
// overflow corner.
//
// One Jolt semantic is passed through verbatim (documented + tested):
// when a body falls asleep Jolt removes its contacts, so a resting pair
// reports `ended` at sleep and `began` again on wake. Box2D keeps
// sleeping contacts silent. The lib does not paper over engine
// semantics; a parity pass can add suppression behind this seam later.
//
// Forces: Jolt clears accumulated force/torque on every simulated body
// at the end of the last collision step (PhysicsSystem.cpp, the sleep
// check) - the public "forces clear at the end of every tick" holds
// with no backend-side work. AddForce/AddTorque activate by default, so
// a body carrying a force is always simulated and always reset.
//
// Damping is zeroed to match Box2D's default: the lib's BodyDesc has no
// damping in v1 and the two dimensions must behave alike (Jolt defaults
// both to 0.05).
//
// Thread pool (Desc::thread_count): 0 -> JobSystemSingleThreaded
// (every callback on the stepping thread), N -> JobSystemThreadPool with
// N workers. Jolt's own determinism contract matches the lib's: same
// binary + same thread count -> identical results.

#include <Jolt/Jolt.h>  // must be first: the JPH_* config macros

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyType.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/DecoratedShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/EPhysicsUpdateError.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

#include <ws/core/log.hpp>
#include <ws/physics/errors.hpp>

#include "core/backend3d.hpp"

namespace ws::physics::detail {
namespace {

using physics3d::Aabb;
using physics3d::BodyDesc;
using physics3d::Part;
using physics3d::Transform;
using ws::math::Quat;
using ws::math::Vec3;

[[nodiscard]] JPH::Vec3 to_jph(Vec3 v) {
  return JPH::Vec3{v.x, v.y, v.z};
}
[[nodiscard]] JPH::Quat to_jph(Quat q) {
  return JPH::Quat{q.x, q.y, q.z, q.w};
}
[[nodiscard]] Vec3 from_jph(JPH::Vec3Arg v) {
  return Vec3{v.GetX(), v.GetY(), v.GetZ()};
}
[[nodiscard]] Quat from_jph(const JPH::Quat& q) {
  return Quat{q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
}

// Job-system sizing straight from Jolt's PhysicsSettings.h (cMaxPhysicsJobs
// / cMaxPhysicsBarriers) - the constants its own HelloWorld passes. Powers of
// two by design: FixedSizeFreeList::Init asserts IsPowerOf2(page size), and
// in debug builds JPH_DEBUG auto-enables JPH_ENABLE_ASSERTS (Core.h:562 ->
// IssueReporting.h:14) even with the CMake USE_ASSERTS option off, so a bad
// value is an instant __debugbreak(). Fixed at construction, nothing per step.
constexpr unsigned int kMaxPhysicsJobs = 2048;
constexpr unsigned int kMaxPhysicsBarriers = 8;
// One collision step per tick, like Jolt's samples; the lib's fixed_dt
// is the granularity both engines see.
constexpr int kCollisionSteps = 1;
// Scratch for solver temporaries: one allocation at construction, reused
// every tick (C heap; the malloc fallback only kicks in if the fixed part
// runs out, and that is engine business).
constexpr unsigned int kTempAllocatorBytes = 10u * 1024u * 1024u;
// The raw event buffer holds a multiple of the lib's cap: overflow beyond
// that drops nondeterministically (see the file header).
constexpr std::size_t kRawEventHeadroom = 4;
// mUserData carries slot + 1; 0 means "never set".
constexpr std::uint32_t kNoSlot = 0xffffffffu;

// Jolt's bootstrap is process-wide, not per world: the default allocator
// hooks, the Factory holding the RTTI registry, then the type registration
// - in that exact order (RegisterTypes asserts on a missing
// factory/allocator and, with asserts compiled out, dereferences the null
// Factory). Worlds can be created many times (several per test binary).
std::once_flag& jolt_registration_once() {
  static std::once_flag flag;
  return flag;
}

// Collision layers are NOT exposed in v1 (deferred with the masks), so
// this is the minimal split Jolt's samples use: static bodies never
// refit the broadphase, everything else lives in the moving layer - and
// every layer pair collides (the default filters accept everything, so
// the stock base classes are passed to Init unchanged).
constexpr JPH::ObjectLayer kLayerStatic = 0;
constexpr JPH::ObjectLayer kLayerMoving = 1;
// Direct-init braces, not "= 0": the ctor is explicit, so copy-init from
// an int is rejected.
constexpr JPH::BroadPhaseLayer kBpLayerStatic{0};
constexpr JPH::BroadPhaseLayer kBpLayerMoving{1};

class BpLayerInterface final : public JPH::BroadPhaseLayerInterface {
 public:
  [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override {
    return 2;
  }
  [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(
      JPH::ObjectLayer layer) const override {
    return layer == kLayerStatic ? kBpLayerStatic : kBpLayerMoving;
  }
  // GetBroadPhaseLayerName exists only under JPH_PROFILE_ENABLED /
  // JPH_EXTERNAL_PROFILE - this build defines neither, so there is
  // nothing to override (and layer names are a profiler nicety anyway).
};

// The ws layer/mask rule, Jolt-shaped: every body packs the lib's
// (collision_layer, collision_mask) into CollisionGroup's
// (GroupID, SubGroupID) and this world-owned filter answers the
// both-directions check the pair gate asks (Body::sFindCollidingPairsCanCollide,
// which feeds contacts AND sensors). The ObjectLayer static/moving split
// above is only broadphase bucketing and stays untouched: an object layer
// says WHERE a body sorts, the group says WHAT it may touch.
class LayerMaskFilter final : public JPH::GroupFilter {
 public:
  [[nodiscard]] bool CanCollide(const JPH::CollisionGroup& g1,
                                const JPH::CollisionGroup& g2) const override {
    return (g1.GetGroupID() & g2.GetSubGroupID()) != 0 &&
           (g2.GetGroupID() & g1.GetSubGroupID()) != 0;
  }
};

// Cache key for the shape cache. Density rides the key because Jolt bakes
// mass into the shape (mDensity), so two bodies with the same geometry
// but different densities must NOT share - while equal (shape, density)
// pairs do share, which is what keeps steady-state create_body free of
// engine-side shape churn.
struct ShapeKey {
  std::uint8_t type = 0;  // 0 sphere, 1 box, 2 capsule
  float v0 = 0.0f;
  float v1 = 0.0f;
  float v2 = 0.0f;
  float density = 0.0f;
};

[[nodiscard]] bool operator<(const ShapeKey& l, const ShapeKey& r) {
  return std::tie(l.type, l.v0, l.v1, l.v2, l.density) <
         std::tie(r.type, r.v0, r.v1, r.v2, r.density);
}

[[nodiscard]] ShapeKey make_shape_key(const physics3d::Shape& shape,
                                      float density) {
  ShapeKey key{};
  std::visit(
      [&](const auto& s) {
        using S = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<S, physics3d::Sphere>) {
          key.type = 0;
          key.v0 = s.radius;
        } else if constexpr (std::is_same_v<S, physics3d::Box>) {
          key.type = 1;
          key.v0 = s.half_extents.x;
          key.v1 = s.half_extents.y;
          key.v2 = s.half_extents.z;
        } else {
          key.type = 2;
          key.v0 = s.half_height;
          key.v1 = s.radius;
        }
      },
      shape);
  key.density = density;
  return key;
}

// One buffered contact lifecycle change. Began/hit entries know their
// slots at callback time (mUserData); ended entries start as raw BodyIDs
// (OnContactRemoved cannot touch bodies) and are resolved after the tick.
struct RawEvent {
  std::uint32_t slot_a = 0;
  std::uint32_t slot_b = 0;
  physics3d::ContactPhase phase = physics3d::ContactPhase::began;
  Vec3 point{};
  Vec3 normal{};
  float speed = 0.0f;
};

struct RawRemoved {
  JPH::BodyID id_a{};
  JPH::BodyID id_b{};
};

// Jolt body index -> the lib slot it currently holds. FLAT on purpose:
// an unordered_map node is a heap allocation per insert, and the
// zero-alloc window destroys AND creates bodies mid-tick. Jolt's index
// stays below Init's max_bodies, and the 8-bit sequence number is the
// liveness token (BodyID.h): a queued event for a destroyed body misses
// (its entry was cleared at destroy), and a recycled index misses too
// (the sequence bumped on reuse) - the same stale-id drop the 2D adapter
// gets from b2Shape_IsValid.
struct JoltIndexEntry {
  std::uint32_t slot = kNoSlot;
  std::uint8_t seq = 0;
};

[[nodiscard]] std::uint32_t slot_for_index(
    const std::vector<JoltIndexEntry>& index, const JPH::BodyID& id) {
  const std::uint32_t body_index = id.GetIndex();
  if (body_index >= index.size()) {
    return kNoSlot;  // defensive: Jolt caps indices at Init's max_bodies
  }
  const JoltIndexEntry& entry = index[body_index];
  return entry.slot != kNoSlot && entry.seq == id.GetSequenceNumber()
             ? entry.slot
             : kNoSlot;
}

// The full tiebreak chain, on the AS-REPORTED slots: a pure function of
// event content, so the sorted feed (and therefore which events the
// lib's cap drops) is identical every run.
[[nodiscard]] bool raw_event_less(const RawEvent& l, const RawEvent& r) {
  // make_tuple, not tie: the phase cast makes a temporary, and tie needs
  // lvalues. By-value tuples compare lexicographically all the same.
  return std::make_tuple(l.slot_a, l.slot_b,
                         static_cast<std::uint8_t>(l.phase),
                         l.point.x, l.point.y, l.point.z,
                         l.normal.x, l.normal.y, l.normal.z, l.speed) <
         std::make_tuple(r.slot_a, r.slot_b,
                         static_cast<std::uint8_t>(r.phase),
                         r.point.x, r.point.y, r.point.z,
                         r.normal.x, r.normal.y, r.normal.z, r.speed);
}

// Broadphase query collector that folds every overlapping body's lib slot
// into the output. Overlap runs at BROADPHASE fidelity on purpose:
// Box2D v3's b2World_OverlapCircle/OverlapAabb are dynamic-tree queries
// against fat AABBs too (no narrowphase shape test), so the dimensions
// agree. Queries are not the hot path (world.hpp says so) - the collector
// lives on the stack per call.
class OverlapCollector final : public JPH::CollideShapeBodyCollector {
 public:
  OverlapCollector(const std::vector<JoltIndexEntry>& index,
                   std::vector<std::uint32_t>& out)
      : index_(index), out_(out) {}

  void AddHit(const JPH::BodyID& id) override {
    const std::uint32_t slot = slot_for_index(index_, id);
    if (slot != kNoSlot) {
      out_.push_back(slot);
    }
  }

 private:
  const std::vector<JoltIndexEntry>& index_;
  std::vector<std::uint32_t>& out_;
};

[[nodiscard]] JPH::EMotionType motion_type_of(ws::physics::BodyKind kind) {
  switch (kind) {
    case ws::physics::BodyKind::static_:
      return JPH::EMotionType::Static;
    case ws::physics::BodyKind::kinematic:
      return JPH::EMotionType::Kinematic;
    default:
      return JPH::EMotionType::Dynamic;
  }
}

// Any unit vector perpendicular to the hinge axis: Jolt constrains the
// relative rotation between the two NORMAL axes, and both sides sharing
// one vector makes the relative angle read 0 at the creation pose
// (Box2D's referenceAngle). Same pick-a-helper trick Jolt's own samples
// use for their fixed-basis hinges.
[[nodiscard]] JPH::Vec3 perp_to_axis(JPH::Vec3Arg axis) {
  const JPH::Vec3 helper =
      std::abs(JPH::Vec3::sAxisY().Dot(axis)) < 0.5f
          ? JPH::Vec3::sAxisY()
          : JPH::Vec3::sAxisX();  // axis is near +-Y: use X as the helper
  return axis.Cross(helper).Normalized();
}

class JoltWorld3D final : public IWorld3DBackend,
                          private JPH::ContactListener {
 public:
  explicit JoltWorld3D(const BackendDesc3D& desc)
      : hit_threshold_(desc.hit_speed_threshold) {
    // FIRST statement, before anything Jolt allocates: every JPH container
    // (Array through STLAllocator), the temp allocator, the job system and
    // Init() all go through the global JPH::Allocate hooks, which are null
    // function pointers until this one-time bootstrap runs - the bootstrap
    // used to sit below active_ids_.reserve() and the first world died
    // calling a null Allocate. Member initializers above this line are
    // safe: the JPH members (BodyIDVector, PhysicsSystem) have
    // allocation-free default constructors.
    std::call_once(jolt_registration_once(), [] {
      JPH::RegisterDefaultAllocator();
      JPH::Factory::sInstance = new JPH::Factory();
      JPH::RegisterTypes();
      // Deliberately no UnregisterTypes(): it clears the shared Factory and
      // the registry must outlive every world - one process-lifetime
      // registration is exactly what Jolt's own samples do in main().
    });
    // After the bootstrap (see the member's comment): the world-owned
    // pair rule every body's CollisionGroup will point at.
    mask_filter_ = new LayerMaskFilter();

    bodies_.resize(desc.max_bodies);   // slot -> engine id (invalid = free)
    body_live_.assign(desc.max_bodies, 0);
    // The lib does not gate apply_*/set_velocity by kind (world_core.hpp);
    // Jolt asserts on impulse/force into a static body, so the adapter
    // self-gates and needs the kind per slot without a body lock.
    kinds_.assign(desc.max_bodies, ws::physics::BodyKind::static_);
    joints_.resize(desc.max_joints);   // slot -> engine constraint
    joint_live_.assign(desc.max_joints, 0);
    slot_by_index_.resize(desc.max_bodies);  // flat: no insert allocs, ever
    // Per-part surface materials, one flat table per property (slot-major,
    // kMaxPartsPerBody stride). Pre-sized here so a mid-tick compound spawn
    // writes in place: the zero-alloc window test covers those too.
    part_friction_.assign(std::size_t{desc.max_bodies} *
                              ws::physics::kMaxPartsPerBody,
                          0.0f);
    part_restitution_.assign(std::size_t{desc.max_bodies} *
                                 ws::physics::kMaxPartsPerBody,
                             0.0f);
    const std::size_t raw_capacity =
        std::max<std::size_t>(kRawEventHeadroom * desc.max_contact_events,
                              16u);
    raw_events_.reserve(raw_capacity);
    removed_.reserve(raw_capacity);
    active_ids_.reserve(desc.max_bodies);

    // Solver scratch, one buffer for the world's lifetime (malloc fallback
    // only past the fixed part - engine business, not counted by the
    // wrapper's alloc tests).
    temp_allocator_ =
        std::make_unique<JPH::TempAllocatorImplWithMallocFallback>(
            kTempAllocatorBytes);

    if (desc.thread_count == 0) {
      jobs_ = std::make_unique<JPH::JobSystemSingleThreaded>(
          kMaxPhysicsJobs);
    } else {
      jobs_ = std::make_unique<JPH::JobSystemThreadPool>(
          kMaxPhysicsJobs, kMaxPhysicsBarriers,
          static_cast<int>(desc.thread_count));
    }

    // Pair/constraint budgets sized past what the body cap can produce
    // (Jolt's samples run 10240 bodies on 10240 pairs/constraints).
    const std::uint32_t budget =
        std::clamp<std::uint32_t>(desc.max_bodies * 8u, 1024u, 65536u);
    // Jolt's Array-backed containers reserve(max_bodies) through the global
    // JPH::Allocate, which asserts inSize > 0 (Memory.cpp:24) - feeding the
    // engine a literal zero aborts the first debug build that creates a
    // zero-capacity world. That world is legal by contract: the lib's slot
    // table refuses every body before the backend is ever asked. So floor
    // only the ENGINE's view of capacity at 1; the tables above keep the
    // true zero and stay the sole authority on who may create bodies.
    const std::uint32_t jolt_capacity = std::max(desc.max_bodies, 1u);
    system_.Init(jolt_capacity, 0 /* auto body mutexes */, budget, budget,
                layers_, obj_vs_bp_, pair_filter_);
    system_.SetGravity(to_jph(desc.gravity));
    system_.SetContactListener(this);
  }

  ~JoltWorld3D() override = default;
  JoltWorld3D(const JoltWorld3D&) = delete;
  JoltWorld3D& operator=(const JoltWorld3D&) = delete;

  // --- bodies ---------------------------------------------------------------

  [[nodiscard]] ws::core::Status create_body(
      std::uint32_t slot, const BodyDesc& desc) override {
    if (slot >= bodies_.size()) {
      return fail(PhysicsError::backend_error, "body slot out of range");
    }

    // Only dynamic bodies consume density; never hand the engine a
    // non-positive one for them (the lib validates dynamic density > 0,
    // but non-dynamic bodies may carry any finite value).
    const float density = desc.kind == ws::physics::BodyKind::dynamic
                              ? desc.density
                              : std::max(desc.density, 0.0f);

    // Shape + surface-material source: part_count != 0 defines ALL shapes
    // and the body-level friction/restitution are ignored for physics.
    JPH::Ref<JPH::Shape> shape;
    float friction = desc.friction;
    float restitution = desc.restitution;
    bool multi_part = false;
    if (desc.part_count == 0) {
      shape = shape_for(desc.shape, density);
      if (shape == nullptr) {
        return fail(PhysicsError::backend_error, "Jolt refused a shape");
      }
    } else if (desc.part_count == 1) {
      // One part: the part's material IS the body's (baked body-level
      // below - a compound would collapse anyway: Create returns the bare
      // child for a single sub-shape, dropping any per-sub-shape data). A
      // non-zero offset rides a RotatedTranslated decorator over the
      // cached leaf.
      const Part& part = desc.parts[0];
      JPH::Ref<JPH::Shape> leaf = shape_for(part.shape, density);
      if (leaf == nullptr) {
        return fail(PhysicsError::backend_error, "Jolt refused a part shape");
      }
      if (part.offset == Vec3{}) {
        shape = leaf;
      } else {
        JPH::RotatedTranslatedShapeSettings rotated(
            to_jph(part.offset), JPH::Quat::sIdentity(), leaf.GetPtr());
        JPH::ShapeSettings::ShapeResult result = rotated.Create();
        if (result.HasError()) {
          return fail(PhysicsError::backend_error,
                      "Jolt refused an offset part shape");
        }
        shape = result.Get();
      }
      friction = part.friction;
      restitution = part.restitution;
    } else {
      // Two or more parts: a StaticCompoundShape. AddShape order IS parts
      // order (the BVH build only permutes its own node array), so the
      // sub-shape index names the part at contact time. Jolt's material
      // mixing does not see sub-shapes, so the per-part materials ride the
      // flat tables and the contact listener re-combines; friction here
      // is only the unreachable-with-parts fallback.
      JPH::StaticCompoundShapeSettings compound;
      for (std::uint32_t p = 0; p < desc.part_count; ++p) {
        const Part& part = desc.parts[p];
        JPH::Ref<JPH::Shape> leaf = shape_for(part.shape, density);
        if (leaf == nullptr) {
          return fail(PhysicsError::backend_error, "Jolt refused a part shape");
        }
        compound.AddShape(to_jph(part.offset), JPH::Quat::sIdentity(),
                          leaf.GetPtr());
      }
      JPH::ShapeSettings::ShapeResult result = compound.Create();
      if (result.HasError()) {
        return fail(PhysicsError::backend_error,
                    "Jolt refused the compound shape");
      }
      shape = result.Get();
      const std::size_t base =
          std::size_t{slot} * ws::physics::kMaxPartsPerBody;
      for (std::uint32_t i = 0; i < desc.part_count; ++i) {
        part_friction_[base + i] = desc.parts[i].friction;
        part_restitution_[base + i] = desc.parts[i].restitution;
      }
      multi_part = true;
    }

    JPH::BodyCreationSettings bcs(
        shape.GetPtr(), to_jph(desc.transform.position),
        to_jph(desc.transform.rotation), motion_type_of(desc.kind),
        desc.kind == ws::physics::BodyKind::static_ ? kLayerStatic
                                                    : kLayerMoving);
    bcs.mUserData = static_cast<std::uint64_t>(slot) + 1u;
    bcs.mLinearVelocity = to_jph(desc.linear_velocity);
    bcs.mAngularVelocity = to_jph(desc.angular_velocity);
    bcs.mFriction = friction;        // body-level, or the lone part's
    bcs.mRestitution = restitution;  // (multi-part pairs override per part)
    bcs.mGravityFactor = desc.gravity_scale;
    // CCD: LinearCast sweeps the body from start to end pose, so a fast
    // body stops at a thin static wall instead of tunneling. Discrete is
    // the cheap default; statics never move so the quality never matters
    // for them (the setting is per MotionProperties).
    bcs.mMotionQuality = desc.is_bullet
                             ? JPH::EMotionQuality::LinearCast
                             : JPH::EMotionQuality::Discrete;
    if (multi_part) {
      // Coplanar parts would merge their manifolds and keep ONE sub-shape's
      // material for the whole merged contact (the manifold carries a single
      // sub-shape pair); turning reduction off keeps per-part materials
      // honest at the cost of a few extra contact constraints.
      bcs.mUseManifoldReduction = false;
    }
    bcs.mIsSensor = desc.is_sensor;
    // (layer, mask) ride CollisionGroup's ids; the world-owned
    // LayerMaskFilter turns them back into the both-directions rule.
    bcs.mCollisionGroup = JPH::CollisionGroup(mask_filter_.GetPtr(),
                                               desc.collision_layer,
                                               desc.collision_mask);
    // Box2D parity: no damping unless the lib exposes it (v1 does not).
    bcs.mLinearDamping = 0.0f;
    bcs.mAngularDamping = 0.0f;

    JPH::Body* body = interface().CreateBody(bcs);
    if (body == nullptr) {
      return fail(PhysicsError::backend_error, "Jolt refused a body");
    }
    const JPH::BodyID id = body->GetID();
    // Awake at birth, like Box2D's default body def; a fresh body joins
    // the active set and reports itself moved on its first tick.
    interface().AddBody(id, desc.kind == ws::physics::BodyKind::static_
                                ? JPH::EActivation::DontActivate
                                : JPH::EActivation::Activate);
    bodies_[slot] = id;
    body_live_[slot] = 1;
    kinds_[slot] = desc.kind;
    slot_by_index_[id.GetIndex()] = JoltIndexEntry{slot,
                                                   id.GetSequenceNumber()};
    return {};
  }

  void destroy_body(std::uint32_t slot) override {
    if (slot >= bodies_.size() || !body_live_[slot]) {
      return;
    }
    const JPH::BodyID id = bodies_[slot];
    interface().RemoveBody(id);
    interface().DestroyBody(id);
    bodies_[slot] = JPH::BodyID{};
    body_live_[slot] = 0;
    // Contacts of the destroyed body fire OnContactRemoved at the next
    // Update with this (now dead) BodyID - the cleared index entry drops
    // them (and a recycled Jolt index is seq-checked away).
    slot_by_index_[id.GetIndex()] = JoltIndexEntry{};
  }

  void set_transform(std::uint32_t slot, const Transform& t) override {
    if (slot >= bodies_.size() || !body_live_[slot]) {
      return;
    }
    interface().SetPositionAndRotation(bodies_[slot], to_jph(t.position),
                                        to_jph(t.rotation),
                                        JPH::EActivation::Activate);
  }

  void set_velocity(std::uint32_t slot, Vec3 linear, Vec3 angular) override {
    if (slot >= bodies_.size() || !body_live_[slot]) {
      return;
    }
    // Static bodies carry no motion state in Jolt (no MotionProperties);
    // Box2D silently stores and forgets, so the 3D contract is "no-op".
    if (kinds_[slot] == ws::physics::BodyKind::static_) {
      return;
    }
    interface().SetLinearVelocity(bodies_[slot], to_jph(linear));
    interface().SetAngularVelocity(bodies_[slot], to_jph(angular));
    interface().ActivateBody(bodies_[slot]);
  }

  void set_filter(std::uint32_t slot, std::uint32_t layer,
                  std::uint32_t mask) override {
    if (slot >= bodies_.size() || !body_live_[slot]) {
      return;
    }
    // SetCollisionGroup does NOT activate: a sleeping body keeps its
    // contacts until something wakes it - the documented public contract.
    // The broadphase re-gates every pair each tick, so awake pairs flip
    // on the very next one.
    interface().SetCollisionGroup(
        bodies_[slot],
        JPH::CollisionGroup(mask_filter_.GetPtr(), layer, mask));
  }

  void set_bullet(std::uint32_t slot, bool enabled) override {
    if (slot >= bodies_.size() || !body_live_[slot]) {
      return;
    }
    // BodyManager::SetMotionQuality swaps the quality under the
    // active-bodies lock and adjusts the CCD-body counter - no
    // activation, and none is needed (only a moving body can tunnel,
    // and moving bodies are awake). A static/kinematic slot is a no-op
    // in practice: no MotionProperties means the quality never gates
    // anything the engine does for it. MotionPropertiesUnchecked null
    // guards it inside Jolt itself.
    interface().SetMotionQuality(
        bodies_[slot], enabled ? JPH::EMotionQuality::LinearCast
                               : JPH::EMotionQuality::Discrete);
  }

  void set_gravity(ws::math::Vec3 gravity) override {
    // SetGravity only stores the vector (PhysicsSystem.h): a sleeping
    // body would stay glued to its contacts through the whole tilt, so
    // wake every live body - ActivateBody skips statics
    // (BodyManager::ActivateBodies checks IsStatic) and merely resets
    // the sleep timer on bodies that are already awake.
    system_.SetGravity(to_jph(gravity));
    for (std::uint32_t slot = 0; slot < bodies_.size(); ++slot) {
      if (body_live_[slot]) {
        interface().ActivateBody(bodies_[slot]);
      }
    }
  }

  void apply_linear_impulse(std::uint32_t slot, Vec3 impulse) override {
    if (!dynamic_live_at(slot)) {
      return;
    }
    interface().ActivateBody(bodies_[slot]);
    interface().AddImpulse(bodies_[slot], to_jph(impulse));
  }

  void apply_angular_impulse(std::uint32_t slot, Vec3 impulse) override {
    if (!dynamic_live_at(slot)) {
      return;
    }
    interface().ActivateBody(bodies_[slot]);
    interface().AddAngularImpulse(bodies_[slot], to_jph(impulse));
  }

  void apply_force(std::uint32_t slot, Vec3 force) override {
    if (!dynamic_live_at(slot)) {
      return;
    }
    // EActivation::Activate is Jolt's default and Box2D's wake=true: a
    // sleeping body takes the force and simulates this very tick.
    interface().AddForce(bodies_[slot], to_jph(force),
                         JPH::EActivation::Activate);
  }

  void apply_torque(std::uint32_t slot, Vec3 torque) override {
    if (!dynamic_live_at(slot)) {
      return;
    }
    interface().AddTorque(bodies_[slot], to_jph(torque),
                          JPH::EActivation::Activate);
  }

  [[nodiscard]] BackendVelocity3D get_velocity(
      std::uint32_t slot) const override {
    BackendVelocity3D v{};
    if (slot >= bodies_.size() || !body_live_[slot]) {
      return v;
    }
    v.linear = from_jph(interface().GetLinearVelocity(bodies_[slot]));
    v.angular = from_jph(interface().GetAngularVelocity(bodies_[slot]));
    return v;
  }

  // --- joints -----------------------------------------------------------------

  [[nodiscard]] ws::core::Status create_joint(
      std::uint32_t slot, const BackendJoint3D& joint) override {
    if (slot >= joints_.size() || joint.slot_a >= bodies_.size() ||
        joint.slot_b >= bodies_.size() || !body_live_[joint.slot_a] ||
        !body_live_[joint.slot_b]) {
      return fail(PhysicsError::backend_error, "joint slots out of range");
    }
    const JPH::BodyID a = bodies_[joint.slot_a];
    const JPH::BodyID b = bodies_[joint.slot_b];
    JPH::TwoBodyConstraint* constraint = nullptr;
    switch (joint.type) {
      case BackendJoint3D::Type::fixed: {
        // Weld at the CURRENT relative pose: no snap on creation.
        JPH::FixedConstraintSettings settings;
        settings.mAutoDetectPoint = true;
        constraint = interface().CreateConstraint(&settings, a, b);
        break;
      }
      case BackendJoint3D::Type::hinge: {
        JPH::HingeConstraintSettings settings;
        settings.mSpace = JPH::EConstraintSpace::WorldSpace;
        settings.mPoint1 = to_jph(joint.world_anchor);
        settings.mPoint2 = to_jph(joint.world_anchor);
        const JPH::Vec3 axis = to_jph(joint.world_axis);
        settings.mHingeAxis1 = axis;
        settings.mHingeAxis2 = axis;
        // Jolt measures the relative angle between the two normal axes
        // around the hinge; both sides get the same world vector, so the
        // angle reads 0 at the creation pose (Box2D's referenceAngle).
        const JPH::Vec3 normal = perp_to_axis(axis);
        settings.mNormalAxis1 = normal;
        settings.mNormalAxis2 = normal;
        if (joint.enable_limits) {
          settings.mLimitsMin = joint.lower_angle;
          settings.mLimitsMax = joint.upper_angle;
        }
        if (joint.enable_motor) {
          // SetMotorState asserts the settings are valid unless Off, so
          // the torque limits go on first (the lib guarantees > 0).
          settings.mMotorSettings.mMinTorqueLimit = -joint.max_motor_torque;
          settings.mMotorSettings.mMaxTorqueLimit = joint.max_motor_torque;
        }
        constraint = interface().CreateConstraint(&settings, a, b);
        if (constraint != nullptr && joint.enable_motor) {
          auto* hinge = static_cast<JPH::HingeConstraint*>(constraint);
          hinge->SetMotorState(JPH::EMotorState::Velocity);
          hinge->SetTargetAngularVelocity(joint.motor_speed);
        }
        break;
      }
      case BackendJoint3D::Type::distance: {
        JPH::DistanceConstraintSettings settings;
        settings.mSpace = JPH::EConstraintSpace::WorldSpace;
        // Anchored at both body origins; the target length is the
        // caller's, not the current distance (the 2D contract).
        settings.mPoint1 = interface().GetPosition(a);
        settings.mPoint2 = interface().GetPosition(b);
        settings.mMinDistance = joint.length;
        settings.mMaxDistance = joint.length;
        constraint = interface().CreateConstraint(&settings, a, b);
        break;
      }
    }
    if (constraint == nullptr) {
      return fail(PhysicsError::backend_error, "Jolt refused a joint");
    }
    system_.AddConstraint(constraint);
    // Box2D wakes both bodies when a joint lands; mirror that so a joint
    // into a sleeping pair actually starts simulating.
    interface().ActivateConstraint(constraint);
    joints_[slot] = constraint;
    joint_live_[slot] = 1;
    return {};
  }

  void destroy_joint(std::uint32_t slot) override {
    if (slot >= joints_.size() || !joint_live_[slot]) {
      return;
    }
    system_.RemoveConstraint(joints_[slot]);
    joints_[slot] = nullptr;
    joint_live_[slot] = 0;
  }

  // --- stepping -----------------------------------------------------------------

  std::size_t step(float dt, IEventCollector3D& collector,
                   std::span<BackendMove3D> moves_out) override {
    // Per-CALL drain (the review's event-loss fix): both buffers empty
    // before the engine runs. Callbacks only fire inside Update, so
    // nothing can race the clear - the lock is the belt over the braces.
    {
      const std::scoped_lock lock{events_mutex_};
      raw_events_.clear();
      removed_.clear();
    }

    const JPH::EPhysicsUpdateError err =
        system_.Update(dt, kCollisionSteps, temp_allocator_.get(),
                       jobs_.get());
    if (err != JPH::EPhysicsUpdateError::None) {
      // Soft failure: Jolt kept stepping but dropped pairs/constraints
      // past its caches. Surface it, carry on - the lib's Status does not
      // model a partial tick.
      ws::core::warn("physics: Jolt update hit a cache limit (mask {})",
                     static_cast<std::uint32_t>(err));
    }

    // Moved bodies = the awake set (Box2D's move events are exactly the
    // bodies it simulated). Statics never wake; a fresh body is born
    // awake, so it reports itself moved on its first tick - un-lerped
    // first render, same as 2D.
    active_ids_.clear();
    system_.GetActiveBodies(JPH::EBodyType::RigidBody, active_ids_);
    std::size_t count = 0;
    for (const JPH::BodyID& id : active_ids_) {
      if (count >= moves_out.size()) {
        break;  // defensive: the lib sizes the span at max_bodies
      }
      const std::uint32_t moved_slot = slot_for_index(slot_by_index_, id);
      if (moved_slot == kNoSlot) {
        continue;  // defensive: index table and slot table stay in lockstep
      }
      JPH::RVec3 position;
      JPH::Quat rotation;
      interface().GetPositionAndRotation(id, position, rotation);
      moves_out[count++] =
          BackendMove3D{moved_slot, Transform{from_jph(position),
                                              from_jph(rotation)}};
    }

    // Post-tick fold. Update has returned, so no callback can be in
    // flight; the lock stays because the buffers are shared with the
    // callback path by design. Resolve the buffered removals - a dead
    // BodyID misses the index table and drops, exactly the 2D stale-shape
    // contract - then sort the whole batch on the tiebreak chain and
    // feed it in that order. With the feed value-sorted, which hits a
    // spent lib cap drops is a pure function of event content (edges
    // bypass the cap entirely).
    {
      const std::scoped_lock lock{events_mutex_};
      for (const RawRemoved& removed : removed_) {
        const std::uint32_t a = slot_for_index(slot_by_index_, removed.id_a);
        const std::uint32_t b = slot_for_index(slot_by_index_, removed.id_b);
        if (a == kNoSlot || b == kNoSlot) {
          continue;
        }
        if (raw_events_.size() < raw_events_.capacity()) {
          // Ended events carry no geometry (Box2D's end events do not
          // either); the tiebreak chain only reaches point/normal/speed
          // after slots and phase.
          raw_events_.push_back(RawEvent{a, b,
                                         physics3d::ContactPhase::ended});
        }
      }
      std::sort(raw_events_.begin(), raw_events_.end(), raw_event_less);
      for (const RawEvent& e : raw_events_) {
        collector.add(BackendEvent3D{e.slot_a, e.slot_b, e.phase, e.point,
                                     e.normal, e.speed});
      }
      raw_events_.clear();
      removed_.clear();
    }
    return count;
  }

  // --- queries ----------------------------------------------------------------

  [[nodiscard]] std::optional<BackendRayHit3D> raycast(
      Vec3 origin, Vec3 direction, float max_distance) const override {
    // direction arrives unit (world3d.cpp normalizes); the ray length IS
    // the search distance - the shape of the query the 2D seam makes too.
    const JPH::RRayCast ray{to_jph(origin), to_jph(direction) * max_distance};
    JPH::RayCastResult result;
    if (!system_.GetNarrowPhaseQuery().CastRay(ray, result)) {
      return std::nullopt;
    }
    const std::uint32_t hit_slot =
        slot_for_index(slot_by_index_, result.mBodyID);
    if (hit_slot == kNoSlot) {
      return std::nullopt;  // defensive: hit body retired mid-query
    }
    const JPH::RVec3 point =
        ray.mOrigin + result.mFraction * ray.mDirection;
    BackendRayHit3D hit{};
    hit.slot = hit_slot;
    hit.t = result.mFraction * max_distance;  // seam t = distance, like 2D
    hit.point = from_jph(point);
    // Closest-hit gives no surface normal: read it off the body under a
    // read lock (control thread; locking interface is the safe one here).
    // Succeeded() is the whole validity story - GetBody() returns a
    // reference, so there is no null to check.
    const JPH::BodyLockRead lock(system_.GetBodyLockInterface(),
                                 result.mBodyID);
    if (!lock.Succeeded()) {
      return std::nullopt;  // defensive: body vanished before the read
    }
    hit.normal = from_jph(
        lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2,
                                                  point));
    return hit;
  }

  void overlap_sphere(Vec3 center, float radius,
                      std::vector<std::uint32_t>& out) const override {
    OverlapCollector collector(slot_by_index_, out);
    system_.GetBroadPhaseQuery().CollideSphere(to_jph(center), radius,
                                               collector);
  }

  void overlap_aabb(const Aabb& box,
                    std::vector<std::uint32_t>& out) const override {
    OverlapCollector collector(slot_by_index_, out);
    const JPH::AABox aabb{to_jph(box.min), to_jph(box.max)};
    system_.GetBroadPhaseQuery().CollideAABox(aabb, collector);
  }

 private:
  // --- ContactListener: worker threads during Update, bodies locked ----
  // Contract from ContactListener.h: the passed refs are readable and
  // NOTHING else may be touched (a locking interface deadlocks, state
  // changes race). Bodies arrive sorted by BodyID (body1 < body2), NOT by
  // lib slot; the lib's collector re-orders the pair and flips the
  // normal, so events feed in engine order.

  void OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2,
                      const JPH::ContactManifold& inManifold,
                      JPH::ContactSettings& ioSettings) override {
    // Per-part materials land on the first step of a fresh pair; the
    // listener owns the flat tables (no world registry a plain function
    // pointer would need), so this is where the override lives.
    apply_part_materials(inBody1, inManifold.mSubShapeID1, inBody2,
                         inManifold.mSubShapeID2, ioSettings);
    const std::uint32_t a = user_slot(inBody1);
    const std::uint32_t b = user_slot(inBody2);
    if (a == kNoSlot || b == kNoSlot) {
      return;  // defensive: a body older than the slot mapping
    }
    // One witness point: the first manifold point, falling back to the
    // COM midpoint for an empty (speculative) manifold.
    JPH::RVec3 point = 0.5f * (inBody1.GetCenterOfMassPosition() +
                               inBody2.GetCenterOfMassPosition());
    if (inManifold.mRelativeContactPointsOn1.size() > 0) {
      point = inManifold.GetWorldSpaceContactPointOn1(0);
    }
    // mWorldSpaceNormal points body 1 -> body 2 (the lib re-orients).
    const JPH::Vec3 normal = inManifold.mWorldSpaceNormal;
    // Approach speed from the PRE-solve velocities - the solver has not
    // run yet, exactly when Box2D measures its hit events. The getters
    // are static-safe (zero for static bodies).
    const JPH::Vec3 v1 =
        inBody1.GetLinearVelocity() +
        inBody1.GetAngularVelocity().Cross(
            point - inBody1.GetCenterOfMassPosition());
    const JPH::Vec3 v2 =
        inBody2.GetLinearVelocity() +
        inBody2.GetAngularVelocity().Cross(
            point - inBody2.GetCenterOfMassPosition());
    const float speed = std::max(0.0f, -(v2 - v1).Dot(normal));
    push_raw(RawEvent{a, b, physics3d::ContactPhase::began,
                      from_jph(point), from_jph(normal), speed});
    // Jolt has no separate hit callback: same rule as Box2D's hit events,
    // approach speed over the world's threshold (> 0 keeps resting
    // contacts quiet when the threshold is the default 0).
    if (speed > hit_threshold_) {
      push_raw(RawEvent{a, b, physics3d::ContactPhase::hit,
                        from_jph(point), from_jph(normal), speed});
    }
  }

  void OnContactPersisted(const JPH::Body& inBody1, const JPH::Body& inBody2,
                          const JPH::ContactManifold& inManifold,
                          JPH::ContactSettings& ioSettings) override {
    // Still no EVENTS per tick while a pair rests (began/ended fire once
    // per overlap - the public contact.hpp contract). But the combined
    // material is NOT cached by the engine: every step rebuilds each
    // contact constraint from fresh defaults, so the per-part override
    // must land here too or it would silently revert to body-level
    // mixing from the second step of a resting pair onward.
    apply_part_materials(inBody1, inManifold.mSubShapeID1, inBody2,
                         inManifold.mSubShapeID2, ioSettings);
  }

  void OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair) override {
    // The bodies may already be destroyed (a destroy cascade fires its
    // removals at the NEXT Update): buffer raw ids, resolve post-tick. A
    // dead pair misses the index table and drops - the destroy-silence
    // contract.
    push_removed(RawRemoved{inSubShapePair.GetBody1ID(),
                            inSubShapePair.GetBody2ID()});
  }

  // Which part of a multi-part body a sub-shape ID touches, or -1 when the
  // touching shape is not a compound (single shape, or a lone part under a
  // RotatedTranslated decorator - both take the body-level material, which
  // the one-part path bakes to the part's values). Multi-part bodies carry
  // the StaticCompoundShape bare (no decorators), and AddShape order IS
  // parts order (the BVH build permutes only its own node array), so the
  // compound's sub-shape index is the part index. GetSubShapeUserData is
  // useless here: it returns the LEAF's own user data, and our cached
  // leaves are shared across bodies.
  [[nodiscard]] static int part_index(const JPH::Body& body,
                                      const JPH::SubShapeID& id) {
    const JPH::Shape* shape = body.GetShape();
    if (shape == nullptr ||
        shape->GetSubType() != JPH::EShapeSubType::StaticCompound) {
      return -1;
    }
    const auto* compound = static_cast<const JPH::CompoundShape*>(shape);
    JPH::SubShapeID remainder;
    const std::uint32_t index =
        compound->GetSubShapeIndexFromID(id, remainder);
    return index < compound->GetNumSubShapes() ? int(index) : -1;
  }

  // Per-part materials at contact time. A pair where at least one side
  // touches a compound part overrides the combined values; plain pairs
  // keep the engine's own combine untouched (bit-identical v1 behavior).
  // The override repeats the engine's default mixes - friction geometric
  // mean, restitution max, the same formulas Box2D applies natively - so
  // both dimensions and both pair kinds agree. Worker-thread safety: the
  // tables are written only by create_body on the control thread, never
  // during Update (single-threaded API contract).
  void apply_part_materials(const JPH::Body& inBody1,
                            const JPH::SubShapeID& inId1,
                            const JPH::Body& inBody2,
                            const JPH::SubShapeID& inId2,
                            JPH::ContactSettings& ioSettings) {
    const int p1 = part_index(inBody1, inId1);
    const int p2 = part_index(inBody2, inId2);
    if (p1 < 0 && p2 < 0) {
      return;
    }
    float f1 = inBody1.GetFriction();
    float e1 = inBody1.GetRestitution();
    float f2 = inBody2.GetFriction();
    float e2 = inBody2.GetRestitution();
    const std::uint32_t a = user_slot(inBody1);
    const std::uint32_t b = user_slot(inBody2);
    if (p1 >= 0 && a != kNoSlot) {
      const std::size_t i = std::size_t{a} * ws::physics::kMaxPartsPerBody +
                            std::size_t(p1);
      f1 = part_friction_[i];
      e1 = part_restitution_[i];
    }
    if (p2 >= 0 && b != kNoSlot) {
      const std::size_t i = std::size_t{b} * ws::physics::kMaxPartsPerBody +
                            std::size_t(p2);
      f2 = part_friction_[i];
      e2 = part_restitution_[i];
    }
    ioSettings.mCombinedFriction = JPH::Sqrt(f1 * f2);
    ioSettings.mCombinedRestitution = std::max(e1, e2);
  }

  // Callback-side appends: a worker thread, so the lock is required; a
  // full buffer drops (the 4x-headroom corner in the file header).
  void push_raw(const RawEvent& e) {
    const std::scoped_lock lock{events_mutex_};
    if (raw_events_.size() < raw_events_.capacity()) {
      raw_events_.push_back(e);
    }
  }
  void push_removed(const RawRemoved& r) {
    const std::scoped_lock lock{events_mutex_};
    if (removed_.size() < removed_.capacity()) {
      removed_.push_back(r);
    }
  }

  [[nodiscard]] static std::uint32_t user_slot(const JPH::Body& body) {
    const std::uint64_t ud = body.GetUserData();
    return ud == 0 ? kNoSlot : static_cast<std::uint32_t>(ud - 1u);
  }

  // --- helpers ----------------------------------------------------------------

  // The control-thread face. The LOCKING interface is the only correct
  // one outside Update; callbacks use no interface at all (see
  // OnContactAdded), and queries use their own const accessors.
  [[nodiscard]] JPH::BodyInterface& interface() {
    return system_.GetBodyInterface();
  }
  [[nodiscard]] const JPH::BodyInterface& interface() const {
    return system_.GetBodyInterface();
  }

  // The lib does not gate apply_* by kind (world_core.hpp); Jolt asserts
  // on a static, and kinematics carry no mass to push. So: forces,
  // torques and impulses land only on live DYNAMIC bodies.
  [[nodiscard]] bool dynamic_live_at(std::uint32_t slot) const {
    return slot < bodies_.size() && body_live_[slot] != 0 &&
           kinds_[slot] == ws::physics::BodyKind::dynamic;
  }

  // One shape per distinct (geometry, density) pair - Jolt bakes mass
  // into the shape, which is why density rides the cache key. A new key
  // allocates once (warm-up covers it); everything after is a map hit
  // and create_body stays allocation-free.
  [[nodiscard]] JPH::Ref<JPH::Shape> shape_for(const physics3d::Shape& shape,
                                               float density) {
    const ShapeKey key = make_shape_key(shape, density);
    const auto it = shape_cache_.find(key);
    if (it != shape_cache_.end()) {
      return it->second;
    }
    JPH::ShapeSettings::ShapeResult result;
    std::visit(
        [&](const auto& s) {
          using S = std::decay_t<decltype(s)>;
          // Settings are mutated (SetDensity), so they cannot be const.
          if constexpr (std::is_same_v<S, physics3d::Sphere>) {
            JPH::SphereShapeSettings settings(s.radius);
            settings.SetDensity(density);
            result = settings.Create();
          } else if constexpr (std::is_same_v<S, physics3d::Box>) {
            // The default convex radius is safe: Jolt clamps it down to
            // the smallest half extent itself (BoxShape.h).
            JPH::BoxShapeSettings settings(to_jph(s.half_extents));
            settings.SetDensity(density);
            result = settings.Create();
          } else {
            JPH::CapsuleShapeSettings settings(s.half_height, s.radius);
            settings.SetDensity(density);
            result = settings.Create();
          }
        },
        shape);
    if (result.HasError()) {
      return nullptr;  // create_body turns this into a backend_error
    }
    JPH::Ref<JPH::Shape> made = result.Get();
    shape_cache_.emplace(key, made);
    return made;
  }

  // --- members ---------------------------------------------------------------
  // Declaration order is REVERSE destruction order and it matters:
  // system_ must die FIRST, while the allocator, job system and filter
  // objects it holds pointers into are all still alive - the same order
  // Jolt's own samples declare theirs in.
  std::unique_ptr<JPH::TempAllocatorImplWithMallocFallback> temp_allocator_;
  BpLayerInterface layers_;  // Init stores this pointer
  JPH::ObjectVsBroadPhaseLayerFilter obj_vs_bp_;  // stock: accept all
  JPH::ObjectLayerPairFilter pair_filter_;        // stock: accept all
  // The ws (layer, mask) rule every body carries; ref-held so bodies'
  // CollisionGroups can point at it for the world's lifetime. Allocated
  // in the ctor body AFTER the allocator bootstrap - GroupFilter-derived
  // news ride JPH::Allocate, which is null until RegisterDefaultAllocator.
  JPH::Ref<LayerMaskFilter> mask_filter_;
  std::unique_ptr<JPH::JobSystem> jobs_;
  std::vector<JPH::BodyID> bodies_;               // slot -> engine id
  // Per-part surface materials, slot-major with kMaxPartsPerBody stride;
  // read by the contact callbacks (see apply_part_materials).
  std::vector<float> part_friction_;
  std::vector<float> part_restitution_;
  std::vector<std::uint8_t> body_live_;
  std::vector<ws::physics::BodyKind> kinds_;  // adapter self-gates on it
  std::vector<JPH::TwoBodyConstraint*> joints_;  // slot -> engine constraint
  std::vector<std::uint8_t> joint_live_;
  std::vector<JoltIndexEntry> slot_by_index_;  // Jolt body index -> lib slot
  std::mutex events_mutex_;  // guards raw_events_ / removed_
  std::vector<RawEvent> raw_events_;
  std::vector<RawRemoved> removed_;
  JPH::BodyIDVector active_ids_;  // Array<BodyID>: Jolt's C-heap allocator
  float hit_threshold_ = 0.0f;
  std::map<ShapeKey, JPH::Ref<JPH::Shape>> shape_cache_;
  JPH::PhysicsSystem system_;
};

}  // namespace

std::unique_ptr<IWorld3DBackend> make_jolt_world_3d(
    const BackendDesc3D& desc) {
  try {
    return std::make_unique<JoltWorld3D>(desc);
  } catch (const std::bad_alloc&) {
    ws::core::warn("physics: Jolt backend out of memory");
    return nullptr;
  } catch (const std::exception& e) {
    ws::core::warn("physics: Jolt backend failed to start: {}", e.what());
    return nullptr;
  }
}

}  // namespace ws::physics::detail
