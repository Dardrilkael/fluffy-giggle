// The Box2D v3.1.1 backend behind the 2D seam (core/backend2d.hpp). This is
// the only 2D file that sees box2d.h - the workspace no-leak rule; apps see
// ws::physics2d and nothing else.
//
// Slot mapping: the lib owns the generational handles and addresses this
// backend by dense slot. slot -> b2BodyId/b2JointId live in pre-sized
// vectors (one allocation at create, none after), and the reverse direction
// rides Box2D body userData as slot + 1 (0 = never set). Contact/sensor
// events name SHAPES, so each shape's userData packs its full owner -
// (slot + 1) | (part + 1) << 32 - and the drain reads body AND part in one
// call (owner_of_shape): the lib's touching table keys on the shape pair.
//
// Task pool (Desc::thread_count = N > 0): N worker threads with FIXED
// indices 0..N-1; the stepping thread is worker N. Box2D gets
// workerCount = N + 1, clamped so it never exceeds Box2D's own
// B2_MAX_WORKERS (64) - worker indices select per-worker scratch inside
// Box2D, so every concurrently running task must hold a distinct index,
// and every index must be < workerCount.
//
//   Box2D enqueues its constraint solver as workerCount UNIT tasks
//   (itemCount = minRange = 1) whose instances synchronize with each other
//   across internal stage barriers: they must all run CONCURRENTLY or the
//   barriers deadlock. Unit tasks are therefore always queued, never run
//   inline, and finish() helps - the stepping thread runs queued work
//   instead of idling, which is what runs the last solver task.
//   Parallel-for tasks (itemCount > minRange) are split into chunks by a
//   pure function of (itemCount, minRange, workerCount), so same
//   thread_count -> same chunks; the ranges are disjoint and the per-worker
//   scratch is union-merged, so which thread runs which chunk cannot change
//   results. Same binary + same thread_count produces an identical
//   simulation (pinned by the determinism test).
//
//   thread_count = 0 registers no callbacks at all: Box2D then runs every
//   phase serially on the stepping thread, its own default mode.
//
// Forces: Box2D clears accumulated force/torque on every simulated body at
// the end of each step (v3.1.1 solver.c, per-body finalization) - the
// public "forces are cleared at the end of every tick" holds with no
// backend-side work.
//
// Moved bodies: Box2D reports every simulated (awake) body per step via
// b2World_GetBodyEvents - asleep bodies cost nothing - and each event
// carries the body's userData, so the slot mapping is free.

#include <box2d/box2d.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

#include <ws/core/log.hpp>
#include <ws/physics/errors.hpp>

#include "core/backend2d.hpp"

namespace ws::physics::detail {
namespace {

using physics2d::Aabb;
using physics2d::BodyDesc;
using physics2d::Part;
using physics2d::Transform;
using ws::math::Vec2;

[[nodiscard]] b2Vec2 to_b2(Vec2 v) { return b2Vec2{v.x, v.y}; }
[[nodiscard]] Vec2 from_b2(b2Vec2 v) { return Vec2{v.x, v.y}; }

// Box2D's own recommendation ("usually 4"); a fixed constant so a tick
// stays exactly desc.fixed_dt and results stay reproducible.
constexpr int kSubStepCount = 4;

// Fixed task-pool sizing: nothing is allocated after construction. Box2D
// runs at most B2_MAX_WORKERS (64) workers; the arrays must hold a full
// solver phase (workerCount unit tasks) plus whatever parallel-for chunks
// coexist with it, with room to spare.
constexpr std::uint32_t kMaxPoolThreads = 63;  // + the stepping thread = 64
constexpr std::size_t kMaxTaskGroups = 128;
constexpr std::size_t kMaxChunkJobs = 128;

// b2OverlapResultFcn: gather the lib slot behind every overlapping shape.
bool collect_overlap(b2ShapeId shape, void* context) {
  auto* out = static_cast<std::vector<std::uint32_t>*>(context);
  const b2BodyId body = b2Shape_GetBody(shape);
  const auto raw = reinterpret_cast<std::uintptr_t>(b2Body_GetUserData(body));
  if (raw != 0) {
    out->push_back(static_cast<std::uint32_t>(raw - 1u));
  }
  return true;  // keep walking the tree
}

class Box2DWorld final : public IWorld2DBackend {
 public:
  explicit Box2DWorld(const BackendDesc2D& desc)
      : hit_threshold_(desc.hit_speed_threshold) {
    bodies_.resize(desc.max_bodies);
    body_live_.resize(desc.max_bodies);
    kinds_.resize(desc.max_bodies);
    joints_.resize(desc.max_joints);
    joint_live_.resize(desc.max_joints);

    b2WorldDef def = b2DefaultWorldDef();
    def.gravity = to_b2(desc.gravity);
    def.hitEventThreshold = desc.hit_speed_threshold;
    const std::uint32_t threads = std::min(desc.thread_count, kMaxPoolThreads);
    if (threads > 0) {
      // The pool starts before the world exists: a failed start must
      // leave nothing behind (the factory turns the throw into nullptr).
      if (!start_pool(threads)) {
        throw std::runtime_error{"worker-pool startup failed"};
      }
      worker_count_ = static_cast<int>(threads) + 1;
      def.workerCount = worker_count_;
      def.enqueueTask = &Box2DWorld::enqueue_task;
      def.finishTask = &Box2DWorld::finish_task;
      def.userTaskContext = this;
    }
    world_ = b2CreateWorld(&def);
  }

