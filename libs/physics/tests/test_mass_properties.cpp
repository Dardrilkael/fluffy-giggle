// The C4 unit suite: the lib's closed-form mass properties (CONTRACT.md 4)
// against independent math. Expected values are hand-derived expressions or
// numeric integrals, never re-runs of the code under test. The capsule
// formulas are the reason this suite exists in this shape: they carry h*r
// cross terms (the cap centroids sit past the segment/cylinder ends) that
// BOTH degenerate limits - L/h = 0 and r -> 0 - completely hide, so the
// numeric integrals are what catch their absence. The 2D capsule shipped
// without its cross term for exactly that reason during development; the
// integral caught it.

// No DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN - test_common.cpp owns doctest's
// main (one binary, several TUs).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "core/mass_properties.hpp"

#include <ws/math/vecmath.hpp>
#include <ws/physics/2d/body.hpp>
#include <ws/physics/2d/shape.hpp>
#include <ws/physics/3d/body.hpp>
#include <ws/physics/3d/shape.hpp>

namespace {

namespace mp = ws::physics::detail;
namespace p2 = ws::physics2d;
namespace p3 = ws::physics3d;
using ws::math::Vec2;
using ws::math::Vec3;

constexpr float kPi = std::numbers::pi_v<float>;

}  // namespace

TEST_CASE("2d: circle, box and capsule against hand-derived forms") {
  const mp::ShapeMass2D c = mp::shape_mass_2d(
      p2::Shape{p2::Circle{1.0f}}, 3.0f);
  CHECK(c.mass == doctest::Approx(3.0f * kPi).epsilon(1e-5f));
  CHECK(c.inertia == doctest::Approx(1.5f * kPi).epsilon(1e-5f));
  CHECK(c.centroid.x == 0.0f);
  CHECK(c.centroid.y == 0.0f);

  const mp::ShapeMass2D b = mp::shape_mass_2d(
      p2::Shape{p2::Box{Vec2{1.0f, 1.0f}}}, 3.0f);
  CHECK(b.mass == doctest::Approx(12.0f).epsilon(1e-6f));
  CHECK(b.inertia == doctest::Approx(12.0f * 2.0f / 3.0f).epsilon(1e-6f));

  // a=(0,-1) b=(0,1) r=0.5, rho=2: rectangle m=4, caps = one full disc.
  // Rectangle: m (L^2 + 4 r^2) / 12. Caps: m_d (r^2/2 + h^2 + 2 h lc) with
  // lc = 4r / (3 pi) the half-disc centroid offset - the cross term the
  // degenerates hide.
  const float lc = (4.0f * 0.5f) / (3.0f * kPi);
  const float caps = 2.0f * kPi * 0.25f;
  const mp::ShapeMass2D k = mp::shape_mass_2d(
      p2::Shape{p2::Capsule{Vec2{0.0f, -1.0f},
                                          Vec2{0.0f, 1.0f}, 0.5f}},
      2.0f);
  CHECK(k.mass == doctest::Approx(4.0f + caps).epsilon(1e-5f));
  CHECK(k.inertia == doctest::Approx(4.0f * 5.0f / 12.0f +
                                     caps * (0.125f + 1.0f + 2.0f * lc))
                             .epsilon(1e-5f));
  CHECK(k.centroid.x == 0.0f);
  CHECK(k.centroid.y == 0.0f);
}

