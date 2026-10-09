#pragma once

#include <ws/ui/gui/rect.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

namespace ws::ui::gui {

// Placement within a line's height, next()'s optional third argument.
// The line's height is only known as it builds (an immediate-mode line
// has no "end"), so alignment is against the line height SO FAR - what
// stands to the widget's left - with the widget itself counting when it
// is the tallest, so a tall latecomer tops out at the line's top and
// never pokes above it. Widgets placed earlier never re-center when a
// taller one joins later; place the tallest first when a centered or
// bottom row must look even.
enum class Align : std::uint8_t { top, center, bottom };

// Stack-based layout cursor. A panel is a padded rect plus a "where the
// next widget goes" position; next(w, h) hands out the next widget's
// rect and advances the cursor. No flex, no solver, no measurement -
// sizes come in from the widget. Panels stack: begin_panel parks the
// parent's cursor, the panel lays its children out independently,
// end_panel resumes the parent.
//
// Line rule: next() places at the cursor, then advances below the
// TALLEST widget placed on the line so far plus the panel's spacing;
// same_line() rewinds to place the next widget beside the previous one
// instead, one spacing to its right. The cursor never clips - widgets
// may overflow their panel; clipping is a render concern.
class LayoutCursor {
public:
    // Deep panel stacks mean nesting bugs; overflow is clamped (and
    // logged) rather than allocating.
    static constexpr int kMaxPanels = 8;

    // Opens a panel filling `bounds`, inset by `padding` on every side,
    // and parks the enclosing panel's cursor. `spacing` is the gap the
    // flow inserts between widgets - between lines vertically, between
    // same_line() items horizontally; 0 (the default) is the unspaced
    // look. Returns the content rect (bounds shrunk by the padding,
    // width and height clamped at 0 so a bounds too small for its
    // padding is an empty panel, never a backwards one) so callers can
    // clip or place custom content without redoing the math. Overflowing
    // the stack (deeper than kMaxPanels) clamps (logged once): the depth
    // keeps counting so the matching end_panel() stays balanced - it can
    // never pop a real ancestor - while the children flow into the
    // deepest real panel and the returned content rect is empty.
    Rect begin_panel(const Rect& bounds, float padding, float spacing = 0.0f);
    // The next widget's rect: size (w, h) at the cursor, placed within
    // the line's height per `align` (top, the flush-at-the-line-top
    // default, is today's look), then the cursor advances below the
    // line's tallest plus the panel spacing. No panel open: an empty
    // rect (drop-safe).
    Rect next(float w, float h, Align align = Align::top);
    // Places the next widget on the current line, `gap` right of the
    // previous widget, instead of opening a new line; a negative gap
    // (the default) is the panel's spacing. No-op when nothing has been
    // placed yet in the panel.
    void same_line(float gap = -1.0f);
    // Closes the innermost panel and resumes the parent's cursor.
    // Closing with none open is a no-op.
    void end_panel();

    // Number of open panels (0 = none). Past the clamp this counts the
    // overflowed degenerate panels too - honest bookkeeping for balanced
    // end_panel() calls.
    int depth() const { return top_ + 1; }

    // The innermost open panel's content rect - the same rect its
    // begin_panel returned, re-readable without threading it through to
    // a later call (a scroll region's end needs it back). Empty with no
    // panel open. Past the overflow clamp this reports the deepest real
    // panel, where the degenerate children actually flowed.
    Rect panel_content() const;

    // The room left on the current line: from the cursor to the content
    // rect's right edge, clamped at 0 (a cursor already past the edge,
    // or no panel open). At a line start that is the full content width
    // - the fill-width answer, the one size a caller cannot know before
    // placing: hand it to next() and the widget spans to the edge.
    float remaining_width() const;

    // How far the innermost panel's placements reach below its content
    // origin: the bottom of the last placed line, or of the line a
    // same_line() resumed onto - 0 with no panel open or nothing placed.
    // The gaps between lines count; a trailing one does not (the cursor
    // parks one spacing past the last line, and the extent stops at the
    // line's bottom). A scroll region reads it after its children to
    // size its content ("how tall was what I flowed?") without tracking
    // placements itself.
    float flow_extent() const;

private:
    struct Panel {
        Rect content;  // the padded rect widgets flow within
        Rect last;     // previous widget's rect (same_line resumes after it)
        float cursor_x = 0.0f;   // next widget's top-left (gui space)
        float cursor_y = 0.0f;
        float line_top = 0.0f;    // where the line being placed starts -
                                  // alignment places within it, same_line rewinds to it
        float line_height = 0.0f;  // tallest widget on the line being placed
        float spacing = 0.0f;  // the flow's gap: between lines, between
                               // same_line() items
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
