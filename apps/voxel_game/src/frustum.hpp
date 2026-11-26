// apps/voxel_game - frustum.hpp: conservative CPU frustum culling.
// Engine-free so the renderer's exact culling math can be regression-tested
// by the headless voxel logic suite.

#pragma once

#include <cmath>

namespace voxel::culling {

struct Frustum {
  // ax + by + cz + d >= 0 is the retained half-space for each plane.
  float planes[6][4]{};
};

// A zero-plane frustum deliberately retains every AABB. It is the safe
// fallback when a camera basis is degenerate or its projection is invalid.
[[nodiscard]] inline Frustum keep_everything() noexcept { return {}; }

[[nodiscard]] inline Frustum extract_frustum(
    const float eye[3], const float target[3], const float up_hint[3],
    float fov_y, float aspect, float near_z, float far_z) noexcept {
  Frustum fr{};

  if (!std::isfinite(aspect) || aspect <= 0.0f ||
      !std::isfinite(eye[0]) || !std::isfinite(eye[1]) ||
      !std::isfinite(eye[2]) || !std::isfinite(target[0]) ||
      !std::isfinite(target[1]) || !std::isfinite(target[2]) ||
      !std::isfinite(up_hint[0]) || !std::isfinite(up_hint[1]) ||
      !std::isfinite(up_hint[2])) {
    return fr;
  }

  constexpr float kDefaultFov = 1.2217f;
  constexpr float kPi = 3.14159265358979323846f;
  const float fov = std::isfinite(fov_y) && fov_y > 0.0f && fov_y < kPi
                        ? fov_y
                        : kDefaultFov;
  const bool valid_depth_range = std::isfinite(near_z) && near_z > 0.0f &&
                                 std::isfinite(far_z) && far_z > near_z;
  const float near_plane = valid_depth_range ? near_z : 0.1f;
  const float far_plane = valid_depth_range ? far_z : 260.0f;
  const float tan_v = std::tan(fov * 0.5f);
  const float tan_hx = tan_v * aspect;
  if (!std::isfinite(tan_v) || !std::isfinite(tan_hx) ||
      tan_v <= 0.0f || tan_hx <= 0.0f) {
    return fr;
  }

  float f[3] = {target[0] - eye[0], target[1] - eye[1],
                target[2] - eye[2]};
  const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  if (!std::isfinite(fl) || fl < 1.0e-6f) {
    return fr;
  }
  f[0] /= fl; f[1] /= fl; f[2] /= fl;

  float r[3] = {f[1] * up_hint[2] - f[2] * up_hint[1],
                f[2] * up_hint[0] - f[0] * up_hint[2],
                f[0] * up_hint[1] - f[1] * up_hint[0]};
  const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
  if (!std::isfinite(rl) || rl < 1.0e-6f) {
    return fr;
  }
  r[0] /= rl; r[1] /= rl; r[2] /= rl;

  const float u[3] = {r[1] * f[2] - r[2] * f[1],
                      r[2] * f[0] - r[0] * f[2],
                      r[0] * f[1] - r[1] * f[0]};
  const float eye_dot_f = eye[0] * f[0] + eye[1] * f[1] + eye[2] * f[2];

  const auto set = [&](int i, const float n[3], float d) noexcept {
    const float length =
        std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!std::isfinite(length) || length < 1.0e-6f || !std::isfinite(d)) {
      return false;
    }
    fr.planes[i][0] = n[0] / length;
    fr.planes[i][1] = n[1] / length;
    fr.planes[i][2] = n[2] / length;
    fr.planes[i][3] = d / length;
    return true;
  };

  const float negf[3] = {-f[0], -f[1], -f[2]};
  if (!set(0, f, -eye_dot_f - near_plane) ||
      !set(1, negf, eye_dot_f + far_plane)) {
    return Frustum{};
  }

  // Inward side normals are tan(half-FOV) * forward +/- perpendicular.
  // Scaling forward by tan is essential when the FOV is not 90 degrees.
  const float negr[3] = {-r[0], -r[1], -r[2]};
  const float negu[3] = {-u[0], -u[1], -u[2]};
  const auto side = [&](int i, const float a[3], const float b[3],
                        float tangent) noexcept {
    const float n[3] = {tangent * a[0] + b[0],
                        tangent * a[1] + b[1],
                        tangent * a[2] + b[2]};
    const float d = -(n[0] * eye[0] + n[1] * eye[1] + n[2] * eye[2]);
    return set(i, n, d);
  };
  if (!side(2, f, r, tan_hx) ||
      !side(3, f, negr, tan_hx) ||
      !side(4, f, u, tan_v) ||
      !side(5, f, negu, tan_v)) {
    return Frustum{};
  }

  return fr;
}

// AABB vs frustum: visible when its positive support vertex reaches every
// retained half-space. This is conservative at frustum corners.
[[nodiscard]] inline bool aabb_visible(const Frustum& fr, const float min_[3],
                                       const float max_[3]) noexcept {
  for (int i = 0; i < 6; ++i) {
    const float* p = fr.planes[i];
    const float px = p[0] > 0.0f ? max_[0] : min_[0];
    const float py = p[1] > 0.0f ? max_[1] : min_[1];
    const float pz = p[2] > 0.0f ? max_[2] : min_[2];
    const float support = p[0] * px + p[1] * py + p[2] * pz + p[3];
    if (support < 0.0f) {
      return false;
    }
  }
  return true;
}

}  // namespace voxel::culling
