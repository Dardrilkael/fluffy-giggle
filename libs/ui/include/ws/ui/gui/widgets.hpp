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
// right; no travel animation in v1 (the frame clock's first consumer,
// the text input's caret blink, spends the tick on typing instead).
// The row auto-sizes to track +
// spacing + the label's display advance, the hit area covers it whole,
// the focused row wears the ring, and a begin_disabled scope dims and
// deadens it as one.
bool toggle_switch(GuiContext& gui, LayoutCursor& layout,
                   const ChromeTheme& theme, const char* label, bool& value);

// A collapsing header: a row that flips `open` on a click or Enter/
// Space (the caller's bool IS the state) and returns it AFTER the flip,
// so the content is the caller's `if` branch, exactly a tab strip's -
// the header lays out only its own row. The marker square at the left
// is the faint border color closed and the accent open; the label sits
// beside it like a checkbox's, left-aligned. A negative `width` fills
// the rest of the line (the default - headers want their container's
// width). The same citizenship as a checkbox: focusable, the ring, and
// a begin_disabled scope dims and deadens it as one.
bool collapsing_header(GuiContext& gui, LayoutCursor& layout,
                       const ChromeTheme& theme, const char* label,
                       bool& open, float width = -1.0f);

// A selectable row: true on a full click or Enter/Space - the selection
// is the CALLER'S to keep (`selected` is display state the row reads
// and never writes, so a click on the already-selected row still
// reports true, unlike a radio - multi-select lists build on exactly
// that). The selected row wears a 3-px accent bar down its left edge
// (the v1 look, a hardcoded pixel like the separator's 2); an idle
// unselected row draws NO face at all - the bare list look - while
// hover, press and the disabled scope take the usual faces. The label
// draws left-aligned past the bar's room, so selected and unselected
// rows align. A negative `width` fills the rest of the line (the
// default - list rows want their container's width).
bool selectable(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
               const char* label, bool selected, float width = -1.0f);

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

// A drag float: press inside to GRAB (the value does not jump - unlike
// a slider there is nothing to click-to-set), then while the pointer
// holds the press the value follows its horizontal travel at `speed`
// per pixel, anchored where the grab landed, clamped to [min, max].
// True on the frame the value moved. The grab outlives sweeps outside
// the row (the press still owns it) and ends at the lift - and a plain
// click (press and release with no travel) never moves the value. The
// anchor lives in the context's persistent state under the widget's
// id, so it survives frames and clear_persistent_state evicts it. The
// row draws "label: value" (three decimals, the v1 spelling) fitted
// and left-aligned. The keyboard steps the focused row 1% of the range
// per arrow press, like a slider. Inside a begin_disabled scope the row
// dims as one and no pointer or key can move the value. `width`
// defaults to 120 px; a negative width fills the rest of the line.
bool drag_float(GuiContext& gui, LayoutCursor& layout,
                const ChromeTheme& theme, const char* label, float& value,
                float min, float max, float speed = 1.0f,
                float width = 120.0f);

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

// --- the overlay scopes ---------------------------------------------------------
// The three floating panels: popup, tooltip, modal. All take the same shape
// as begin_panel - chrome, clip, id scope, layout panel - and add the two
// things a panel floating OVER other content needs: a LAYER stratum
// (children draw and claim above base content through the layer rules) and
// an ESCAPING clip pair (r2d::push_clip_escape and its hit-testing twin,
// pushed at the same rect like every panel clip: the overlay draws AND
// hit-tests past the clip of the panel it floats over - a dropdown near its
// host's edge would shear off and go unclickable there otherwise). All
// three answer the caller's condition in the bool they return, so the
// content lives in the caller's `if` branch - and the matching end lives
// INSIDE that branch, right after the last child: a begin that answered
// false pushed nothing, and though the layer guard makes a stray end a
// no-op anyway, the discipline keeps the scopes reading balanced.

