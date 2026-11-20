#pragma once

// ws::sch - private geometry helpers shared by model.cpp, router.cpp and
// commands.cpp (one epsilon, one on_segment; they used to be copied into
// each file, plus a fourth time inside replay_split).

#include <algorithm>
#include <cmath>

#include <ws/schematic/model.hpp>

namespace ws::sch::detail {

inline constexpr double kEps = 1e-9;

[[nodiscard]] inline bool near(const Vec2& a, const Vec2& b) {
    return std::fabs(a.x - b.x) <= kEps && std::fabs(a.y - b.y) <= kEps;
}

// p lies on the closed segment a-b.
[[nodiscard]] inline bool on_segment(const Vec2& a, const Vec2& b,
                                     const Vec2& p) {
    const double cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    if (std::fabs(cross) > kEps) {
        return false;
    }
    return p.x >= std::min(a.x, b.x) - kEps &&
           p.x <= std::max(a.x, b.x) + kEps &&
           p.y >= std::min(a.y, b.y) - kEps && p.y <= std::max(a.y, b.y) + kEps;
}

}  // namespace ws::sch::detail
