// The reference 3D backend behind the same seam as Jolt
// (core/backend3d.hpp): the 3D twin of reference_2d.cpp - CONTRACT.md
// computed by the simplest possible code, no engine underneath. It exists
// so the conformance suite (C7) can run the lib's machinery over a second
// 3D implementation and so tests can pin semantics with hand-checkable
// numbers - every trajectory below is plain kinematics a reader can
// recompute.
//
// It is an honest SUBSET, not a toy solver:
//   - NO contact response: overlapping pairs pass through each other.
//     Detection, events, ballistics and queries are contract-true; resting
//     contact is not (there is no solver to produce it).
//   - NO CCD: is_bullet is stored and never read - a fast body can tunnel
//     by construction.
//   - NO joints: create_joint fails cleanly (backend_error), destroy_joint
//     is a no-op.
//   - Spheres and boxes ONLY: a capsule fails create_body cleanly.
//   - It NEVER sleeps: idle heuristics are backend compute (CONTRACT.md 6)
//     and this backend computes "never", so is_sleeping is always false
//     (statics included, per the contract) and every wake rule is vacuous -
//     nothing can be woken from anything. thread_count is ignored (serial).
//
// Integration: semi-implicit Euler on the LIB's mass data (CONTRACT.md 4) -
// v += (g * gravity_scale + F * inv_m) dt, then w += R (I^-1) (R^T tau) dt
// (the world-frame torque through the stored local inverse - two basis
// turns, no per-tick matrix inverse) - then the COM-frame state moves and
// the origin's pose is reconstructed as origin = com_world - R_new *
// com_local. Rotation steps the Euler quaternion derivative
// dq = 1/2 * omega (x) q and renormalizes; Jolt instead composes the exact
// axis-angle map (Body.inl), so the two agree to third order per tick -
// the conformance suite's spin checks carry that band. Kinematics
// integrate their set velocity only (position-driven; no gravity, no
// forces). Forces/torques clear at the end of every tick (CONTRACT.md 7).
//
// Efficiency (fast without gaining an engine's complexity): the O(n^2)
// pair sweep keeps its lex-sorted nesting, but each body caches its world
// basis, per-part centers and world AABBs once per tick (statics once, at
// create/set_transform - they never move), so the sweep rejects pairs on
// cached AABBs before any shape math, and box frames turn through the
// cached basis (three dots, not a quaternion rotate). Integration skips
// work that cannot change the pose: no spin means no rotation step, a
// zero body-local COM skips the frame reconstruction (the common
// single-shape case), a zero torque skips the inertia turn. Queries
// reject parts on a precomputed bounding-sphere radius before the exact
// test - conservative, never changes a result. Every buffer is sized at
// construction, so the steady-state step never allocates (CONTRACT.md 11).
//
// Detection: brute force, every live pair, every part pair - keys arrive
// lexicographically sorted by construction (the nesting is a, pa, b, pb),
// so the touching diff is one two-pointer sweep and the feed order is
// deterministic. began feeds the witness geometry (zeros for sensor pairs
// - Jolt's sensor events carry no manifold), ended feeds zeros (Jolt's end
// events carry none), hit rides a fast NON-sensor began only (no solver
// means a pair can only impact when it begins). Pairs need >= 1 dynamic
// side, sensor-sensor pairs are skipped, and the filter rule is the lib's
// both-directions mask test.
//
// Destroying a body silences its pairs immediately (the keys are purged) -
// Jolt's observable behavior, and the lib's collector would drop a
// dead-slot event anyway. The key vectors hold 4 * max_bodies + 256 shape
// pairs; a pathological scene past that DROPS the lexicographically
// highest keys, one time warned - bounded and deterministic, like the
// engines' own contact caps.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <variant>
#include <vector>

#include <ws/core/log.hpp>
#include <ws/physics/errors.hpp>

#include "core/backend3d.hpp"