// A popup: the floating panel combo dropdowns, context menus and their
// kin open above base content. `open` is the caller's bool (the caller
// owns the state, like every widget here); begin_popup returns it back and
// the content is the caller's `if` branch:
//
//     if (button(gui, layout, theme, "menu", 60.0f)) menu_open = !menu_open;
//     if (begin_popup(gui, layout, theme, "menu", anchor_rect, menu_rect,
//                     menu_open)) {
//         if (selectable(gui, layout, theme, "cut", false)) { ... }
//         end_popup(gui, layout);
//     }
//
// While open the scope pushes: the popup stratum (a full click on a
// popup child beats whatever sits underneath), the chrome at `bounds`,
// the escaping clip pair at the bounds, the content clip pair inside it
// (children nest against the escaped bounds as usual), an id scope under
// `label`, and a keyless wheel claim at the bounds (begin_panel's eat-
// not-consume claim: a popup over a scroll panel takes the wheel).
// `anchor` is the control the popup hangs from: a fresh press that lands
// outside `bounds` AND outside the anchor closes the popup - `open`
// flips false and begin_popup returns false on that same frame, so the
// content vanishes while the press carries on to whatever sits beneath
// it (the click passes through). A press inside the anchor is NOT a
// close: that click belongs to the anchor's own widget (a combo row
// toggles itself). A degenerate `anchor` (a zero rect) hangs from
// nothing - every press outside the bounds then closes. Touches close
// it too, per pressed contact. Under a raised modal veil the popup is
// inert like every lower stratum - its close rule eats no press the
// veil owns. v1 limits, deliberate: no Escape or focus-loss close (no
// such events exist), no auto-flip above the anchor when the frame
// lacks room (the caller places `bounds`), and a press on the popup's
// own padding - not on a child - falls through to whatever sits beneath
// (fill-width children leave almost no padding to press).
bool begin_popup(GuiContext& gui, LayoutCursor& layout,
                 const ChromeTheme& theme, const char* label,
                 const Rect& anchor, const Rect& bounds, bool& open);
// Closes the popup scope begin_popup opened - the caller calls it inside
// the `if` branch, right after the last child (see the idiom above).
// Drop-safe through the layer: an end_popup with no popup stratum open (a
// begin that answered false, a stray call) is a no-op.
void end_popup(GuiContext& gui, LayoutCursor& layout);

// A tooltip: chrome floating at the tooltip stratum - the panel scope for
// a hint that hovers above everything else. `show` is the caller's
// condition (typically an is_hot() check on the widget being explained);
// the content is the caller's `if` branch, the same idiom as
// begin_popup's. The scope pushes the tooltip stratum, the chrome at
// `bounds`, an escaping r2d clip at the bounds (the hint draws past any
// panel) and an id scope - but NO hit clip, no wheel claim and no open
// state: a tooltip never claims, never closes on anything, and the widget
// it hovers over keeps its hot. Nothing inside is meant to be
// interactive; a caller who puts a live widget in one gets exactly what
// they asked for (it claims at the tooltip stratum, above everything,
// unclipped by any hit clip).
bool begin_tooltip(GuiContext& gui, LayoutCursor& layout,
                   const ChromeTheme& theme, const char* label,
                   const Rect& bounds, bool show);
// Closes the tooltip scope. Drop-safe through the layer, like end_popup.
void end_tooltip(GuiContext& gui, LayoutCursor& layout);

