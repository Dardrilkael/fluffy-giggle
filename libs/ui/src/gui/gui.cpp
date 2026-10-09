// ws::ui::gui - the gui library's out-of-line surface (see
// libs/ui/gui-lib-buildplan.md), one file per concern in src/gui/: this
// file is the shell glue only - the live accumulator frame_input() reads
// while App::run() is active. gui_input.cpp carries the InputFrame,
// gui_id.cpp the IdStack hashing, gui_layout.cpp the LayoutCursor panels,
// gui_context.cpp the GuiContext hit-testing; widgets themselves (Phase
// 5+) live in src/gui/gui_widgets.cpp. The bodies stay out-of-line on
// purpose: apps resolve these symbols through the ws_ui archive, so a
// consumer compiling against the headers also proves the library links.

#include "gui.h"

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

}  // namespace

void set_active_input(InputFrame* input) {
    g_active_input = input;
}

const InputFrame& frame_input() {
    return g_active_input != nullptr ? *g_active_input : empty_input();
}

}  // namespace ws::ui::gui