namespace ws::physics::detail {
namespace {

using physics3d::Aabb;
using physics3d::Box;
using physics3d::Sphere;
using physics3d::Transform;
using ws::math::Mat3;
using ws::math::Quat;
using ws::math::Vec3;

constexpr std::uint32_t kPartsPerSlot = ws::physics::kMaxPartsPerBody;

// --- small local math (ws::math has no conjugate, no 3x3 inverse) ------------

[[nodiscard]] inline Vec3 flip(Vec3 v) {
  return Vec3{-v.x, -v.y, -v.z};
}

[[nodiscard]] inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

[[nodiscard]] inline bool nonzero3(Vec3 v) {
  return v.x != 0.0f || v.y != 0.0f || v.z != 0.0f;
}

// The world images of the body-local basis (R's columns, orthonormal
// while the stored quaternion is unit - which every entry path keeps).
// Basis turns replace quaternion rotates below: to_local is three dots,
// to_world three scaled adds.
struct Basis {
  Vec3 x{1.0f, 0.0f, 0.0f};
  Vec3 y{0.0f, 1.0f, 0.0f};
  Vec3 z{0.0f, 0.0f, 1.0f};
};

[[nodiscard]] inline Basis basis_from_quat(const Quat& q) {
  const float xx = q.x * q.x;
  const float yy = q.y * q.y;
  const float zz = q.z * q.z;
  Basis u{};
  u.x = Vec3{1.0f - 2.0f * (yy + zz), 2.0f * (q.x * q.y + q.z * q.w),
             2.0f * (q.x * q.z - q.y * q.w)};
  u.y = Vec3{2.0f * (q.x * q.y - q.z * q.w), 1.0f - 2.0f * (xx + zz),
             2.0f * (q.y * q.z + q.x * q.w)};
  u.z = Vec3{2.0f * (q.x * q.z + q.y * q.w), 2.0f * (q.y * q.z - q.x * q.w),
             1.0f - 2.0f * (xx + yy)};
  return u;
}

[[nodiscard]] inline Vec3 to_local(const Basis& u, Vec3 v) {
  return Vec3{ws::math::dot(u.x, v), ws::math::dot(u.y, v),
              ws::math::dot(u.z, v)};
}

[[nodiscard]] inline Vec3 to_world(const Basis& u, Vec3 v) {
  return u.x * v.x + u.y * v.y + u.z * v.z;
}

// Inverse of a 3x3 matrix (adjugate / determinant). Column-major like
// ws::math::Mat3: element (row r, col c) is m[c][r]. Returns the zero
// matrix when singular - the caller treats that as "no angular
// response", mirroring the 2D twin's inv_inertia = 0 fallback.
[[nodiscard]] inline Mat3 mat3_inverse(const Mat3& m) {
  const float a = m[0][0], b = m[1][0], c = m[2][0];  // row 0: a b c
  const float d = m[0][1], e = m[1][1], f = m[2][1];  // row 1: d e f
  const float g = m[0][2], h = m[1][2], k = m[2][2];  // row 2: g h k
  const float c11 = e * k - f * h;   // cofactors of the row-major grid
  const float c12 = f * g - d * k;
  const float c13 = d * h - e * g;
  const float det = a * c11 + b * c12 + c * c13;
  if (std::abs(det) < 1e-12f) {
    return Mat3{0.0f};
  }
  const float s = 1.0f / det;
  Mat3 out{0.0f};
  out[0][0] = c11 * s;
  out[1][0] = (c * h - b * k) * s;
  out[2][0] = (b * f - c * e) * s;
  out[0][1] = c12 * s;
  out[1][1] = (a * k - c * g) * s;
  out[2][1] = (c * d - a * f) * s;
  out[0][2] = c13 * s;
  out[1][2] = (b * g - a * h) * s;
  out[2][2] = (a * e - b * d) * s;
  return out;
}

// --- shape-pair tests: fill the witness and a unit normal, or false ----------

// Normal points CA -> CB; witness is the midpoint of the overlap segment
// (purely geometric - engines report a solver manifold point, and this
// backend has no solver). Coincident centers pick +x. Squared-distance
// reject first: no sqrt for far pairs.
[[nodiscard]] inline bool sphere_sphere_touch(Vec3 ca, float ra, Vec3 cb,
                                              float rb, Vec3& point,
                                              Vec3& normal) {
  const Vec3 d = cb - ca;
  const float sum = ra + rb;
  const float d2 = ws::math::dot(d, d);
  if (d2 > sum * sum) {
    return false;
  }
  const float dist = std::sqrt(d2);
  const Vec3 n = dist > 0.0f ? d * (1.0f / dist) : Vec3{1.0f, 0.0f, 0.0f};
  normal = n;
  point = ca + n * ((dist + ra - rb) * 0.5f);
  return true;
}

// Normal points BOX -> SPHERE (the pushout direction); witness is the
// closest point on the box, or a point on the nearest face when the
// sphere center is inside. The caller flips when the sphere is pair-side a.
[[nodiscard]] inline bool sphere_box_touch(Vec3 bc, Vec3 h, const Basis& u,
                                           Vec3 sc, float r, Vec3& point,
                                           Vec3& normal) {
  // Sphere center in the box frame.
  const Vec3 lo = to_local(u, sc - bc);
  const Vec3 q{clampf(lo.x, -h.x, h.x), clampf(lo.y, -h.y, h.y),
               clampf(lo.z, -h.z, h.z)};
  Vec3 n_local{};
  if (q.x == lo.x && q.y == lo.y && q.z == lo.z) {
    // Center inside the box: push out through the nearest face.
    const float dx = h.x - std::abs(lo.x);
    const float dy = h.y - std::abs(lo.y);
    const float dz = h.z - std::abs(lo.z);
    if (dx <= dy && dx <= dz) {
      const float sx = lo.x >= 0.0f ? 1.0f : -1.0f;
      n_local = Vec3{sx, 0.0f, 0.0f};
      point = bc + to_world(u, Vec3{sx * h.x, lo.y, lo.z});
    } else if (dy <= dz) {
      const float sy = lo.y >= 0.0f ? 1.0f : -1.0f;
      n_local = Vec3{0.0f, sy, 0.0f};
      point = bc + to_world(u, Vec3{lo.x, sy * h.y, lo.z});
    } else {
      const float sz = lo.z >= 0.0f ? 1.0f : -1.0f;
      n_local = Vec3{0.0f, 0.0f, sz};
      point = bc + to_world(u, Vec3{lo.x, lo.y, sz * h.z});
    }
  } else {
    const Vec3 d = lo - q;
    const float dist = ws::math::length(d);
    if (dist > r) {
      return false;
    }
    n_local = dist > 0.0f ? d * (1.0f / dist) : Vec3{1.0f, 0.0f, 0.0f};
    point = bc + to_world(u, q);  // the closest point on the box
  }
  normal = to_world(u, n_local);
  return true;
}

// SAT over both boxes' face axes (6 candidates). On A's own face axis A's
// projected extent is exactly ha.k; on B's it is hb.k, and each box's
// half extents project onto the other's axes. The min-overlap axis is
// the separation normal, oriented A -> B (sign from B's center measured
// along it). Witness: B's support vertex along -normal - the corner
// deepest into A. First-minimum wins ties: A's x, y, z, then B's x, y,
// z - deterministic.
[[nodiscard]] inline bool box_box_touch(Vec3 ac, Vec3 ha, const Basis& ua,
                                        Vec3 bc, Vec3 hb, const Basis& ub,
                                        Vec3& point, Vec3& normal) {
  const Vec3 d = bc - ac;
  const Vec3 va[3] = {ub.x, ub.y, ub.z};
  // B's half extents projected onto one of A's axes.
  const auto b_extent_on = [&](const Vec3& axis) {
    return hb.x * std::abs(ws::math::dot(va[0], axis)) +
           hb.y * std::abs(ws::math::dot(va[1], axis)) +
           hb.z * std::abs(ws::math::dot(va[2], axis));
  };
  float best = 1e30f;
  Vec3 n_world{};
  const Vec3 a_axes[3] = {ua.x, ua.y, ua.z};
  const float ha_a[3] = {ha.x, ha.y, ha.z};
  for (int k = 0; k < 3; ++k) {
    const Vec3& axis = a_axes[k];
    const float o = (ha_a[k] + b_extent_on(axis)) -
                    std::abs(ws::math::dot(d, axis));
    if (o <= 0.0f) {
      return false;
    }
    if (o < best) {
      best = o;
      n_world = axis * (ws::math::dot(d, axis) >= 0.0f ? 1.0f : -1.0f);
    }
  }
  const Vec3 b_axes[3] = {ub.x, ub.y, ub.z};
  const float hb_a[3] = {hb.x, hb.y, hb.z};
  for (int k = 0; k < 3; ++k) {
    const Vec3& axis = b_axes[k];
    const float a_ext = ha.x * std::abs(ws::math::dot(ua.x, axis)) +
                        ha.y * std::abs(ws::math::dot(ua.y, axis)) +
                        ha.z * std::abs(ws::math::dot(ua.z, axis));
    const float o = (a_ext + hb_a[k]) - std::abs(ws::math::dot(d, axis));
    if (o <= 0.0f) {
      return false;
    }
    if (o < best) {
      best = o;
      n_world = axis * (ws::math::dot(d, axis) >= 0.0f ? 1.0f : -1.0f);
    }
  }
  normal = n_world;
  // B's support vertex along -normal.
  const Vec3 n_local_b = to_local(ub, normal);
  point = bc + to_world(ub, Vec3{n_local_b.x >= 0.0f ? -hb.x : hb.x,
                                n_local_b.y >= 0.0f ? -hb.y : hb.y,
                                n_local_b.z >= 0.0f ? -hb.z : hb.z});
  return true;
}

// OBB vs AABB for overlap_aabb: SAT over the six face axes (three per box).
[[nodiscard]] inline bool obb_aabb_overlap(Vec3 oc, Vec3 h, const Basis& u,
                                           Vec3 ac, Vec3 a) {
  const Vec3 d = oc - ac;
  const Vec3 ua[3] = {u.x, u.y, u.z};
  const float hs[3] = {h.x, h.y, h.z};
  for (int k = 0; k < 3; ++k) {
    // The OBB's own axis k: its extent along it is h[k], the AABB's is
    // its half extents projected.
    const Vec3& ax = ua[k];
    const float r_a = a.x * std::abs(ax.x) + a.y * std::abs(ax.y) +
                      a.z * std::abs(ax.z);
    if (std::abs(ws::math::dot(d, ax)) > r_a + hs[k]) {
      return false;
    }
  }
  // The AABB's own axes: symmetric, per world component.
  const float r_o_x = h.x * std::abs(u.x.x) + h.y * std::abs(u.y.x) +
                      h.z * std::abs(u.z.x);
  if (std::abs(d.x) > a.x + r_o_x) {
    return false;
  }
  const float r_o_y = h.x * std::abs(u.x.y) + h.y * std::abs(u.y.y) +
                      h.z * std::abs(u.z.y);
  if (std::abs(d.y) > a.y + r_o_y) {
    return false;
  }
  const float r_o_z = h.x * std::abs(u.x.z) + h.y * std::abs(u.y.z) +
                      h.z * std::abs(u.z.z);
  if (std::abs(d.z) > a.z + r_o_z) {
    return false;
  }
  return true;
}

// --- ray casts (unit direction): an ENTERING ray only - an origin inside or
// on the shape is a miss, both shapes. t is the entry distance.

[[nodiscard]] inline bool ray_sphere(Vec3 o, Vec3 d, Vec3 c, float r,
                                     float& t_out) {
  const Vec3 oc = o - c;
  const float cc = ws::math::dot(oc, oc) - r * r;
  if (cc <= 0.0f) {
    return false;  // origin inside (or on) the sphere
  }
  const float b = ws::math::dot(oc, d);
  const float disc = b * b - cc;
  if (disc < 0.0f) {
    return false;
  }
  const float t = -b - std::sqrt(disc);
  if (t <= 0.0f) {
    return false;
  }
  t_out = t;
  return true;
}

[[nodiscard]] inline bool ray_box(Vec3 o, Vec3 d, Vec3 bc, Vec3 h,
                                  const Basis& u, float& t_out,
                                  Vec3& normal) {
  const Vec3 lo = to_local(u, o - bc);
  const Vec3 ld = to_local(u, d);
  const float lo_a[3] = {lo.x, lo.y, lo.z};
  const float ld_a[3] = {ld.x, ld.y, ld.z};
  const float h_a[3] = {h.x, h.y, h.z};
  float tmin = 0.0f;
  float tmax = 1e30f;
  int axis = -1;
  float sign = 0.0f;
  for (int k = 0; k < 3; ++k) {
    if (std::abs(ld_a[k]) < 1e-12f) {
      if (lo_a[k] < -h_a[k] || lo_a[k] > h_a[k]) {
        return false;  // parallel and outside this slab
      }
      continue;
    }
    // t1 crosses the -h face, t2 the +h face; the swap flag remembers
    // which face t1 (the entry) belongs to.
    float t1 = (-h_a[k] - lo_a[k]) / ld_a[k];
    float t2 = (h_a[k] - lo_a[k]) / ld_a[k];
    float s = -1.0f;
    if (t1 > t2) {
      const float tmp = t1;
      t1 = t2;
      t2 = tmp;
      s = 1.0f;
    }
    if (t1 > tmin) {
      tmin = t1;
      axis = k;
      sign = s;
    }
    if (t2 < tmax) {
      tmax = t2;
    }
    if (tmin > tmax) {
      return false;
    }
  }
  if (axis < 0 || tmin <= 0.0f) {
    return false;  // origin inside (or on the surface), or fully behind
  }
  t_out = tmin;
  // The entry face's outward normal, back toward the ray origin.
  Vec3 n_local{};
  if (axis == 0) {
    n_local = Vec3{sign, 0.0f, 0.0f};
  } else if (axis == 1) {
    n_local = Vec3{0.0f, sign, 0.0f};
  } else {
    n_local = Vec3{0.0f, 0.0f, sign};
  }
  normal = to_world(u, n_local);
  return true;
}

// Conservative reject for the queries: is any point of the segment
// origin + [0, max_distance] * d within `bound` of c? False rejects
// nothing the exact test would accept (the bound encloses the shape).
[[nodiscard]] inline bool ray_near_sphere(Vec3 origin, Vec3 d,
                                          float max_distance, Vec3 c,
                                          float bound) {
  const Vec3 to_c = c - origin;
  const float t_ca = ws::math::dot(to_c, d);
  const float t = clampf(t_ca, 0.0f, max_distance);
  const Vec3 miss = d * t - to_c;  // closest point - c (sign irrelevant)
  return ws::math::dot(miss, miss) <= bound * bound;
}

// --- the backend ----------------------------------------------------------------

// One shape of a body in body-local space: spheres and boxes only (a part
// or body-level capsule fails create_body).
struct PartGeo {
  bool sphere = true;
  float radius = 0.5f;  // sphere
  Vec3 half{};          // box
  Vec3 offset{};        // body-local translation (no per-part rotation in v2)
};

// One touching shape pair this tick: the diff key (a, pa, b, pb) plus the
// geometry the began/hit events carry. Keys are produced in lexicographic
// order (the detection loop nests a, pa, b, pb), so the per-tick vectors
// are sorted by construction and the diff is one two-pointer sweep.
struct Touch {
  std::uint32_t a = 0;
  std::uint8_t pa = 0;
  std::uint32_t b = 0;
  std::uint8_t pb = 0;
  Vec3 point{};        // witness, world space
  Vec3 normal{};       // unit, a -> b
  float speed = 0.0f;  // approach speed at the witness
};

[[nodiscard]] inline int key_cmp(const Touch& l, const Touch& r) {
  if (l.a != r.a) {
    return l.a < r.a ? -1 : 1;
  }
  if (l.pa != r.pa) {
    return l.pa < r.pa ? -1 : 1;
  }
  if (l.b != r.b) {
    return l.b < r.b ? -1 : 1;
  }
  if (l.pb != r.pb) {
    return l.pb < r.pb ? -1 : 1;
  }
  return 0;
}

class ReferenceWorld3D final : public IWorld3DBackend {
 public:
  explicit ReferenceWorld3D(const BackendDesc3D& desc)
      : capacity_(desc.max_bodies) {
    gravity_ = desc.gravity;
    hit_threshold_ = desc.hit_speed_threshold;
    kinds_.assign(capacity_, ws::physics::BodyKind::static_);
    live_.assign(capacity_, 0);
    sensor_.assign(capacity_, 0);
    inv_mass_.assign(capacity_, 0.0f);
    inv_inertia_.assign(capacity_, Mat3{0.0f});
    gravity_scale_.assign(capacity_, 1.0f);
    pos_.assign(capacity_, Vec3{});
    rot_.assign(capacity_, Quat{});
    vel_.assign(capacity_, Vec3{});
    ang_.assign(capacity_, Vec3{});
    com_.assign(capacity_, Vec3{});
    force_.assign(capacity_, Vec3{});
    torque_.assign(capacity_, Vec3{});
    layer_.assign(capacity_, 1u);
    mask_.assign(capacity_, 0xffffffffu);
    bullet_.assign(capacity_, 0);
    part_count_.assign(capacity_, 0);
    parts_.assign(std::size_t{capacity_} * kPartsPerSlot, PartGeo{});
    basis_.assign(capacity_, Basis{});
    part_center_.assign(std::size_t{capacity_} * kPartsPerSlot, Vec3{});
    aabb_min_.assign(std::size_t{capacity_} * kPartsPerSlot, Vec3{});
    aabb_max_.assign(std::size_t{capacity_} * kPartsPerSlot, Vec3{});
    bound_.assign(std::size_t{capacity_} * kPartsPerSlot, 0.0f);
    body_min_.assign(capacity_, Vec3{});
    body_max_.assign(capacity_, Vec3{});
    // Both key vectors pre-reserved: the steady-state step never allocates
    // (CONTRACT.md 11). The bound is generous but finite - see the header.
    const std::size_t touch_cap = std::size_t{4} * capacity_ + 256;
    prev_.reserve(touch_cap);
    now_.reserve(touch_cap);
  }

