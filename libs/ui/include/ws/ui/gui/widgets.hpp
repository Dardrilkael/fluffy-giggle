#pragma once

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/rect.hpp>
#include <ws/ui/gui/theme.hpp>

namespace ws::ui::gui {

// The immediate-mode widget set. Each call is one frame's worth of one
// widget: it lays out through the cursor, hit-tests through the context
// under its id, emits its draws through r2d at the context's current
// layer scope (base without a begin_layer), and returns the interaction
// result. The functions hold no state of their own - everything
// persistent lives in the GuiContext side tables - so a future
// retained-mode tree could drive the same calls unchanged.
//
// Id discipline: a widget's id comes from its label under the current
// scope, so same-label siblings in ONE scope share identity and fight
// over hot/active - give loop-generated children ids().push(index)
// between them. The context flags a duplicate id once per process.
//
// Calls are legal only between GuiContext::begin_frame and end_frame, and
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
//
// Buttons take the keyboard: they register focusable (a press on one
// takes the focus at the press edge, Tab reaches them), Enter and Space
// activate the focused button exactly like a click, and the focused
// button wears a 1px accent ring - the focus rules live on GuiContext
// (state/register_focusable/is_focused). Inside a begin_disabled scope
// the button goes dead as a whole: the control_disabled face, the icon
// tint and the label dimmed by theme.disabled_alpha through dim().
bool button(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
            const char* label, float width = 120.0f, r2d::Sprite icon = {});

// A checkbox: true on the frame a full click toggled `value` - the
// caller's bool IS the state, the widget itself holds none. The box is a
// control_height square (accent fill while on, corners rounded by
// theme.corner_radius) followed by the label
// bar; the hit area covers the whole row. The row auto-sizes to box +
// spacing + label. Checkboxes take the keyboard like buttons: the row
// registers focusable, Enter and Space toggle the focused checkbox
// exactly like a click, and the focused row wears the accent ring.
// Inside a begin_disabled scope the row dims as a whole (dead face on
// the box, fill and label faded) and no click or key can flip the
// value.
bool checkbox(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
              const char* label, bool& value);

// A horizontal slider, click-to-set AND drag: a press inside the track
// jumps the value to the pointer and tracks it while held; the value
// clamps to [min, max]. True on the frame the value moved. The track is
// control_height tall (rail, min-to-handle fill, handle; rail and fill
// rounded by theme.corner_radius); the label bar draws right-aligned over
// it. Sliders take the keyboard too: the row registers focusable, the
// focused row wears the ring, and the arrows step the focused slider -
// 1% of the range per press (slider_int: 1), clamped to [min, max],
// reported as changed only on a real move. Inside a begin_disabled
// scope the whole row dims (dead face on the rail, fill, handle and
// label faded) and no pointer or key can move the value.
bool slider_float(GuiContext& gui, LayoutCursor& layout,
                  const ChromeTheme& theme, const char* label, float& value,
                  float min, float max, float width = 120.0f);

// slider_float over an int reference - the value rounds to the nearest
// integer after the float math, and the arrows step it by 1.
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

// A tab strip: `count` tabs side by side on one line, `width` px each
// (the strip is count*width wide), theme.control_height tall. The
// caller's `active` IS the selection state - a full click on a tab
// writes the clicked index into it, and true returns only on the frame
// the selection MOVED (clicking the already-active tab reports false).
// An out-of-range `active` selects nothing until a click brings it back
// in range; count <= 0 places and ids nothing. Tabs id by INDEX under
// the strip's own scope (`label`), so duplicate tab labels inside one
// strip stay distinct widgets; the labels shape the centered bars like
// every other widget's. Tabs take the keyboard like buttons: each tab
// registers focusable in strip order (Tab walks the strip left to
// right), Enter or Space selects the focused tab exactly like a click,
// and the focused tab wears the ring. Inside a begin_disabled scope
// the strip dims as a whole - idle tabs take the dead face, the
// selected tab's accent and every label fade - and no click or key
// can move the selection. What shows per tab is the caller's branch -
// the strip is the selection primitive, not a content container.
bool tab_bar(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
             const char* label, const char* const* tabs, int count,
             int& active, float width = 120.0f);

// A scrollable panel: the same chrome, clip and id scope begin_panel
// pushes, plus a wheel-driven vertical scroll. The scroll offset lives
// in the context's persistent state (keyed by the panel's scope - the
// Phase 3 mechanism), so it survives frames AND the panel vanishing for
// frames (a tab that scrolls keeps its place), with no caller storage.
// While the pointer is inside the viewport the wheel moves it
// theme.scroll_speed px per unit (positive dy is wheel-up, toward the
// top), clamped to [0, content_height - viewport_height] as measured
// LAST frame - the immediate-mode answer to "this frame's content is
// not laid out yet" (so the very first frame cannot scroll, and a
// content that shrinks pulls the offset in one frame later). Children
// flow from the content origin shifted up by the offset - they draw AND
// hit-test at their scrolled positions - while the clip stays at the
// UNSCROLLED viewport, so the scissor cuts exactly what pokes past it.
// Returns the viewport (unshifted, clamped empty when the bounds cannot
// hold the padding): the window the content shows through. The matching
// end_scroll_panel() takes the theme too (it draws): it measures this
// frame's content extent, re-clamps, draws the scrollbar thumb (only
// while there is room to scroll), then pops the scope, clip and layout
// panel in reverse.
//
// v1 limits, deliberate: vertical only (scroll_dx is ignored); and the
// wheel scrolls every scroll viewport under the pointer, so overlapping
// regions are the caller's layout to avoid.
Rect begin_scroll_panel(GuiContext& gui, LayoutCursor& layout,
                        const ChromeTheme& theme, const char* label,
                        const Rect& bounds);
void end_scroll_panel(GuiContext& gui, LayoutCursor& layout,
                      const ChromeTheme& theme);

}  // namespace ws::ui::gui
