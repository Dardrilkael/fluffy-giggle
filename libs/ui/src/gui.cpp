// ws::ui::gui - Phase 0 skeleton (see libs/ui/gui-lib-buildplan.md).
// Every body below is a declared stub; the plan fills them in phase by
// phase (Phase 1 input, Phase 2 ids, Phase 3 hit-testing, Phase 4
// layout). The bodies stay out-of-line on purpose: apps resolve these
// symbols through the ws_ui archive, so a consumer compiling against
// the headers also proves the library links.

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/id_stack.hpp>
#include <ws/ui/gui/input_frame.hpp>
#include <ws/ui/gui/layout_cursor.hpp>

namespace ws::ui::gui {

// --- InputFrame (Phase 1: platform layer fills + advances) ---------------------

void InputFrame::next_frame() {}

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

// --- GuiContext (Phase 1 frames, Phase 3 hot/active + side tables) ---------------

void GuiContext::new_frame(const InputFrame&) {}

void GuiContext::end_frame() {}

HitTest GuiContext::hit_test(WidgetId, const Rect&) { return {}; }

}  // namespace ws::ui::gui
