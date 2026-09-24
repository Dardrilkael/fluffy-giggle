#pragma once

#include <ws/ui/gui/rect.hpp>

#include <array>

namespace ws::ui::gui {

// Stack-based layout cursor. A panel is a padded rect plus a "where the
// next widget goes" position; next(w, h) hands out the next widget's
// rect and advances the cursor. No flex, no solver, no measurement -
// sizes come in from the widget. Panels stack: begin_panel parks the
// parent's cursor, the panel lays its children out independently,
// end_panel resumes the parent. (Bodies are stubs in the Phase 0
// skeleton; layout lands in Phase 4.)
class LayoutCursor {
public:
    // Deep panel stacks mean nesting bugs; overflow is clamped (and
    // logged) rather than allocating.
    static constexpr int kMaxPanels = 8;

    // Opens a panel filling `bounds`, inset by `padding` on every side,
    // and parks the enclosing panel's cursor. Returns the content rect
    // (bounds shrunk by the padding) so callers can clip or place custom
    // content without redoing the math.
    Rect begin_panel(const Rect& bounds, float padding);
    // The next widget's rect: size (w, h) at the cursor, then the cursor
    // advances below it. Valid only inside an open panel.
    Rect next(float w, float h);
    // Places the next widget on the current line, after the previous
    // widget, instead of opening a new line.
    void same_line();
    // Closes the innermost panel and resumes the parent's cursor.
    // Closing with none open is a no-op.
    void end_panel();

    // Number of open panels (0 = none).
    int depth() const { return top_ + 1; }

private:
    struct Panel {
        Rect content;  // the padded rect widgets flow within
        Rect last;     // previous widget's rect (same_line resumes after it)
        float cursor_x = 0.0f;      // next widget's top-left, content-relative
        float cursor_y = 0.0f;
        float line_height = 0.0f;    // tallest widget so far on this line
    };
    std::array<Panel, kMaxPanels> panels_{};
    int top_ = -1;  // index of the innermost open panel; -1 = none
};

}  // namespace ws::ui::gui
