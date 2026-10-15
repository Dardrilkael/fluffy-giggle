#pragma once

// Internal to ws::ui - never visible to apps. The shell's side of the gui
// input plumbing: app.cpp owns the per-frame InputFrame that the window
// callbacks fill (the platform window TU, via WindowEventSink::input) and
// points the public ws::ui::gui::frame_input() at it for the duration of
// App::run(). The public surface stays <ws/ui/input.hpp>.
// Also internal: the warn-once flags owned outside gui_context.cpp - the
// seam reset_warn_once() re-arms (tests reach that function through the
// public header; the flags themselves never leave src/).

#include <ws/ui/input.hpp>

namespace ws::ui::gui {

// Points frame_input() at the shell's live accumulator for the duration of
// App::run(), and clears it at shutdown - after which frame_input()
// reports an empty frame (drop-safe, like r2d::frame_size()).
void set_active_input(InputFrame* input);

namespace detail {

// The id-stack and panel-stack overflow warn-once flags (gui_id.cpp /
// gui_layout.cpp own the definitions). External linkage solely so
// reset_warn_once() - defined in gui_context.cpp - can re-arm them;
// single-threaded like every gui global (see gui.cpp).
extern bool id_overflow_warned;
extern bool panel_overflow_warned;

}  // namespace ws::ui::gui::detail

}  // namespace ws::ui::gui