  ~Box2DWorld() override {
    stop_pool();  // workers run Box2D code: joined before the world dies
    if (b2World_IsValid(world_)) {
      b2DestroyWorld(world_);
    }
  }

  Box2DWorld(const Box2DWorld&) = delete;
  Box2DWorld& operator=(const Box2DWorld&) = delete;

  // --- bodies ---------------------------------------------------------------

  [[nodiscard]] ws::core::Status create_body(
      std::uint32_t slot, const BodyDesc& desc,
      const MassData2D& mass) override {
    if (slot >= bodies_.size()) {
      return fail(PhysicsError::backend_error, "body slot out of range");
    }

    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = desc.kind == ws::physics::BodyKind::dynamic
                  ? b2_dynamicBody
                  : (desc.kind == ws::physics::BodyKind::kinematic
                         ? b2_kinematicBody
                         : b2_staticBody);
    bd.position = to_b2(desc.transform.position);
    bd.rotation = b2MakeRot(desc.transform.rotation);
    bd.linearVelocity = to_b2(desc.linear_velocity);
    bd.angularVelocity = desc.angular_velocity;
    bd.gravityScale = desc.gravity_scale;
    bd.isBullet = desc.is_bullet;  // CCD: pairs with this body use sweeps
    bd.enableSleep = desc.allow_sleep;  // false pins the body awake
    bd.userData =
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(slot) + 1u);

    b2ShapeDef sd = b2DefaultShapeDef();
    // Only dynamic bodies consume density; never hand the engine a negative
    // one (the lib allows any finite value for non-dynamic bodies).
    sd.density = desc.kind == ws::physics::BodyKind::dynamic
                     ? desc.density
                     : std::max(desc.density, 0.0f);
    sd.material.friction = desc.friction;
    sd.material.restitution = desc.restitution;
    // The lib's rule IS b2ShouldShapesCollide's groupIndex==0 branch:
    // (catA & maskB) && (catB & maskA). groupIndex stays 0.
    sd.filter.categoryBits = desc.collision_layer;
    sd.filter.maskBits = desc.collision_mask;
    // Sensor events need the flag on BOTH shapes in Box2D v3 (sensor.c
    // skips a pair whose OTHER shape lacks it), so every shape opts in:
    // the lib's contract is "sensor overlaps fire began/ended events"
    // and there is no per-shape subscription knob to expose. Solid
    // pairs never appear in the sensor arrays, so nothing extra fires.
    sd.enableSensorEvents = true;
    if (desc.is_sensor) {
      sd.isSensor = true;
    } else {
      // Contact lifecycle for solid shapes; hit events only when asked for.
      sd.enableContactEvents = true;
      sd.enableHitEvents = hit_threshold_ > 0.0f;
    }
    // Every shape names its owning (slot, part) - the event drain resolves
    // both in one read (owner_of_shape). Part 0 is the body-level shape.
    sd.userData = reinterpret_cast<void*>(pack_shape_owner(slot, 0));

    const b2BodyId body = b2CreateBody(world_, &bd);
    if (!b2Body_IsValid(body)) {
      return fail(PhysicsError::backend_error, "Box2D refused a body");
    }
    // One shape at a body-local offset; the single-shape path passes a zero
    // offset, which is bit-identical to the old centered constructors
    // (b2MakeOffsetBox with identity rotation and a zero center).
    const auto make_shape = [&](const physics2d::Shape& s, Vec2 off,
                                const b2ShapeDef& shape_sd) -> b2ShapeId {
      return std::visit(
          [&](const auto& sh) -> b2ShapeId {
            using S = std::decay_t<decltype(sh)>;
            if constexpr (std::is_same_v<S, physics2d::Circle>) {
              const b2Circle circle{to_b2(off), sh.radius};
              return b2CreateCircleShape(body, &shape_sd, &circle);
            } else if constexpr (std::is_same_v<S, physics2d::Box>) {
              const b2Polygon box = b2MakeOffsetBox(
                  sh.half_extents.x, sh.half_extents.y, to_b2(off),
                  b2MakeRot(0.0f));
              return b2CreatePolygonShape(body, &shape_sd, &box);
            } else {
              const b2Capsule capsule{to_b2(sh.a + off), to_b2(sh.b + off),
                                     sh.radius};
              return b2CreateCapsuleShape(body, &shape_sd, &capsule);
            }
          },
          s);
    };

