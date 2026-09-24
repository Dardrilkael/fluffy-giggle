#pragma once

#include <array>
#include <cstdint>

namespace ws::ui::gui {

// Widget identity: 64 bits hashed from the id stack's path of scope
// labels and indices. Ids are stable across frames as long as the code
// path is - re-describing the same UI re-derives the same ids - which is
// what lets per-widget state survive under an immediate-mode "nothing
// persists" discipline. 0 is the invalid sentinel (hot/active "none");
// the hash maps would-be zeros to a nonzero constant so no real widget
// ever ids as invalid.
using WidgetId = std::uint64_t;
inline constexpr WidgetId kInvalidWidgetId = 0;

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
    // WITHOUT pushing: hash(current, label). Each widget derives its own
    // id through this, so its state stays keyed while its scope moves.
    WidgetId current(const char* label) const;

    // Current scope depth (0 = root). Past the clamp this still counts
    // the overflowed pushes - honest bookkeeping for balanced pops.
    int depth() const { return depth_; }

private:
    std::array<WidgetId, kMaxDepth> stack_{};
    int depth_ = 0;
};

}  // namespace ws::ui::gui