TEST_CASE("2d: diagonal capsule vs a numeric integral") {
  const Vec2 a{-0.3f, -0.7f};
  const Vec2 b{0.5f, 0.4f};
  const float r = 0.45f;
  const float rho = 1.7f;
  const mp::ShapeMass2D k =
      mp::shape_mass_2d(p2::Shape{p2::Capsule{a, b, r}}, rho);

  // Midpoint rule over the bounding box; distance-to-segment membership.
  // Double accumulation so the comparison error is pure discretization.
  const Vec2 d = b - a;
  const float dd = ws::math::dot(d, d);
  const int n = 600;
  const float x0 = std::min(a.x, b.x) - r, x1 = std::max(a.x, b.x) + r;
  const float y0 = std::min(a.y, b.y) - r, y1 = std::max(a.y, b.y) + r;
  const float sx = (x1 - x0) / n, sy = (y1 - y0) / n;
  double area = 0.0, mx = 0.0, my = 0.0, i_origin = 0.0;
  for (int i = 0; i < n; ++i) {
    const float px = x0 + (i + 0.5f) * sx;
    for (int j = 0; j < n; ++j) {
      const float py = y0 + (j + 0.5f) * sy;
      const float t = std::clamp(
          ws::math::dot(Vec2{px, py} - a, d) / dd, 0.0f, 1.0f);
      const Vec2 e = Vec2{px, py} - (a + d * t);
      if (ws::math::dot(e, e) <= r * r) {
        const double cell = double(sx) * sy;
        area += cell;
        mx += double(px) * cell;
        my += double(py) * cell;
        i_origin += (double(px) * px + double(py) * py) * cell;
      }
    }
  }
  const double mass = rho * area;
  const double cx = mx / area, cy = my / area;
  const double inertia =
      rho * i_origin - mass * (cx * cx + cy * cy);

  CHECK(k.mass == doctest::Approx(float(mass)).epsilon(5e-3f));
  CHECK(k.centroid.x == doctest::Approx(float(cx)).epsilon(5e-3f));
  CHECK(k.centroid.y == doctest::Approx(float(cy)).epsilon(5e-3f));
  CHECK(k.inertia == doctest::Approx(float(inertia)).epsilon(5e-3f));
}

TEST_CASE("2d: capsule degenerates to the circle at L = 0") {
  const mp::ShapeMass2D c =
      mp::shape_mass_2d(p2::Shape{p2::Circle{0.7f}}, 1.3f);
  const mp::ShapeMass2D k = mp::shape_mass_2d(
      p2::Shape{p2::Capsule{Vec2{0.3f, -0.2f},
                                          Vec2{0.3f, -0.2f}, 0.7f}},
      1.3f);
  CHECK(k.mass == doctest::Approx(c.mass).epsilon(1e-6f));
  CHECK(k.inertia == doctest::Approx(c.inertia).epsilon(1e-6f));
  CHECK(k.centroid.x == doctest::Approx(0.3f).epsilon(1e-6f));
  CHECK(k.centroid.y == doctest::Approx(-0.2f).epsilon(1e-6f));
}

TEST_CASE("2d: parts sum, weigh and parallel-axis") {
  // Box half=(1,1) at the origin (m=4, I=8/3) + box half=(0.5,0.5) at
  // (2,0) (m=1, I=1/6): m=5, com=(0.4,0), I about the com =
  // 8/3 + 1/6 + 4*(0.4)^2 + 1*(1.6)^2.
  p2::BodyDesc d{};
  d.density = 1.0f;
  d.parts[0].shape = p2::Box{Vec2{1.0f, 1.0f}};
  d.parts[1].shape = p2::Box{Vec2{0.5f, 0.5f}};
  d.parts[1].offset = Vec2{2.0f, 0.0f};
  d.part_count = 2;
  const mp::MassData2D m = mp::mass_data_2d(d);
  CHECK(m.mass == doctest::Approx(5.0f).epsilon(1e-6f));
  CHECK(m.center.x == doctest::Approx(0.4f).epsilon(1e-6f));
  CHECK(m.center.y == 0.0f);
  CHECK(m.inertia == doctest::Approx(8.0f / 3.0f + 1.0f / 6.0f +
                                     4.0f * 0.4f * 0.4f +
                                     1.0f * 1.6f * 1.6f)
                             .epsilon(1e-6f));

  // part_count == 0: the body-level shape, straight through.
  p2::BodyDesc single{};
  single.density = 3.0f;
  single.shape = p2::Box{Vec2{1.0f, 1.0f}};
  const mp::MassData2D sm = mp::mass_data_2d(single);
  CHECK(sm.mass == doctest::Approx(12.0f).epsilon(1e-6f));
  CHECK(sm.inertia == doctest::Approx(8.0f).epsilon(1e-6f));
  CHECK(sm.center.x == 0.0f);
  CHECK(sm.center.y == 0.0f);
}