  // --- bodies ------------------------------------------------------------------

  [[nodiscard]] ws::core::Status create_body(
      std::uint32_t slot, const physics3d::BodyDesc& desc,
      const MassData3D& mass) override {
    if (slot >= capacity_) {
      return fail(PhysicsError::backend_error, "body slot out of range");
    }
    const std::uint32_t count =
        desc.part_count == 0 ? 1u : desc.part_count;
    if (count > kPartsPerSlot) {
      return fail(PhysicsError::backend_error, "part count out of range");
    }
    for (std::uint32_t p = 0; p < count; ++p) {
      const physics3d::Shape& shape =
          desc.part_count == 0 ? desc.shape : desc.parts[p].shape;
      const Vec3 offset =
          desc.part_count == 0 ? Vec3{} : desc.parts[p].offset;
      PartGeo& g = parts_[std::size_t{slot} * kPartsPerSlot + p];
      if (const auto* sphere = std::get_if<Sphere>(&shape)) {
        g.sphere = true;
        g.radius = sphere->radius;
        g.half = Vec3{};
        g.offset = offset;
      } else if (const auto* box = std::get_if<Box>(&shape)) {
        g.sphere = false;
        g.radius = 0.0f;
        g.half = box->half_extents;
        g.offset = offset;
      } else {
        return fail(PhysicsError::backend_error,
                    "reference 3D backend: capsules are not implemented "
                    "(spheres and boxes only)");
      }
      bound_[std::size_t{slot} * kPartsPerSlot + p] =
          g.sphere ? g.radius : ws::math::length(g.half);
    }
    kinds_[slot] = desc.kind;
    live_[slot] = 1;
    sensor_[slot] = desc.is_sensor ? 1 : 0;
    layer_[slot] = desc.collision_layer;
    mask_[slot] = desc.collision_mask;
    bullet_[slot] = desc.is_bullet ? 1 : 0;
    gravity_scale_[slot] = desc.gravity_scale;
    pos_[slot] = desc.transform.position;
    // The raw seam can hand us an unnormalized rotation (the machinery
    // sanitizes; a direct backend test does not) - the basis and rotate()
    // need unit quaternions, so normalize at the door. A zero quaternion
    // (a broken caller) becomes the identity rather than NaNs.
    rot_[slot] = normalized_or_identity(desc.transform.rotation);
    vel_[slot] = desc.linear_velocity;
    ang_[slot] = desc.angular_velocity;
    force_[slot] = Vec3{};
    torque_[slot] = Vec3{};
    part_count_[slot] = static_cast<std::uint8_t>(count);
    // The LIB's mass data, applied as given (CONTRACT.md 4) - meaningful
    // for dynamics only; statics/kinematics carry zeros that must NOT be
    // applied (the seam's rule, matched to what WorldCore passes).
    if (desc.kind == ws::physics::BodyKind::dynamic) {
      inv_mass_[slot] = mass.mass > 0.0f ? 1.0f / mass.mass : 0.0f;
      inv_inertia_[slot] = mat3_inverse(mass.inertia);
      com_[slot] = mass.center;
    } else {
      inv_mass_[slot] = 0.0f;
      inv_inertia_[slot] = Mat3{0.0f};
      com_[slot] = Vec3{};
    }
    cache_body(slot);
    return {};
  }

