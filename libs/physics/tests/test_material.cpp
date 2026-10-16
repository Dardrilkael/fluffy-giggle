// The C5 unit suite: the lib's ONE material-mixing rule (CONTRACT.md 5,
// core/material.hpp). Both backends route every touching shape pair
// through these functions - the world callbacks in Box2D, the
// contact-settings override in Jolt's listener - so this file pins the
// formula the whole workspace runs on. The engines' own defaults happen
// to be the same numbers (which is why the ice/rough behavior twins
// stay green through the takeover); what the suite must catch is the
// formula drifting into something else. The values discriminate:
// 0.25 with 1.0 is 0.5 under sqrt, but 0.25 as a product and 0.625 as
// an arithmetic mean.

// No DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN - test_common.cpp owns doctest's
// main (one binary, several TUs).

#include <doctest/doctest.h>

#include "core/material.hpp"

namespace {

namespace mp = ws::physics::detail;

}  // namespace

TEST_CASE("material mix: friction is the geometric mean") {
  CHECK(mp::mix_friction(1.0f, 1.0f) == doctest::Approx(1.0f));
  CHECK(mp::mix_friction(0.25f, 1.0f) == doctest::Approx(0.5f));
  CHECK(mp::mix_friction(1.0f, 0.25f) == doctest::Approx(0.5f));  // symmetric
  CHECK(mp::mix_friction(0.09f, 0.49f) ==
        doctest::Approx(0.21f).epsilon(1e-6f));
  // A frictionless surface nulls any pairing (the ice-slide twins).
  CHECK(mp::mix_friction(0.0f, 0.8f) == 0.0f);
}

TEST_CASE("material mix: restitution is the max") {
  CHECK(mp::mix_restitution(0.2f, 0.7f) == doctest::Approx(0.7f));
  CHECK(mp::mix_restitution(0.9f, 0.1f) == doctest::Approx(0.9f));
  CHECK(mp::mix_restitution(0.0f, 0.0f) == 0.0f);
  CHECK(mp::mix_restitution(0.5f, 0.5f) == doctest::Approx(0.5f));
}
