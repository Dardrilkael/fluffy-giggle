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
// Id discipline: a widget's id comes from its label under the current
// scope, so same-label siblings in ONE scope share identity and fight
// over hot/active - give loop-generated children ids().push(index)
// between them. The context flags a duplicate id once per process.
//
// Calls are legal only between GuiContext::new_frame and end_frame, and
// the draws land in the caller's r2d begin()/end() block (screen mode:
// the same space layout and hit-testing use). Every draw is drop-safe
// headless, so the widgets test without a window.

// A push button: true on a full click inside it (press and release over
// the same laid-out rect). `theme` supplies the faces and metrics
// (height = theme.control_height, corners = theme.corner_radius);
// `label` ids the widget - derived from the current id scope, so the same
// label in two panels is two widgets - and is drawn as a centered bar that
// tracks the label's length: real text needs a font, which r2d does not
// have yet. `icon` (a Sprite from load_atlas; the default {} draws none)
// renders as a square at the left, inset clear of the rounded corners and
// tinted white so the asset's own colors show; the label bar centers in
// the room the icon leaves. `width` defaults to 120 px.
bool button(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
            const char* label, float width = 120.0f, r2d::Sprite icon = {});

// A checkbox: true on the frame a full click toggled `value` - the
// caller's bool IS the state, the widget itself holds none. The box is a
// control_height square (accent fill while on, corners rounded by
// theme.corner_radius) followed by the label
// bar; the hit area covers the whole row. The row auto-sizes to box +
// spacing + label.
bool checkbox(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
              const char* label, bool& value);

// A horizontal slider, click-to-set AND drag: a press inside the track
// jumps the value to the pointer and tracks it while held; the value
// clamps to [min, max]. True on the frame the value moved. The track is
// control_height tall (rail, min-to-handle fill, handle; rail and fill
// rounded by theme.corner_radius); the label bar draws right-aligned over
// it.
bool slider_float(GuiContext& gui, LayoutCursor& layout,
                  const ChromeTheme& theme, const char* label, float& value,
                  float min, float max, float width = 120.0f);

// slider_float over an int reference - the value rounds to the nearest
// integer after the float math.
bool slider_int(GuiContext& gui, LayoutCursor& layout,
                const ChromeTheme& theme, const char* label, int& value,
                int min, int max, float width = 120.0f);

// A panel: chrome (1px border frame + panel face), a clip rect at its
// content area (children cannot draw outside it, whatever they submit), an
// id scope (children id under the panel, so same-label widgets inside and
// outside stay distinct), and a layout panel (children flow from the
// content's top-left, inset by theme.padding). Returns the content rect;
// the matching end_panel() pops all three in reverse - every pop is
// drop-safe against its begin having been dropped. The panel is chrome,
// not a widget: it draws and clips but does not capture input (that
// arrives when panels grow drag headers).
Rect begin_panel(GuiContext& gui, LayoutCursor& layout,
                 const ChromeTheme& theme, const char* label,
                 const Rect& bounds);
void end_panel(GuiContext& gui, LayoutCursor& layout);

}  // namespace ws::ui::gui