  void destroy_body(std::uint32_t slot) override {
    if (slot >= capacity_ || !live_[slot]) {
      return;
    }
    live_[slot] = 0;
    // Silence the destroyed body's pairs right away (Jolt's observable
    // behavior): no began/ended ever names a dead slot, and a reused slot
    // cannot inherit a stale key. Erase-in-place, allocation-free.
    const auto mentions = [slot](const Touch& t) {
      return t.a == slot || t.b == slot;
    };
    std::erase_if(prev_, mentions);
    std::erase_if(now_, mentions);
  }

  void set_transform(std::uint32_t slot, const Transform& t) override {
    pos_[slot] = t.position;
    rot_[slot] = normalized_or_identity(t.rotation);
    cache_body(slot);
  }

  void set_velocity(std::uint32_t slot, Vec3 linear, Vec3 angular) override {
    vel_[slot] = linear;
    ang_[slot] = angular;
  }

  void set_filter(std::uint32_t slot, std::uint32_t layer,
                  std::uint32_t mask) override {
    layer_[slot] = layer;
    mask_[slot] = mask;
  }

  void set_bullet(std::uint32_t slot, bool enabled) override {
    // Stored, never read: no CCD in the reference (documented subset).
    bullet_[slot] = enabled ? 1 : 0;
  }

