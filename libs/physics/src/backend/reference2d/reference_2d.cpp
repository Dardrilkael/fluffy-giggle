// The reference 2D backend behind the same seam as Box2D
// (core/backend2d.hpp): CONTRACT.md computed by the simplest possible code,
// no engine underneath. It exists so the conformance suite (C7) can run the
// lib's machinery over a second implementation and so tests can pin
// semantics with hand-checkable numbers - every trajectory below is plain
// kinematics a reader can recompute.
//
// It is an honest SUBSET, not a toy solver:
//   - NO contact response: overlapping pairs pass through each other.
//     Detection, events, ballistics and queries are contract-true; resting
//     contact is not (there is no solver to produce it).
//   - NO CCD: is_bullet is stored and never read - a fast body can tunnel
//     by construction.
//   - NO joints: create_joint fails cleanly (backend_error), destroy_joint
//     is a no-op.
//   - Circles and boxes ONLY: a capsule fails create_body cleanly.
//   - It NEVER sleeps: idle heuristics are backend compute (CONTRACT.md 6)
//     and this backend computes "never", so is_sleeping is always false
//     (statics included, per the contract) and every wake rule is vacuous -
//     nothing can be woken from anything. thread_count is ignored (serial).
//
// Integration: semi-implicit Euler on the LIB's mass data (CONTRACT.md 4) -
// v += (g * gravity_scale + F * inv_m) dt, w += tau * inv_I dt - then the
// COM-frame state moves and the origin's pose is reconstructed as
// origin = com_world - R(theta) * com_local. Kinematics integrate their set
// velocity only (position-driven; no gravity, no forces). Forces/torques
// clear at the end of every tick (CONTRACT.md 7).
//
// Detection: brute force, every live pair, every part pair, O(n^2) - the
// point is legibility, not throughput. Touching state is a sorted vector of
// shape-pair keys diffed against the previous tick: began feeds the witness
// geometry (zeros for sensor pairs - Box2D's sensor events carry no
// manifold), ended feeds zeros (Box2D's end events carry none), hit rides a
// fast NON-sensor began only (no solver means a pair can only impact when
// it begins). Feed order is the sorted key order, deterministic by
// construction. Pairs need >= 1 dynamic side (a world of statics and
// kinematics never self-collides here), sensor-sensor pairs are skipped,
// and the filter rule is the lib's both-directions mask test.
//
// Destroying a body silences its pairs immediately (the keys are purged) -
// Box2D's observable behavior, and the lib's collector would drop a
// dead-slot event anyway (its guard: bodies retire between calls, never
// mid-call).
//
// Allocation: every per-slot array and both key vectors are sized at
// construction, so the steady-state step never allocates (CONTRACT.md 11).
// The key vectors hold 4 * max_bodies + 256 shape pairs; a pathological
// scene past that DROPS the lexicographically highest keys, one time
// warned - bounded and deterministic, like the engines' own contact caps.

#include <cmath>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <variant>
#include <vector>

#include <ws/core/log.hpp>
#include <ws/physics/errors.hpp>

#include "core/backend2d.hpp"

