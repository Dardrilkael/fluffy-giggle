# ws::physics - CLAUDE.md

2D + 3D physics behind one API: `ws::physics2d` / `ws::physics3d`. Box2D v3
(2D) and Jolt Physics (3D) are PRIVATE backends behind an internal seam
(`src/core/backend2d.hpp` / `backend3d.hpp`) - no engine header under
`include/`, no engine type in a public signature (workspace rule). A backend
swap touches no public header and no user code.

## Layout
- `include/ws/physics/2d|3d/` - the public API (world, body, joint, contact,
  raycast). Only ws::core and ws::math types cross the boundary.
- `src/core/world_core.hpp` - the dimension-independent machinery shared by
  both worlds via a traits type (Dim2D / Dim3D, extracted to
  src/core/dim_2d.hpp / dim_3d.hpp): generational handle tables, the pose
  mirror + its prev-call
  copy, the moved-bodies list, contact-event normalize/sort/cap, the
  fixed-timestep accumulator, bulk set_transforms atomicity, and the
  destroy-body -> joint cascade.
- `src/core/backend_factory.cpp` - the ONE place a backend is chosen:
  make_world_backend_2d/3d, called by both worlds' create(). A compile-time
  dev probe (CMake option WS_PHYSICS_DEFAULT_BACKEND_REFERENCE, default
  OFF) flips BOTH picks to their reference twins (2D Box2D -> reference,
  3D Jolt -> reference3d).
- `src/backend/reference2d/` - the reference 2D backend: CONTRACT.md's
  detection, events, ballistics and queries computed from scratch, no
  engine. An HONEST subset, not a toy solver: no contact response (pairs
  pass through), no CCD, no joints, circles/boxes only, never sleeps -
  the raw-seam suite pins its exact numbers.
- `src/backend/reference3d/` - the reference 3D twin (C7b), the same
  honest-subset rules grown to the 3D seam: spheres/boxes with all three
  narrowphase pairs (capsule descs fail backend_error), multi-part
  bodies, sensors, filters, entering-only queries, COM-frame
  semi-implicit Euler + the Euler quaternion step on the lib's mass
  properties. No joints, no solver (pairs pass through), never sleeps.
  Its per-tick caches and union-AABB broadphase never change results or
  pair order - the raw-seam suite (test_reference_3d.cpp) pins exact
  numbers, including the one-tick closed quaternion forms.
- `src/backend/box2d/` - the real 2D engine (Box2D v3, FetchContent).
- `src/backend/jolt3d/` - the real 3D engine (Jolt Physics v5.6,
  FetchContent). Contact callbacks fire on worker threads during the
  step, so this backend buffers raw events per tick and feeds them
  value-sorted after it completes. The backend floors Jolt's VIEW of
  body capacity at 1 (its allocator asserts on a zero-sized reserve in
  debug builds); the lib's slot table keeps the true capacity - even 0 -
  and stays the sole authority on who may create bodies. C7b pinned its
  event/ray conventions to the lib's (the 3D conformance suite forced
  five adapter fixes): sensor pairs fire began/ended only, with zero
  geometry and never a hit (Jolt would report a real sensor manifold);
  solid began carries zero speed ("hit events only", the public header's
  promise); hit threshold 0 DISABLES hits entirely - the same rule as
  Box2D's enableHitEvents gate, not "every began"; raycasts opt out of
  Jolt's treat-convex-as-solid so a ray starting inside a body misses it
  (entering rays only, CONTRACT.md 9).

## Contracts (tested, not just documented)
- Zero heap allocation in steady-state step()/advance(): every per-step
  buffer is pre-reserved at create(); the test suite proves it with global
  new/delete counters (the wrapper only - the engine's own C heap is its
  business).
- Contact events are PER step()/advance() CALL - a multi-tick advance()
  unions its ticks. prev_transforms() and moved_bodies() are PER TICK: the
  prev mirror snaps on a call's first tick and the moved list is rebuilt
  then, so a zero-tick advance() (fast frame, big fixed_dt) keeps both
  standing - interpolation endpoints stay stable and the in-motion set
  stays visible while alpha grows. Events are sorted by the full
  tiebreak chain (a.index, b.index, phase, point, normal, speed).