  // The wake RULES are the lib's (CONTRACT.md 6) and hold vacuously here:
  // nothing ever falls asleep, so wake() and set_sleepable() have nothing
  // to do - and is_sleeping is false for everything, statics included.
  void set_sleepable(std::uint32_t, bool) override {}
  void wake(std::uint32_t) override {}
  [[nodiscard]] bool is_sleeping(std::uint32_t) const override {
    return false;
  }

  void set_gravity(Vec3 gravity) override {
    // The seam asks backends to wake every live body; nothing sleeps here,
    // so storing is the whole job.
    gravity_ = gravity;
  }

  // Forces/impulses reach DYNAMICS only: statics and kinematics carry no
  // mass semantics (CONTRACT.md 4), and the engines no-op them the same
  // way (infinite mass).
  void apply_linear_impulse(std::uint32_t slot, Vec3 impulse) override {
    if (kinds_[slot] == ws::physics::BodyKind::dynamic) {
      vel_[slot] = vel_[slot] + impulse * inv_mass_[slot];
    }
  }

  void apply_angular_impulse(std::uint32_t slot, Vec3 impulse) override {
    if (kinds_[slot] == ws::physics::BodyKind::dynamic) {
      // World-frame impulse through the stored local inverse, like the
      // per-tick torque turn: dw = R (I^-1 (R^T J)).
      const Vec3 local = to_local(basis_[slot], impulse);
      ang_[slot] =
          ang_[slot] + to_world(basis_[slot], inv_inertia_[slot] * local);
    }
  }

  void apply_force(std::uint32_t slot, Vec3 force) override {
    if (kinds_[slot] == ws::physics::BodyKind::dynamic) {
      force_[slot] = force_[slot] + force;
    }
  }

  void apply_torque(std::uint32_t slot, Vec3 torque) override {
    if (kinds_[slot] == ws::physics::BodyKind::dynamic) {
      torque_[slot] = torque_[slot] + torque;
    }
  }

  [[nodiscard]] BackendVelocity3D get_velocity(
      std::uint32_t slot) const override {
    return BackendVelocity3D{vel_[slot], ang_[slot]};
  }

  // --- joints ------------------------------------------------------------------

  [[nodiscard]] ws::core::Status create_joint(
      std::uint32_t, const BackendJoint3D&) override {
    return fail(PhysicsError::backend_error,
                "reference 3D backend: joints are not implemented");
  }

  void destroy_joint(std::uint32_t) override {}

  // --- one tick ----------------------------------------------------------------