// A modal dialog: the blocking overlay. `open` is the caller's bool BY
// VALUE - the modal never closes itself (no outside-press rule: blocking
// IS the point; the dialog's own buttons flip the caller's bool). While
// open the scope paints theme.veil over the whole frame - the veil the
// input rules already enforce, made visible, drawn under its own
// fullscreen clip escape so it covers the frame even when the modal
// opens inside a panel scope - and pushes the modal stratum, under
// which strictly lower strata go inert: no hover, no hot, no fresh
// press, the wheel dies, the keyboard is trapped (Tab wraps within the
// modal's own widgets) and the focus stashed at the veil's rise comes
// back when it lifts (the begin_layer rules - see GuiContext::
// begin_layer). On top of the veil the scope is a panel like
// begin_popup's: chrome at `bounds`, the escaping clip pairs, a keyless
// wheel claim, an id scope. The content and end_modal live in the
// caller's `if` branch, the same idiom as begin_popup's.
bool begin_modal(GuiContext& gui, LayoutCursor& layout,
                 const ChromeTheme& theme, const char* label,
                 const Rect& bounds, bool open);
// Closes the modal scope. Drop-safe through the layer, like end_popup -
// but note the VEIL the scope raised outlives this end: it is frame-wide
// and lifts at the next begin_frame (see begin_layer).
void end_modal(GuiContext& gui, LayoutCursor& layout);

// A combo box: the popup's row-widget shape - ONE widget under one
// label, the row and the dropdown together. The row wears the button
// faces (a marker square at its right edge says "opens": the faint
// border closed, the accent open, the collapsing_header's marker
// mirrored) and draws "label: item" like a drag_float composes
// "label: value" (the display cut first; no item in range or count 0
// shows the label alone). A click on the row toggles `open` (the
// caller's bool, by reference); Enter/Space on the focused row toggles
// it too, like a header. The dropdown is a begin_popup the widget
// places ITSELF: below the row while the frame has room (flush at the
// row's bottom, one control_height per item plus the panel padding and
// spacing, the row's width), else flipped above the row, clamped at
// the frame's edges - a frame too short for every item shows what fits
// (the popup's clip cuts the rest; the caller places nothing). A click
// on an item writes `selected` (the caller's int, by reference) and
// answers true ONLY when the selection MOVED - a re-click on the
// current item is a no-op close, the radio's answer - and closes the
// dropdown; the row click that opened it and the outside-press close
// are begin_popup's rules unmodified (the row IS the anchor). `items`
// is a `const char* const*` array, like tab_bar's; `count` 0 or a
// `selected` out of range lay a bare row that opens nothing (a combo
// with nothing to choose does not pretend otherwise); the items id
// under the row's label scope with a pushed index, so one label list
// works anywhere. `width` lays the row like a button's (negative =
// fill the line). Keyboard: the items are citizens like every row -
// while the dropdown is open Tab reaches them in list order, Enter/
// Space picks the focused one, and a choice hands the focus back to
// the combo row so the next Tab continues the screen's walk (an
// outside-press close leaves it wherever the pass-through press
// landed - the focus-follows-press rule, no help wanted).
bool combo(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
          const char* label, const char* const* items, int count,
          int& selected, bool& open, float width = -1.0f);

// A text input: one editable line in a caller-owned buffer. `buf` is
// NUL-terminated UTF-8 the caller owns (the house rule - the widget
// holds no copy), `capacity` counts the NUL, and the answer is true on
// the frame the text CHANGED (typing, Backspace/Delete, a cut, a
// paste) - caret and selection moves edit nothing and answer false.
// v1 edits ASCII 32..126, the font's bake range: typed codepoints
// outside it drop, a paste filters byte-wise, and bytes already in the
// buffer outside it sit inert (no advance, no draw - the font skips
// them). The row wears the button faces with the content left-aligned
// inside the padding; the label draws as a dimmed placeholder only
// while the buffer is empty. The caret: a click places it (x to glyph
// index at the mid-advance), Left/Right/Home/End move it, and it
// blinks on the frame clock (dt_seconds' first consumer) - solid
// through every edit and move, half on/half off at rest, hidden while
// unfocused or under a disabled scope. Selection: Shift extends the
// arrows/Home/End from the anchor, a press-and-sweep drags it, Ctrl+A
// takes all, and typing or Backspace/Delete replaces it; Escape
// clears it. The range draws as the accent band under the text, the
// selected tab's face. Ctrl+C/X copy/cut the selection and Ctrl+V
// pastes at the caret through the gui's clipboard seam
// (GuiContext::set_clipboard; App::install_clipboard wires the
// platform - an unwired seam, Android today, makes the trio no-ops).
// Enter is not the widget's to take - a single line has no commit
// edge, so the caller reads its own: is_focused(ids().id(label)) and
// key_pressed[ws::input::keys::kEnter] on the frame after the edit. Tab cycles
// the focus away like any row's (the field registers focusable);
// Space types, it does not activate. A line longer than the row
// truncates with "..." at the right edge (v1 has no horizontal
// scroll; the caret rides the edge while the keys still edit the
// hidden tail). A null `buf` or `capacity` under 2 lays the same row
// inert - nothing to edit, nothing to place. `width` lays the row
// like a button's (negative = fill the line).
bool text_input(GuiContext& gui, LayoutCursor& layout,
                const ChromeTheme& theme, const char* label, char* buf,
                int capacity, float width = -1.0f);