- Backends feed events in DETERMINISTIC order: WHICH hit events the
  max_contact_events budget drops must be the same every run. began/ended
  are lossless - they bypass the budget entirely (a dropped `ended` would
  leave a consumer's touching state stuck forever), so a pathological tick
  grows the event buffer past its pre-reserved bound; that growth is
  amortized and never happens in steady state, keeping the zero-alloc
  contract intact.
- Forces/torques clear at the end of every tick, whichever engine runs.
- Sleep is event-silent in BOTH engines since C3 (the lib-owned touching
  table, world_core.hpp): the table keys pairs on the full SHAPE pair (a
  second part's began is never read as a duplicate), suppresses Jolt's
  sleep-purge `ended` and wake-re-add `began`, and reconciles the one
  edge sleep can owe - a real separation while a side sleeps is
  delivered by the table's two-tick deadline (zeroed geometry), never
  dropped. Destroying a body silences its pairs and purges the slot's
  keys, so reuse cannot collide a stale key. Both engine suites pin the
  same observable shape.
- The remaining documented divergence: a runtime `set_collision_filter`
  wakes the island in Box2D (`b2Shape_SetFilter`) but NOT in Jolt
  (`SetCollisionGroup` does not activate), so a sleeping 3D body keeps
  its contacts until woken by set_velocity/impulse - the public setter
  documents the conservative contract. Behavior tests pin both engines.
- Box2D v3 needs `enableSensorEvents` on BOTH shapes of a sensor pair
  (sensor.c skips pairs whose other side lacks it), so the adapter opts
  every shape in: the lib's contract is "sensor overlaps fire events",
  with no per-shape subscription knob to expose.
- Determinism: same binary, same thread_count, same call sequence produces
  identical results (replay test). Cross-platform and cross-thread-count
  determinism are NOT promised.
- The reference twins (2D since C6, 3D since C7b) are the machinery's
  second opinion: they compute the lib's rules with zero engine help
  (O(n^2) detection nested so touch keys are lex-sorted by construction,
  entering-rays-only raycasts, the began/ended touching diff, COM-frame
  semi-implicit Euler on the lib's mass properties), and every rule they
  cannot honor fails honestly instead of faking it: unsupported shapes
  and joints return backend_error, solid pairs pass through (no
  solver), is_sleeping is always false. Flipping the dev probe
  (WS_PHYSICS_DEFAULT_BACKEND_REFERENCE=ON) runs the WHOLE existing
  machinery suite against BOTH twins - the cases that need resting
  contact or joints fail there, which is the point of a second
  implementation.

## v1 scope (decided - review point 7)
In scope: sensors (BodyDesc::is_sensor), the three v1 joint kinds, hit
events above a speed threshold, the bulk set_transforms path.

## v2 progress
- DONE: body user data - `BodyDesc::user_data` (uintptr_t, opaque) +
  `body_user_data()`/`set_body_user_data()` on both worlds. Stored in
  the lib's own slot table and never forwarded to any engine (no backend
  needed to know), so retire/reuse hygiene is pure lib logic.
- DONE: collision layers/masks - `BodyDesc::collision_layer/mask`
  (both-directions bitmask rule, defaults keep v1 behavior) +
  `set_collision_filter()` on both worlds. Box2D (reworked in C7a):
  shapes carry a NEUTRAL engine filter - a lib-legal mask of 0 must not
  hide a body from queries (CONTRACT.md 9 vs b2ShouldQueryCollide's
  both-direction check) - and the world's custom filter callback answers
  the rule from the adapter's per-slot layer/mask tables, the same
  architecture as the Jolt GroupFilter below. Runtime changes still ride
  `b2Shape_SetFilter`: the category alternates 1/2 as an opaque
  revalidation token so the call never early-outs (reset proxy, wake
  island, next tick's pair pass re-runs the rule). Sensors: the 3.1.1
  overlap pass never consults the callback, so sensor events are
  rule-gated at the adapter's drain; a sensor pair whose filter flips
  mid-overlap keeps its pre-flip event lifetime until separation. Jolt:
  (layer, mask) ride CollisionGroup's ids + a
  world-owned GroupFilter answering the rule; the ObjectLayer split is
  untouched (it sorts, the group decides). Sensors obey the same rule
  in both engines. Queries ignore filters in v2.
- DONE: runtime gravity - `set_gravity()` on both worlds. Both engines
  merely store the vector (`b2World_SetGravity` /
  `PhysicsSystem::SetGravity`), so the adapters also wake every live
  body: statics no-op the wake in both engines (Box2D's sleeping-set
  threshold, Jolt's `ActivateBodies` IsStatic check), and a caller that
  re-sets the same value every frame defeats sleep - the public comment
  says so. Behavior tests pin the redirected fall and the wake of a
  slept-in pile (moved_bodies proves the sleep was real).
- DONE: multi-shape bodies + per-shape materials - `BodyDesc::parts[]` +
  `part_count` (both dims), up to `kMaxPartsPerBody` (common.hpp).
  part_count != 0 means parts define ALL shapes: body-level
  shape/friction/restitution are IGNORED for physics (still validated),
  density stays body-level, parts are solid-only (is_sensor + parts fails).
  A touching body pair can now report one contact event per SHAPE pair per
  step (public ContactEvent doc says so). No per-part rotation in v2.
  - The parts ride a fixed std::array + count, NOT a std::vector: the
    create-time desc copy (rotation sanitization) must stay allocation
    free, and MSVC's debug STL allocates a ~16-byte container proxy on a
    vector's first iterator touch while an empty-vector copy-assign
    RELEASES capacity - a vector broke the zero-alloc window at every
    create. The array copies trivially; no scratch buffer needed.
  - Box2D: native per-part materials (per-part b2ShapeDef with its own
    friction/restitution, offsets via b2Circle.center /
    b2MakeOffsetBox). Mixing: the world callbacks
    (b2WorldDef.frictionCallback/restitutionCallback) route every pair
    through the lib's material.hpp rule (C5), so Box2D's own combine
    never runs.
  - Jolt: sub-shapes do not carry materials through the engine's mixing,
    so the adapter keeps flat per-slot part tables (friction/restitution,
    max_bodies x kMaxPartsPerBody, sized at construction) and the contact
    listener recombines through the lib's one rule (src/core/material.hpp,
    C5): combined friction sqrt(f1*f2), restitution max(e1,e2) - the
    same numbers as Jolt's own defaults, but the lib's formula, applied
    to EVERY pair (plain pairs from body-level values, part pairs from
    the tables), so the engine's combine never survives. The listener -
    not Jolt's CombineFunction hook, a bare function pointer with no
    user context that could never reach the per-part tables - fires in
    OnContactAdded AND OnContactPersisted (Jolt rebuilds contact
    constraints from body defaults every step - an Added-only override
    silently reverts on the second step of a resting pair). The part
    index comes from GetSubShapeIndexFromID on the StaticCompoundShape
    (AddShape order IS parts order); GetSubShapeUserData is useless here
    (it reads the LEAF's user data, and cached leaves are shared across
    bodies). Multi-part bodies set mUseManifoldReduction(false) - a
    merged manifold carries one sub-shape's material. A one-part body is
    NOT a compound (Create returns the bare child): its material bakes
    body-level, and a non-zero offset rides a RotatedTranslated decorator
    (part_index returns -1 there, which is correct by construction).
- DONE: CCD/bullet flags - `BodyDesc::is_bullet` (both dims) +
  `set_bullet()` on both worlds. The engines' CCD stories DIVERGE, and
  both flag comments tell the truth:
  - Box2D v3 already sweeps every fast body against STATIC geometry on
    its own (isFast = maxVelocity*dt > 0.5*minExtent -> b2SolveContinuous,
    free CCD vs walls, no flag needed); isBullet only extends the sweep to
    the kinematic + DYNAMIC trees (the bullet body array). So a static
    wall can never discriminate is_bullet in 2D - the behavior test uses a
    resting DYNAMIC target.
  - Jolt's Discrete default has NO CCD of any kind; the flag is
    LinearCast motion quality, the only tunneling protection.
  - Neither engine's runtime setter wakes the body, and none is needed:
    only a moving body can tunnel, and a moving body is awake.
    b2Body_SetBullet just flips bodySim->isBullet;
    BodyManager::SetMotionQuality swaps the quality in place under the
    active-bodies lock (adjusts mNumActiveCCDBodies, no activation).
  - Test-design lesson: at v=100 m/s and dt=1/60 the body travels 5/3 m
    per tick, so a start pose of x0=0 lands EXACTLY on a wall at x=20 at
    tick 12 - the discrete end-pose test catches the body anyway and the
    test proves nothing. Tunneling must be engineered: pick x0 so the
    overlap window is skipped between consecutive ticks (x0=0.767 ->
    ticks land at 19.10 then 20.77, clearing [19.8, 20.2] by ~0.6 m,
    far past Box2D's 0.02 m speculative distance).
- DONE: sleep control - `BodyDesc::allow_sleep` (both dims) +
  `set_sleepable()`/`wake()`/`is_sleeping()` on both worlds. CONTRACT.md 6
  is the normative wake-rule list; the adapters implement it:
  - Box2D: `b2Body_EnableSleep(id, false)` wakes natively, so
    set_sleepable(false) is one call; is_sleeping needs the lib's kind
    table because a Box2D static reports NOT awake (its sleep-set index
    is the static set) - statics would read as sleeping without the
    guard. The adapter keeps a per-slot `kinds_` vector for that.
  - Jolt: `Body::SetAllowSleeping` wakes NOTHING (it flips the flag and
    resets the timer), so the adapter activates on forbid itself. Statics
    have null MotionProperties - guarded before any MotionProperties
    touch. TRAP (found the hard way): the adapter's BodyLockWrite must
    be out of scope before `ActivateBody` - BodyInterface::ActivateBody
    takes its own BodyLockWrite on the same body, and a SharedMutex
    exclusively locked twice by one thread is UB (MSVC debug raises
    SEH; release would deadlock).
  - Wake-rule normalization this phase: set_velocity used to wake
    unconditionally in both adapters; the contract says wake iff the
    new velocity is nonzero, and both engines natively agree with that
    shape (Box2D's SetLinearVelocity/SetAngularVelocity wake on nonzero;
    Jolt's setters never wake, so the adapter gates ActivateBody on the
    component-wise nonzero test). A zero set on a sleeping body leaves
    it asleep in both dims now.
  - Behavior tests: twins on a floor (one pinned at create, one free)
    pin that the free twin sleeps while the pinned one cannot, statics
    read awake, wake() works with no step, zero velocity-set keeps a
    sleeper asleep, nonzero wakes, set_sleepable(true) never wakes and
    (false) wakes from sleep. Machinery suites pin stale-handle and
    moved-from failure codes for the trio.

The engine-semantic rework arc ("the lib owns semantics, backends only
compute"), one commit per phase: CONTRACT.md (C1, done) -> sleep control
(C2, done) -> lib-owned touching table (C3, done) -> lib-computed mass
properties (C4, done) -> one material-mixing rule (C5, done) -> backend
factory + reference 2D backend (C6, done) -> conformance suite encoding
CONTRACT.md (C7, done: C7a - Dim traits headers + the both-backend 2D
suite; C7b - the 3D reference twin, the both-backend 3D suite over Jolt +
it, and the Jolt-adapter conformance fixes above).
Also queued behind those: the machinery sweeps' "every path" names are
still aspirational - the stale sweeps miss set_transform + the apply_*
family in 3D, and the moved-from tests miss the apply_* family and
destroy_joint/queries in 2D. A sweep-completeness micro-phase should
make the names true.

## Review decisions, settled
- Virtual backend seam kept (not a compile-time template backend): a mock
  backend for tests is worth more than a vcall that happens once per
  control call, never per solver iteration.
- destroy_body scans joint records O(max_joints) - fine at v1 scales; a
  per-body joint list is the upgrade path if mass despawn ever profiles hot.
- Sort-then-cap REJECTED for event overflow: it would break the
  pre-reserved buffer bound (events beyond max_contact_events are never
  stored). Deterministic backend feed order + cap achieves the same
  determinism within the bound - with one deliberate exception: edges
  (began/ended) bypass the cap and may grow the buffer past it, because
  correctness of consumer touching-state beats the bound (bug #2 of the
  review triage; hits alone stay capped and deterministic).
- Moved-bodies pipeline instead of a bulk copy-out: the backend reports the
  bodies it simulated and the lib folds them into its own mirror - sleeping
  worlds cost nothing to sync, and prev_transforms() falls out for free.
- Single-threaded API contract (documented on World2D/World3D); a command
  buffer can be layered on later without breaking the API.
- Joints require at least one dynamic body; revolute/hinge limits are
  checked against +-0.99*pi (both engines measure the UNWOUND relative
  angle - a limit beyond one turn is meaningless and Box2D asserts there).

## Tests
`ctest --preset debug -R "physics|check_deps" --output-on-failure`
(cwd in the repo root; the preset handles the build dir). Windows+MSVC and
Android NDK (arm64-v8a, android-28) both build ws_physics from day one:
run the suites on the desktop presets, compile-check via
`cmake --build --preset android-local --target ws_physics`.
The conformance suite (tests/test_conformance.cpp, C7a) drives
WorldCore<Dim2D> directly over BOTH backends in one binary - the same
machinery World2D runs - and encodes the cross-backend CONTRACT
clauses: ballistics (velocity tight, position banded by the staircase
formulas in its header), F = ma on the lib's mass numbers, impulse
immediacy, began/ended/hit edges with their zero-geometry promises,
filters at create + runtime, destroy/silence/reuse, queries (closest
hit, sensors seen, filters ignored), per-call events vs per-tick
moved/prev, generational retire. Engine-only semantics (sleep onset,
solver response, materials, joints) stay in the engine suites, and
first-touch TICK numbers are never asserted across engines: touching
scenes step until the edge appears (the reference detects after
integrating the tick, Box2D's pair pass a tick later).
The 3D sibling (tests/test_conformance_3d.cpp, C7b) drives
WorldCore<Dim3D> over Jolt and the reference twin with the same clause
set, TIGHTER where 3D genuinely agrees: the Jolt adapter steps one
collision step with zeroed damping - the same semi-implicit Euler the
reference integrates - so ballistics pin near-exact on both legs and a
fast impact fires began AND hit on the same tick. The one real
divergence is banded: Jolt composes the exact axis-angle exponential
map while the reference steps the Euler quaternion derivative (an
O(theta^3) gap), so spin pins run on the composed angle
2 atan2(q.axis, q.w) at ~0.1%.
apps/bench_physics (desktop console app) is the throughput consumer: a
falling-box rain per dimension, timed while awake and after sleep. Run it
manually - absolute numbers are machine-dependent, so nothing asserts on
them.
