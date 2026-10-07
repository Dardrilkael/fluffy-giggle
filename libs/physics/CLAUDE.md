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
  both worlds via a traits type (Dim2D / Dim3D defined in the respective
  world .cpp): generational handle tables, the pose mirror + its prev-call
  copy, the moved-bodies list, contact-event normalize/sort/cap, the
  fixed-timestep accumulator, bulk set_transforms atomicity, and the
  destroy-body -> joint cascade.
- `src/backend/box2d/` - the real 2D engine (Box2D v3, FetchContent).
- `src/backend/jolt3d/` - the real 3D engine (Jolt Physics v5.6,
  FetchContent). Contact callbacks fire on worker threads during the
  step, so this backend buffers raw events per tick and feeds them
  value-sorted after it completes. The backend floors Jolt's VIEW of
  body capacity at 1 (its allocator asserts on a zero-sized reserve in
  debug builds); the lib's slot table keeps the true capacity - even 0 -
  and stays the sole authority on who may create bodies.

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
- Backends feed events in DETERMINISTIC order: WHICH events the
  max_contact_events cap drops must be the same every run.
- Forces/torques clear at the end of every tick, whichever engine runs.
- One documented backend divergence: Jolt removes contacts when a body
  falls asleep and re-adds them on wake, so a resting 3D pair reports
  `ended` at sleep and `began` again on wake; Box2D keeps sleeping
  contacts silent. Passed through the seam verbatim - the 3D engine
  suite pins the Jolt shape.
- Second divergence, same shape: a runtime `set_collision_filter` wakes
  the island in Box2D (`b2Shape_SetFilter`) but NOT in Jolt
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
  `set_collision_filter()` on both worlds. Box2D: b2Filter at create +
  `b2Shape_SetFilter` at runtime (the adapter now keeps the per-slot
  shape id). Jolt: (layer, mask) ride CollisionGroup's ids + a
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
    b2MakeOffsetBox). Nothing at contact time.
  - Jolt: sub-shapes do not carry materials through the engine's mixing,
    so the adapter keeps flat per-slot part tables (friction/restitution,
    max_bodies x kMaxPartsPerBody, sized at construction) and the contact
    listener recombines: combined friction sqrt(f1*f2), restitution
    max(e1,e2) - Jolt's own defaults, matching Box2D's native mix. The
    override fires in OnContactAdded AND OnContactPersisted (Jolt
    rebuilds contact constraints from body defaults every step - an
    Added-only override silently reverts on the second step of a resting
    pair). The part index comes from GetSubShapeIndexFromID on the
    StaticCompoundShape (AddShape order IS parts order);
    GetSubShapeUserData is useless here (it reads the LEAF's user data,
    and cached leaves are shared across bodies). Multi-part bodies set
    mUseManifoldReduction(false) - a merged manifold carries one
    sub-shape's material. A one-part body is NOT a compound (Create
    returns the bare child): its material bakes body-level, and a
    non-zero offset rides a RotatedTranslated decorator (part_index
    returns -1 there, which is correct by construction).
- Remaining (all additive behind the seam, no public break): CCD/bullet
  flags, sleep control.

## Review decisions, settled
- Virtual backend seam kept (not a compile-time template backend): a mock
  backend for tests is worth more than a vcall that happens once per
  control call, never per solver iteration.
- destroy_body scans joint records O(max_joints) - fine at v1 scales; a
  per-body joint list is the upgrade path if mass despawn ever profiles hot.
- Sort-then-cap REJECTED for event overflow: it would break the
  pre-reserved buffer bound (events beyond max_contact_events are never
  stored). Deterministic backend feed order + cap achieves the same
  determinism within the bound.
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
apps/bench_physics (desktop console app) is the throughput consumer: a
falling-box rain per dimension, timed while awake and after sleep. Run it
manually - absolute numbers are machine-dependent, so nothing asserts on
them.