    if (desc.part_count != 0) {
      // part_count != 0 defines ALL shapes, each with its own material (the
      // body-level friction/restitution are ignored for physics). Box2D
      // mixes per shape pair natively, so nothing else is needed at contact
      // time.
      for (std::uint32_t p = 0; p < desc.part_count; ++p) {
        const Part& part = desc.parts[p];
        b2ShapeDef part_sd = sd;  // same density/filter/flags; material below
        part_sd.material.friction = part.friction;
        part_sd.material.restitution = part.restitution;
        part_sd.userData =
            reinterpret_cast<void*>(pack_shape_owner(slot, p));
        const b2ShapeId shape = make_shape(part.shape, part.offset, part_sd);
        if (!b2Shape_IsValid(shape)) {
          b2DestroyBody(body);  // cascades the shapes made so far
          return fail(PhysicsError::backend_error, "Box2D refused a part shape");
        }
      }
    } else {
      const b2ShapeId shape = make_shape(desc.shape, Vec2{}, sd);
      if (!b2Shape_IsValid(shape)) {
        b2DestroyBody(body);
        return fail(PhysicsError::backend_error, "Box2D refused a shape");
      }
    }
    // The lib owns mass (CONTRACT.md 4): apply its closed-form numbers,
    // overriding whatever Box2D derived while adding the shapes. The
    // override must carry inertia ABOUT THE COM plus the COM in body-local
    // space - b2Body_SetMassData stores both verbatim (it does not
    // re-center, unlike Box2D's own automatic path). Statics/kinematics
    // arrive with zeros and skip this entirely: they carry no mass
    // semantics, and b2Body_SetMassData on a non-dynamic body would assert.
    if (desc.kind == ws::physics::BodyKind::dynamic) {
      b2MassData md;
      md.mass = mass.mass;
      md.center = to_b2(mass.center);
      md.rotationalInertia = mass.inertia;
      b2Body_SetMassData(body, md);
      // Re-pin the create velocity: each shape attach re-derived the
      // localCenter and converted the stored velocity by
      // omega x (center - oldCenter) (b2UpdateBodyMassData), so the
      // def's velocity landed ORIGIN-frame. The lib's linear_velocity is
      // the COM velocity - set_velocity()/body_state() already are, in
      // both engines - so the desc must read back identically. This is
      // a no-op for COM-at-origin bodies (the only kind v1 had).
      b2Body_SetLinearVelocity(body, to_b2(desc.linear_velocity));
    }
    bodies_[slot] = body;
    body_live_[slot] = 1;
    kinds_[slot] = desc.kind;
    return {};
  }

  void destroy_body(std::uint32_t slot) override {
    if (slot >= bodies_.size() || !body_live_[slot]) {
      return;
    }
    // Box2D cascades attached shapes; the lib already destroyed its
    // joints before the body, so none remain here.
    b2DestroyBody(bodies_[slot]);
    bodies_[slot] = b2_nullBodyId;
    body_live_[slot] = 0;
  }

  void set_transform(std::uint32_t slot, const Transform& t) override {
    if (slot >= bodies_.size() || !body_live_[slot]) {
      return;
    }
    b2Body_SetTransform(bodies_[slot], to_b2(t.position), b2MakeRot(t.rotation));
    // A teleport invalidates cached contacts; the body must re-evaluate.
    b2Body_SetAwake(bodies_[slot], true);
  }

  void set_velocity(std::uint32_t slot, Vec2 linear, float angular) override {
    if (slot >= bodies_.size() || !body_live_[slot]) {
      return;
    }
    // Wake rule (CONTRACT.md 6): set_velocity wakes iff the new velocity
    // is nonzero - exactly Box2D's native shape (body.c wakes only when
    // the stored value is nonzero), so no explicit wake. A zero set on a
    // sleeping body leaves it asleep.
    b2Body_SetLinearVelocity(bodies_[slot], to_b2(linear));
    b2Body_SetAngularVelocity(bodies_[slot], angular);
  }

