#pragma once

// Internal to ws::ui - never visible to apps.
//
// The warn-once flags behind the id-stack, panel-stack, toast-store and
// menu-stack overflow warnings: each warning prints once, then stays quiet
// until reset_warn_once() re-arms it (defined in gui_context.cpp; tests
// reach it through the public header, the flags themselves never leave
// src/). The flags have external linkage solely so that one function can
// reach them. Single-threaded, like every gui global.

namespace ws::ui::gui::detail {

extern bool id_overflow_warned;     // defined in gui_id.cpp
extern bool panel_overflow_warned;  // defined in gui_layout.cpp
extern bool toast_overflow_warned;  // defined in widgets_toasts.cpp
extern bool menu_overflow_warned;

}  // namespace ws::ui::gui::detail
