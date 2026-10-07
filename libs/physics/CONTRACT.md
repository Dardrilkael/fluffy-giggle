# ws::physics - CONTRACT.md

The engine-independent guarantee list. The LIB owns every semantic in this
file; a backend (Box2D, Jolt, the test-only reference backend) only
computes. If an engine would do something different, the lib's adapter
translates so the clause below still holds - never the other way around.
The conformance test suite encodes this file clause by clause; a behavior
not testable against EVERY backend does not belong here.

Language: MUST / NEVER are normative. "Backend compute" marks a detail the
lib deliberately does not constrain (an engine's internals may differ).

## 1. Units, frames, and value conventions
- SI: meters, kilograms, seconds, radians, hertz. Gravity is m/s^2,
  density is kg/m^2 (2D) / kg/m^3 (3D), `fixed_dt` is seconds.
- The lib speaks WORLD space only; screen orientation (Y-up or Y-down)
  is the caller's business and never leaks into the lib.
- 2D rotation is radians CCW about +Z. 3D rotation is a ws::math Quat
  (w-first, w=1 identity); the lib normalizes the quaternion at create
  (sanitize_rotation) and rejects non-finite floats on every setter.

## 2. Bodies, handles, and lifetimes
- BodyId is generational: O(1) slot lookup, retired slots bump their
  generation, stale handles return `invalid_handle` (never alias),
  BodyId{} is never issued. Slot reuse never leaks the retired body's
  user_data.
- `max_bodies` bounds creation; a full world returns `world_full`
  (including capacity 0 refusing the first body). The lib's slot table,
  not the engine, is the sole authority on who may create.
- Kinds: dynamic (density MUST be > 0 and finite), static, kinematic.
- `user_data` is an opaque uintptr the lib stores and never interprets.
- Sensors are body-wide (is_sensor + parts fails create) and collide for
  events only: no solver response, no hit events.

## 3. Shape conventions
- Shapes are VALUE types; equal shapes share one cached backend shape.
- Circle/Sphere: radius > 0. Box: half_extents, every component > 0.
- 2D Capsule: the body-local segment a->b inflated by radius (a general
  segment capsule). 3D Capsule: Y-aligned, segment
  (0,-half_height,0)-(0,+half_height,0) inflated by radius. The 2D and 3D
  capsule conventions differ - each is its dimension's public API.
- Multi-part bodies: parts[] define ALL shapes, each with its own
  friction/restitution at a body-local offset (no per-part rotation);
  density stays body-level; part_count is capped by kMaxPartsPerBody.

## 4. Mass properties (lib-computed)
- The lib computes mass, center of mass, and (rotational) inertia from
  OUR shapes + density using its own closed-form formulas (verified
  against analytic tests); backends receive explicit mass data and apply
  it through their override mechanisms (b2Body_SetMassData / Jolt's
  mOverrideMassProperties). Identical descs MUST produce identical mass
  data in every backend.
- Multi-part bodies: per-part masses sum; the composite center of mass is
  the mass-weighted mean; inertia sums through the parallel-axis theorem.
- Statics have no mass semantics (infinite); kinematics carry none
  (engines treat them as position-driven).

## 5. Material mixing (one lib rule)
- Friction combines as sqrt(f1*f2); restitution combines as max(e1,e2).
- The rule is applied per touching shape pair (multi-part: per part pair),
  through each backend's per-contact mechanism - natively in Box2D, via
  the contact-override path in Jolt. No engine-specific mixing survives.

## 6. Sleep policy and wake rules
- `allow_sleep` defaults to true (BodyDesc at create,
  set_sleepable at runtime). A body with allow_sleep == false NEVER
  sleeps.
- set_sleepable(id, false) wakes the body immediately (even if it was
  asleep). set_sleepable(id, true) never wakes; it only permits future
  sleep. wake(id) activates the body unconditionally.
- is_sleeping(id) is true only for a sleeping dynamic/kinematic body;
  statics NEVER sleep (is_sleeping == false).
- Which setters wake: set_transform wakes ALWAYS (a teleported body must
  resimulate); set_velocity wakes iff the new linear OR angular velocity
  is nonzero; impulses wake; create_joint wakes the island. Backends that
  do not wake natively MUST be made to by their adapter - the wake rule
  is the lib's, not the engine's.
- Idle heuristics (sleep thresholds/timers) are backend compute: WHEN a
  body falls asleep may differ per engine. The observable policy above,
  plus what sleeping means for events (§7) and movement, is contractual:
  a sleeping body reports no moves and consumes no per-step cost.

## 7. Contact events and touching state (lib-owned)
- Phases: began / ended / hit. A touching body pair reports at most one
  event per step per SHAPE pair (multi-part bodies).
- hit fires only above Desc::hit_speed_threshold; sensors fire began /
  ended only (never hit).
- The "currently touching" pair table lives in the LIB (pre-sized at
  create, zero-alloc in steady state). Backends report RAW begin/end
  touches; began/ended are decided by the lib's table, so:
  sleep/wake NEVER produces began or ended (the Box2D-like rule, enforced
  by the lib in every backend - a sleeping pair that is still
  geometrically touching stays touching, and its wake adds no `began`).
  began fires exactly once per not-touching -> touching transition; ended
  exactly once per touching -> not-touching transition.
- Events are PER step()/advance() CALL (a multi-tick advance unions its
  ticks); moved_bodies() and prev_transforms() are PER TICK (a zero-tick
  advance keeps both standing).
- Feed order is deterministic: events are sorted by (a.index, b.index,
  phase, point, normal, speed) before the caller sees them. The
  max_contact_events budget caps hit events only; began/ended are
  lossless (a pathological tick may grow the buffer past the budget;
  steady state never allocates).
- Forces and torques clear at the end of every tick in every backend.

## 8. Joints
- Kinds: Fixed, Revolute/Hinge (with limits + motor), Distance. Joints
  require >= 1 dynamic body; self-joints are rejected.
- Hinge limits are within +-0.99*pi and lower <= upper; an enabled motor
  needs max torque > 0; distance length >= 0.
- Destroying a body cascades to its joints (the joint handles go stale).
- Joint creation wakes the island it touches.

## 9. Queries
- raycast(origin, direction, max_distance): direction is a unit vector in
  world space, max_distance in meters; returns the CLOSEST hit only.
- overlap_sphere/overlap_aabb return every overlapping body.
- Queries see ALL bodies - sensors included - and IGNORE collision
  layers/masks. Aabb requires min <= max component-wise.
- Query results order: by slot index ascending. (Deterministic drain.)

## 10. Determinism scope
- Same binary + same thread_count + same call sequence MUST produce
  identical results (pinned by the replay tests).
- NOTHING is promised across engines, platforms, or thread counts.
- Cross-engine comparisons in the conformance suite use tolerances,
  never exact floats.

## 11. Steady-state allocation
- step()/advance() in steady state performs ZERO C++ heap allocations
  (the wrapper new/delete counters; an engine's own C heap is backend
  compute). Every per-step buffer is pre-reserved at create; mid-window
  spawn/destroy of already-cached shapes stays allocation-free.

## 12. Error model
- Public APIs return ws::core::Result<T> / Status. Stale handle ->
  invalid_handle; moved-from world -> invalid_state; validation ->
  invalid_argument; full world -> world_full; engine failures ->
  backend_error. Lib error enums convert to ws::core::Error at the
  boundary; core never knows lib codes. No exceptions cross boundaries.
