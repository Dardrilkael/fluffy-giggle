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
- Contact events, moved_bodies() and prev_transforms() are PER
  step()/advance() CALL - a multi-tick advance() unions its ticks. Events
  are sorted by the full tiebreak chain (a.index, b.index, phase, point,
  normal, speed).
- Backends feed events in DETERMINISTIC order: WHICH events the
  max_contact_events cap drops must be the same every run.
- Forces/torques clear at the end of every tick, whichever engine runs.
- One documented backend divergence: Jolt removes contacts when a body
  falls asleep and re-adds them on wake, so a resting 3D pair reports
  `ended` at sleep and `began` again on wake; Box2D keeps sleeping
  contacts silent. Passed through the seam verbatim - the 3D engine
  suite pins the Jolt shape.
- Determinism: same binary, same thread_count, same call sequence produces
  identical results (replay test). Cross-platform and cross-thread-count
  determinism are NOT promised.

## v1 scope (decided - review point 7)
In scope: sensors (BodyDesc::is_sensor), the three v1 joint kinds, hit
events above a speed threshold, the bulk set_transforms path.
Deferred to v2 (the seam is internal, so every one of these grows
additively with no public break): collision layers/masks, body user data,
multi-shape bodies, per-shape materials, CCD/bullet flags, sleep control.

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
