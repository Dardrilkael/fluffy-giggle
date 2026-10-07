#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace ws::ui::gui {

// Widget identity: 64 bits hashed from the id stack's path of scope
// labels and indices. Ids are stable across frames as long as the code
// path is - re-describing the same UI re-derives the same ids - which is
// what lets per-widget state survive under an immediate-mode "nothing
// persists" discipline. 0 is the invalid sentinel (hot/active "none");
// the hash maps would-be zeros to a nonzero constant so no real widget
// ever ids as invalid.
//
// A strong struct on purpose: no integer converts into a widget id, so
// a texture id or a body slot can never be passed where a widget goes.
// Compare with ==, order with < (both defaulted); the side tables hash
// through the std::hash specialization below.
struct WidgetId {
    std::uint64_t value;

    bool operator==(const WidgetId&) const = default;
    std::strong_ordering operator<=>(const WidgetId&) const = default;
};
inline constexpr WidgetId kInvalidWidgetId{0};

}  // namespace ws::ui::gui

// The side tables key on ids; hash the underlying bits. (A std template
// specialization: declared in namespace std proper - MSVC rejects the
// qualified `struct std::hash<...>` spelling inside the gui namespace.)
template <>
struct std::hash<ws::ui::gui::WidgetId> {
    std::size_t operator()(const ws::ui::gui::WidgetId& id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value);
    }
};

namespace ws::ui::gui {

// The id scope stack. Widgets with children (panels, tabs, list rows)
// push a scope so same-named widgets in different scopes get different
// ids; pop() restores the parent scope. The stack is what gives labels
// meaning: "ok" in a file dialog and "ok" in an error dialog must never
// collide.
class IdStack {
public:
    // Realistic immediate-mode nesting is a handful deep. Pushes past
    // this depth clamp (logged once): the remembered scope stops
    // deepening, but the depth KEEPS COUNTING, so the matching pop()
    // stays balanced and can never eat a real ancestor scope.
    static constexpr int kMaxDepth = 16;

    // Pushes a scope from a string label (widget class names, panel
    // titles): pushed id = hash(current, label). Past the clamp the id
    // stops deepening (still valid - derived from the deepest remembered
    // scope) while push/pop stays balanced.
    void push(const char* label);
    // Same, for indexed children (list rows, grid cells) - no
    // stringifying. pushed id = hash(current, index).
    void push(int index);
    // Pops the innermost scope. Popping an empty stack is a no-op.
    void pop();

    // The id a widget labeled `label` has under the current scope,
    // WITHOUT pushing: hash(scope, label). Each widget derives its own
    // id through this, so its state stays keyed while its scope moves.
    WidgetId id(const char* label) const;

    // Current scope depth (0 = root). Past the clamp this still counts
    // the overflowed pushes - honest bookkeeping for balanced pops.
    int depth() const { return depth_; }

private:
    std::array<WidgetId, kMaxDepth> stack_{};
    int depth_ = 0;
};

// An RAII id scope: pushes on construction, pops on destruction, so an
// early return cannot leak a scope the way a manual push/pop pair can.
// Non-copyable and non-movable on purpose: a second owner would pop the
// same scope twice - the guard's lifetime IS its correctness.
class ScopedId {
public:
    ScopedId(IdStack& ids, const char* label) : ids_(ids) { ids_.push(label); }
    ScopedId(IdStack& ids, int index) : ids_(ids) { ids_.push(index); }
    ~ScopedId() { ids_.pop(); }

    ScopedId(const ScopedId&) = delete;
    ScopedId& operator=(const ScopedId&) = delete;
    ScopedId(ScopedId&&) = delete;
    ScopedId& operator=(ScopedId&&) = delete;

private:
    IdStack& ids_;
};

}  // namespace ws::ui::gui
