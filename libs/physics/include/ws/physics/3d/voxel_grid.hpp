#pragma once

// Backend-independent collision queries for axis-aligned voxel worlds.
//
// This is deliberately separate from World3D's rigid-body backend: static
// terrain is data, not thousands of rigid bodies. Games provide a read-only
// occupancy callback over their own chunk storage; this type performs DDA
// ray traversal and swept-AABB movement without copying or owning that data.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include <ws/math/vecmath.hpp>
#include <ws/physics/3d/shape.hpp>

namespace ws::physics3d {

class VoxelGrid3D {
 public:
  using SolidQuery = bool (*)(const void* context, int x, int y,
                              int z) noexcept;

  struct Desc {
    ws::math::IVec3 origin{};
    ws::math::IVec3 dimensions{};
    const void* context = nullptr;
    SolidQuery is_solid = nullptr;
  };

  struct RayHit {
    bool hit = false;
    ws::math::IVec3 cell{};
    ws::math::IVec3 normal{};
    float distance = 0.0f;
  };

  struct MoveResult {
    Aabb bounds{};
    ws::math::Vec3 applied{};
    bool hit_x = false;
    bool hit_y = false;
    bool hit_z = false;

    [[nodiscard]] bool hit() const noexcept {
      return hit_x || hit_y || hit_z;
    }
    [[nodiscard]] bool grounded() const noexcept {
      return hit_y && applied.y <= 0.0f;
    }
  };

  explicit VoxelGrid3D(Desc desc) noexcept : desc_(desc) {}

  [[nodiscard]] bool valid() const noexcept {
    return desc_.is_solid != nullptr && desc_.dimensions.x > 0 &&
           desc_.dimensions.y > 0 && desc_.dimensions.z > 0;
  }

  // Out-of-bounds cells are empty. This matches open-ended voxel worlds and
  // lets the owner decide whether world borders should be solid.
  [[nodiscard]] bool is_solid(int x, int y, int z) const noexcept {
    if (!valid() || x < desc_.origin.x || y < desc_.origin.y ||
        z < desc_.origin.z || x >= desc_.origin.x + desc_.dimensions.x ||
        y >= desc_.origin.y + desc_.dimensions.y ||
        z >= desc_.origin.z + desc_.dimensions.z) {
      return false;
    }
    return desc_.is_solid(desc_.context, x, y, z);
  }

  // Amanatides-Woo traversal. Direction is normalized internally; distance
  // is expressed in world units. The returned normal is the outward normal
  // of the face through which the ray entered the hit cell.
  [[nodiscard]] RayHit raycast(ws::math::Vec3 origin,
                               ws::math::Vec3 direction,
                               float max_distance) const noexcept {
    RayHit out{};
    if (!valid() || !std::isfinite(max_distance) || max_distance < 0.0f) {
      return out;
    }
    const float len = ws::math::length(direction);
    if (!std::isfinite(len) || len < 1.0e-8f) {
      return out;
    }
    direction = direction / len;

    int cell[3] = {static_cast<int>(std::floor(origin.x)),
                   static_cast<int>(std::floor(origin.y)),
                   static_cast<int>(std::floor(origin.z))};
    const float o[3] = {origin.x, origin.y, origin.z};
    const float d[3] = {direction.x, direction.y, direction.z};
    int step[3]{};
    float t_max[3]{};
    float t_delta[3]{};
    constexpr float kInfinity = std::numeric_limits<float>::infinity();
    for (int axis = 0; axis < 3; ++axis) {
      step[axis] = d[axis] > 0.0f ? 1 : (d[axis] < 0.0f ? -1 : 0);
      if (step[axis] == 0) {
        t_max[axis] = kInfinity;
        t_delta[axis] = kInfinity;
      } else {
        const float boundary =
            static_cast<float>(cell[axis] + (step[axis] > 0 ? 1 : 0));
        t_max[axis] = (boundary - o[axis]) / d[axis];
        t_delta[axis] = std::abs(1.0f / d[axis]);
      }
    }

    for (int axis = 0; axis < 3; ++axis) {
      const int cell_min = desc_.origin[static_cast<std::size_t>(axis)];
      const int cell_max = cell_min +
                           desc_.dimensions[static_cast<std::size_t>(axis)];
      if (step[axis] == 0 &&
          (cell[axis] < cell_min || cell[axis] >= cell_max)) {
        return out;
      }
    }

    int entered_axis = -1;
    int entered_step = 0;
    float distance = 0.0f;
    const auto outside_steps = [](int value, int origin_cell) noexcept {
      const long long diff = static_cast<long long>(value) - origin_cell;
      return diff < 0 ? -diff : diff;
    };
    const long long step_budget =
        static_cast<long long>(desc_.dimensions.x) + desc_.dimensions.y +
        desc_.dimensions.z + outside_steps(cell[0], desc_.origin.x) +
        outside_steps(cell[1], desc_.origin.y) +
        outside_steps(cell[2], desc_.origin.z) + 4;
    const int max_steps = static_cast<int>(
        std::min(step_budget, 1'000'000LL));
    for (int guard = 0; guard < max_steps; ++guard) {
      if (is_solid(cell[0], cell[1], cell[2])) {
        out.hit = true;
        out.cell = {cell[0], cell[1], cell[2]};
        if (entered_axis >= 0) {
          out.normal[static_cast<std::size_t>(entered_axis)] = -entered_step;
        }
        out.distance = distance;
        return out;
      }

      int axis = 0;
      if (t_max[1] < t_max[axis]) axis = 1;
      if (t_max[2] < t_max[axis]) axis = 2;
      if (t_max[axis] > max_distance) break;

      distance = t_max[axis];
      t_max[axis] += t_delta[axis];
      cell[axis] += step[axis];
      entered_axis = axis;
      entered_step = step[axis];

      // A ray cannot re-enter the finite grid after leaving it.
      if (cell[axis] < desc_.origin[static_cast<std::size_t>(axis)] ||
          cell[axis] >= desc_.origin[static_cast<std::size_t>(axis)] +
                            desc_.dimensions[static_cast<std::size_t>(axis)]) {
        break;
      }
    }
    return out;
  }