TEST_CASE("3d: sphere, box and capsule against hand-derived forms") {
  const mp::ShapeMass3D s = mp::shape_mass_3d(
      p3::Shape{p3::Sphere{1.0f}}, 2.0f);
  CHECK(s.mass == doctest::Approx(8.0f * kPi / 3.0f).epsilon(1e-5f));
  CHECK(s.inertia[0][0] ==
        doctest::Approx(0.4f * 8.0f * kPi / 3.0f).epsilon(1e-5f));
  CHECK(s.inertia[1][1] == s.inertia[0][0]);
  CHECK(s.inertia[2][2] == s.inertia[0][0]);
  CHECK(s.inertia[1][0] == 0.0f);

  const mp::ShapeMass3D b = mp::shape_mass_3d(
      p3::Shape{p3::Box{Vec3{1.0f, 1.0f, 1.0f}}}, 1.0f);
  CHECK(b.mass == doctest::Approx(8.0f).epsilon(1e-6f));
  CHECK(b.inertia[0][0] == doctest::Approx(16.0f / 3.0f).epsilon(1e-6f));
  CHECK(b.inertia[1][1] == doctest::Approx(16.0f / 3.0f).epsilon(1e-6f));
  CHECK(b.inertia[2][2] == doctest::Approx(16.0f / 3.0f).epsilon(1e-6f));

  // h=1 r=0.5 rho=1: cylinder m = pi/2, hemispheres m = pi/6. Iyy gets no
  // hemisphere shift (displacement parallel to the axis); Ixx/Izz gain
  // m_s (h^2 + 3 h r / 4) - the cross term both degenerates hide.
  const float cyl = kPi * 0.5f;
  const float sph = kPi / 6.0f;
  const mp::ShapeMass3D k = mp::shape_mass_3d(
      p3::Shape{p3::Capsule{1.0f, 0.5f}}, 1.0f);
  CHECK(k.mass == doctest::Approx(cyl + sph).epsilon(1e-5f));
  CHECK(k.inertia[1][1] == doctest::Approx(0.5f * cyl * 0.25f +
                                           0.4f * sph * 0.25f)
                             .epsilon(1e-5f));
  CHECK(k.inertia[0][0] == doctest::Approx(cyl * (3.0f * 0.25f + 4.0f) /
                                               12.0f +
                                           sph * (0.1f + 1.0f + 0.375f))
                             .epsilon(1e-5f));
  CHECK(k.inertia[2][2] == doctest::Approx(k.inertia[0][0]).epsilon(1e-6f));
  CHECK(k.centroid.x == 0.0f);
  CHECK(k.centroid.y == 0.0f);
  CHECK(k.centroid.z == 0.0f);

  // part_count == 0 routes the body-level shape straight through.
  p3::BodyDesc single{};
  single.density = 2.0f;
  single.shape = p3::Sphere{1.0f};
  const mp::MassData3D sm = mp::mass_data_3d(single);
  CHECK(sm.mass == doctest::Approx(8.0f * kPi / 3.0f).epsilon(1e-5f));
  CHECK(sm.center.x == 0.0f);
  CHECK(sm.center.y == 0.0f);
  CHECK(sm.center.z == 0.0f);
}

TEST_CASE("3d: capsule vs a numeric integral") {
  // Off-degenerate numbers so the cross term is fully exercised.
  const float h = 0.8f, r = 0.45f, rho = 1.3f;
  const mp::ShapeMass3D k = mp::shape_mass_3d(
      p3::Shape{p3::Capsule{h, r}}, rho);

  const int n = 150;
  const float y0 = -(h + r), y1 = h + r;
  const float sx = 2.0f * r / n, sy = (y1 - y0) / n, sz = 2.0f * r / n;
  double vol = 0.0, i_xx = 0.0, i_yy = 0.0, i_zz = 0.0;
  for (int ix = 0; ix < n; ++ix) {
    const float px = -r + (ix + 0.5f) * sx;
    for (int iy = 0; iy < n; ++iy) {
      const float py = y0 + (iy + 0.5f) * sy;
      const float dy = std::max(std::abs(py) - h, 0.0f);
      for (int iz = 0; iz < n; ++iz) {
        const float pz = -r + (iz + 0.5f) * sz;
        if (px * px + dy * dy + pz * pz <= r * r) {
          const double cell = double(sx) * sy * sz;
          vol += cell;
          i_xx += (double(py) * py + double(pz) * pz) * cell;
          i_yy += (double(px) * px + double(pz) * pz) * cell;
          i_zz += (double(px) * px + double(py) * py) * cell;
        }
      }
    }
  }
  CHECK(k.mass == doctest::Approx(float(rho * vol)).epsilon(8e-3f));
  CHECK(k.inertia[0][0] == doctest::Approx(float(rho * i_xx)).epsilon(8e-3f));
  CHECK(k.inertia[1][1] == doctest::Approx(float(rho * i_yy)).epsilon(8e-3f));
  CHECK(k.inertia[2][2] == doctest::Approx(float(rho * i_zz)).epsilon(8e-3f));
}

