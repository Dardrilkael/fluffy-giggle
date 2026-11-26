// Procedural noise properties; test_math.cpp owns doctest main.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include <ws/math/noise.hpp>

TEST_CASE("noise: integer hash is deterministic, stable and bounded") {
  const float first = ws::math::noise::hash01(123u, 456u, 789u);
  CHECK(first == ws::math::noise::hash01(123u, 456u, 789u));
  CHECK(first >= 0.0f);
  CHECK(first < 1.0f);
  CHECK(first != ws::math::noise::hash01(123u, 456u, 790u));
}

TEST_CASE("noise: 2D value noise is deterministic and continuous at lattice edges") {
  using ws::math::noise::value_noise2d;
  const float a = value_noise2d(12.25f, 7.75f, 42u);
  CHECK(a == value_noise2d(12.25f, 7.75f, 42u));
  CHECK(a >= 0.0f);
  CHECK(a < 1.0f);
  const float left = value_noise2d(3.0f - 1.0e-4f, 4.37f, 11u);
  const float edge = value_noise2d(3.0f, 4.37f, 11u);
  const float right = value_noise2d(3.0f + 1.0e-4f, 4.37f, 11u);
  CHECK(std::abs(left - edge) < 1.0e-4f);
  CHECK(std::abs(right - edge) < 1.0e-4f);
}

TEST_CASE("noise: FBM is deterministic and handles non-positive octave counts") {
  using ws::math::noise::fbm2d;
  const float a = fbm2d(14.0f, -2.0f, 0xC0FFEEu);
  CHECK(a == fbm2d(14.0f, -2.0f, 0xC0FFEEu));
  CHECK(a >= 0.0f);
  CHECK(a < 1.0f);
  CHECK(fbm2d(1.0f, 2.0f, 3u, 0) == 0.0f);
}

TEST_CASE("noise: 3D value noise is deterministic and bounded") {
  const float a = ws::math::noise::value_noise3d(1.25f, 3.5f, -9.75f, 7u);
  CHECK(a == ws::math::noise::value_noise3d(1.25f, 3.5f, -9.75f, 7u));
  CHECK(a >= 0.0f);
  CHECK(a < 1.0f);
}