  // Tests strict volume overlap (touching a voxel face is not penetration).
  [[nodiscard]] bool overlaps(const Aabb& box) const noexcept {
    if (!valid()) return false;
    const int x0 = static_cast<int>(std::floor(box.min.x));
    const int y0 = static_cast<int>(std::floor(box.min.y));
    const int z0 = static_cast<int>(std::floor(box.min.z));
    const int x1 = static_cast<int>(std::floor(box.max.x));
    const int y1 = static_cast<int>(std::floor(box.max.y));
    const int z1 = static_cast<int>(std::floor(box.max.z));
    for (int y = y0; y <= y1; ++y) {
      for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
          if (is_solid(x, y, z) && box.max.x > static_cast<float>(x) &&
              box.min.x < static_cast<float>(x + 1) &&
              box.max.y > static_cast<float>(y) &&
              box.min.y < static_cast<float>(y + 1) &&
              box.max.z > static_cast<float>(z) &&
              box.min.z < static_cast<float>(z + 1)) {
            return true;
          }
        }
      }
    }
    return false;
  }

  // Sweeps an AABB through the voxel field, resolving one axis at a time.
  // Axis separation gives stable wall sliding and prevents tunnelling even
  // when displacement spans multiple cells. Work is proportional to the
  // swept local bounds, not the total world size or number of solid voxels.
  [[nodiscard]] MoveResult sweep_aabb(const Aabb& start,
                                      ws::math::Vec3 displacement,
                                      float skin = 0.001f) const noexcept {
    MoveResult result{};
    result.bounds = start;
    if (!valid()) return result;
    skin = std::max(0.0f, skin);

    // Recover from a small pre-existing overlap first (e.g. after a
    // teleported spawn or edited block). The bounded pass count prevents
    // pathological geometry from creating an unbounded loop.
    depenetrate(result, skin);

    // Resolve vertical motion first, then horizontal axes. A blocked axis
    // loses only its own displacement; the remaining axes still slide.
    sweep_axis(result, 1, displacement.y, skin);
    sweep_axis(result, 0, displacement.x, skin);
    sweep_axis(result, 2, displacement.z, skin);
    return result;
  }

 private:
  void depenetrate(MoveResult& result, float skin) const noexcept {
    for (int pass = 0; pass < 8; ++pass) {
      const Aabb& box = result.bounds;
      const int x0 = static_cast<int>(std::floor(box.min.x));
      const int y0 = static_cast<int>(std::floor(box.min.y));
      const int z0 = static_cast<int>(std::floor(box.min.z));
      const int x1 = static_cast<int>(std::floor(box.max.x));
      const int y1 = static_cast<int>(std::floor(box.max.y));
      const int z1 = static_cast<int>(std::floor(box.max.z));
      float best = std::numeric_limits<float>::infinity();
      int best_axis = -1;
      float best_sign = 0.0f;
      for (int y = y0; y <= y1; ++y) {
        for (int z = z0; z <= z1; ++z) {
          for (int x = x0; x <= x1; ++x) {
            if (!is_solid(x, y, z)) continue;
            const float cell_min[3] = {static_cast<float>(x),
                                       static_cast<float>(y),
                                       static_cast<float>(z)};
            const float cell_max[3] = {cell_min[0] + 1.0f,
                                       cell_min[1] + 1.0f,
                                       cell_min[2] + 1.0f};
            const float box_min[3] = {box.min.x, box.min.y, box.min.z};
            const float box_max[3] = {box.max.x, box.max.y, box.max.z};
            bool overlaps = true;
            for (int axis = 0; axis < 3; ++axis) {
              if (box_max[axis] <= cell_min[axis] ||
                  box_min[axis] >= cell_max[axis]) {
                overlaps = false;
                break;
              }
            }
            if (!overlaps) continue;
            for (int axis = 0; axis < 3; ++axis) {
              const float push_negative = box_max[axis] - cell_min[axis];
              const float push_positive = cell_max[axis] - box_min[axis];
              if (push_negative < best) {
                best = push_negative;
                best_axis = axis;
                best_sign = -1.0f;
              }
              if (push_positive < best) {
                best = push_positive;
                best_axis = axis;
                best_sign = 1.0f;
              }
            }
          }
        }
      }
      if (best_axis < 0 || !std::isfinite(best)) break;
      const float applied = (best + skin) * best_sign;
      result.bounds.min[static_cast<std::size_t>(best_axis)] += applied;
      result.bounds.max[static_cast<std::size_t>(best_axis)] += applied;
      result.applied[static_cast<std::size_t>(best_axis)] += applied;
      if (best_axis == 0) result.hit_x = true;
      if (best_axis == 1) result.hit_y = true;
      if (best_axis == 2) result.hit_z = true;
    }
  }

  void sweep_axis(MoveResult& result, int axis, float delta,
                  float skin) const noexcept {
    if (std::abs(delta) < 1.0e-8f) return;

    const Aabb& box = result.bounds;
    const float mins[3] = {box.min.x, box.min.y, box.min.z};
    const float maxs[3] = {box.max.x, box.max.y, box.max.z};
    const int a0 = static_cast<int>(std::floor(
        std::min(mins[axis], mins[axis] + delta)));
    const int a1 = static_cast<int>(std::floor(
        std::max(maxs[axis], maxs[axis] + delta)));
    int lo[3] = {static_cast<int>(std::floor(box.min.x)),
                 static_cast<int>(std::floor(box.min.y)),
                 static_cast<int>(std::floor(box.min.z))};
    int hi[3] = {static_cast<int>(std::floor(box.max.x)),
                 static_cast<int>(std::floor(box.max.y)),
                 static_cast<int>(std::floor(box.max.z))};
    lo[axis] = a0;
    hi[axis] = a1;

    float nearest = 1.0f;
    bool collided = false;
    for (int y = lo[1]; y <= hi[1]; ++y) {
      for (int z = lo[2]; z <= hi[2]; ++z) {
        for (int x = lo[0]; x <= hi[0]; ++x) {
          if (!is_solid(x, y, z)) continue;
          const int cell[3] = {x, y, z};
          bool overlaps_other_axes = true;
          for (int other = 0; other < 3; ++other) {
            if (other == axis) continue;
            const float cell_min = static_cast<float>(cell[other]);
            const float cell_max = cell_min + 1.0f;
            if (maxs[other] <= cell_min || mins[other] >= cell_max) {
              overlaps_other_axes = false;
              break;
            }
          }
          if (!overlaps_other_axes) continue;

          const float cell_min = static_cast<float>(cell[axis]);
          const float cell_max = cell_min + 1.0f;
          float distance = 0.0f;
          if (delta > 0.0f) {
            if (maxs[axis] > cell_min + 1.0e-6f) continue;
            distance = cell_min - maxs[axis];
          } else {
            if (mins[axis] < cell_max - 1.0e-6f) continue;
            distance = mins[axis] - cell_max;
          }
          const float t = distance / delta;
          if (t >= 0.0f && t <= nearest) {
            nearest = t;
            collided = true;
          }
        }
      }
    }

    float applied = delta;
    if (collided) {
      const float allowed = std::max(0.0f, std::abs(delta) * nearest - skin);
      applied = std::copysign(allowed, delta);
      if (axis == 0) result.hit_x = true;
      if (axis == 1) result.hit_y = true;
      if (axis == 2) result.hit_z = true;
    }
    result.applied[static_cast<std::size_t>(axis)] += applied;
    result.bounds.min[static_cast<std::size_t>(axis)] += applied;
    result.bounds.max[static_cast<std::size_t>(axis)] += applied;
  }

  Desc desc_{};
};

}  // namespace ws::physics3d
