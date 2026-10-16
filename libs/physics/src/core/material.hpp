#pragma once

// The ONE lib material-mixing rule (CONTRACT.md 5): how two touching
// surfaces combine - friction as the geometric mean, restitution as the
// max. The lib owns the formula; an engine's own mixing never runs:
//   - 2D: the world callbacks (b2WorldDef.frictionCallback /
//     restitutionCallback) route every touching shape pair through these,
//     so Box2D's b2DefaultFrictionCallback/b2DefaultRestitutionCallback
//     never fire.
//   - 3D: Jolt seeds each contact constraint from its own combine, then
//     the adapter's listener overwrites both combined values for EVERY
//     pair before the constraint stores them.
// Both engines' own defaults are exactly these formulas, so the takeover
// is bit-identical today - the point is ownership: if either engine ever
// changed its mix, or a new backend arrived, every pair in every
// dimension would still combine through this one function.
//
// Applied per touching SHAPE pair (multi-part bodies: per part pair).
// Pure float math - no state, no allocation - which is what both engines'
// hook contracts demand (Box2D documents its callbacks as worker-thread,
// no-state-change; Jolt's listener runs with bodies locked).
// Internal - never included by a public header.

#include <algorithm>
#include <cmath>

namespace ws::physics::detail {

[[nodiscard]] inline float mix_friction(float friction_a, float friction_b) {
  return std::sqrt(friction_a * friction_b);
}

[[nodiscard]] inline float mix_restitution(float restitution_a,
                                           float restitution_b) {
  return std::max(restitution_a, restitution_b);
}

}  // namespace ws::physics::detail