  void set_filter(std::uint32_t slot, std::uint32_t layer,
                  std::uint32_t mask) override {
    if (slot >= bodies_.size() || !body_live_[slot]) {
      return;
    }
    b2Filter filter{};
    filter.categoryBits = layer;  // uint32 -> uint64 widens clean
    filter.maskBits = mask;
    // A body may own up to kMaxPartsPerBody shapes now, so ask the engine
    // for the list (a stack buffer beats a per-slot shape table, and it
    // works whatever the body's shape count is). SetFilter resets the
    // broadphase proxy and wakes the island (a filter change may kill
    // contacts), so the change lands next tick.
    b2ShapeId shapes[ws::physics::kMaxPartsPerBody];
    const int count = b2Body_GetShapes(
        bodies_[slot], shapes, static_cast<int>(ws::physics::kMaxPartsPerBody));
    for (int i = 0; i < count; ++i) {
      b2Shape_SetFilter(shapes[i], filter);
    }
  }

  void set_bullet(std::uint32_t slot, bool enabled) override {
    if (slot >= bodies_.size() || !body_live_[slot]) {
      return;
    }
    // body.c SetBullet only flips bodySim->isBullet - no wake, and none
    // is needed (only a moving body can tunnel, and moving bodies are
    // awake). The next step's collision pairing reads the new flag.
    b2Body_SetBullet(bodies_[slot], enabled);
  }

  void set_sleepable(std::uint32_t slot, bool allowed) override {
    if (slot >= bodies_.size() || !body_live_[slot]) {
      return;
    }
    // b2Body_EnableSleep(false) wakes the body itself (body.c:1549) - the
    // lib's rule (CONTRACT.md 6) for free. Allowing sleep never wakes.
    b2Body_EnableSleep(bodies_[slot], allowed);
  }

  void wake(std::uint32_t slot) override {
    if (slot >= bodies_.size() || !body_live_[slot]) {
      return;
    }
    b2Body_SetAwake(bodies_[slot], true);
  }

  [[nodiscard]] bool is_sleeping(std::uint32_t slot) const override {
    if (slot >= bodies_.size() || !body_live_[slot]) {
      return false;
    }
    // Statics never sleep, but Box2D reports them as not-awake
    // (IsAwake == setIndex == b2_awakeSet, and statics live elsewhere),
    // so the kind table decides before the engine is asked.
    if (kinds_[slot] == ws::physics::BodyKind::static_) {
      return false;
    }
    return !b2Body_IsAwake(bodies_[slot]);
  }

  void set_gravity(ws::math::Vec2 gravity) override {
    // SetGravity only stores the vector (world.c): a sleeping body
    // would stay glued to its contacts through the whole tilt, so wake
    // every live body - SetAwake(true) is a no-op for statics (their
    // set index never reaches the sleeping threshold) and just repeats
    // the awake bit for the rest.
    b2World_SetGravity(world_, to_b2(gravity));
    for (std::uint32_t slot = 0; slot < bodies_.size(); ++slot) {
      if (body_live_[slot]) {
        b2Body_SetAwake(bodies_[slot], true);
      }
    }
  }

  void apply_linear_impulse(std::uint32_t slot, Vec2 impulse) override {
    if (slot >= bodies_.size() || !body_live_[slot]) {
      return;
    }
    b2Body_ApplyLinearImpulseToCenter(bodies_[slot], to_b2(impulse), true);
  }

  void apply_angular_impulse(std::uint32_t slot, float impulse) override {
    if (slot >= bodies_.size() || !body_live_[slot]) {
      return;
    }
    b2Body_ApplyAngularImpulse(bodies_[slot], impulse, true);
  }

  void apply_force(std::uint32_t slot, Vec2 force) override {
    if (slot >= bodies_.size() || !body_live_[slot]) {
      return;
    }
    b2Body_ApplyForceToCenter(bodies_[slot], to_b2(force), true);
  }

  void apply_torque(std::uint32_t slot, float torque) override {
    if (slot >= bodies_.size() || !body_live_[slot]) {
      return;
    }
    b2Body_ApplyTorque(bodies_[slot], torque, true);
  }

  [[nodiscard]] BackendVelocity2D get_velocity(
      std::uint32_t slot) const override {
    if (slot >= bodies_.size() || !body_live_[slot]) {
      return {};
    }
    BackendVelocity2D v;
    v.linear = from_b2(b2Body_GetLinearVelocity(bodies_[slot]));
    v.angular = b2Body_GetAngularVelocity(bodies_[slot]);
    return v;
  }

  // --- joints -----------------------------------------------------------------

