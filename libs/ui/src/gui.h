#pragma once

// Internal to ws::ui - never visible to apps. The shell's side of the gui
// input plumbing: app.cpp owns the per-frame InputFrame that the window
// callbacks fill (src/window.cpp, via WindowEventSink::input) and points
// the public ws::ui::gui::frame_input() at it for the duration of
// App::run(). The public surface stays <ws/ui/gui/input_frame.hpp>.

#include <ws/ui/gui/input_frame.hpp>

namespace ws::ui::gui {

// Points frame_input() at the shell's live accumulator for the duration of
// App::run(), and clears it at shutdown - after which frame_input()
// reports an empty frame (drop-safe, like r2d::frame_size()).
void set_active_input(InputFrame* input);

}  // namespace ws::ui::gui