  std::size_t step(float dt, IEventCollector3D& collector,
                   std::span<BackendMove3D> moves_out) override {
    // Integrate, then report every simulated body. Nothing sleeps, so
    // "every body the backend simulated" is every live non-static body,
    // in slot order (the lib folds these into its pose mirror). Zero
    // states skip work that cannot change the pose (the header): most
    // bodies carry no spin, no torque and a zero body-local COM.
    std::size_t count = 0;
    for (std::uint32_t s = 0; s < capacity_; ++s) {
      if (!live_[s]) {
        continue;
      }
      const auto kind = kinds_[s];
      if (kind == ws::physics::BodyKind::static_) {
        continue;  // statics never move (their caches stay create-fresh)
      }
      if (kind == ws::physics::BodyKind::dynamic) {
        vel_[s] = vel_[s] + (gravity_ * gravity_scale_[s] +
                             force_[s] * inv_mass_[s]) * dt;
        if (nonzero3(torque_[s])) {
          // World-frame torque about the COM through the stored local
          // inverse: w += R (I^-1 (R^T tau)) dt. The cached basis still
          // holds the START-of-tick rotation, which is the pose the
          // torque arrives in.
          const Vec3 local = to_local(basis_[s], torque_[s]);
          ang_[s] =
              ang_[s] + to_world(basis_[s], inv_inertia_[s] * local) * dt;
        }
        force_[s] = Vec3{};  // cleared at the end of every tick (CONTRACT 7)
        torque_[s] = Vec3{};
      }
      // Kinematics integrate their set velocity only (position-driven).
      // The state is COM-frame (CONTRACT.md 4): move the COM, then
      // reconstruct the origin's pose under the END-of-tick rotation.
      const Vec3 com_local = com_[s];
      const Vec3 com_world = nonzero3(com_local)
                                 ? pos_[s] + to_world(basis_[s], com_local)
                                 : pos_[s];
      const Vec3 moved = com_world + vel_[s] * dt;
      if (nonzero3(ang_[s])) {
        // The Euler quaternion step, renormalized: rotate() and the basis
        // need unit quaternions (Jolt composes the exact axis-angle map
        // instead - see the header for the divergence order).
        const Quat spin{ang_[s].x * 0.5f * dt, ang_[s].y * 0.5f * dt,
                        ang_[s].z * 0.5f * dt, 0.0f};
        const Quat dq = spin * rot_[s];
        rot_[s] = ws::math::normalize(
            Quat{rot_[s].x + dq.x, rot_[s].y + dq.y, rot_[s].z + dq.z,
                 rot_[s].w + dq.w});
      }
      pos_[s] = nonzero3(com_local)
                    ? moved - to_world(basis_from_quat(rot_[s]), com_local)
                    : moved;
      cache_body(s);
      if (count < moves_out.size()) {
        moves_out[count++] = BackendMove3D{s, Transform{pos_[s], rot_[s]}};
      }
    }

    // Detect: every live pair, every part pair, keys in lexicographic
    // order (a, pa, b, pb) so the diff below is a two-pointer sweep. The
    // pair-level tests re-run per part pair - harmless at reference scale
    // - and the cached AABBs reject far pairs before any shape math.
    now_.clear();
    for (std::uint32_t a = 0; a < capacity_; ++a) {
      if (!live_[a]) {
        continue;
      }
      for (std::uint8_t pa = 0; pa < part_count_[a]; ++pa) {
        for (std::uint32_t b = a + 1; b < capacity_; ++b) {
          if (!live_[b]) {
            continue;
          }
          if (sensor_[a] && sensor_[b]) {
            continue;  // sensor-sensor pairs never report
          }
          if (kinds_[a] != ws::physics::BodyKind::dynamic &&
              kinds_[b] != ws::physics::BodyKind::dynamic) {
            continue;  // a simulated side is required
          }
          if ((layer_[a] & mask_[b]) == 0 || (layer_[b] & mask_[a]) == 0) {
            continue;  // the lib's both-directions filter rule
          }
          if (body_max_[a].x < body_min_[b].x ||
              body_max_[b].x < body_min_[a].x ||
              body_max_[a].y < body_min_[b].y ||
              body_max_[b].y < body_min_[a].y ||
              body_max_[a].z < body_min_[b].z ||
              body_max_[b].z < body_min_[a].z) {
            continue;  // union-AABB broadphase (cached this tick)
          }
          const std::size_t ia = std::size_t{a} * kPartsPerSlot + pa;
          for (std::uint8_t pb = 0; pb < part_count_[b]; ++pb) {
            const std::size_t ib = std::size_t{b} * kPartsPerSlot + pb;
            if (aabb_max_[ia].x < aabb_min_[ib].x ||
                aabb_max_[ib].x < aabb_min_[ia].x ||
                aabb_max_[ia].y < aabb_min_[ib].y ||
                aabb_max_[ib].y < aabb_min_[ia].y ||
                aabb_max_[ia].z < aabb_min_[ib].z ||
                aabb_max_[ib].z < aabb_min_[ia].z) {
              continue;  // per-part AABB broadphase
            }
            Vec3 point{};
            Vec3 normal{};
            if (!part_pair(a, pa, b, pb, point, normal)) {
              continue;
            }
            push_touch(a, pa, b, pb, point, normal,
                       approach_speed(a, b, point, normal));
          }
        }
      }
    }

    // Diff prev_ vs now_ - both sorted by key. began feeds the witness
    // geometry (zeros for sensor pairs: sensor events carry no manifold),
    // ended always feeds zeros (engines' end events carry none), and hit
    // rides a fast non-sensor began only (no solver: a pair can only
    // impact when it begins). 0 threshold disables hits, the public Desc
    // promise.
    std::size_t i = 0;
    std::size_t j = 0;
    const auto feed_began = [&](const Touch& t) {
      const bool solid = !(sensor_[t.a] || sensor_[t.b]);
      BackendEvent3D ev{};
      ev.slot_a = t.a;
      ev.slot_b = t.b;
      ev.part_a = t.pa;
      ev.part_b = t.pb;
      ev.phase = physics3d::ContactPhase::began;
      if (solid) {
        ev.point = t.point;
        ev.normal = t.normal;
      }
      collector.add(ev);
      if (solid && hit_threshold_ > 0.0f && t.speed > hit_threshold_) {
        BackendEvent3D hit = ev;
        hit.phase = physics3d::ContactPhase::hit;
        hit.speed = t.speed;
        collector.add(hit);
      }
    };
    const auto feed_ended = [&](const Touch& t) {
      BackendEvent3D ev{};
      ev.slot_a = t.a;
      ev.slot_b = t.b;
      ev.part_a = t.pa;
      ev.part_b = t.pb;
      ev.phase = physics3d::ContactPhase::ended;
      collector.add(ev);
    };
    while (i < prev_.size() && j < now_.size()) {
      const int c = key_cmp(prev_[i], now_[j]);
      if (c < 0) {
        feed_ended(prev_[i++]);
      } else if (c > 0) {
        feed_began(now_[j++]);
      } else {
        ++i;  // persisted: touching both ticks, no event
        ++j;
      }
    }
    while (i < prev_.size()) {
      feed_ended(prev_[i++]);
    }
    while (j < now_.size()) {
      feed_began(now_[j++]);
    }
    prev_.swap(now_);  // now_ (the old prev_) is cleared at the next detect
    return count;
  }

  // --- queries (allocating by design, per the seam) ----------------------------