// One column of a table: the header text and the width policy. A width
// above zero is fixed pixels; zero or negative STRETCHES - the column
// shares equally with the other stretch columns whatever width the fixed
// columns leave, clamped at zero (fixed columns win first; a squeezed
// table never lays a backwards cell). `label` is the header cell's text;
// null lays an empty header cell.
struct TableColumn {
    const char* label = nullptr;
    float width = 0.0f;
};

// A table: the widget arc's grid rung - ONE call lays a header strip, a
// 2-px rule under it (the separator's own look) and `row_count`
// contiguous rows of text cells, all as one slot in the parent's flow.
// `cells` is row-major - `cells[row * column_count + column]`, the
// combo's items discipline: the caller owns the strings and
// re-describes them every frame; a null cell is an empty one. Column
// widths clamp the CELLS to the table's own right edge - a fixed set
// that sums past the width never paints outside the rect.
//
// `label` ids the table and never draws - the header row is the visible
// labeling, and the rows id under it (push(label), push(index)), so the
// same grid works anywhere and two tables differ by label like any
// widget. The table keeps NO context state of its own: the selection is
// the caller's int, and the scroll is the caller's scroll panel - a
// table taller than its panel simply flows (the scroll panel's clips
// hide and unclaim whatever scrolls out; v1 culls nothing, every row
// records).
//
// `selected` is what makes the table interactive: full citizenship per
// row, selectable's exact look - the bare list (no idle face), the
// hover and press faces, the 3-px accent bar on the selected row, the
// 1px focus ring on the focused one. Each row registers focusable in
// row order (Tab walks the grid top-down), Enter/Space pick the focused
// row, Up/Down step the focused row clamped at the grid's ends (the
// slider's arrow rule - Tab's wrap belongs to the screen, not the
// grid), and a pick that MOVED the selection writes `*selected` and
// answers true - the radio's idempotence: a re-pick on the current row
// answers false and writes nothing. A NULL `selected` lays the same
// grid display-only: no ids, no claims, no registrations - the
// statics' "cannot interact with you" spelled by the pointer, like the
// clipboard seam's null answer.
//
// `width` lays the whole table like a button's (negative = fill the
// line). A table with no columns lays a bare slot and pretends nothing
// (the empty combo's rule); one with no rows draws its header alone -
// the empty dataset's honest look. v1 limits, deliberate: cells are
// TEXT (the compositional table - a scope whose cells hold live
// widgets - is a future shape, not this one), every cell
// left-aligned, no sorting, resizing, reordering or zebra striping
// (the bare-list look reads on its hover faces), and the header is
// chrome - a header click claims nothing (sorting's future seam).
bool table(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
           const char* label, const TableColumn* columns, int column_count,
           const char* const* cells, int row_count,
           int* selected = nullptr, float width = -1.0f);

}  // namespace ws::ui::gui
