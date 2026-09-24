// ws::ui::gui - the gui library's out-of-line surface (see
// libs/ui/gui-lib-buildplan.md). The input plumbing is real (Phase 1): the
// shell accumulates one InputFrame per frame and publishes it through
// frame_input(); GuiContext::new_frame adopts it. Ids (Phase 2),
// hit-testing (Phase 3) and layout (Phase 4) are still declared stubs. The
// bodies stay out-of-line on purpose: apps resolve these symbols through
// the ws_ui archive, so a consumer compiling against the headers also
// proves the library links.

#include "gui.h"

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/id_stack.hpp>
#include <ws/ui/gui/input_frame.hpp>
#include <ws/ui/gui/layout_cursor.hpp>

namespace ws::ui::gui {
namespace {

// The shell's accumulator while App::run() is active; null before startup
// and after shutdown.
InputFrame* g_active_input = nullptr;

// The answer frame_input() gives without a running app - a valid empty
// frame, so callers never see a dangling or partially filled snapshot.
const InputFrame& empty_input() {
    static const InputFrame empty;
    return empty;
}

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

// --- IdStack (Phase 2: hash-based ids) -------------------------------------------

void IdStack::push(const char*) {}

void IdStack::push(int) {}

void IdStack::pop() {}

WidgetId IdStack::current(const char*) const { return kInvalidWidgetId; }

// --- LayoutCursor (Phase 4: stack layout) -----------------------------------------

Rect LayoutCursor::begin_panel(const Rect&, float) { return {}; }

Rect LayoutCursor::next(float, float) { return {}; }

void LayoutCursor::same_line() {}

void LayoutCursor::end_panel() {}

// --- GuiContext (hot/active + side tables land in Phase 3) -----------------------

void GuiContext::new_frame(const InputFrame& input) {
    input_ = input;  // adopt the snapshot; hover resets join in Phase 3
}

void GuiContext::end_frame() {}

HitTest GuiContext::hit_test(WidgetId, const Rect&) { return {}; }

}  // namespace ws::ui::gui
