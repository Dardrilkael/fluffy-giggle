#pragma once

// Lib-computed mass properties (CONTRACT.md 4): the lib owns mass, center
// of mass and rotational inertia. Closed-form from OUR shapes and the
// body-level density, identical for every backend; the adapters receive
// the finished numbers and apply them through their override mechanisms
// (b2Body_SetMassData / Jolt BodyCreationSettings::mOverrideMassProperties
// = MassAndInertiaProvided) - an engine never derives its own.
// Internal - never included by a public header.
//
// Frame conventions, matched to what the engines' overrides expect:
//   - center is the COM in BODY-LOCAL space (the body origin's frame).
//   - inertia is about the COM. Box2D centers its body->inertia the same
//     way before storing it (body.c: "Center the inertia about the center
//     of mass"), and Jolt's MassProperties are COM-frame by construction -
//     shapes report about their own center of mass and compounds never
//     shift back (sub-shape positions are stored COM-relative).
//   - Jolt never receives the COM as a number: it derives its rotation
//     pivot from the shape, and a StaticCompoundShape's pivot is the
//     volume-weighted child centroid. Parts share the body-level density,
//     so that centroid IS the lib's mass-weighted COM - the two agree by
//     construction, and the spin tests pin the agreement end to end.
//
// Multi-part composites: per-part masses sum, the COM is the mass-weighted
// mean, and inertia sums through the parallel-axis theorem. Parts carry no
// rotation in v2, so each child tensor stays where the shape put it.
//
// Everything here is pure float math over the desc: stack only, nothing
// allocates, so create_body inside the zero-alloc window stays free.

#include <array>
#include <cmath>
#include <numbers>
#include <type_traits>
#include <variant>

#include <ws/math/vecmath.hpp>
#include <ws/physics/2d/body.hpp>
#include <ws/physics/2d/shape.hpp>
#include <ws/physics/3d/body.hpp>
#include <ws/physics/3d/shape.hpp>
#include <ws/physics/common.hpp>

namespace ws::physics::detail {

// --- 2D ---------------------------------------------------------------------

// What the adapters receive: mass, body-local COM, inertia about the COM.
// Meaningful for dynamic bodies only - statics have no mass semantics and
// kinematics carry none (CONTRACT.md 4) - so the lib passes zeros for them
// and the adapters must not apply those zeros.
struct MassData2D {
  float mass = 0.0f;                 // kg
  ws::math::Vec2 center{};           // body-local COM
  float inertia = 0.0f;              // kg m^2 about the COM; the planar
                                     // tensor is one scalar
};

// (mass, centroid, inertia about the centroid) of one shape at a density.
// The centroid is in the SHAPE's own frame - a part's world-local position
// is shape centroid + part offset.
struct ShapeMass2D {
  float mass = 0.0f;
  ws::math::Vec2 centroid{};
  float inertia = 0.0f;
};

[[nodiscard]] inline ShapeMass2D shape_mass_2d(const physics2d::Shape& shape,
                                               float density) {
  using physics2d::Box;
  using physics2d::Capsule;
  using physics2d::Circle;
  ShapeMass2D out{};
  std::visit(
      [&](const auto& s) {
        using S = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<S, Circle>) {
          // Solid disc about its center.
          out.mass = density * std::numbers::pi_v<float> * s.radius * s.radius;
          out.inertia = 0.5f * out.mass * s.radius * s.radius;
        } else if constexpr (std::is_same_v<S, Box>) {
          const float hx = s.half_extents.x;
          const float hy = s.half_extents.y;
          out.mass = density * 4.0f * hx * hy;
          // m (w^2 + h^2) / 12 with w = 2hx, h = 2hy.
          out.inertia = out.mass * (hx * hx + hy * hy) / 3.0f;
        } else {
          // General-segment capsule: an L x 2r rectangle along the segment
          // plus the two end half-discs. The rectangle contributes
          // m_r (L^2 + 4 r^2) / 12. The half-discs sit at +-L/2, each with
          // its own inertia m_h r^2 / 2 about its disc center (half a full
          // disc's) and its centroid lc = 4r / (3 pi) PAST the segment end,
          // so the pair carries m_d (r^2/2 + h^2 + 2 h lc) about the
          // segment midpoint - the lc^2 in the parallel-axis shift cancels
          // against the shift to the half-disc's own centroid, but the
          // cross term 2 h lc SURVIVES. That term vanishes at BOTH
          // degenerates (L = 0 and r -> 0), so limit checks alone cannot
          // catch its absence - the unit tests pin it against a numeric
          // integral. Same closed form as b2ComputeCapsuleMass
          // (geometry.c). The centroid is the segment midpoint
          // (a <-> b symmetry).
          const ws::math::Vec2 seg = s.b - s.a;
          const float L = ws::math::length(seg);
          const float r = s.radius;
          const float lc = (4.0f * r) / (3.0f * std::numbers::pi_v<float>);
          const float rect = density * 2.0f * r * L;
          const float caps = density * std::numbers::pi_v<float> * r * r;
          out.mass = rect + caps;
          out.inertia =
              rect * (L * L + 4.0f * r * r) / 12.0f +
              caps * (0.5f * r * r + 0.25f * L * L + L * lc);
          out.centroid = (s.a + s.b) * 0.5f;
        }
      },
      shape);
  return out;
}