  [[nodiscard]] std::optional<BackendRayHit3D> raycast(
      Vec3 origin, Vec3 direction, float max_distance) const override {
    const float dl = ws::math::length(direction);
    if (dl == 0.0f) {
      return std::nullopt;  // the seam promises non-zero; defensive
    }
    const Vec3 d = direction * (1.0f / dl);
    float best_t = max_distance;
    bool found = false;
    std::optional<BackendRayHit3D> best;
    for (std::uint32_t s = 0; s < capacity_; ++s) {
      if (!live_[s]) {
        continue;
      }
      for (std::uint8_t p = 0; p < part_count_[s]; ++p) {
        const std::size_t ip = std::size_t{s} * kPartsPerSlot + p;
        const PartGeo& g = parts_[ip];
        const Vec3 c = part_center_[ip];
        if (!ray_near_sphere(origin, d, max_distance, c, bound_[ip])) {
          continue;  // conservative bounding reject
        }
        float t = 0.0f;
        Vec3 point{};
        Vec3 normal{};
        if (g.sphere) {
          if (!ray_sphere(origin, d, c, g.radius, t)) {
            continue;
          }
          point = origin + d * t;
          normal = (point - c) * (1.0f / g.radius);
        } else {
          if (!ray_box(origin, d, c, g.half, basis_[s], t, normal)) {
            continue;
          }
          point = origin + d * t;
        }
        // Closest hit wins; ties keep the LOWEST slot, then part index
        // (strict <, ascending iteration). t <= max_distance, per the
        // public [0, max_distance] promise - the cap gate runs before the
        // best-so-far test, so even a lone first candidate beyond it is
        // rejected.
        if (t > max_distance) {
          continue;
        }
        if (!found || t < best_t) {
          found = true;
          best_t = t;
          best = BackendRayHit3D{s, t, point, normal};
        }
      }
    }
    return best;
  }

  void overlap_sphere(Vec3 center, float radius,
                      std::vector<std::uint32_t>& out) const override {
    for (std::uint32_t s = 0; s < capacity_; ++s) {
      if (!live_[s]) {
        continue;
      }
      bool hit = false;
      for (std::uint8_t p = 0; p < part_count_[s] && !hit; ++p) {
        const std::size_t ip = std::size_t{s} * kPartsPerSlot + p;
        const PartGeo& g = parts_[ip];
        const Vec3 c = part_center_[ip];
        const Vec3 dc = center - c;
        const float d2 = ws::math::dot(dc, dc);
        const float reach = radius + bound_[ip];
        if (d2 > reach * reach) {
          continue;  // conservative bounding reject
        }
        if (g.sphere) {
          const float sum = radius + g.radius;
          hit = d2 <= sum * sum;
        } else {
          // Query sphere vs oriented box: clamp the query center to the
          // box in its own frame.
          const Vec3 lo = to_local(basis_[s], dc);
          const Vec3 q{clampf(lo.x, -g.half.x, g.half.x),
                       clampf(lo.y, -g.half.y, g.half.y),
                       clampf(lo.z, -g.half.z, g.half.z)};
          hit = ws::math::distance(lo, q) <= radius;
        }
      }
      if (hit) {
        out.push_back(s);
      }
    }
  }

  void overlap_aabb(const Aabb& box,
                    std::vector<std::uint32_t>& out) const override {
    const Vec3 ac = (box.min + box.max) * 0.5f;
    const Vec3 a = (box.max - box.min) * 0.5f;
    for (std::uint32_t s = 0; s < capacity_; ++s) {
      if (!live_[s]) {
        continue;
      }
      bool hit = false;
      for (std::uint8_t p = 0; p < part_count_[s] && !hit; ++p) {
        const std::size_t ip = std::size_t{s} * kPartsPerSlot + p;
        const PartGeo& g = parts_[ip];
        const Vec3 c = part_center_[ip];
        if (aabb_max_[ip].x < box.min.x || box.max.x < aabb_min_[ip].x ||
            aabb_max_[ip].y < box.min.y || box.max.y < aabb_min_[ip].y ||
            aabb_max_[ip].z < box.min.z || box.max.z < aabb_min_[ip].z) {
          continue;  // cached world AABB vs the query box
        }
        if (g.sphere) {
          const Vec3 q{clampf(c.x, box.min.x, box.max.x),
                       clampf(c.y, box.min.y, box.max.y),
                       clampf(c.z, box.min.z, box.max.z)};
          hit = ws::math::distance(c, q) <= g.radius;
        } else {
          hit = obb_aabb_overlap(c, g.half, basis_[s], ac, a);
        }
      }
      if (hit) {
        out.push_back(s);
      }
    }
  }

 private:
  [[nodiscard]] static Quat normalized_or_identity(const Quat& q) {
    const float ls =
        q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    return ls > 0.0f ? ws::math::normalize(q) : Quat{};
  }

  // Refresh the per-body caches read by detection and the queries: the
  // world basis, per-part world centers, per-part world AABBs and the
  // union AABB. Runs at create, at set_transform and after each tick's
  // integration for every live non-static - statics never move, so their
  // create-time cache stays fresh for their whole life (set_transform
  // refreshes it on demand).
  void cache_body(std::uint32_t slot) {
    const Basis u = basis_from_quat(rot_[slot]);
    basis_[slot] = u;
    Vec3 bmin{1e30f, 1e30f, 1e30f};
    Vec3 bmax{-1e30f, -1e30f, -1e30f};
    for (std::uint8_t p = 0; p < part_count_[slot]; ++p) {
      const std::size_t ip = std::size_t{slot} * kPartsPerSlot + p;
      const PartGeo& g = parts_[ip];
      const Vec3 c = pos_[slot] + to_world(u, g.offset);
      Vec3 ext{};
      if (g.sphere) {
        ext = Vec3{g.radius, g.radius, g.radius};
      } else {
        ext = Vec3{std::abs(u.x.x) * g.half.x +
                       std::abs(u.y.x) * g.half.y +
                       std::abs(u.z.x) * g.half.z,
                   std::abs(u.x.y) * g.half.x +
                       std::abs(u.y.y) * g.half.y +
                       std::abs(u.z.y) * g.half.z,
                   std::abs(u.x.z) * g.half.x +
                       std::abs(u.y.z) * g.half.y +
                       std::abs(u.z.z) * g.half.z};
      }
      const Vec3 lo = c - ext;
      const Vec3 hi = c + ext;
      part_center_[ip] = c;
      aabb_min_[ip] = lo;
      aabb_max_[ip] = hi;
      bmin = Vec3{std::min(bmin.x, lo.x), std::min(bmin.y, lo.y),
                  std::min(bmin.z, lo.z)};
      bmax = Vec3{std::max(bmax.x, hi.x), std::max(bmax.y, hi.y),
                  std::max(bmax.z, hi.z)};
    }
    body_min_[slot] = bmin;
    body_max_[slot] = bmax;
  }

