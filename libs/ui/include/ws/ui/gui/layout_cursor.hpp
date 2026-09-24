#pragma once

#include <ws/ui/gui/rect.hpp>

#include <algorithm>
#include <array>

namespace ws::ui::gui {

// Stack-based layout cursor. A panel is a padded rect plus a "where the
// next widget goes" position; next(w, h) hands out the next widget's
// rect and advances the cursor. No flex, no solver, no measurement -
// sizes come in from the widget. Panels stack: begin_panel parks the
// parent's cursor, the panel lays its children out independently,
// end_panel resumes the parent.
//
// Line rule: next() places at the cursor, then advances below the
// TALLEST widget placed on the line so far; same_line() rewinds to place
// the next widget beside the previous one instead. The cursor never
// clips - widgets may overflow their panel; clipping is a render concern.
class LayoutCursor {
public:
    // Deep panel stacks mean nesting bugs; overflow is clamped (and
    // logged) rather than allocating.
    static constexpr int kMaxPanels = 8;

    // Opens a panel filling `bounds`, inset by `padding` on every side,
    // and parks the enclosing panel's cursor. Returns the content rect
    // (bounds shrunk by the padding) so callers can clip or place custom
    // content without redoing the math. Overflowing the stack (deeper
    // than kMaxPanels) clamps (logged once): the depth keeps counting so
    // the matching end_panel() stays balanced - it can never pop a real
    // ancestor - while the children flow into the deepest real panel and
    // the returned content rect is empty.
    Rect begin_panel(const Rect& bounds, float padding);
    // The next widget's rect: size (w, h) at the cursor, then the cursor
    // advances below the line's tallest widget. No panel open: an empty
    // rect (drop-safe).
    Rect next(float w, float h);
    // Places the next widget on the current line, after the previous
    // widget, instead of opening a new line. No-op when nothing has been
    // placed yet in the panel.
    void same_line();
    // Closes the innermost panel and resumes the parent's cursor.
    // Closing with none open is a no-op.
    void end_panel();

    // Number of open panels (0 = none). Past the clamp this counts the
    // overflowed degenerate panels too - honest bookkeeping for balanced
    // end_panel() calls.
    int depth() const { return top_ + 1; }

private:
    struct Panel {
        Rect content;  // the padded rect widgets flow within
        Rect last;     // previous widget's rect (same_line resumes after it)
        float cursor_x = 0.0f;   // next widget's top-left (gui space)
        float cursor_y = 0.0f;
        float line_height = 0.0f;  // tallest widget on the line being placed
        bool placed_any = false;  // next() ran here (same_line resumes only then)
        bool at_line_start = true;  // the cursor opens a fresh line - explicit
                                    // state, never a float position compare
    };
    std::array<Panel, kMaxPanels> panels_{};
    int top_ = -1;  // innermost open panel; -1 = none. Past the clamp this
                    // keeps counting (balanced pushes); slot() clamps it.

    // The active panel's storage slot - the index clamps past kMaxPanels
    // so overflowed (degenerate) pushes index the deepest real panel
    // instead of out of bounds.
    Panel& slot() {
        return panels_[static_cast<std::size_t>(
            std::min(top_, kMaxPanels - 1))];
    }
};

}  // namespace ws::ui::gui