// The body's mass data: the single body-level shape, or the composite of
// parts[] when part_count != 0 (density stays body-level, so every part
// shares it).
[[nodiscard]] inline MassData2D mass_data_2d(const physics2d::BodyDesc& desc) {
  MassData2D out{};
  if (desc.part_count == 0) {
    const ShapeMass2D m = shape_mass_2d(desc.shape, desc.density);
    out.mass = m.mass;
    out.center = m.centroid;
    out.inertia = m.inertia;
    return out;
  }
  // Pass 1: per-part mass and first moment (the part offset shifts the
  // shape's own centroid).
  std::array<ShapeMass2D, ws::physics::kMaxPartsPerBody> parts{};
  ws::math::Vec2 moment{};
  for (std::uint32_t p = 0; p < desc.part_count; ++p) {
    const physics2d::Part& part = desc.parts[p];
    parts[p] = shape_mass_2d(part.shape, desc.density);
    out.mass += parts[p].mass;
    moment = moment + (parts[p].centroid + part.offset) * parts[p].mass;
  }
  out.center = moment / out.mass;
  // Pass 2: inertia about the composite COM, parallel axis per part.
  for (std::uint32_t p = 0; p < desc.part_count; ++p) {
    const physics2d::Part& part = desc.parts[p];
    const ws::math::Vec2 d = parts[p].centroid + part.offset - out.center;
    out.inertia += parts[p].inertia + parts[p].mass * ws::math::dot(d, d);
  }
  return out;
}

// --- 3D ---------------------------------------------------------------------

struct MassData3D {
  float mass = 0.0f;                 // kg
  ws::math::Vec3 center{};           // body-local COM
  ws::math::Mat3 inertia{};          // about the COM, column-major: element
                                     // (row r, col c) is inertia[c][r]
};

struct ShapeMass3D {
  float mass = 0.0f;
  ws::math::Vec3 centroid{};
  ws::math::Mat3 inertia{};  // about the centroid; diagonal for every v2
                            // shape (sphere, box, Y-aligned capsule)
};

