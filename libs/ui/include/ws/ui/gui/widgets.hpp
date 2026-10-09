#pragma once

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/rect.hpp>
#include <ws/ui/gui/theme.hpp>

#include <cstddef>

namespace ws::ui::gui {

// The immediate-mode widget set. Each call is one frame's worth of one
// widget: it lays out through the cursor, hit-tests through the context
// under its id, emits its draws through r2d at the context's current
// layer scope (base without a begin_layer), and returns the interaction
// result. The static chrome - text, separator, progress - is the
// exception on the last two seams: no id to hit-test under, nothing to
// return, and no GuiContext in the signature at all, which is how the
// set spells "this one cannot interact with you".
// The functions hold no state of their own - everything
// persistent lives in the GuiContext side tables - so a future
// retained-mode tree could drive the same calls unchanged.
//
// Id discipline: a widget's id comes from its label under the current
// scope, so same-label siblings in ONE scope share identity and fight
// over hot/active - give loop-generated children ids().push(index)
// between them. The context flags a duplicate id once per process.
// A label's "##" suffix ids without drawing ("Save##one" shows "Save"),
// and "###" redirects the id to the marker onward so the display can
// change while the identity holds - see label_display and IdStack::id.
//
// Calls are legal only between GuiContext::begin_frame and end_frame, and
// the draws land in the caller's r2d begin()/end() block (screen mode:
// the same space layout and hit-testing use). Every draw is drop-safe
// headless, so the widgets test without a window.

// fit_text's answer: the text to draw and its advance. `text` points at
// the caller's own label bytes when nothing needed changing, or at the
// caller's scratch buffer when something did - never anywhere else - so
// it goes straight to r2d::draw_text with no copy and no allocation
// (the gui never allocates mid-frame).
struct LabelFit {
    const char* text = nullptr;
    float advance = 0.0f;
};

// Fits a label for drawing: the display text (label_display's cut -
// "##" id suffixes never draw) truncated to `max_width` on a character
// boundary, ending in ASCII "..." when anything was cut. A negative
// max_width skips the width pass (marker-cut only - the spelling for a
// caller sizing a row to its label). Measures through the theme font
// when one is set - prefix by prefix, the honest glyph advances - or by
// theme.label_char_width per character when it is not. `scratch` (with
// its byte count) bounds the copy; it is written only when the display
// was cut or truncated. Nothing fitting - not even "..." alone - draws
// nothing: empty text, a 0 advance. Never returns null text.
LabelFit fit_text(const char* label, float max_width,
                  const ChromeTheme& theme, char* scratch,
                  std::size_t scratch_bytes);

// A push button: true on a full click inside it (press and release over
// the same laid-out rect). `theme` supplies the faces and metrics
// (height = theme.control_height, corners = theme.corner_radius);
// `label` ids the widget - derived from the current id scope, so the same
// label in two panels is two widgets - and is drawn as a centered bar that
// tracks the label's length: real text needs a font, which r2d does not
// have yet. `icon` (a Sprite from load_atlas; the default {} draws none)
// renders as a square at the left, inset clear of the rounded corners and
// tinted white so the asset's own colors show; the label bar centers in
// the room the icon leaves. `width` defaults to 120 px; a negative width
// instead FILLS the rest of the current line (the panel content's
// remaining run, from the cursor to the content's right edge).
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

// A radio button: true on the frame a click or Enter/Space MOVED the
// group's selection to this option. `value` is the caller's group state
// (one int shared by every radio of the group), `this_option` the value
// a selection writes; an interaction on the already-selected option is
// an idempotent write that reports false - true means "the selection is
// this option now, and it was not a frame ago", the answer a caller
// branching on the change wants. The dot is a control_height circle
// with the faces a button wears; the selection is an accent circle
// inset like the checkbox's fill. The row auto-sizes to dot + spacing +
// the label's display advance and the hit area covers it whole. Radios
// take the keyboard like checkboxes: focusable, Enter and Space select,
// the focused row wears the ring. Inside a begin_disabled scope the row
// dims as one and no click or key can move the selection.
bool radio(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
           const char* label, int& value, int this_option);

// A toggle switch: checkbox semantics under a switch look - true on the
// frame a full click or Enter/Space flipped `value` (the caller's bool
// is the state; the widget holds none). The track is a pill twice the
// control height wide and the height tall: the accent face while on
// (dimmed under a disabled scope, not deadened - the state stays
// readable, like the checkbox's fill) and the control faces while off.
// The knob is a near-white circle snapped to its end - off left, on
// right; no travel animation in v1 (the frame clock's first consumer
// is the text caret, a later phase). The row auto-sizes to track +
// spacing + the label's display advance, the hit area covers it whole,
// the focused row wears the ring, and a begin_disabled scope dims and
// deadens it as one.
bool toggle_switch(GuiContext& gui, LayoutCursor& layout,
                   const ChromeTheme& theme, const char* label, bool& value);

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
// label faded) and no pointer or key can move the value. `width`
// defaults to 120 px; a negative width instead FILLS the rest of the
// current line, like button's.
bool slider_float(GuiContext& gui, LayoutCursor& layout,
                  const ChromeTheme& theme, const char* label, float& value,
                  float min, float max, float width = 120.0f);

// slider_float over an int reference - the value rounds to the nearest
// integer after the float math, and the arrows step it by 1. A negative
// width fills the rest of the current line, like slider_float's.
bool slider_int(GuiContext& gui, LayoutCursor& layout,
                const ChromeTheme& theme, const char* label, int& value,
                int min, int max, float width = 120.0f);

// A static label: lays out and draws, and that is all - no id, no
// hit-test, nothing focusable, no GuiContext in the signature (chrome,
// like a panel's frame; a caller wanting a faded label dims its own
// theme copy through dim()). The row is control_height tall. A negative
// `width` sizes the row to the label's DISPLAY advance - "##" suffixes
// never draw and never size anything here, and nothing truncates, like
// a checkbox's sizing fit; any other width fixes the row and truncates
// the label to it ("..." on the cut). Draws like every widget label:
// the theme font, or the fontless bar at the fitted advance.
void text(LayoutCursor& layout, const ChromeTheme& theme,
          const char* label, float width = -1.0f);

// A horizontal rule: a theme.border-colored 2-px line (a hardcoded
// pixel like the focus ring's 1, not a theme metric) with the flow's
// own spacing for air on both sides. A negative `width` fills the rest
// of the current line, like button's.
void separator(LayoutCursor& layout, const ChromeTheme& theme,
               float width = -1.0f);

// A progress bar: a control_height track in the control face with
// `fraction` of it filled by the accent - clamped to [0, 1], and a NaN
// reads as empty (the inverted comparisons catch it). Chrome like text:
// no id, no hit-test. A negative `width` fills the rest of the current
// line, like button's; the fill rounds with the theme's corners and
// clamps to its own half-height when the fraction runs thin, so a
// sliver reads as a dot rather than an over-rounded bar.
void progress(LayoutCursor& layout, const ChromeTheme& theme,
              float fraction, float width = -1.0f);

// A panel: chrome (1px border frame + panel face), a clip rect at its
// content area (children cannot draw outside it, whatever they submit), an
// id scope (children id under the panel, so same-label widgets inside and
// outside stay distinct), and a layout panel (children flow from the
// content's top-left, inset by theme.padding, spaced theme.spacing apart
// - between lines and between same_line() items; theme.spacing = 0 is
// the unspaced look). Returns the content rect; the matching end_panel()
// pops all three in reverse - every pop is drop-safe against its begin
// having been dropped. The panel is chrome, not a widget: it draws and
// clips but does not capture input (that arrives when panels grow drag
// headers).
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
// pushes (and its theme.spacing item gaps - the virtual panel the
// content flows in carries them), plus wheel-driven and touch-dragged
// vertical scroll. The scroll offset lives in the context's persistent
// state (keyed by the panel's scope - the Phase 3 mechanism), so it
// survives frames AND the panel vanishing for frames (a tab that
// scrolls keeps its place), with no caller storage.
//
// The wheel is ARBITRATED: every panel (plain or scrollable) registers
// as a wheel claimant each frame, and the scroll panel consumes only
// when it won last frame's resolution - so nested scroll panels stop
// double-scrolling (the innermost bounds under the pointer wins), a
// popup panel over the panel eats the wheel, and a modal veil kills it
// for everything below. The settle costs one frame: the first notch
// after the cursor arrives over a panel settles the ownership instead
// of scrolling (GuiContext::register_wheel_claim documents the rules).
// The touch drag is singular the same way: a fresh press inside the
// viewport is watched, and past theme.drag_threshold the scroll takes
// the finger (cancel_pointer ends the child's press with no click) -
// but only the panel that won the pointer's arm claim drags, so
// nested panels never both scroll. The offset moves
// theme.scroll_speed px per wheel unit (positive dy is wheel-up, toward
// the top), clamped to [0, content_height - viewport_height] as
// measured LAST frame - the immediate-mode answer to "this frame's
// content is not laid out yet" (so the very first frame cannot scroll,
// and a content that shrinks pulls the offset in one frame later).
// Children flow from the content origin shifted up by the offset -
// they draw AND hit-test at their scrolled positions - while the clip
// stays at the UNSCROLLED viewport, so the scissor cuts exactly what
// pokes past it. Tab-focus lands in view too: the frame after the
// keyboard focus moves onto a widget inside the panel, the panel
// shifts the offset the minimum that pulls the widget into the
// viewport (see GuiContext::focus_into_view_pending).
// Returns the viewport (unshifted, clamped empty when the bounds
// cannot hold the padding): the window the content shows through. The
// matching end_scroll_panel() takes the theme too (it draws): it
// measures this frame's content extent, re-clamps, runs the Tab-focus
// into-view shift, and draws the scrollbar thumb (only while there is
// room to scroll). The thumb is GRABBABLE - the right gutter band is
// its hit area, a press there ends any child press beneath it, and the
// drag maps the pointer 1:1 through the track - then pops the scope,
// clip and layout panel in reverse.
//
// v1 limits, deliberate: vertical only (scroll_dx is ignored).
Rect begin_scroll_panel(GuiContext& gui, LayoutCursor& layout,
                        const ChromeTheme& theme, const char* label,
                        const Rect& bounds);
void end_scroll_panel(GuiContext& gui, LayoutCursor& layout,
                      const ChromeTheme& theme);

}  // namespace ws::ui::gui