  [[nodiscard]] ws::core::Status create_joint(
      std::uint32_t slot, const BackendJoint2D& joint) override {
    if (slot >= joints_.size() || joint.slot_a >= bodies_.size() ||
        joint.slot_b >= bodies_.size() || !body_live_[joint.slot_a] ||
        !body_live_[joint.slot_b]) {
      return fail(PhysicsError::backend_error, "joint slots out of range");
    }
    const b2BodyId a = bodies_[joint.slot_a];
    const b2BodyId b = bodies_[joint.slot_b];
    const float angle_a = b2Rot_GetAngle(b2Body_GetRotation(a));
    const float angle_b = b2Rot_GetAngle(b2Body_GetRotation(b));
    b2JointId id = b2_nullJointId;
    switch (joint.type) {
      case BackendJoint2D::Type::fixed: {
        // Weld at the CURRENT relative pose: no snap on creation.
        b2WeldJointDef def = b2DefaultWeldJointDef();
        def.bodyIdA = a;
        def.bodyIdB = b;
        def.localAnchorA = b2Vec2{0.0f, 0.0f};
        def.localAnchorB = b2Body_GetLocalPoint(b, b2Body_GetPosition(a));
        def.referenceAngle = angle_b - angle_a;
        id = b2CreateWeldJoint(world_, &def);
        break;
      }
      case BackendJoint2D::Type::revolute: {
        b2RevoluteJointDef def = b2DefaultRevoluteJointDef();
        def.bodyIdA = a;
        def.bodyIdB = b;
        const b2Vec2 anchor = to_b2(joint.world_anchor);
        def.localAnchorA = b2Body_GetLocalPoint(a, anchor);
        def.localAnchorB = b2Body_GetLocalPoint(b, anchor);
        def.referenceAngle = angle_b - angle_a;
        def.enableLimit = joint.enable_limits;
        def.lowerAngle = joint.lower_angle;
        def.upperAngle = joint.upper_angle;
        def.enableMotor = joint.enable_motor;
        def.motorSpeed = joint.motor_speed;
        def.maxMotorTorque = joint.max_motor_torque;
        id = b2CreateRevoluteJoint(world_, &def);
        break;
      }
      case BackendJoint2D::Type::distance: {
        // Anchored at both body origins; the rest length is the caller's.
        b2DistanceJointDef def = b2DefaultDistanceJointDef();
        def.bodyIdA = a;
        def.bodyIdB = b;
        def.localAnchorA = b2Vec2{0.0f, 0.0f};
        def.localAnchorB = b2Vec2{0.0f, 0.0f};
        def.length = joint.length;
        id = b2CreateDistanceJoint(world_, &def);
        break;
      }
    }
    if (!b2Joint_IsValid(id)) {
      return fail(PhysicsError::backend_error, "Box2D refused a joint");
    }
    joints_[slot] = id;
    joint_live_[slot] = 1;
    return {};
  }

  void destroy_joint(std::uint32_t slot) override {
    if (slot >= joints_.size() || !joint_live_[slot]) {
      return;
    }
    b2DestroyJoint(joints_[slot]);
    joints_[slot] = b2_nullJointId;
    joint_live_[slot] = 0;
  }

  // --- stepping -----------------------------------------------------------------