  [[nodiscard]] Vec3 part_center(std::uint32_t slot, std::uint8_t p) const {
    return part_center_[std::size_t{slot} * kPartsPerSlot + p];
  }

  // One shape-pair test in the a < b convention: fills the witness and the
  // a -> b unit normal, or returns false when apart.
  [[nodiscard]] bool part_pair(std::uint32_t a, std::uint8_t pa,
                               std::uint32_t b, std::uint8_t pb,
                               Vec3& point, Vec3& normal) const {
    const PartGeo& ga = parts_[std::size_t{a} * kPartsPerSlot + pa];
    const PartGeo& gb = parts_[std::size_t{b} * kPartsPerSlot + pb];
    const Vec3 ca = part_center(a, pa);
    const Vec3 cb = part_center(b, pb);
    if (ga.sphere && gb.sphere) {
      return sphere_sphere_touch(ca, ga.radius, cb, gb.radius, point,
                                 normal);
    }
    if (!ga.sphere && !gb.sphere) {
      return box_box_touch(ca, ga.half, basis_[a], cb, gb.half, basis_[b],
                           point, normal);
    }
    if (ga.sphere) {
      // a is the sphere, b the box: the helper speaks box -> sphere (b -> a).
      if (!sphere_box_touch(cb, gb.half, basis_[b], ca, ga.radius, point,
                            normal)) {
        return false;
      }
      normal = flip(normal);
      return true;
    }
    // a is the box, b the sphere: box -> sphere IS a -> b.
    return sphere_box_touch(ca, ga.half, basis_[a], cb, gb.radius, point,
                            normal);
  }

  // How fast the pair closes along the a -> b normal, measured at the
  // witness with the post-integration velocities (there is no solver to
  // change them). Kinematics carry their set velocity like anyone else;
  // only the COM frame matters (CONTRACT.md 4).
  [[nodiscard]] float approach_speed(std::uint32_t a, std::uint32_t b,
                                     Vec3 point, Vec3 normal) const {
    const Vec3 ra = point - (pos_[a] + to_world(basis_[a], com_[a]));
    const Vec3 rb = point - (pos_[b] + to_world(basis_[b], com_[b]));
    const Vec3 rel = (vel_[b] + ws::math::cross(ang_[b], rb)) -
                     (vel_[a] + ws::math::cross(ang_[a], ra));
    const float closing = -ws::math::dot(rel, normal);
    return closing > 0.0f ? closing : 0.0f;
  }

  void push_touch(std::uint32_t a, std::uint8_t pa, std::uint32_t b,
                  std::uint8_t pb, Vec3 point, Vec3 normal, float speed) {
    if (now_.size() >= now_.capacity()) {
      // Bounded, deterministic tail drop (keys arrive sorted, so the
      // highest go) - see the header.
      if (!touch_overflow_warned_) {
        touch_overflow_warned_ = true;
        ws::core::warn("physics: reference 3D backend touch-key budget "
                       "spent; dropping the highest pairs");
      }
      return;
    }
    now_.push_back(Touch{a, pa, b, pb, point, normal, speed});
  }

  std::uint32_t capacity_ = 0;
  Vec3 gravity_{};
  float hit_threshold_ = 0.0f;

  // Slot-indexed state, pre-sized at construction (the slot table is the
  // lib's; this backend trusts the slots it is given).
  std::vector<ws::physics::BodyKind> kinds_;
  std::vector<char> live_;
  std::vector<char> sensor_;
  std::vector<char> bullet_;  // stored, never read: no CCD
  std::vector<float> inv_mass_;
  std::vector<Mat3> inv_inertia_;  // local-frame inverse, dynamics only
  std::vector<float> gravity_scale_;
  std::vector<Vec3> pos_;  // origin-frame (what the lib mirrors)
  std::vector<Quat> rot_;
  std::vector<Vec3> vel_;  // COM-frame
  std::vector<Vec3> ang_;
  std::vector<Vec3> com_;    // body-local, dynamics only
  std::vector<Vec3> force_;  // accumulation, cleared per tick
  std::vector<Vec3> torque_;
  std::vector<std::uint32_t> layer_;
  std::vector<std::uint32_t> mask_;
  std::vector<std::uint8_t> part_count_;
  std::vector<PartGeo> parts_;  // capacity_ x kPartsPerSlot, flat

  // Per-tick caches (detection and the queries read these; refreshed at
  // create, set_transform and after each tick's integration - statics
  // once, they never move).
  std::vector<Basis> basis_;
  std::vector<Vec3> part_center_;             // world-space, per part
  std::vector<Vec3> aabb_min_, aabb_max_;     // per part, world-space
  std::vector<Vec3> body_min_, body_max_;     // union over the parts
  std::vector<float> bound_;  // per part, bounding-sphere radius (create)

  std::vector<Touch> prev_;  // last tick's touches
  std::vector<Touch> now_;   // this tick's, sorted by construction
  bool touch_overflow_warned_ = false;
};

}  // namespace

std::unique_ptr<IWorld3DBackend> make_reference_world_3d(
    const BackendDesc3D& desc) {
  try {
    return std::make_unique<ReferenceWorld3D>(desc);
  } catch (const std::bad_alloc&) {
    ws::core::warn("physics: reference 3D backend out of memory");
    return nullptr;
  } catch (const std::exception& e) {
    ws::core::warn("physics: reference 3D backend failed to start: {}",
                   e.what());
    return nullptr;
  }
}

}  // namespace ws::physics::detail