TEST_CASE("3d: capsule degenerates to the sphere at h = 0") {
  const mp::ShapeMass3D s = mp::shape_mass_3d(
      p3::Shape{p3::Sphere{0.5f}}, 1.0f);
  const mp::ShapeMass3D k = mp::shape_mass_3d(
      p3::Shape{p3::Capsule{0.0f, 0.5f}}, 1.0f);
  CHECK(k.mass == doctest::Approx(s.mass).epsilon(1e-6f));
  CHECK(k.inertia[0][0] == doctest::Approx(s.inertia[0][0]).epsilon(1e-6f));
  CHECK(k.inertia[1][1] == doctest::Approx(s.inertia[1][1]).epsilon(1e-6f));
  CHECK(k.inertia[2][2] == doctest::Approx(s.inertia[2][2]).epsilon(1e-6f));
}

TEST_CASE("3d: composite tensor with off-diagonals") {
  // Box half=(1,1,1) at the origin + sphere r=0.5 at (3,1,0), rho=1. The
  // sphere's offset makes every I_x* / I_y* product term live.
  p3::BodyDesc d{};
  d.density = 1.0f;
  d.parts[0].shape = p3::Box{Vec3{1.0f, 1.0f, 1.0f}};
  d.parts[1].shape = p3::Sphere{0.5f};
  d.parts[1].offset = Vec3{3.0f, 1.0f, 0.0f};
  d.part_count = 2;
  const mp::MassData3D m = mp::mass_data_3d(d);

  // Hand-derived, independently: masses, composite com, then the
  // parallel-axis sums written from the geometry (d_box = -com,
  // d_sphere = (3,1,0) - com; every part's dz = 0).
  const float m_b = 8.0f;
  const float m_s = kPi / 6.0f;
  const float cx = 3.0f * m_s / (m_b + m_s);
  const float cy = m_s / (m_b + m_s);
  const float i_box = 16.0f / 3.0f;
  const float i_sph = 0.4f * m_s * 0.25f;
  const float dbx = -cx, dby = -cy;
  const float dsx = 3.0f - cx, dsy = 1.0f - cy;

  CHECK(m.mass == doctest::Approx(m_b + m_s).epsilon(1e-6f));
  CHECK(m.center.x == doctest::Approx(cx).epsilon(1e-6f));
  CHECK(m.center.y == doctest::Approx(cy).epsilon(1e-6f));
  CHECK(m.center.z == 0.0f);
  CHECK(m.inertia[0][0] == doctest::Approx(i_box + m_b * dby * dby +
                                           i_sph + m_s * dsy * dsy)
                             .epsilon(1e-6f));
  CHECK(m.inertia[1][1] == doctest::Approx(i_box + m_b * dbx * dbx +
                                           i_sph + m_s * dsx * dsx)
                             .epsilon(1e-6f));
  CHECK(m.inertia[2][2] ==
        doctest::Approx(i_box + m_b * (dbx * dbx + dby * dby) +
                        i_sph + m_s * (dsx * dsx + dsy * dsy))
            .epsilon(1e-6f));
  // Column-major storage: element (row r, col c) is inertia[c][r].
  CHECK(m.inertia[1][0] == doctest::Approx(-(m_b * dbx * dby +
                                             m_s * dsx * dsy))
                             .epsilon(1e-6f));
  CHECK(m.inertia[0][1] == doctest::Approx(m.inertia[1][0]).epsilon(1e-6f));
  CHECK(std::abs(m.inertia[2][0]) < 1e-6f);
  CHECK(std::abs(m.inertia[2][1]) < 1e-6f);
  CHECK(std::abs(m.inertia[0][2]) < 1e-6f);
  CHECK(std::abs(m.inertia[1][2]) < 1e-6f);
}
