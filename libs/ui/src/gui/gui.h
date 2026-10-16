#pragma once

// Internal to ws::ui - never visible to apps. The warn-once flags owned
// outside gui_context.cpp: the seam reset_warn_once() re-arms (tests reach
// that function through the public header; the flags themselves never
// leave src/). The input plumbing this header once carried moved to
// ws::input with the input family (stage 3a): set_active_input is public
// in <ws/input/input.hpp> now, and the frame_input() glue lives in
// libs/input/src/input.cpp.

namespace ws::ui::gui {
namespace detail {

// The id-stack and panel-stack overflow warn-once flags (gui_id.cpp /
// gui_layout.cpp own the definitions). External linkage solely so
// reset_warn_once() - defined in gui_context.cpp - can re-arm them;
// single-threaded like every gui global.
extern bool id_overflow_warned;
extern bool panel_overflow_warned;

}  // namespace ws::ui::gui::detail

}  // namespace ws::ui::gui
