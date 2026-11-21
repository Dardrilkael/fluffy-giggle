#pragma once

// ws::sch - private helpers shared by model.cpp, router.cpp and
// commands.cpp: the one epsilon, the one on_segment, and the id-keyed
// container helpers.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

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

// --- id-keyed containers -------------------------------------------------------
//
// Doc's vectors are kept SORTED BY ID: ids are minted monotonically (so
// the add_* builders append in order) and re-insertions (undo / redo /
// split replay) go through insert_by_id. Lookup is a binary search.
//
// The storage vectors are public and the adapter / loader may fill them
// directly, so sortedness is a fast path, never a correctness assumption:
// a binary-search MISS falls back to one linear scan. Hits (the hot case)
// are O(log n); a miss (a dangling reference) costs O(n) once.

inline constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

template <class V, class Id>
[[nodiscard]] auto find_by_id(V& v, Id id) -> decltype(&*v.begin()) {
    const auto it =
        std::lower_bound(v.begin(), v.end(), id,
                         [](const auto& e, Id k) { return e.id < k; });
    if (it != v.end() && it->id == id) {
        return &*it;
    }
    for (auto& e : v) {  // unsorted-storage fallback
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

template <class V, class Id>
[[nodiscard]] std::size_t index_by_id(const V& v, Id id) {
    const auto* p = find_by_id(v, id);
    return p != nullptr ? static_cast<std::size_t>(p - v.data()) : kNpos;
}

template <class V, class T>
void insert_by_id(V& v, T item) {
    const auto it =
        std::upper_bound(v.begin(), v.end(), item.id,
                         [](auto k, const auto& e) { return k < e.id; });
    v.insert(it, std::move(item));
}

template <class V, class Id>
bool erase_by_id(V& v, Id id) {
    const std::size_t i = index_by_id(v, id);
    if (i == kNpos) {
        return false;
    }
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
    return true;
}

}  // namespace ws::sch::detail