  std::size_t step(float dt, IEventCollector2D& collector,
                   std::span<BackendMove2D> moves_out) override {
    b2World_Step(world_, dt, kSubStepCount);

    // Move events: one per simulated (awake) body this step, each carrying
    // the body's userData - our slot + 1 - so no reverse lookup is needed.
    // Sleep transitions ride the same event (fellAsleep); the pose it holds
    // is the body's final one either way.
    const b2BodyEvents body_events = b2World_GetBodyEvents(world_);
    std::size_t count = 0;
    for (int i = 0; i < body_events.moveCount; ++i) {
      const b2BodyMoveEvent& e = body_events.moveEvents[i];
      const auto raw = reinterpret_cast<std::uintptr_t>(e.userData);
      if (raw == 0 || count >= moves_out.size()) {
        continue;  // defensive: userData never set / span bounds
      }
      moves_out[count++] =
          BackendMove2D{static_cast<std::uint32_t>(raw - 1u),
                        Transform{from_b2(e.transform.p),
                                  b2Rot_GetAngle(e.transform.q)}};
    }

    const b2ContactEvents contacts = b2World_GetContactEvents(world_);
    for (int i = 0; i < contacts.beginCount; ++i) {
      const b2ContactBeginTouchEvent& e = contacts.beginEvents[i];
      std::uint32_t a = 0;
      std::uint32_t b = 0;
      std::uint8_t pa = 0;
      std::uint8_t pb = 0;
      if (!owner_of_shape(e.shapeIdA, a, pa) ||
          !owner_of_shape(e.shapeIdB, b, pb)) {
        continue;
      }
      BackendEvent2D ev{};
      ev.slot_a = a;
      ev.slot_b = b;
      ev.part_a = pa;
      ev.part_b = pb;
      ev.phase = physics2d::ContactPhase::began;
      if (e.manifold.pointCount > 0) {
        ev.point = from_b2(e.manifold.points[0].point);
      }
      ev.normal = from_b2(e.manifold.normal);  // engine A->B; the lib re-faces
      collector.add(ev);
    }
    for (int i = 0; i < contacts.endCount; ++i) {
      const b2ContactEndTouchEvent& e = contacts.endEvents[i];
      std::uint32_t a = 0;
      std::uint32_t b = 0;
      std::uint8_t pa = 0;
      std::uint8_t pb = 0;
      if (!owner_of_shape(e.shapeIdA, a, pa) ||
          !owner_of_shape(e.shapeIdB, b, pb)) {
        continue;
      }
      BackendEvent2D ev{};
      ev.slot_a = a;
      ev.slot_b = b;
      ev.part_a = pa;
      ev.part_b = pb;
      ev.phase = physics2d::ContactPhase::ended;
      collector.add(ev);  // Box2D end events carry no manifold
    }
    for (int i = 0; i < contacts.hitCount; ++i) {
      const b2ContactHitEvent& e = contacts.hitEvents[i];
      std::uint32_t a = 0;
      std::uint32_t b = 0;
      std::uint8_t pa = 0;
      std::uint8_t pb = 0;
      if (!owner_of_shape(e.shapeIdA, a, pa) ||
          !owner_of_shape(e.shapeIdB, b, pb)) {
        continue;
      }
      BackendEvent2D ev{};
      ev.slot_a = a;
      ev.slot_b = b;
      ev.part_a = pa;
      ev.part_b = pb;
      ev.phase = physics2d::ContactPhase::hit;
      ev.point = from_b2(e.point);
      ev.normal = from_b2(e.normal);
      ev.speed = e.approachSpeed;
      collector.add(ev);
    }

    // Sensors report their overlap lifecycle separately (no collision
    // response, no manifold, no hit events).
    const b2SensorEvents sensors = b2World_GetSensorEvents(world_);
    for (int i = 0; i < sensors.beginCount; ++i) {
      const b2SensorBeginTouchEvent& e = sensors.beginEvents[i];
      std::uint32_t sensor = 0;
      std::uint32_t visitor = 0;
      std::uint8_t sensor_part = 0;
      std::uint8_t visitor_part = 0;
      if (!owner_of_shape(e.sensorShapeId, sensor, sensor_part) ||
          !owner_of_shape(e.visitorShapeId, visitor, visitor_part)) {
        continue;
      }
      BackendEvent2D ev{};
      ev.slot_a = sensor;
      ev.slot_b = visitor;
      ev.part_a = sensor_part;
      ev.part_b = visitor_part;
      ev.phase = physics2d::ContactPhase::began;
      collector.add(ev);
    }
    for (int i = 0; i < sensors.endCount; ++i) {
      const b2SensorEndTouchEvent& e = sensors.endEvents[i];
      std::uint32_t sensor = 0;
      std::uint32_t visitor = 0;
      std::uint8_t sensor_part = 0;
      std::uint8_t visitor_part = 0;
      if (!owner_of_shape(e.sensorShapeId, sensor, sensor_part) ||
          !owner_of_shape(e.visitorShapeId, visitor, visitor_part)) {
        continue;
      }
      BackendEvent2D ev{};
      ev.slot_a = sensor;
      ev.slot_b = visitor;
      ev.part_a = sensor_part;
      ev.part_b = visitor_part;
      ev.phase = physics2d::ContactPhase::ended;
      collector.add(ev);
    }
    return count;
  }

  // --- queries -------------------------------------------------------------------

  [[nodiscard]] std::optional<BackendRayHit2D> raycast(
      Vec2 origin, Vec2 direction, float max_distance) const override {
    const b2Vec2 translation{direction.x * max_distance,
                             direction.y * max_distance};
    const b2RayResult hit = b2World_CastRayClosest(
        world_, to_b2(origin), translation, b2DefaultQueryFilter());
    if (!hit.hit) {
      return std::nullopt;
    }
    std::uint32_t slot = 0;
    std::uint8_t part = 0;  // a ray hits one shape; the lib speaks bodies
    if (!owner_of_shape(hit.shapeId, slot, part)) {
      return std::nullopt;  // defensive: the shape has no lib body
    }
    return BackendRayHit2D{slot, hit.fraction * max_distance, from_b2(hit.point),
                           from_b2(hit.normal)};
  }

  void overlap_circle(Vec2 center, float radius,
                      std::vector<std::uint32_t>& out) const override {
    b2ShapeProxy proxy{};
    proxy.points[0] = to_b2(center);
    proxy.count = 1;
    proxy.radius = radius;
    b2World_OverlapShape(world_, &proxy, b2DefaultQueryFilter(),
                         &collect_overlap, &out);
  }

  void overlap_aabb(const Aabb& box,
                    std::vector<std::uint32_t>& out) const override {
    const b2AABB aabb{to_b2(box.min), to_b2(box.max)};
    b2World_OverlapAABB(world_, aabb, b2DefaultQueryFilter(), &collect_overlap,
                        &out);
  }

