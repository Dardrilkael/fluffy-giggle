// ws::ui::gui - the gui library's out-of-line surface (see
// libs/ui/gui-lib-buildplan.md). The Phase 1-4 core is real: the shell
// accumulates one InputFrame per frame (frame_input()), IdStack hashes
// widget identity, GuiContext hit-tests hot/active with persistent side
// tables, and LayoutCursor flows widgets through stacked panels. Widgets
// themselves (Phase 5+) live in src/gui_widgets.cpp. The bodies stay
// out-of-line on purpose: apps resolve these symbols through the ws_ui
// archive, so a consumer compiling against the headers also proves the
// library links.

#include "gui.h"

#include <ws/core/log.hpp>

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/id_stack.hpp>
#include <ws/ui/gui/input_frame.hpp>
#include <ws/ui/gui/layout_cursor.hpp>

#include <algorithm>
#include <array>

namespace ws::ui::gui {
namespace {

// Process-wide gui state (the shell's input tap, the warn-once flags).
// Single-threaded BY DESIGN: the immediate-mode loop runs on the shell's
// main thread only, so these are plain globals on purpose - not a data
// race waiting to be "fixed" with atomics.

// The shell's accumulator while App::run() is active; null before startup
// and after shutdown.
InputFrame* g_active_input = nullptr;

// The answer frame_input() gives without a running app - a valid empty
// frame, so callers never see a dangling or partially filled snapshot.
const InputFrame& empty_input() {
    static const InputFrame empty;
    return empty;
}

// --- IdStack hashing ------------------------------------------------------------

// FNV-1a 64-bit: cheap, dependency-free, fine dispersion over the short
// strings widget labels come from. Scopes chain by seeding each mix with
// the parent scope's id, so an id encodes its whole scope path, not just
// its leaf label.
constexpr WidgetId kFnvPrime = 1099511628211ULL;
constexpr WidgetId kFnvOffsetBasis = 14695981039346656037ULL;

// The root scope (depth 0) seeds from a fixed nonzero constant - never 0,
// the invalid sentinel.
constexpr WidgetId kRootScopeId = kFnvOffsetBasis;

// Mixed in after every payload, so scope paths are delimited: pushing
// "ab" then "c" is a different path than pushing "a" then "bc".
constexpr char kScopeDelimiter = ';';

// A would-be zero hash maps here instead: no real widget may id as
// kInvalidWidgetId. The substitution adds no collision of its own.
constexpr WidgetId kZeroHash = 0x9E3779B97F4A7C15ULL;

// Depth overflow is a programming error - surfaced in the log rather than
// a crash; the stack clamps and this keeps it to one warn per process.
bool id_overflow_warned = false;

// True when the stack is full; warns the first time only.
bool id_stack_full(int depth) {
    if (depth < IdStack::kMaxDepth) {
        return false;
    }
    if (!id_overflow_warned) {
        id_overflow_warned = true;
        core::warn("ui: gui id stack clamped at depth {} - deeper scopes stop "
                   "deepening (push/pop stays balanced)",
                   IdStack::kMaxDepth);
    }
    return true;
}

WidgetId fnv_step(WidgetId hash, WidgetId byte) {
    return (hash ^ byte) * kFnvPrime;
}

// hash(scope, label): 'L' tags the payload family (a label never ids
// like an index), then the bytes, then the delimiter. Never returns 0.
WidgetId hash_label(WidgetId scope, const char* label) {
    WidgetId hash = fnv_step(scope, 'L');
    if (label == nullptr) {
        label = "";  // a null label ids like an empty one; no UB
    }
    for (const char* p = label; *p != '\0'; ++p) {
        hash = fnv_step(hash, static_cast<unsigned char>(*p));
    }
    hash = fnv_step(hash, static_cast<unsigned char>(kScopeDelimiter));
    return hash == 0 ? kZeroHash : hash;
}

// hash(scope, index): 'I' tags the family, then the value's bytes, low to
// high - a fixed order, never a stringified form.
WidgetId hash_index(WidgetId scope, int index) {
    WidgetId hash = fnv_step(scope, 'I');
    const auto value = static_cast<std::uint32_t>(index);
    for (int shift = 0; shift < 32; shift += 8) {
        hash = fnv_step(hash, (value >> shift) & 0xFFu);
    }
    hash = fnv_step(hash, static_cast<unsigned char>(kScopeDelimiter));
    return hash == 0 ? kZeroHash : hash;
}

// The scope id `depth` levels deep (0 = the fixed root seed). Past the
// clamp the slot index stops deepening with the counter, so overflowed
// scopes derive from the deepest remembered scope.
WidgetId scope_id(const std::array<WidgetId, IdStack::kMaxDepth>& stack,
                  int depth) {
    if (depth == 0) {
        return kRootScopeId;
    }
    const int slot = std::min(depth, IdStack::kMaxDepth) - 1;
    return stack[static_cast<std::size_t>(slot)];
}

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

// A widget id hit-tested twice in one frame means two widgets share
// identity (same-label siblings without a push(index) between them).
// Flagged once per process; the duplicate is tolerated after.
bool duplicate_id_warned = false;

}  // namespace

// --- InputFrame: one frame's snapshot (the shell fills, next_frame advances) ----

void InputFrame::next_frame() {
    mouse_pressed.fill(false);
    mouse_released.fill(false);
    scroll_dx = 0.0f;
    scroll_dy = 0.0f;
    key_pressed.fill(false);
    text_input.clear();
}

void set_active_input(InputFrame* input) {
    g_active_input = input;
}

const InputFrame& frame_input() {
    return g_active_input != nullptr ? *g_active_input : empty_input();
}

// --- IdStack: hash-based widget identity -----------------------------------------

void IdStack::push(const char* label) {
    if (id_stack_full(depth_)) {
        ++depth_;  // balanced with the matching pop; the scope stops deepening
        return;
    }
    stack_[static_cast<std::size_t>(depth_)] =
        hash_label(scope_id(stack_, depth_), label);
    ++depth_;
}

void IdStack::push(int index) {
    if (id_stack_full(depth_)) {
        ++depth_;  // balanced with the matching pop; the scope stops deepening
        return;
    }
    stack_[static_cast<std::size_t>(depth_)] =
        hash_index(scope_id(stack_, depth_), index);
    ++depth_;
}

void IdStack::pop() {
    if (depth_ > 0) {
        --depth_;  // popping an empty stack is the documented no-op
    }
}

WidgetId IdStack::current(const char* label) const {
    return hash_label(scope_id(stack_, depth_), label);
}

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

// --- GuiContext: hot/active hit-testing + persistent side tables ------------------

void GuiContext::new_frame(const InputFrame& input) {
    input_ = input;
    hot_id_ = kInvalidWidgetId;  // widgets re-claim hot as their hit tests run
    hit_ids_.clear();  // this frame's hit-test census starts empty
}

void GuiContext::end_frame() {
    // A release edge no hit_test consumed (the owner stopped running
    // mid-drag - the UI changed) ends the press here; stale ownership
    // would block the next frame's hot claims.
    if (active_id_ != kInvalidWidgetId && input_.mouse_released[0]) {
        active_id_ = kInvalidWidgetId;
    }
}

HitTest GuiContext::hit_test(WidgetId id, const Rect& rect) {
    // The invalid id claims nothing - it is the "no widget" sentinel.
    if (id == kInvalidWidgetId) {
        return {};
    }

    // One widget = one hit_test per frame. A repeat means two widgets
    // share identity (same-label siblings without a push(index) between
    // them) and will fight over hot and the side tables - flagged once
    // per process, tolerated after.
    if (!hit_ids_.insert(id).second && !duplicate_id_warned) {
        duplicate_id_warned = true;
        core::warn("ui: gui widget id hit-tested twice in one frame - "
                   "same-label siblings need ids().push(index) between them");
    }

    HitTest hit;
    const bool inside = contains(rect, input_.mouse_x, input_.mouse_y);

    // While a press is owned, the pointer is captured: only the owner
    // can be hot. Otherwise the widget under the pointer claims hot;
    // later widgets draw on top, so a later claim correctly wins.
    if (inside && (active_id_ == kInvalidWidgetId || active_id_ == id)) {
        hot_id_ = id;
    }
    // Hot, not merely inside: the frame has ONE interactive winner, so a
    // widget overlapped by somebody else's drag does not report hover.
    hit.hovered = hot_id_ == id;
    // Mouse-down inside claims the press, if none is owned yet.
    if (inside && input_.mouse_pressed[0] && active_id_ == kInvalidWidgetId) {
        active_id_ = id;
    }
    if (active_id_ == id) {
        hit.pressed = true;  // owns the live press, wherever the pointer is
        if (input_.mouse_released[0]) {
            hit.released = true;           // the drag ended, inside or out
            hit.clicked = inside;          // a full click only if inside
            active_id_ = kInvalidWidgetId;  // the press is over either way
        }
    }
    return hit;
}

}  // namespace ws::ui::gui
