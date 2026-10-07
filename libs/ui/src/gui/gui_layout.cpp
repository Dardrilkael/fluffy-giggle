// LayoutCursor's out-of-line half: stacked flow panels. The panel-stack
// overflow warn-once flag is local to this TU.

#include <ws/ui/gui/layout_cursor.hpp>

#include <ws/core/log.hpp>

#include <algorithm>

namespace ws::ui::gui {
namespace {

// --- LayoutCursor panels ---------------------------------------------------------

// Depth overflow is a programming error - surfaced in the log rather than
// a crash; the panel stack clamps and this keeps it to one warn per process.
bool panel_overflow_warned = false;

// True when the panel stack is full; warns the first time only.
bool panel_stack_full(int top) {
    if (top + 1 < LayoutCursor::kMaxPanels) {
        return false;
    }
    if (!panel_overflow_warned) {
        panel_overflow_warned = true;
        core::warn("ui: gui layout clamped at {} panels - deeper panels flow "
                   "into the deepest real one (begin/end stays balanced)",
                   LayoutCursor::kMaxPanels);
    }
    return true;
}

}  // namespace

// --- LayoutCursor: stack-based layout --------------------------------------------

Rect LayoutCursor::begin_panel(const Rect& bounds, float padding) {
    if (panel_stack_full(top_)) {
        // Balanced with the matching end_panel(): the depth counts on,
        // children flow into the deepest real panel, and this panel's
        // content rect is degenerate.
        ++top_;
        return {};
    }
    ++top_;
    Panel& panel = slot();
    panel.content = {bounds.x + padding, bounds.y + padding,
                     bounds.width - 2.0f * padding,
                     bounds.height - 2.0f * padding};
    panel.cursor_x = panel.content.x;
    panel.cursor_y = panel.content.y;
    panel.last = {};
    panel.line_height = 0.0f;
    panel.placed_any = false;
    panel.at_line_start = true;
    return panel.content;
}

Rect LayoutCursor::next(float w, float h) {
    if (top_ < 0) {
        return {};  // no panel open: nothing to place (drop-safe)
    }
    Panel& panel = slot();
    if (panel.at_line_start) {
        panel.line_height = 0.0f;  // this placement opens a fresh line
    }
    const Rect placed{panel.cursor_x, panel.cursor_y, w, h};
    panel.last = placed;
    panel.placed_any = true;
    panel.line_height = std::max(panel.line_height, h);
    // Default: the next widget opens a fresh line below this line's tallest.
    panel.cursor_x = panel.content.x;
    panel.cursor_y += panel.line_height;
    panel.at_line_start = true;
    return placed;
}

void LayoutCursor::same_line() {
    if (top_ < 0) {
        return;
    }
    Panel& panel = slot();
    if (!panel.placed_any) {
        return;  // nothing placed yet: nowhere to resume after
    }
    panel.cursor_x = panel.last.x + panel.last.width;
    panel.cursor_y = panel.last.y;
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

float LayoutCursor::flow_extent() const {
    if (top_ < 0) {
        return 0.0f;
    }
    const Panel& panel =
        panels_[static_cast<std::size_t>(std::min(top_, kMaxPanels - 1))];
    // At a line start the cursor sits below the last placed line; a
    // same_line() rewind parks it on the line's top with the line's
    // tallest still accumulated - the line's eventual bottom adds it.
    const float bottom = panel.at_line_start
                             ? panel.cursor_y
                             : panel.cursor_y + panel.line_height;
    return bottom - panel.content.y;
}

}  // namespace ws::ui::gui