namespace ws::physics::detail {
namespace {

using physics2d::BodyDesc;
using physics2d::Box;
using physics2d::Circle;
using physics2d::Transform;
using ws::math::Vec2;

constexpr std::uint32_t kPartsPerSlot = ws::physics::kMaxPartsPerBody;

// --- small local geometry (Vec2 has no rotate of its own) --------------------

[[nodiscard]] inline Vec2 rot2(Vec2 v, float angle) {
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  return Vec2{c * v.x - s * v.y, s * v.x + c * v.y};
}

[[nodiscard]] inline Vec2 flip(Vec2 v) {
  return Vec2{-v.x, -v.y};
}

[[nodiscard]] inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// omega x r in 2D (r from the rotation center to the point).
[[nodiscard]] inline Vec2 cross_w(float omega, Vec2 r) {
  return Vec2{-omega * r.y, omega * r.x};
}

// --- shape-pair tests: fill the witness and a unit normal, or false ----------

// Normal points CA -> CB; witness is the midpoint of the overlap segment
// (purely geometric - the engines report a solver manifold point, and this
// backend has no solver). Coincident centers pick +x.
[[nodiscard]] inline bool circle_circle_touch(Vec2 ca, float ra, Vec2 cb,
                                              float rb, Vec2& point,
                                              Vec2& normal) {
  const Vec2 d = cb - ca;
  const float dist = ws::math::length(d);
  if (dist > ra + rb) {
    return false;
  }
  const Vec2 n = dist > 0.0f ? d * (1.0f / dist) : Vec2{1.0f, 0.0f};
  normal = n;
  point = ca + n * ((dist + ra - rb) * 0.5f);
  return true;
}

// Normal points BOX -> CIRCLE (the pushout direction); witness is the
// closest point on the box, or a point on the nearest face when the circle
// center is inside. The caller flips when the circle is pair-side a.
[[nodiscard]] inline bool circle_box_touch(Vec2 bc, Vec2 h, float theta,
                                           Vec2 cc, float r, Vec2& point,
                                           Vec2& normal) {
  // Circle center in the box frame.
  const Vec2 lo = rot2(cc - bc, -theta);
  const Vec2 q{clampf(lo.x, -h.x, h.x), clampf(lo.y, -h.y, h.y)};
  Vec2 n_local{};
  if (q.x == lo.x && q.y == lo.y) {
    // Center inside the box: push out through the nearest face.
    const float dx = h.x - std::abs(lo.x);
    const float dy = h.y - std::abs(lo.y);
    if (dx <= dy) {
      const float sx = lo.x >= 0.0f ? 1.0f : -1.0f;
      n_local = Vec2{sx, 0.0f};
      point = bc + rot2(Vec2{sx * h.x, lo.y}, theta);
    } else {
      const float sy = lo.y >= 0.0f ? 1.0f : -1.0f;
      n_local = Vec2{0.0f, sy};
      point = bc + rot2(Vec2{lo.x, sy * h.y}, theta);
    }
  } else {
    const Vec2 d = lo - q;
    const float dist = ws::math::length(d);
    if (dist > r) {
      return false;
    }
    n_local = dist > 0.0f ? d * (1.0f / dist) : Vec2{1.0f, 0.0f};
    point = bc + rot2(q, theta);  // the closest point on the box
  }
  normal = rot2(n_local, theta);
  return true;
}

// SAT over both boxes' face axes (4 candidates); the min-overlap axis is
// the separation normal, oriented A -> B (each candidate's sign comes from
// B's center measured along it, so the normal always points from A toward
// B). Witness: B's support vertex along -normal - the corner deepest into
// A. First-minimum wins ties: A's x, A's y, B's x, B's y, deterministic.
[[nodiscard]] inline bool box_box_touch(Vec2 ac, Vec2 ha, float ta, Vec2 bc,
                                        Vec2 hb, float tb, Vec2& point,
                                        Vec2& normal) {
  const float delta = tb - ta;
  const float cd = std::cos(delta);
  const float sd = std::sin(delta);
  // B's center in A's frame, and each box's extents projected on the
  // other's axes (axis-aligned components of the rotated half extents).
  const Vec2 lc = rot2(bc - ac, -ta);
  const float ex = std::abs(cd) * hb.x + std::abs(sd) * hb.y;
  const float ey = std::abs(sd) * hb.x + std::abs(cd) * hb.y;
  const float eax = std::abs(cd) * ha.x + std::abs(sd) * ha.y;
  const float eay = std::abs(sd) * ha.x + std::abs(cd) * ha.y;
  // A->B in B's frame: rot2(lc, -delta).
  const float lbx = lc.x * cd + lc.y * sd;
  const float lby = -lc.x * sd + lc.y * cd;
  const float ox = (ha.x + ex) - std::abs(lc.x);
  const float oy = (ha.y + ey) - std::abs(lc.y);
  const float px = (hb.x + eax) - std::abs(lbx);
  const float py = (hb.y + eay) - std::abs(lby);
  if (ox <= 0.0f || oy <= 0.0f || px <= 0.0f || py <= 0.0f) {
    return false;
  }
  float best = ox;
  Vec2 n_world = rot2(Vec2{lc.x >= 0.0f ? 1.0f : -1.0f, 0.0f}, ta);
  if (oy < best) {
    best = oy;
    n_world = rot2(Vec2{0.0f, lc.y >= 0.0f ? 1.0f : -1.0f}, ta);
  }
  if (px < best) {
    best = px;
    n_world = rot2(Vec2{lbx >= 0.0f ? 1.0f : -1.0f, 0.0f}, tb);
  }
  if (py < best) {
    best = py;
    n_world = rot2(Vec2{0.0f, lby >= 0.0f ? 1.0f : -1.0f}, tb);
  }
  normal = n_world;
  // B's support corner along -normal.
  const Vec2 n_local_b = rot2(normal, -tb);
  point = bc + rot2(Vec2{n_local_b.x >= 0.0f ? -hb.x : hb.x,
                         n_local_b.y >= 0.0f ? -hb.y : hb.y},
                    tb);
  return true;
}

// OBB vs AABB for overlap_aabb: SAT over the four face axes (two per box).
[[nodiscard]] inline bool obb_aabb_overlap(Vec2 oc, Vec2 h, float theta,
                                           Vec2 ac, Vec2 a) {
  const Vec2 d = oc - ac;
  const float c = std::cos(theta);
  const float s = std::sin(theta);
  const Vec2 u1{c, s};    // the OBB's local +x in world space
  const Vec2 u2{-s, c};   // ...and local +y
  // Axis u1: the AABB's projected radius vs the OBB's (h.x along u1, h.y
  // skewed by |sin|).
  const float r1_a = a.x * std::abs(u1.x) + a.y * std::abs(u1.y);
  const float r1_b = h.x + h.y * std::abs(s);
  if (std::abs(ws::math::dot(d, u1)) > r1_a + r1_b) {
    return false;
  }
  const float r2_a = a.x * std::abs(u2.x) + a.y * std::abs(u2.y);
  const float r2_b = h.y + h.x * std::abs(s);
  if (std::abs(ws::math::dot(d, u2)) > r2_a + r2_b) {
    return false;
  }
  // The AABB's own axes.
  const float r3_b = h.x * std::abs(u1.x) + h.y * std::abs(u2.x);
  if (std::abs(d.x) > a.x + r3_b) {
    return false;
  }
  const float r4_b = h.x * std::abs(u1.y) + h.y * std::abs(u2.y);
  if (std::abs(d.y) > a.y + r4_b) {
    return false;
  }
  return true;
}

// Ray casts (unit direction): an ENTERING ray only - an origin inside or on
// the shape is a miss, both shapes. t is the entry distance.
[[nodiscard]] inline bool ray_circle(Vec2 o, Vec2 d, Vec2 c, float r,
                                     float& t_out) {
  const Vec2 oc = o - c;
  const float cc = ws::math::dot(oc, oc) - r * r;
  if (cc <= 0.0f) {
    return false;  // origin inside (or on) the disc
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

[[nodiscard]] inline bool ray_box(Vec2 o, Vec2 d, Vec2 bc, Vec2 h,
                                  float theta, float& t_out, Vec2& normal) {
  const Vec2 lo = rot2(o - bc, -theta);
  const Vec2 ld = rot2(d, -theta);
  const float lo_a[2] = {lo.x, lo.y};
  const float ld_a[2] = {ld.x, ld.y};
  const float h_a[2] = {h.x, h.y};
  float tmin = 0.0f;
  float tmax = 1e30f;
  int axis = -1;
  float sign = 0.0f;
  for (int k = 0; k < 2; ++k) {
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
  const Vec2 n_local = axis == 0 ? Vec2{sign, 0.0f} : Vec2{0.0f, sign};
  normal = rot2(n_local, theta);
  return true;
}

// --- the backend ----------------------------------------------------------------

// One shape of a body in body-local space: circles and boxes only (a part
// or body-level capsule fails create_body).
struct PartGeo {
  bool circle = true;
  float radius = 0.5f;  // circle
  Vec2 half{};          // box
  Vec2 offset{};        // body-local translation (no per-part rotation in v2)
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
  Vec2 point{};    // witness, world space
  Vec2 normal{};   // unit, a -> b
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

class ReferenceWorld2D final : public IWorld2DBackend {
 public:
  explicit ReferenceWorld2D(const BackendDesc2D& desc)
      : capacity_(desc.max_bodies) {
    gravity_ = desc.gravity;
    hit_threshold_ = desc.hit_speed_threshold;
    kinds_.assign(capacity_, ws::physics::BodyKind::static_);
    live_.assign(capacity_, 0);
    sensor_.assign(capacity_, 0);
    inv_mass_.assign(capacity_, 0.0f);
    inv_inertia_.assign(capacity_, 0.0f);
    gravity_scale_.assign(capacity_, 1.0f);
    pos_.assign(capacity_, Vec2{});
    rot_.assign(capacity_, 0.0f);
    vel_.assign(capacity_, Vec2{});
    ang_.assign(capacity_, 0.0f);
    com_.assign(capacity_, Vec2{});
    force_.assign(capacity_, Vec2{});
    torque_.assign(capacity_, 0.0f);
    layer_.assign(capacity_, 1u);
    mask_.assign(capacity_, 0xffffffffu);
    bullet_.assign(capacity_, 0);
    part_count_.assign(capacity_, 0);
    parts_.assign(std::size_t{capacity_} * kPartsPerSlot, PartGeo{});
    // Both key vectors pre-reserved: the steady-state step never allocates
    // (CONTRACT.md 11). The bound is generous but finite - see the header.
    const std::size_t touch_cap = std::size_t{4} * capacity_ + 256;
    prev_.reserve(touch_cap);
    now_.reserve(touch_cap);
  }

  // --- bodies ------------------------------------------------------------------

  [[nodiscard]] ws::core::Status create_body(
      std::uint32_t slot, const BodyDesc& desc,
      const MassData2D& mass) override {
    if (slot >= capacity_) {
      return fail(PhysicsError::backend_error, "body slot out of range");
    }
    const std::uint32_t count =
        desc.part_count == 0 ? 1u : desc.part_count;
    if (count > kPartsPerSlot) {
      return fail(PhysicsError::backend_error, "part count out of range");
    }
    for (std::uint32_t p = 0; p < count; ++p) {
      const physics2d::Shape& shape =
          desc.part_count == 0 ? desc.shape : desc.parts[p].shape;
      const Vec2 offset =
          desc.part_count == 0 ? Vec2{} : desc.parts[p].offset;
      PartGeo& g = parts_[std::size_t{slot} * kPartsPerSlot + p];
      if (const auto* circle = std::get_if<Circle>(&shape)) {
        g.circle = true;
        g.radius = circle->radius;
        g.half = Vec2{};
        g.offset = offset;
      } else if (const auto* box = std::get_if<Box>(&shape)) {
        g.circle = false;
        g.radius = 0.0f;
        g.half = box->half_extents;
        g.offset = offset;
      } else {
        return fail(PhysicsError::backend_error,
                    "reference 2D backend: capsules are not implemented "
                    "(circles and boxes only)");
      }
    }
    kinds_[slot] = desc.kind;
    live_[slot] = 1;
    sensor_[slot] = desc.is_sensor ? 1 : 0;
    layer_[slot] = desc.collision_layer;
    mask_[slot] = desc.collision_mask;
    bullet_[slot] = desc.is_bullet ? 1 : 0;
    gravity_scale_[slot] = desc.gravity_scale;
    pos_[slot] = desc.transform.position;
    rot_[slot] = desc.transform.rotation;
    vel_[slot] = desc.linear_velocity;
    ang_[slot] = desc.angular_velocity;
    force_[slot] = Vec2{};
    torque_[slot] = 0.0f;
    part_count_[slot] = static_cast<std::uint8_t>(count);
    // The LIB's mass data, applied as given (CONTRACT.md 4) - meaningful
    // for dynamics only; statics/kinematics carry zeros that must NOT be
    // applied (the seam's rule, matched to what WorldCore passes).
    if (desc.kind == ws::physics::BodyKind::dynamic) {
      inv_mass_[slot] = mass.mass > 0.0f ? 1.0f / mass.mass : 0.0f;
      inv_inertia_[slot] = mass.inertia > 0.0f ? 1.0f / mass.inertia : 0.0f;
      com_[slot] = mass.center;
    } else {
      inv_mass_[slot] = 0.0f;
      inv_inertia_[slot] = 0.0f;
      com_[slot] = Vec2{};
    }
    return {};
  }

  void destroy_body(std::uint32_t slot) override {
    if (slot >= capacity_ || !live_[slot]) {
      return;
    }
    live_[slot] = 0;
    // Silence the destroyed body's pairs right away (Box2D's observable
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
    rot_[slot] = t.rotation;
  }

  void set_velocity(std::uint32_t slot, Vec2 linear, float angular) override {
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

  void set_gravity(Vec2 gravity) override {
    // The seam asks backends to wake every live body; nothing sleeps here,
    // so storing is the whole job.
    gravity_ = gravity;
  }

  // Forces/impulses reach DYNAMICS only: statics and kinematics carry no
  // mass semantics (CONTRACT.md 4), and the engines no-op them the same
  // way (infinite mass).
  void apply_linear_impulse(std::uint32_t slot, Vec2 impulse) override {
    if (kinds_[slot] == ws::physics::BodyKind::dynamic) {
      vel_[slot] = vel_[slot] + impulse * inv_mass_[slot];
    }
  }

  void apply_angular_impulse(std::uint32_t slot, float impulse) override {
    if (kinds_[slot] == ws::physics::BodyKind::dynamic) {
      ang_[slot] += impulse * inv_inertia_[slot];
    }
  }

  void apply_force(std::uint32_t slot, Vec2 force) override {
    if (kinds_[slot] == ws::physics::BodyKind::dynamic) {
      force_[slot] = force_[slot] + force;
    }
  }

  void apply_torque(std::uint32_t slot, float torque) override {
    if (kinds_[slot] == ws::physics::BodyKind::dynamic) {
      torque_[slot] += torque;
    }
  }

  [[nodiscard]] BackendVelocity2D get_velocity(
      std::uint32_t slot) const override {
    return BackendVelocity2D{vel_[slot], ang_[slot]};
  }

  // --- joints ------------------------------------------------------------------

  [[nodiscard]] ws::core::Status create_joint(
      std::uint32_t, const BackendJoint2D&) override {
    return fail(PhysicsError::backend_error,
                "reference 2D backend: joints are not implemented");
  }

  void destroy_joint(std::uint32_t) override {}

  // --- one tick ----------------------------------------------------------------

  std::size_t step(float dt, IEventCollector2D& collector,
                   std::span<BackendMove2D> moves_out) override {
    // Integrate, then report every simulated body. Nothing sleeps, so
    // "every body the backend simulated" is every live non-static body,
    // in slot order (the lib folds these into its pose mirror).
    std::size_t count = 0;
    for (std::uint32_t s = 0; s < capacity_; ++s) {
      if (!live_[s]) {
        continue;
      }
      const auto kind = kinds_[s];
      if (kind == ws::physics::BodyKind::static_) {
        continue;  // statics never move
      }
      if (kind == ws::physics::BodyKind::dynamic) {
        vel_[s] = vel_[s] + (gravity_ * gravity_scale_[s] +
                             force_[s] * inv_mass_[s]) * dt;
        ang_[s] += torque_[s] * inv_inertia_[s] * dt;
        force_[s] = Vec2{};  // cleared at the end of every tick (CONTRACT 7)
        torque_[s] = 0.0f;
      }
      // Kinematics integrate their set velocity only (position-driven).
      // The state is COM-frame (CONTRACT.md 4): move the COM, then
      // reconstruct the origin's pose from the body-local COM vector.
      const Vec2 com_local = com_[s];
      const Vec2 com_world = pos_[s] + rot2(com_local, rot_[s]);
      const Vec2 moved = com_world + vel_[s] * dt;
      const float new_rot = rot_[s] + ang_[s] * dt;
      pos_[s] = moved - rot2(com_local, new_rot);
      rot_[s] = new_rot;
      if (count < moves_out.size()) {
        moves_out[count++] =
            BackendMove2D{s, Transform{pos_[s], rot_[s]}};
      }
    }

    // Detect: every live pair, every part pair, keys in lexicographic
    // order (a, pa, b, pb) so the diff below is a two-pointer sweep. The
    // pair-level tests re-run per part pair - harmless at reference scale.
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
          for (std::uint8_t pb = 0; pb < part_count_[b]; ++pb) {
            Vec2 point{};
            Vec2 normal{};
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
      BackendEvent2D ev{};
      ev.slot_a = t.a;
      ev.slot_b = t.b;
      ev.part_a = t.pa;
      ev.part_b = t.pb;
      ev.phase = physics2d::ContactPhase::began;
      if (solid) {
        ev.point = t.point;
        ev.normal = t.normal;
      }
      collector.add(ev);
      if (solid && hit_threshold_ > 0.0f && t.speed > hit_threshold_) {
        BackendEvent2D hit = ev;
        hit.phase = physics2d::ContactPhase::hit;
        hit.speed = t.speed;
        collector.add(hit);
      }
    };
    const auto feed_ended = [&](const Touch& t) {
      BackendEvent2D ev{};
      ev.slot_a = t.a;
      ev.slot_b = t.b;
      ev.part_a = t.pa;
      ev.part_b = t.pb;
      ev.phase = physics2d::ContactPhase::ended;
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

  [[nodiscard]] std::optional<BackendRayHit2D> raycast(
      Vec2 origin, Vec2 direction, float max_distance) const override {
    const float dl = ws::math::length(direction);
    if (dl == 0.0f) {
      return std::nullopt;  // the seam promises non-zero; defensive
    }
    const Vec2 d = direction * (1.0f / dl);
    float best_t = max_distance;
    bool found = false;
    std::optional<BackendRayHit2D> best;
    for (std::uint32_t s = 0; s < capacity_; ++s) {
      if (!live_[s]) {
        continue;
      }
      for (std::uint8_t p = 0; p < part_count_[s]; ++p) {
        const PartGeo& g = parts_[std::size_t{s} * kPartsPerSlot + p];
        const Vec2 c = part_center(s, p);
        float t = 0.0f;
        Vec2 point{};
        Vec2 normal{};
        if (g.circle) {
          if (!ray_circle(origin, d, c, g.radius, t)) {
            continue;
          }
          point = origin + d * t;
          normal = (point - c) * (1.0f / g.radius);
        } else {
          if (!ray_box(origin, d, c, g.half, rot_[s], t, normal)) {
            continue;
          }
          point = origin + d * t;
        }
        // Closest hit wins; ties keep the LOWEST slot, then part index
        // (strict <, ascending iteration). t <= max_distance, per the
        // public [0, max_distance] promise.
        if (!found || t < best_t) {
          found = true;
          best_t = t;
          best = BackendRayHit2D{s, t, point, normal};
        }
      }
    }
    return best;
  }

  void overlap_circle(Vec2 center, float radius,
                      std::vector<std::uint32_t>& out) const override {
    for (std::uint32_t s = 0; s < capacity_; ++s) {
      if (!live_[s]) {
        continue;
      }
      bool hit = false;
      for (std::uint8_t p = 0; p < part_count_[s] && !hit; ++p) {
        const PartGeo& g = parts_[std::size_t{s} * kPartsPerSlot + p];
        const Vec2 c = part_center(s, p);
        if (g.circle) {
          hit = ws::math::distance(center, c) <= radius + g.radius;
        } else {
          // Query circle vs oriented box: clamp the query center to the
          // box in its own frame.
          const Vec2 lo = rot2(center - c, -rot_[s]);
          const Vec2 q{clampf(lo.x, -g.half.x, g.half.x),
                      clampf(lo.y, -g.half.y, g.half.y)};
          hit = ws::math::distance(lo, q) <= radius;
        }
      }
      if (hit) {
        out.push_back(s);
      }
    }
  }

  void overlap_aabb(const physics2d::Aabb& box,
                    std::vector<std::uint32_t>& out) const override {
    const Vec2 ac = (box.min + box.max) * 0.5f;
    const Vec2 a = (box.max - box.min) * 0.5f;
    for (std::uint32_t s = 0; s < capacity_; ++s) {
      if (!live_[s]) {
        continue;
      }
      bool hit = false;
      for (std::uint8_t p = 0; p < part_count_[s] && !hit; ++p) {
        const PartGeo& g = parts_[std::size_t{s} * kPartsPerSlot + p];
        const Vec2 c = part_center(s, p);
        if (g.circle) {
          const Vec2 q{clampf(c.x, box.min.x, box.max.x),
                      clampf(c.y, box.min.y, box.max.y)};
          hit = ws::math::distance(c, q) <= g.radius;
        } else {
          hit = obb_aabb_overlap(c, g.half, rot_[s], ac, a);
        }
      }
      if (hit) {
        out.push_back(s);
      }
    }
  }

 private:
  // World-space center of one part (offset rides the body rotation; parts
  // carry no rotation of their own in v2).
  [[nodiscard]] Vec2 part_center(std::uint32_t slot, std::uint8_t p) const {
    const PartGeo& g = parts_[std::size_t{slot} * kPartsPerSlot + p];
    return pos_[slot] + rot2(g.offset, rot_[slot]);
  }

  // One shape-pair test in the a < b convention: fills the witness and the
  // a -> b unit normal, or returns false when apart.
  [[nodiscard]] bool part_pair(std::uint32_t a, std::uint8_t pa,
                               std::uint32_t b, std::uint8_t pb,
                               Vec2& point, Vec2& normal) const {
    const PartGeo& ga = parts_[std::size_t{a} * kPartsPerSlot + pa];
    const PartGeo& gb = parts_[std::size_t{b} * kPartsPerSlot + pb];
    const Vec2 ca = part_center(a, pa);
    const Vec2 cb = part_center(b, pb);
    if (ga.circle && gb.circle) {
      return circle_circle_touch(ca, ga.radius, cb, gb.radius, point, normal);
    }
    if (!ga.circle && !gb.circle) {
      return box_box_touch(ca, ga.half, rot_[a], cb, gb.half, rot_[b], point,
                           normal);
    }
    if (ga.circle) {
      // a is the circle, b the box: the helper speaks box -> circle (b -> a).
      if (!circle_box_touch(cb, gb.half, rot_[b], ca, ga.radius, point,
                            normal)) {
        return false;
      }
      normal = flip(normal);
      return true;
    }
    // a is the box, b the circle: box -> circle IS a -> b.
    return circle_box_touch(ca, ga.half, rot_[a], cb, gb.radius, point,
                            normal);
  }

  // How fast the pair closes along the a -> b normal, measured at the
  // witness with the post-integration velocities (there is no solver to
  // change them). Kinematics carry their set velocity like anyone else;
  // only the COM frame matters (CONTRACT.md 4).
  [[nodiscard]] float approach_speed(std::uint32_t a, std::uint32_t b,
                                     Vec2 point, Vec2 normal) const {
    const Vec2 ra = point - (pos_[a] + rot2(com_[a], rot_[a]));
    const Vec2 rb = point - (pos_[b] + rot2(com_[b], rot_[b]));
    const Vec2 rel = (vel_[b] + cross_w(ang_[b], rb)) -
                     (vel_[a] + cross_w(ang_[a], ra));
    const float closing = -ws::math::dot(rel, normal);
    return closing > 0.0f ? closing : 0.0f;
  }

  void push_touch(std::uint32_t a, std::uint8_t pa, std::uint32_t b,
                  std::uint8_t pb, Vec2 point, Vec2 normal, float speed) {
    if (now_.size() >= now_.capacity()) {
      // Bounded, deterministic tail drop (keys arrive sorted, so the
      // highest go) - see the header.
      if (!touch_overflow_warned_) {
        touch_overflow_warned_ = true;
        ws::core::warn("physics: reference 2D backend touch-key budget "
                       "spent; dropping the highest pairs");
      }
      return;
    }
    now_.push_back(Touch{a, pa, b, pb, point, normal, speed});
  }

  std::uint32_t capacity_ = 0;
  Vec2 gravity_{};
  float hit_threshold_ = 0.0f;

  // Slot-indexed state, pre-sized at construction (the slot table is the
  // lib's; this backend trusts the slots it is given).
  std::vector<ws::physics::BodyKind> kinds_;
  std::vector<char> live_;
  std::vector<char> sensor_;
  std::vector<char> bullet_;  // stored, never read: no CCD
  std::vector<float> inv_mass_;
  std::vector<float> inv_inertia_;
  std::vector<float> gravity_scale_;
  std::vector<Vec2> pos_;   // origin-frame (what the lib mirrors)
  std::vector<float> rot_;
  std::vector<Vec2> vel_;   // COM-frame
  std::vector<float> ang_;
  std::vector<Vec2> com_;   // body-local, dynamics only
  std::vector<Vec2> force_;  // accumulation, cleared per tick
  std::vector<float> torque_;
  std::vector<std::uint32_t> layer_;
  std::vector<std::uint32_t> mask_;
  std::vector<std::uint8_t> part_count_;
  std::vector<PartGeo> parts_;  // capacity_ x kPartsPerSlot, flat

  std::vector<Touch> prev_;  // last tick's touches
  std::vector<Touch> now_;   // this tick's, sorted by construction
  bool touch_overflow_warned_ = false;
};

}  // namespace

std::unique_ptr<IWorld2DBackend> make_reference_world_2d(
    const BackendDesc2D& desc) {
  try {
    return std::make_unique<ReferenceWorld2D>(desc);
  } catch (const std::bad_alloc&) {
    ws::core::warn("physics: reference 2D backend out of memory");
    return nullptr;
  } catch (const std::exception& e) {
    ws::core::warn("physics: reference 2D backend failed to start: {}",
                   e.what());
    return nullptr;
  }
}

}  // namespace ws::physics::detail