[[nodiscard]] inline ShapeMass3D shape_mass_3d(const physics3d::Shape& shape,
                                               float density) {
  using physics3d::Box;
  using physics3d::Capsule;
  using physics3d::Sphere;
  ShapeMass3D out{};
  std::visit(
      [&](const auto& s) {
        using S = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<S, Sphere>) {
          out.mass = density * (4.0f / 3.0f) * std::numbers::pi_v<float> *
                     s.radius * s.radius * s.radius;
          const float i = 0.4f * out.mass * s.radius * s.radius;
          out.inertia[0][0] = i;
          out.inertia[1][1] = i;
          out.inertia[2][2] = i;
        } else if constexpr (std::is_same_v<S, Box>) {
          const float hx = s.half_extents.x;
          const float hy = s.half_extents.y;
          const float hz = s.half_extents.z;
          out.mass = density * 8.0f * hx * hy * hz;
          // m (w^2 + d^2) / 12 per axis with full extents = 2 * half.
          out.inertia[0][0] = out.mass * (hy * hy + hz * hz) / 3.0f;
          out.inertia[1][1] = out.mass * (hx * hx + hz * hz) / 3.0f;
          out.inertia[2][2] = out.mass * (hx * hx + hy * hy) / 3.0f;
        } else {
          // Y-aligned capsule: a cylinder 2h tall, radius r, plus the two
          // hemispheres at +-h (together a full sphere's mass, but NOT at
          // the center). Cylinder: m r^2 / 2 about Y, m (3 r^2 + 4 h^2) / 12
          // about X/Z. Hemispheres: (2/5) m_s r^2 about every axis, and
          // they shift out to +-h - free along Y (displacement parallel to
          // the axis never touches I_yy) but perpendicular it adds
          // m_s (h^2 + 3 h r / 4): each hemisphere's centroid sits 3r/8
          // past the cylinder end and the (3r/8)^2 cancels against its
          // parallel-axis shift, leaving the h*r cross term. That cross
          // term dies at BOTH degenerates (h = 0 and r -> 0), so the unit
          // tests pin it against CapsuleShape::GetMassProperties and the
          // sphere limit. Centroid is the origin by symmetry.
          const float h = s.half_height;
          const float r = s.radius;
          const float cyl = density * std::numbers::pi_v<float> * r * r *
                            2.0f * h;
          const float sph =
              density * (4.0f / 3.0f) * std::numbers::pi_v<float> * r * r * r;
          out.mass = cyl + sph;
          const float sph_i_axial = 0.4f * sph * r * r;  // both hemispheres
          out.inertia[1][1] = 0.5f * cyl * r * r + sph_i_axial;
          const float i_perp = cyl * (3.0f * r * r + 4.0f * h * h) / 12.0f +
                               sph_i_axial +
                               sph * (h * h + 0.75f * h * r);
          out.inertia[0][0] = i_perp;
          out.inertia[2][2] = i_perp;
        }
      },
      shape);
  return out;
}

// I += mass * (|d|^2 E - d d^T) - the parallel-axis shift, column-major:
// element (row r, col c) sits at inertia[c][r].
inline void add_parallel_axis_3d(ws::math::Mat3& inertia, ws::math::Vec3 d,
                                 float mass) {
  const float d2 = ws::math::dot(d, d);
  for (int c = 0; c < 3; ++c) {
    for (int r = 0; r < 3; ++r) {
      inertia[c][r] +=
          mass * (d2 * ((r == c) ? 1.0f : 0.0f) - d[r] * d[c]);
    }
  }
}

[[nodiscard]] inline MassData3D mass_data_3d(const physics3d::BodyDesc& desc) {
  MassData3D out{};
  if (desc.part_count == 0) {
    const ShapeMass3D m = shape_mass_3d(desc.shape, desc.density);
    out.mass = m.mass;
    out.center = m.centroid;
    out.inertia = m.inertia;
    return out;
  }
  std::array<ShapeMass3D, ws::physics::kMaxPartsPerBody> parts{};
  ws::math::Vec3 moment{};
  for (std::uint32_t p = 0; p < desc.part_count; ++p) {
    const physics3d::Part& part = desc.parts[p];
    parts[p] = shape_mass_3d(part.shape, desc.density);
    out.mass += parts[p].mass;
    moment = moment + (parts[p].centroid + part.offset) * parts[p].mass;
  }
  out.center = moment / out.mass;
  for (std::uint32_t p = 0; p < desc.part_count; ++p) {
    const physics3d::Part& part = desc.parts[p];
    add_parallel_axis_3d(out.inertia,
                        parts[p].centroid + part.offset - out.center,
                        parts[p].mass);
    out.inertia[0][0] += parts[p].inertia[0][0];
    out.inertia[1][1] += parts[p].inertia[1][1];
    out.inertia[2][2] += parts[p].inertia[2][2];
  }
  return out;
}

}  // namespace ws::physics::detail