 private:
  // --- task pool ------------------------------------------------------------
  // See the file header for the concurrency contract. enqueue/finish run
  // on the stepping thread only; workers only pop and run chunks. All
  // bookkeeping happens under mutex_; the only lock-free state is the
  // atomic per-group remaining counter.

  struct TaskGroup {
    std::atomic<int> remaining{0};  // chunks not yet completed
    bool in_use = false;             // slot owns an unfinished enqueue
  };

  struct ChunkJob {
    b2TaskCallback* fn = nullptr;
    int start = 0;
    int end = 0;
    void* task_ctx = nullptr;
    TaskGroup* group = nullptr;
    bool live = false;  // queued and not yet popped
  };

  static void* enqueue_task(b2TaskCallback* fn, int itemCount, int minRange,
                            void* taskCtx, void* userCtx) {
    return static_cast<Box2DWorld*>(userCtx)
        ->enqueue(fn, itemCount, minRange, taskCtx);
  }

  static void finish_task(void* userTask, void* userCtx) {
    static_cast<Box2DWorld*>(userCtx)->finish(static_cast<TaskGroup*>(userTask));
  }

  [[nodiscard]] bool start_pool(std::uint32_t count) {
    try {
      workers_.reserve(count);
      for (std::uint32_t i = 0; i < count; ++i) {
        workers_.emplace_back(&Box2DWorld::work_loop, this, i);
      }
    } catch (...) {
      stop_pool();  // join what started; leave nothing behind
      return false;
    }
    return true;
  }

  void stop_pool() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
    for (std::thread& worker : workers_) {
      worker.join();
    }
    workers_.clear();
  }

  // Queue [start, end) as one chunk of `group`. If the pool is momentarily
  // full, run it inline on the stepping thread (correct - distinct range,
  // distinct worker index - just serial). Only parallel-for chunks can
  // reach that path: a solver phase never exceeds the array bounds.
  void dispatch(b2TaskCallback* fn, int start, int end, void* taskCtx,
                TaskGroup* group) {
    {
      std::lock_guard lock(mutex_);
      if (queue_size_ < kMaxChunkJobs) {
        for (std::size_t i = 0; i < kMaxChunkJobs; ++i) {
          if (!chunks_[i].live) {
            chunks_[i] = ChunkJob{fn, start, end, taskCtx, group, true};
            queue_[queue_tail_] = i;
            queue_tail_ = (queue_tail_ + 1) % kMaxChunkJobs;
            ++queue_size_;
            cv_.notify_all();
            return;
          }
        }
      }
    }
    fn(start, end, worker_count_ - 1, taskCtx);
    complete_chunk(group);
  }

  // Count one finished chunk; wake finish() when its group completes.
  void complete_chunk(TaskGroup* group) {
    std::lock_guard lock(mutex_);
    if (group->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      cv_.notify_all();
    }
  }

  [[nodiscard]] TaskGroup* acquire_group() {
    std::lock_guard lock(mutex_);
    for (TaskGroup& group : groups_) {
      if (!group.in_use) {
        group.in_use = true;
        return &group;
      }
    }
    return nullptr;
  }

  void* enqueue(b2TaskCallback* fn, int itemCount, int minRange,
                void* taskCtx) {
    if (itemCount <= 0) {
      return nullptr;
    }
    if (itemCount == 1 && minRange == 1) {
      // Unit task (solver stage / tree refit / island split): possibly
      // barrier-synchronized with its siblings - must go to the pool so
      // they can run concurrently.
      TaskGroup* group = acquire_group();
      if (group == nullptr) {
        // Unreachable with the arrays sized past B2_MAX_WORKERS; the
        // serial fallback is still correct for non-synchronizing units.
        fn(0, 1, worker_count_ - 1, taskCtx);
        return nullptr;
      }
      group->remaining.store(1, std::memory_order_release);
      dispatch(fn, 0, 1, taskCtx, group);
      return group;
    }
    if (itemCount <= minRange) {
      // Too small to be worth splitting: serial, like Box2D's own
      // default task system.
      fn(0, itemCount, worker_count_ - 1, taskCtx);
      return nullptr;
    }
    const int chunks =
        std::min(worker_count_, itemCount / std::max(minRange, 1));
    if (chunks < 2) {
      fn(0, itemCount, worker_count_ - 1, taskCtx);
      return nullptr;
    }
    TaskGroup* group = acquire_group();
    if (group == nullptr) {
      fn(0, itemCount, worker_count_ - 1, taskCtx);
      return nullptr;
    }
    group->remaining.store(chunks, std::memory_order_release);
    const int chunk_size = (itemCount + chunks - 1) / chunks;
    for (int i = 0; i < chunks; ++i) {
      const int start = i * chunk_size;
      const int end = std::min(start + chunk_size, itemCount);
      if (start < end) {
        dispatch(fn, start, end, taskCtx, group);
      } else {
        complete_chunk(group);  // degenerate tail chunk: nothing to run
      }
    }
    return group;
  }

