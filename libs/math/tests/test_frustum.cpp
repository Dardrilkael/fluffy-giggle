// Frustum extraction and conservative AABB tests. test_math.cpp owns doctest main.
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <numbers>

#include <ws/math/frustum.hpp>

namespace {
using ws::math::culling::aabb_visible;
using ws::math::culling::extract_frustum;

[[nodiscard]] bool all_zero(const ws::math::culling::Frustum& frustum) {
  for (const auto& plane : frustum.planes)
    for (float component : plane)
      if (component != 0.0f) return false;
  return true;
}
}  // namespace

TEST_CASE("frustum: non-right-angle horizontal bounds use tangent-scaled forward") {
  const float eye[3] = {0.0f, 0.0f, 0.0f};
  const float target[3] = {0.0f, 0.0f, 1.0f};
  const float up[3] = {0.0f, -1.0f, 0.0f};
  const auto fr = extract_frustum(
      eye, target, up, 70.0f * std::numbers::pi_v<float> / 180.0f,
      16.0f / 9.0f, 0.03f, 260.0f);
  const float left_inside[3] = {-10.0f, 0.0f, 10.0f};
  const float right_inside[3] = {10.0f, 0.0f, 10.0f};
  const float left_outside[3] = {-14.0f, 0.0f, 10.0f};
  const float right_outside[3] = {14.0f, 0.0f, 10.0f};
  CHECK(aabb_visible(fr, left_inside, left_inside));
  CHECK(aabb_visible(fr, right_inside, right_inside));
  CHECK_FALSE(aabb_visible(fr, left_outside, left_outside));
  CHECK_FALSE(aabb_visible(fr, right_outside, right_outside));
}

TEST_CASE("frustum: positive support vertex is conservative for AABBs") {
  const float eye[3] = {4.0f, 2.0f, -6.0f};
  const float target[3] = {4.3f, 1.9f, -5.0f};
  const float up[3] = {0.0f, 1.0f, 0.0f};
  const auto fr = extract_frustum(eye, target, up, 1.2f, 1.6f, 0.1f, 80.0f);

  int sampled_inside = 0;
  for (int z = -20; z <= 20; z += 2) {
    for (int y = -10; y <= 10; y += 2) {
      for (int x = -20; x <= 20; x += 2) {
        const float p[3] = {static_cast<float>(x), static_cast<float>(y),
                            static_cast<float>(z)};
        bool oracle_inside = true;
        for (const auto& plane : fr.planes) {
          if (plane[0] * p[0] + plane[1] * p[1] +
                  plane[2] * p[2] + plane[3] < 0.0f) {
            oracle_inside = false;
            break;
          }
        }
        CHECK(aabb_visible(fr, p, p) == oracle_inside);
        sampled_inside += oracle_inside ? 1 : 0;
      }
    }
  }
  CHECK(sampled_inside > 0);
  const float box_min[3] = {3.0f, -2.0f, -12.0f};
  const float box_max[3] = {6.0f, 5.0f, -2.0f};
  bool has_inside_sample = false;
  for (int z = 0; z <= 8; ++z)
    for (int y = 0; y <= 4; ++y)
      for (int x = 0; x <= 8; ++x) {
        const float p[3] = {
            box_min[0] + (box_max[0] - box_min[0]) * x / 8.0f,
            box_min[1] + (box_max[1] - box_min[1]) * y / 4.0f,
            box_min[2] + (box_max[2] - box_min[2]) * z / 8.0f};
        bool inside = true;
        for (const auto& plane : fr.planes)
          inside &= plane[0] * p[0] + plane[1] * p[1] +
                        plane[2] * p[2] + plane[3] >= 0.0f;
        has_inside_sample |= inside;
      }
  CHECK_FALSE(has_inside_sample && !aabb_visible(fr, box_min, box_max));
}

TEST_CASE("frustum: degenerate camera and invalid aspect fail open without NaNs") {
  const float eye[3] = {1.0f, 2.0f, 3.0f};
  const float same[3] = {1.0f, 2.0f, 3.0f};
  const float target[3] = {1.0f, 2.0f, 4.0f};
  const float parallel_up[3] = {0.0f, 0.0f, 1.0f};
  const float valid_up[3] = {0.0f, 1.0f, 0.0f};
  const float min[3] = {10000.0f, 10000.0f, 10000.0f};
  const float max[3] = {10001.0f, 10001.0f, 10001.0f};
  const auto zero_forward =
      extract_frustum(eye, same, valid_up, 1.2f, 1.7f, 0.1f, 260.0f);
  const auto parallel_basis =
      extract_frustum(eye, target, parallel_up, 1.2f, 1.7f, 0.1f, 260.0f);
  const auto invalid_aspect =
      extract_frustum(eye, target, valid_up, 1.2f, 0.0f, 0.1f, 260.0f);
  CHECK(all_zero(zero_forward));
  CHECK(all_zero(parallel_basis));
  CHECK(all_zero(invalid_aspect));
  CHECK(aabb_visible(zero_forward, min, max));
  CHECK(aabb_visible(parallel_basis, min, max));
  CHECK(aabb_visible(invalid_aspect, min, max));
}
