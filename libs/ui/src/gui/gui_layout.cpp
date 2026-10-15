// LayoutCursor's out-of-line half: stacked flow panels. The panel-stack
// overflow warn-once flag is detail-linkage (gui.h) so reset_warn_once()
// can re-arm it.

#include <ws/ui/gui/layout_cursor.hpp>

#include <ws/core/log.hpp>
#include "gui/gui.h"  // detail::panel_overflow_warned - reset_warn_once()'s seam

#include <algorithm>

namespace ws::ui::gui {
namespace detail {

// Depth overflow is a programming error - surfaced in the log rather
// than a crash; the panel stack clamps and this keeps it to one warn
// per process. External linkage (gui.h) so reset_warn_once() can
// re-arm the test seam; single-threaded like every gui global (see
// gui.cpp).
bool panel_overflow_warned = false;

}  // namespace detail
namespace {

// --- LayoutCursor panels ---------------------------------------------------------

// True when the panel stack is full; warns the first time only.
bool panel_stack_full(int top) {
    if (top + 1 < LayoutCursor::kMaxPanels) {
        return false;
    }
    if (!detail::panel_overflow_warned) {
        detail::panel_overflow_warned = true;
        core::warn("ui: gui layout clamped at {} panels - deeper panels flow "
                   "into the deepest real one (begin/end stays balanced)",
                   LayoutCursor::kMaxPanels);
    }
    return true;
}

}  // namespace

// --- LayoutCursor: stack-based layout --------------------------------------------

Rect LayoutCursor::begin_panel(const Rect& bounds, float padding,
                               float spacing) {
    if (panel_stack_full(top_)) {
        // Balanced with the matching end_panel(): the depth counts on,
        // children flow into the deepest real panel, and this panel's
        // content rect is degenerate.
        ++top_;
        return {};
    }
    ++top_;
    Panel& panel = slot();
    // The content rect clamps empty, never backwards: a bounds too small
    // for its padding is a degenerate panel (the scroll viewport has
    // clamped this way all along; the layout panel now agrees).
    panel.content = {bounds.x + padding, bounds.y + padding,
                     std::max(0.0f, bounds.width - 2.0f * padding),
                     std::max(0.0f, bounds.height - 2.0f * padding)};
    panel.cursor_x = panel.content.x;
    panel.cursor_y = panel.content.y;
    panel.line_top = panel.content.y;
    panel.line_height = 0.0f;
    panel.spacing = std::max(0.0f, spacing);
    panel.last = {};
    panel.placed_any = false;
    panel.at_line_start = true;
    return panel.content;
}

Rect LayoutCursor::next(float w, float h, Align align) {
    if (top_ < 0) {
        return {};  // no panel open: nothing to place (drop-safe)
    }
    Panel& panel = slot();
    if (panel.at_line_start) {
        panel.line_height = 0.0f;  // this placement opens a fresh line
        panel.line_top = panel.cursor_y;
    }
    // Alignment places within the line height SO FAR, the widget counting
    // when it is the tallest, so a tall latecomer tops out at the line's
    // top instead of poking above it (earlier widgets never re-center -
    // place the tallest first for an even row; Align's doc has the rule).
    const float against = std::max(panel.line_height, h);
    float y = panel.line_top;
    if (align == Align::center) {
        y += (against - h) * 0.5f;
    } else if (align == Align::bottom) {
        y += against - h;
    }
    const Rect placed{panel.cursor_x, y, w, h};
    panel.last = placed;
    panel.placed_any = true;
    panel.line_height = std::max(panel.line_height, h);
    // Default: the next widget opens a fresh line below this line's
    // tallest, one spacing down.
    panel.cursor_x = panel.content.x;
    panel.cursor_y = panel.line_top + panel.line_height + panel.spacing;
    panel.at_line_start = true;
    return placed;
}

void LayoutCursor::same_line(float gap) {
    if (top_ < 0) {
        return;
    }
    Panel& panel = slot();
    if (!panel.placed_any) {
        return;  // nothing placed yet: nowhere to resume after
    }
    // A negative gap asks for the panel's own spacing; an explicit one
    // (tab_bar's flush 0) overrides it for this one seam.
    panel.cursor_x = panel.last.x + panel.last.width +
                     (gap < 0.0f ? panel.spacing : gap);
    panel.cursor_y = panel.line_top;  // the line's top, not the (possibly
                                      // aligned) last placement's y
    panel.at_line_start = false;  // resumed onto the current line
}

void LayoutCursor::end_panel() {
    if (top_ >= 0) {
        --top_;  // closing with none open is the documented no-op
    }
}

Rect LayoutCursor::panel_content() const {
    if (top_ < 0) {
        return {};
    }
    return panels_[static_cast<std::size_t>(
                       std::min(top_, kMaxPanels - 1))]
        .content;
}

float LayoutCursor::remaining_width() const {
    if (top_ < 0) {
        return 0.0f;  // no panel open: no line to fill (drop-safe)
    }
    const Panel& panel =
        panels_[static_cast<std::size_t>(std::min(top_, kMaxPanels - 1))];
    return std::max(0.0f, panel.content.right() - panel.cursor_x);
}

float LayoutCursor::flow_extent() const {
    if (top_ < 0) {
        return 0.0f;
    }
    const Panel& panel =
        panels_[static_cast<std::size_t>(std::min(top_, kMaxPanels - 1))];
    if (!panel.placed_any) {
        return 0.0f;  // an untouched panel flows nothing
    }
    // At a line start the cursor parks one spacing BELOW the last placed
    // line - the gap before a line that never came is not content; a
    // same_line() rewind parks it on the line's top with the line's
    // tallest still accumulated - the line's eventual bottom adds it.
    const float bottom = panel.at_line_start
                             ? panel.cursor_y - panel.spacing
                             : panel.cursor_y + panel.line_height;
    return std::max(0.0f, bottom - panel.content.y);
}

}  // namespace ws::ui::gui