  void finish(TaskGroup* group) {
    std::unique_lock lock(mutex_);
    while (group->remaining.load(std::memory_order_acquire) != 0) {
      if (queue_size_ > 0) {
        // Help: run queued work on the stepping thread instead of idling.
        // This is REQUIRED, not an optimization - Box2D's solver unit
        // tasks spin at internal barriers until every sibling arrives,
        // and this thread is the sibling that runs the last one.
        const std::size_t ci = queue_[queue_head_];
        queue_head_ = (queue_head_ + 1) % kMaxChunkJobs;
        --queue_size_;
        const ChunkJob job = chunks_[ci];
        chunks_[ci].live = false;
        lock.unlock();
        job.fn(job.start, job.end, worker_count_ - 1, job.task_ctx);
        lock.lock();
        if (job.group->remaining.fetch_sub(1, std::memory_order_acq_rel) ==
            1) {
          cv_.notify_all();
        }
        continue;
      }
      cv_.wait(lock);
    }
    group->in_use = false;
  }

  void work_loop(std::uint32_t index) {
    for (;;) {
      ChunkJob job{};
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return stopping_ || queue_size_ > 0; });
        if (stopping_ && queue_size_ == 0) {
          return;  // drain what is queued, then leave
        }
        const std::size_t ci = queue_[queue_head_];
        queue_head_ = (queue_head_ + 1) % kMaxChunkJobs;
        --queue_size_;
        job = chunks_[ci];
        chunks_[ci].live = false;
      }
      job.fn(job.start, job.end, index, job.task_ctx);
      complete_chunk(job.group);
    }
  }

  // (slot + 1) | (part + 1) << 32 - zero only when never set, mirroring the
  // body userData sentinel.
  [[nodiscard]] static std::uintptr_t pack_shape_owner(std::uint32_t slot,
                                                       std::uint32_t part) {
    return (static_cast<std::uintptr_t>(slot) + 1u) |
           ((static_cast<std::uintptr_t>(part) + 1u) << 32);
  }

  // Shape id -> its owning (slot, part), packed at create. End events can
  // outlive their shapes: b2DestroyBody queues end events for the dying
  // body's contacts, and they drain one step later holding STALE shape ids
  // (generation bumped at destroy, slot maybe reused). b2Shape_GetUserData
  // hard-asserts on a stale id, so validity goes first (b2Shape_IsValid is
  // the non-asserting twin) and the stale event is dropped - the lib's
  // destroy path already owns that cleanup.
  [[nodiscard]] bool owner_of_shape(b2ShapeId shape, std::uint32_t& out_slot,
                                    std::uint8_t& out_part) const {
    if (!b2Shape_IsValid(shape)) {
      return false;
    }
    const auto raw =
        reinterpret_cast<std::uintptr_t>(b2Shape_GetUserData(shape));
    if (raw == 0) {
      return false;
    }
    out_slot = static_cast<std::uint32_t>(raw & 0xFFFFFFFFull) - 1u;
    out_part = static_cast<std::uint8_t>(raw >> 32) - 1u;
    return true;
  }

  b2WorldId world_{};
  std::vector<b2BodyId> bodies_;    // slot -> engine body
  std::vector<char> body_live_;     // 1 = the slot holds a live body
  std::vector<ws::physics::BodyKind> kinds_;  // statics never sleep (CONTRACT)
  std::vector<b2JointId> joints_;   // slot -> engine joint
  std::vector<char> joint_live_;
  float hit_threshold_ = 0.0f;

  // Task pool (idle when thread_count == 0).
  int worker_count_ = 1;  // pool threads + the stepping thread
  std::vector<std::thread> workers_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  TaskGroup groups_[kMaxTaskGroups];
  ChunkJob chunks_[kMaxChunkJobs];
  std::size_t queue_[kMaxChunkJobs];
  std::size_t queue_head_ = 0;
  std::size_t queue_tail_ = 0;
  std::size_t queue_size_ = 0;
  bool stopping_ = false;  // guarded by mutex_
};

}  // namespace

std::unique_ptr<IWorld2DBackend> make_box2d_world_2d(
    const BackendDesc2D& desc) {
  try {
    return std::make_unique<Box2DWorld>(desc);
  } catch (const std::bad_alloc&) {
    ws::core::warn("physics: Box2D backend out of memory");
    return nullptr;
  } catch (const std::exception& e) {
    ws::core::warn("physics: Box2D backend failed to start: {}", e.what());
    return nullptr;
  }
}

}  // namespace ws::physics::detail
