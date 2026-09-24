#pragma once

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/rect.hpp>
#include <ws/ui/gui/theme.hpp>

namespace ws::ui::gui {

// The immediate-mode widget set. Each call is one frame's worth of one
// widget: it lays out through the cursor, hit-tests through the context
// under its id, emits its draws through r2d at the caller's current
// layer, and returns the interaction result. The functions hold no
// state of their own - everything persistent lives in the GuiContext
// side tables - so a future retained-mode tree could drive the same
// calls unchanged.
//
// Calls are legal only between GuiContext::new_frame and end_frame, and
// the draws land in the caller's r2d begin()/end() block (screen mode:
// the same space layout and hit-testing use). Every draw is drop-safe
// headless, so the widgets test without a window.

// A push button: true on a full click inside it (press and release over
// the same laid-out rect). `theme` supplies the faces and metrics
// (height = theme.control_height); `label` ids the widget - derived from
// the current id scope, so the same label in two panels is two widgets -
// and is drawn as a centered bar that tracks the label's length: real
// text needs a font, which r2d does not have yet. `width` defaults to
// 120 px.
bool button(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
            const char* label, float width = 120.0f);

}  // namespace ws::ui::gui
