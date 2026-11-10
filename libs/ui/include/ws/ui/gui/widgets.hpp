#pragma once

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/rect.hpp>
#include <ws/ui/gui/theme.hpp>

#include <ws/core/result.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>

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

// What a split widget's behavior half hands its draw half: the state the
// row earned this frame (one state() fold), the focus flag the ring
// reads, and the hit itself - everything a look needs, nothing it may
// change. Every <widget>_behavior returns one of these (a widget's own
// result derives from it); every <widget>_draw paints one. The default
// is the inert row: a hand-built result paints dead, never hot.
struct WidgetPaint {
    WidgetState state = WidgetState::Disabled;
    bool focused = false;
    HitTest hit{};
    // P4's face-blend traveler: how far the row's resting face has
    // blended toward its hover face, [0, 1]. -1 (the default) means "no
    // animation spoke" - the look snaps to the state's own face, the
    // exact pre-P4 behavior; hand-built results and
    // animation-disabled themes both land there. The behavior folds it
    // from animate_toward; a custom painter reads it to blend its own.
    float hover_t = -1.0f;
};

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
// The button variants (P6): plain values, resolved through the theme's
// tokens at draw time - no widget hardcodes a color, and a skin that
// overrides the button row owns every variant's pixels.
enum class ButtonStyle : std::uint8_t {
    secondary,  // the plain control ladder - today's look, the default
    primary,    // the accent face, the on-accent label
    ghost,      // no face at rest, the hover/press faces on the live states
    danger,     // the destructive action - the theme's danger family
};
//
// Buttons take the keyboard: they register focusable (a press on one
// takes the focus at the press edge, Tab reaches them), Enter and Space
// activate the focused button exactly like a click, and the focused
// button wears a 1px accent ring - the focus rules live on GuiContext
// (state/register_focusable/is_focused). Inside a begin_disabled scope
// the button goes dead as a whole: the control_disabled face, the icon
// tint and the label dimmed by theme.disabled_alpha through dim(). The
// trailing `disabled` parameter is the same lock on this one row, no
// scope needed.
bool button(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
            const char* label, float width = 120.0f, r2d::Sprite icon = {},
            bool disabled = false,
            ButtonStyle style = ButtonStyle::secondary);

// The split halves: button's interaction without a single draw, and its
// stock look over the answer. Composing them in order IS the widget
// above; swapping the draw half for your own is the custom-look seam -
// the behavior owns identity, focus and the click, the draw owns pixels.
//
// ButtonStyle (P6): the variant the row wears, resolved ENTIRELY through
// tokens - secondary is the plain control ladder (today's look, the
// default), primary the accent family labeled text_on_accent, ghost a
// transparent idle that takes the hover/press faces (a focused-at-rest
// ghost takes the plain background so its ring reads), danger the danger
// family. The style rides ButtonResult so the PAINTER ROW's signature
// stays frozen - a draw half reads result.style and needs no extra
// parameter, and skins that predate the variants keep compiling.
struct ButtonResult : WidgetPaint {
    bool clicked = false;      // a full click inside the rect this frame
    bool key_clicked = false;  // Enter or Space on the focused row
    ButtonStyle style = ButtonStyle::secondary;
    bool activated() const { return clicked || key_clicked; }
};

// The interaction half at a placed rect: registers the focusable, runs
// the one hit-test, reads the key edge. Draws nothing.
ButtonResult button_behavior(GuiContext& gui, WidgetId id, const Rect& rect,
                             bool disabled = false,
                             const HitShape& hit_shape = {},
                             ButtonStyle style = ButtonStyle::secondary);

// The look half: the stock face, ring, icon and centered label. Reads no
// context - the whole look rides the result, the theme and the label.
void button_draw(const Rect& rect, const ButtonResult& result,
                 const ChromeTheme& theme, const char* label,
                 r2d::Sprite icon = {});

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
// value. The trailing `disabled` parameter is the same lock on this
// one row, no scope needed.
bool checkbox(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
              const char* label, bool& value, bool disabled = false);

// The split halves, checkbox's seam: the behavior also owns the flip of
// `value`; the draw takes the (already flipped) value as `checked`.
struct CheckboxResult : WidgetPaint {
    bool toggled = false;  // `value` flipped this frame (click or key)
};

CheckboxResult checkbox_behavior(GuiContext& gui, WidgetId id,
                                 const Rect& rect, bool& value,
                                 bool disabled = false,
                                 const HitShape& hit_shape = {});
void checkbox_draw(const Rect& rect, const CheckboxResult& result,
                   const ChromeTheme& theme, const char* label,
                   bool checked);

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
// dims as one and no click or key can move the selection. The trailing
// `disabled` parameter is the same lock on this one row, no scope needed.
bool radio(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
           const char* label, int& value, int this_option,
           bool disabled = false);

// The split halves, radio's seam: the behavior writes the selection;
// the draw takes the group's answer as `selected`.
struct RadioResult : WidgetPaint {
    bool moved = false;  // the selection moved to this option this frame
};

RadioResult radio_behavior(GuiContext& gui, WidgetId id, const Rect& rect,
                           int& value, int this_option, bool disabled = false,
                           const HitShape& hit_shape = {});
void radio_draw(const Rect& rect, const RadioResult& result,
                const ChromeTheme& theme, const char* label,
                bool selected);

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
// deadens it as one. The trailing `disabled` parameter is the same lock
// on this one row, no scope needed.
bool toggle_switch(GuiContext& gui, LayoutCursor& layout,
                   const ChromeTheme& theme, const char* label,
                   bool& value, bool disabled = false);

// The split halves, toggle_switch's seam: the behavior owns the flip of
// `value`; the draw takes the (already flipped) value as `on`.
struct ToggleSwitchResult : WidgetPaint {
    bool toggled = false;  // `value` flipped this frame (click or key)
    // P4's knob traveler: the knob's position along its travel, [0, 1]
    // (0 = the off end, 1 = the on end). -1 (the default) = "no
    // animation spoke" - the knob snaps to `on`'s end, the exact
    // pre-P4 behavior. The composed widget folds it from
    // animate_toward (the behavior stays theme-free); a custom painter
    // reads it to travel its own knob.
    float travel = -1.0f;
};

ToggleSwitchResult toggle_switch_behavior(GuiContext& gui, WidgetId id,
                                          const Rect& rect, bool& value,
                                          bool disabled = false,
                                          const HitShape& hit_shape = {});
void toggle_switch_draw(const Rect& rect, const ToggleSwitchResult& result,
                        const ChromeTheme& theme, const char* label,
                        bool on);

// A collapsing header: a row that flips `open` on a click or Enter/
// Space (the caller's bool IS the state) and returns it AFTER the flip,
// so the content is the caller's `if` branch, exactly a tab strip's -
// the header lays out only its own row. The marker square at the left
// is the faint border color closed and the accent open; the label sits
// beside it like a checkbox's, left-aligned. A negative `width` fills
// the rest of the line (the default - headers want their container's
// width). The same citizenship as a checkbox: focusable, the ring, and
// a begin_disabled scope dims and deadens it as one. The trailing
// `disabled` parameter is the same lock on this one row, no scope
// needed.
bool collapsing_header(GuiContext& gui, LayoutCursor& layout,
                       const ChromeTheme& theme, const char* label,
                       bool& open, float width = -1.0f,
                       bool disabled = false);

// The split halves, collapsing_header's seam: the behavior owns the
// flip of `open`; the draw takes the (already flipped) value.
struct CollapsingHeaderResult : WidgetPaint {
    bool toggled = false;  // `open` flipped this frame (click or key)
};

CollapsingHeaderResult collapsing_header_behavior(
    GuiContext& gui, WidgetId id, const Rect& rect, bool& open,
    bool disabled = false, const HitShape& hit_shape = {});
void collapsing_header_draw(const Rect& rect,
                            const CollapsingHeaderResult& result,
                            const ChromeTheme& theme, const char* label,
                            bool open);

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
// default - list rows want their container's width). The trailing
// `disabled` parameter is the begin_disabled lock on this one row: the
// dead face, no claim, no focus - Tab walks past it.
bool selectable(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
               const char* label, bool selected, float width = -1.0f,
               bool disabled = false);

// The split halves, selectable's seam: the behavior reports the click -
// the selection stays the caller's to write; the draw reads it.
struct SelectableResult : WidgetPaint {
    bool clicked = false;  // a full click or Enter/Space this frame
};

SelectableResult selectable_behavior(GuiContext& gui, WidgetId id,
                                     const Rect& rect, bool disabled = false,
                                     const HitShape& hit_shape = {});
void selectable_draw(const Rect& rect, const SelectableResult& result,
                     const ChromeTheme& theme, const char* label,
                     bool selected);

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
// label faded) and no pointer or key can move the value. The trailing
// `disabled` parameter is the same lock on this one row, no scope
// needed. `width` defaults to 120 px; a negative width instead FILLS
// the rest of the current line, like button's.
bool slider_float(GuiContext& gui, LayoutCursor& layout,
                  const ChromeTheme& theme, const char* label, float& value,
                  float min, float max, float width = 120.0f,
                  bool disabled = false);

// slider_float over an int reference - the value rounds to the nearest
// integer after the float math, and the arrows step it by 1. A negative
// width fills the rest of the current line, like slider_float's; the
// trailing `disabled` parameter is its row's own lock, like the
// others'.
bool slider_int(GuiContext& gui, LayoutCursor& layout,
                const ChromeTheme& theme, const char* label, int& value,
                int min, int max, float width = 120.0f,
                bool disabled = false);

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
// defaults to 120 px; a negative width fills the rest of the line; the
// trailing `disabled` parameter is the same lock on this one row, no
// scope needed.
bool drag_float(GuiContext& gui, LayoutCursor& layout,
                const ChromeTheme& theme, const char* label, float& value,
                float min, float max, float speed = 1.0f,
                float width = 120.0f, bool disabled = false);

// The split halves, the sliders' seam: the behavior owns the tracking
// (click-to-set and the drag are one rule), the arrow steps and the
// clamped write - theme-free, its whole geometry the range and the key
// step. The draw owns the rail, the fill, the handle - positioned from
// the VALUE the behavior left, never the pointer - and the label. Both
// sliders share the pair: slider_int feeds the float ladder and rounds
// on its way back.
struct SliderResult : WidgetPaint {
    bool changed = false;  // the value moved this frame (pointer or key)
};

SliderResult slider_behavior(GuiContext& gui, WidgetId id, const Rect& rect,
                             float& value, float min, float max,
                             float key_step, bool disabled = false,
                             const HitShape& hit_shape = {});
void slider_draw(const Rect& rect, const SliderResult& result,
                 const ChromeTheme& theme, const char* label, float value,
                 float min, float max);

// The split halves, drag_float's seam: the behavior owns the anchor -
// the grab that survives frames in the persistent state - and the
// arrow steps; the draw owns the row face and the "label: value" line
// (it formats the value itself).
struct DragFloatResult : WidgetPaint {
    bool changed = false;  // the value moved this frame (drag or key)
};

DragFloatResult drag_float_behavior(GuiContext& gui, WidgetId id,
                                    const Rect& rect, float& value,
                                    float min, float max, float speed,
                                    bool disabled = false,
                                    const HitShape& hit_shape = {});
void drag_float_draw(const Rect& rect, const DragFloatResult& result,
                     const ChromeTheme& theme, const char* label,
                     float value);

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

// A section heading - the font hierarchy's loud rung. Draws `label`
// through the theme's STRONG face (font_strong, null = `font`, the
// one-font rule) at that face's own baked size on a row taller than
// text's: the app bakes its strong face a step larger than the body
// (circsim's 18 px against the 16 px body), and the heading simply
// follows it - bold-ish and slightly larger, like a document's h2.
// Chrome like text(): no id, no hit-test, no GuiContext. The width
// contract is text()'s own (negative = size to the label, fixed =
// truncate into it), and the color is the theme's body `text`: a
// heading is structure, not a hint - the muted rung is not invited.
void heading(LayoutCursor& layout, const ChromeTheme& theme,
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

// A spinner: the busy indicator - `size` px square (a negative size,
// the default, takes theme.control_height), eight dots chasing around
// its circle, the bright one wearing the accent and the rest the
// control faces, the phase advancing on the frame clock (dt_seconds)
// in the context's persistent state under `label` - which ids the
// spinner and never draws. Headless the frame clock reads 0: the dots
// hold their phase, the one honest answer without time.
void spinner(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
             const char* label, float size = -1.0f);

// The split halves, spinner's: the behavior advances the persistent
// phase on the frame clock and answers it; the draw orbits the dots. A
// custom look chases its own marks around the same phase. Not a
// WidgetPaint: the spinner claims nothing, so there is nothing to paint.
struct SpinnerResult {
    float phase = 0.0f;  // [0, 1): the turn, the bright dot is int(phase*8)
};

SpinnerResult spinner_behavior(GuiContext& gui, WidgetId id);
void spinner_draw(const Rect& rect, const SpinnerResult& result,
                  const ChromeTheme& theme);

// The indeterminate progress bar: progress's track with nothing to
// show - a band a third of the track's width sweeps it left to right,
// wrapping, the phase on the frame clock in the persistent state under
// `label` (which ids the bar and never draws, progress's own label
// discipline - the caller composes a text row of its own beside it).
// A negative `width` fills the rest of the current line.
void progress_indeterminate(GuiContext& gui, LayoutCursor& layout,
                             const ChromeTheme& theme, const char* label,
                             float width = -1.0f);

// The split halves, progress_indeterminate's: the behavior advances the
// persistent sweep on the frame clock; the draw runs the band across.
struct IndeterminateResult {
    float phase = 0.0f;  // [0, 1): the sweep, the band's trip across
};

IndeterminateResult progress_indeterminate_behavior(GuiContext& gui,
                                                    WidgetId id);
void progress_indeterminate_draw(const Rect& rect,
                                  const IndeterminateResult& result,
                                  const ChromeTheme& theme);

// The status bar: a frame-anchored strip along the bottom edge - full
// width, one control height, `text` left-inset past the padding. It
// takes NO row in the flow and no GuiContext (the statics' signature -
// nothing to interact with): the caller draws it wherever it fits its
// frame and keeps its content clear by the returned rect's top edge,
// the layout arithmetic being the caller's (the bar reserves nothing).
Rect status_bar(const ChromeTheme& theme, const char* text);

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

// A panel's chrome alone: the 1-px border frame behind the face inset
// by 1 - the same two quads begin_panel paints before it pushes its
// clips. For a custom container that wants the stock look without the
// panel scope: draw this, then push your own clips.
void panel_draw(const Rect& bounds, const ChromeTheme& theme);

// The OVERLAY window's chrome: the floating frame a popup, a menu, a
// tooltip or a toast wears. Today the panel's own two quads - the seam
// is the point, not a difference: floating chrome grows (rounded
// corners, a drop shadow) without the panels it floats over following,
// and a skin restyles every floating window in one override. The
// overlay scopes call this at their bounds, inside the clip escape;
// nothing else about the scope changes here.
void popup_frame_draw(const Rect& bounds, const ChromeTheme& theme);

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
// can move the selection. The trailing `disabled` parameter is the
// same lock on the whole strip, no scope needed. What shows per tab is
// the caller's branch - the strip is the selection primitive, not a
// content container.
//
// THE CLOSABLE STRIP: pass a non-null `closed` and every tab grows a
// small round CLOSE BUTTON at its right edge - a 12 px CIRCULAR
// hitbox, the editor-tab spelling, drawn as the circle it hit-tests
// as: a disc in the control face ladder (rest, hover blend, press,
// dead) with a multiplication x of two crossing STROKES inside it -
// geometry, not a glyph, so the mark renders in any font and none.
// The entry starts each call at -1 and names the tab whose x was
// clicked (or key-fired) this frame; the caller closes it. The tab's
// own hit shape EXCLUDES the circle, so the two widgets never fight
// over a press: a click inside the circle closes the tab and moves
// nothing, a click in the button's square corners falls through to
// the tab and selects it. The label pre-fits around the x's zone (the
// fit runs here, before the painter row, so skins see the ellipsized
// text like any tab label). A null `closed` is the plain strip - no
// button drawn, no room reserved, byte-for-byte the old behavior.
//
// THE DRAG-READY STRIP: pass a non-null `held_tab` and the strip
// reports, every frame, the index of the tab whose press is still
// HELD (-1 when none): the tab's capture claim, not a fresh edge - a
// press that started inside a tab keeps reporting it wherever the
// pointer goes, until the release. That is the half a drag needs: the
// shell arms its tab-drag off the claim, waits for the pointer to
// carry it past a threshold, and reads the release as the drop. The
// strip stays the selection primitive; the drag is the caller's
// gesture, built on the same claim the click is.
bool tab_bar(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
             const char* label, const char* const* tabs, int count,
             int& active, float width = 120.0f, bool disabled = false,
             int* closed = nullptr, int* held_tab = nullptr);

// The split halves, the tab strip's seam: the behavior is ONE tab's
// interaction (the strip loops it - the caller owns the id scope, so
// the index-ids the strip builds stay the caller's to shape); the draw
// is one tab's pixels. `selected` is display state the draw reads and
// never writes, like selectable's.
struct TabResult : WidgetPaint {
    bool clicked = false;  // a full click or the focused tab's Enter/Space
    // P4's accent traveler: the selected tab's accent underline fade,
    // [0, 1]. -1 (the default) = "no animation spoke" - the underline
    // draws at full strength, the exact pre-P4 behavior. The composed
    // strip folds it from animate_toward under the tab's own id; a
    // custom painter reads it to fade its own selection mark.
    float accent_t = -1.0f;
};

TabResult tab_behavior(GuiContext& gui, WidgetId id, const Rect& rect,
                       bool disabled = false, const HitShape& hit_shape = {});
void tab_draw(const Rect& rect, const TabResult& result,
              const ChromeTheme& theme, const char* label, bool selected);

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

// --- the scrollable region's split halves ----------------------------------
// The scroll panel's behavior halves, public so a caller laying its
// own scrollable region runs the same rules the panel does: the touch
// flow-drag, and the thumb's grab-and-follow with the paint that
// follows it. Their state travels with them - hold a ScrollState and a
// ScrollDragState in the context's persistent side table under your
// own ids, the way the panel keys its reserved internal ones.

// A scroll region's persistent state: how far the content is scrolled
// up (offset), the scroll range's top as measured at the last end
// (max_offset = content_height - viewport_height - begin clamps
// against the stale max, end refreshes it, the one-frame lag), and
// the thumb grab in flight: the pointer holding it and where in the
// thumb the grab landed.
struct ScrollState {
    float offset = 0.0f;
    float max_offset = 0.0f;
    ws::input::PointerId thumb = ws::input::kNoPointer;  // the grabbing pointer, while held
    float thumb_grab = 0.0f;  // the grab's offset within the thumb
};

// A scroll region's per-contact drag bookkeeping: a track exists from
// the contact's press inside the viewport until it lifts. While it
// exists and !dragging the press is still the CHILD's (a tap in
// progress, watched against the threshold); dragging means the scroll
// owns the finger - the child's press was cancelled and the offset
// follows the contact 1:1.
struct ScrollDragTrack {
    ws::input::PointerId id = ws::input::kNoPointer;
    float press_y = 0.0f;  // the threshold is measured from the press
    float last_y = 0.0f;  // the previous frame's y, for the drag delta
    bool dragging = false;
};

// One per scroll region (the persistent side-table discipline): a
// track per live contact, so two fingers can drag one region and one
// lifting never confuses the survivor's bookkeeping.
struct ScrollDragState {
    std::array<ScrollDragTrack, ws::input::InputFrame::kMaxTouches> tracks{};
};

// The thumb's resting place this frame - what the grab-and-follow
// behavior hands the paint (and any custom hit-testing a caller wants
// over its own region).
struct ScrollbarThumb {
    bool shows = false;    // a fitted region wears no thumb at all
    float top = 0.0f;      // the thumb's resting top, in frame px
    float height = 0.0f;   // proportional to the visible share, floored
};

// The touch flow-drag half: watch every live contact that pressed
// inside `viewport`, and past theme.drag_threshold take the finger -
// cancel_pointer ends the child's press with no click, and from the
// crossing frame the offset follows the contact 1:1 until it lifts
// (finger up = later content). The offset arrives unclamped; the
// caller clamps against max_offset the way the panel does. The arm
// claim (claim_scroll_arm, keyed on `scroll`'s address) keeps a
// nested drag singular - the innermost region that saw the press wins
// - and a track whose arm another region owns drops silently, so the
// child keeps its tap. Nothing draws here.
void scroll_drag_behavior(GuiContext& gui, ScrollState& scroll,
                          ScrollDragState& drag, const Rect& viewport,
                          const ChromeTheme& theme);

// The thumb's grab-and-follow half, run at the END of the frame (the
// children have laid; the offset this frame's grabs land on is last
// frame's content): the whole right gutter (theme.scrollbar_band_width
// wide) is grabbable - a fresh press there ends any child press
// beneath it and takes the pointer, ON the thumb keeping the exact
// grab offset, off it jumping the thumb centered under the pointer -
// and the held pointer maps 1:1 through the track until it lifts. A
// fitted region (max_offset 0, or an empty viewport) answers
// shows=false and takes no press at all. Nothing draws here.
ScrollbarThumb scroll_thumb_behavior(GuiContext& gui, ScrollState& scroll,
                                     ScrollDragState& drag,
                                     const Rect& viewport,
                                     const ChromeTheme& theme);

// The paint half: the thumb at its resting place, the theme's width
// and border face at the gutter's left edge - one rounded quad, only
// while the behavior says the region scrolls.
void scrollbar_draw(const Rect& viewport, const ScrollbarThumb& thumb,
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

// Which side of its anchor a placed popup hangs from: below is the
// dropdown shape (under the anchor's bottom edge), right the submenu
// shape (past its right edge).
enum class PopupAnchor { below, right };

// The placement arithmetic on its own: the size clamps to the viewport
// first (a window taller than the frame lays filling what fits), then
// it prefers the chosen side and FLIPS to the opposite one when the
// viewport lacks the room, then clamps the position so the window never
// leaves the viewport either way. A degenerate anchor - a point, the
// press point of a context menu - anchors at itself: the window hangs
// below/right of the point. Pure: no frame read, nothing drawn,
// nothing claimed, so it tests headless with any viewport, and a caller
// placing into something other than the full frame can.
Rect popup_placement(const Rect& anchor, float width, float height,
                     PopupAnchor side, const r2d::FrameSize& viewport);

// The live-frame spelling of popup_placement: reads the frame and
// delegates. Headless the frame is empty and everything clamps to the
// origin.
Rect place_popup(const Rect& anchor, float width, float height,
                 PopupAnchor side = PopupAnchor::below);

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
// it too, per pressed contact. ESCAPE closes the popup: it claims the
// frame's Escape while open (GuiContext::claim_esc - the topmost open
// overlay wins the key; a text_input that drops its selection first
// spends the Escape so the popup keeps it, one key per intent), and
// the win lands the frame after the press. Under a raised modal veil
// the popup is inert like every lower stratum: its close rule eats no
// press the veil owns and its claim never reaches the arbitration.
// v1 limits, deliberate: no focus-loss close, no auto-flip above the
// anchor when the frame lacks room (place_popup does that arithmetic
// for a caller who wants it - here the caller still places `bounds`),
// and a press on the popup's own padding - not on a child - falls
// through to whatever sits beneath (fill-width children leave almost
// no padding to press).
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
// begin_popup's. `bounds` is caller-sized and must fit the whole hint:
// the scope is a padded panel (the cursor starts inset by
// theme.padding), so a box shorter than the padding plus its content
// rows clips them at the bounds. The scope pushes the tooltip stratum,
// the chrome at
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

// The tooltip delay: true once the pointer has rested on the widget
// whose id this is - the hovered widget's own id, is_hot's spelling -
// for theme.tooltip_delay seconds; false again the frame it leaves.
// The accumulated rest lives in the persistent state under the id, so
// a re-hover starts from zero and a flickering hover (an overlapped
// row) never reaches the threshold. Gate a begin_tooltip scope with
// it; under a raised modal veil the widget beneath cannot be hot at
// all, so the delay needs no gate of its own.
bool tooltip_delayed(GuiContext& gui, WidgetId id, const ChromeTheme& theme);

// The one-line tooltip: the whole hover-to-hint pattern folded into
// one call - tooltip_delayed's rest, then a tooltip scope holding one
// text row, placed by place_popup below `anchor` (flipped above when
// the frame lacks room, clamped at its edges) and sized to the hint
// plus the theme's padding. `label` is the label of the widget being
// explained, the same id scope it was laid under - the shared label
// keys the delay; `hint` is the one line the tooltip draws (its name
// keeps it clear of the text() row widget it lands in). Rich content
// stays the caller's: gate a begin_tooltip scope with tooltip_delayed
// and lay the rows by hand.
void tooltip_text(GuiContext& gui, LayoutCursor& layout,
                  const ChromeTheme& theme, const char* label,
                  const Rect& anchor, const char* hint);

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
// landed - the focus-follows-press rule, no help wanted). The
// trailing `disabled` parameter locks the row (and its dropdown with
// it), the begin_disabled lock without the scope.
bool combo(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
          const char* label, const char* const* items, int count,
          int& selected, bool& open, float width = -1.0f,
          bool disabled = false);

// The split halves, the combo row's seam: the behavior owns the row's
// interaction - the fold, the drop of `open` on an empty or locked combo
// (the whole-widget lock: a dropdown left open by a lock landing
// mid-flight closes), and the toggle, a click or the focused row's
// Enter/Space refused while `empty`. The draw paints the closed row:
// the face, the right-edge marker, the "label: item" line, with `open`
// the post-toggle answer the marker keys on. The pair is the row
// alone - the dropdown stays the composed widget's own half, so a
// caller building a custom combo lays its list through place_popup and
// begin_popup the way the widget does.
struct ComboResult : WidgetPaint {
    bool toggled = false;  // the row flipped `open` this frame
};

ComboResult combo_behavior(GuiContext& gui, WidgetId id, const Rect& rect,
                           bool& open, bool empty, bool disabled = false,
                           const HitShape& hit_shape = {});
void combo_draw(const Rect& rect, const ComboResult& result,
                const ChromeTheme& theme, const char* label,
                const char* const* items, int count, int selected,
                bool open);

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
// like a button's (negative = fill the line). The trailing `disabled`
// parameter locks the field - the dead face, no caret, no focus, no
// keys - the begin_disabled lock without the scope.
bool text_input(GuiContext& gui, LayoutCursor& layout,
                const ChromeTheme& theme, const char* label, char* buf,
                int capacity, float width = -1.0f, bool disabled = false);

// --- the text field's split halves -------------------------------------------
// The field's behavior/draw seam, public for the custom text field:
// the behavior owns the editing - the caret and the selection against
// the caller's buffer, the whole keyboard, the clipboard trio - and
// maps the caret into pixels; the draw owns the stock field's every
// pixel over the answer. The state travels public with them: hold the
// persistent pair in the context's side table under your own id, and
// the EditBuffer lens is yours for whatever else a custom field edits.

// text_input's persistent half: the caret and its selection anchor are
// byte offsets into the CALLER's buffer (v1 edits ASCII, so bytes are
// characters; equal means no selection). The blink phase is the frame
// clock's first consumer - dt_seconds accumulates here, half on/half
// off at rest, and every edit, caret move or press rewinds it so the
// caret reads solid while the user works. `was_focused` lights it the
// frame the field GAINS the focus, so a Tab-arrival caret shows
// immediately instead of mid-blink.
struct TextInputState {
    int caret = 0;
    int anchor = 0;      // the selection origin; caret == anchor: none
    float blink = 0.0f;  // [0, 1): under 0.5 draws the caret
    bool was_focused = false;
};

// The typing filter: v1 edits ASCII - one byte, one glyph, the bake's
// whole range. The test takes the CODEPOINT, never a char: cp 300 cast
// to the byte first would alias into ','. The paste path's
// char->unsigned-char widening is exact, so it shares this one filter.
inline bool is_insertable(std::uint32_t cp) {
    return cp >= 32u && cp <= 126u;
}

// text_input's per-frame lens: the persistent caret pair from
// TextInputState plus the caller's buffer geometry as one object, so
// every edit speaks a verb instead of spelling memmove. Built once per
// frame with the length measured fresh; the frame-end clamp writes the
// pair back into the persistent state. A dead field carries room 0,
// and the verbs' own clamps turn that into a no-op.
struct EditBuffer {
    char* buf = nullptr;  // the caller's buffer (the widget never owns it)
    int room = 0;   // bytes the buffer may hold: capacity minus the NUL
    int len = 0;    // bytes before the terminator, measured this frame
    int caret = 0;   // the editing position; the selection's live end
    int anchor = 0;  // the selection origin; caret == anchor: none

    // The selection as a range, low end first (equal ends: none).
    int sel_lo() const { return std::min(caret, anchor); }
    int sel_hi() const { return std::max(caret, anchor); }
    bool has_selection() const { return caret != anchor; }

    // Both ends onto `at`: the selection dies, the caret stays.
    void collapse(int at) {
        caret = anchor = at;
    }

    // The one eraser every deleter funnels through: [lo, hi) leaves,
    // the tail slides, the terminator follows the new end.
    void erase(int lo, int hi) {
        lo = std::max(0, lo);
        hi = std::min(len, hi);
        if (lo >= hi) {
            return;
        }
        std::memmove(buf + lo, buf + hi, static_cast<std::size_t>(len - hi));
        len -= hi - lo;
        buf[len] = '\0';
    }

    // A selection dies under what replaces it - typing and pasting both
    // spell it; the caret lands where the selection was.
    void erase_selection() {
        if (has_selection()) {
            const int lo = sel_lo();
            erase(lo, sel_hi());
            collapse(lo);
        }
    }

    // n bytes at `at` (clamped into [0, len], never past the room): the
    // tail slides once, the bytes land, the terminator follows. The
    // return is the pen just past the insertion, or -1 when the room
    // refused everything.
    int insert(int at, const char* bytes, int n) {
        at = std::max(0, std::min(len, at));
        n = std::min(n, room - len);
        if (n <= 0) {
            return -1;
        }
        std::memmove(buf + at + n, buf + at,
                     static_cast<std::size_t>(len - at));
        std::memcpy(buf + at, bytes, static_cast<std::size_t>(n));
        len += n;
        buf[len] = '\0';
        return at + n;
    }

    // A paste through the filter: only bytes the filter admits count, and
    // only as many as the room takes - counted in one pass over the text,
    // then landed in one move, so the buffer never walks per byte and
    // the capacity never splits mid-paste. The return is insert's pen.
    // The filter rides as a parameter so a constrained field (a numeric
    // one) admits only its own charset; the default is the v1 field's
    // plain cut, so every existing caller spells the same edit it
    // always did.
    int insert_filtered(int at, const char* text,
                        bool (*filter)(std::uint32_t) = &is_insertable) {
        int take = 0;
        for (const char* p = text; *p != '\0' && len + take < room; ++p) {
            if (filter(static_cast<unsigned char>(*p))) {
                ++take;
            }
        }
        at = std::max(0, std::min(len, at));
        if (take <= 0) {
            return -1;
        }
        std::memmove(buf + at + take, buf + at,
                     static_cast<std::size_t>(len - at));
        int pen = at;
        for (const char* p = text; *p != '\0'; ++p) {
            if (pen - at >= take) {
                break;  // the room's count is landed; the tail never walks
            }
            if (filter(static_cast<unsigned char>(*p))) {
                buf[pen++] = *p;
            }
        }
        len += take;
        buf[len] = '\0';
        return pen;
    }
};

// What the behavior hands a look: the row's paint plus the caret's
// mapped geometry, paint-ready. caret_x is the caret's x in frame px,
// clamped to the row's room; sel_x0/sel_x1 are the selection band's
// edges in frame px, equally clamped (equal: no selection stands - an
// unfocused field still reports its span, and the stock draw gates it
// on the focus); blink is the caret's phase [0, 1). The visibility
// gates - focus, the disabled lock, the blink phase - are the look's
// to apply; edited is the frame's answer, the widget's own return.
struct TextInputResult : WidgetPaint {
    float caret_x = 0.0f;  // the caret's resting x, frame px
    float sel_x0 = 0.0f;   // the selection band's left edge, frame px
    float sel_x1 = 0.0f;   // ...right edge (x0 == x1: none stands)
    float blink = 0.0f;    // the caret's phase [0, 1): under 0.5 solid
    bool edited = false;   // the buffer changed this frame
};

// The interaction half at a placed rect: the citizenship fold, then the
// field's whole editing - the press that places the caret, the sweep
// that selects, the caret keys, the deleters, the frame's codepoints
// through the filter, Ctrl+A, Escape's selection drop and the
// clipboard trio - all against the caller's buffer through the
// EditBuffer verbs, the pair written back into `edit`. `theme` maps
// the caret into pixels: the font's glyph advances, or
// label_char_width fontless. `filter` is the typing and paste
// charset - the codepoints the field admits - and defaults to the
// plain v1 cut (ASCII 32..126); a numeric field passes its own
// (is_float_insertable / is_int_insertable below). Draws nothing.
TextInputResult text_input_behavior(GuiContext& gui, WidgetId id,
                                    const Rect& rect, char* buf, int capacity,
                                    TextInputState& edit,
                                    const ChromeTheme& theme,
                                    bool disabled = false,
                                    const HitShape& hit_shape = {},
                                    bool (*filter)(std::uint32_t) = &is_insertable);

// The look half: the face, the focus ring, the selection band, the
// content - or the placeholder while the buffer is empty - the "..."
// when the line runs past the room, and the blinking caret at the
// mapped x. Reads no context; `buf` is read-only here, the edits are
// the behavior's.
void text_input_draw(const Rect& rect, const TextInputResult& result,
                     const ChromeTheme& theme, const char* label,
                     const char* buf, int capacity);

// --- the numeric field ---------------------------------------------------------
// input_float / input_int: the exact-value row the property grid always
// wanted - a number you can TYPE, where the slider only let you scrub.
// The row is a fitted label at the left and a text field over the rest
// (the property-grid shape; a null or empty label lays the field across
// the whole row, and a label wider than the row's left share truncates
// with "..." like every fitted label - the field keeps a floor of px it
// never shrinks below). The field is text_input's own surface: the same
// caret, selection, clipboard trio and blink, driven through
// text_input_behavior - but through the NUMERIC filter, so only the
// charset a number can spell lands in the buffer.
//
// The buffer discipline (the property grid's old copy-in-every-frame
// trap, fixed): the spelling lives in the widget's own side-table state
// (NumInputState), not the caller's. While the field HOLDS the focus
// the buffer is the user's to edit; every frame without the focus
// re-spells it from `value`, so an external change shows the moment it
// lands. The commit edges: Enter commits (a numeric line HAS one - the
// key is this widget's to take, and it consumes the edge, so a dialog
// behind the field does not also answer it), the focus leaving commits,
// and Up/Down step `step` from the buffer's parsed value (a commit by
// another name - held arrows keep stepping on the OS auto-repeat, the
// slider's rule). A commit parses the buffer strictly, CLAMPS into
// [min, max] when the field is ranged (min < max), and answers true
// only when the value actually MOVED - the radio's idempotence, so
// re-typing the standing value is a no-op. A buffer that will not
// parse (or a range the parse clamps to nothing new) reverts to the
// value's own spelling: an abandoned edit never leaks. Escape while a
// selection stands drops it (the text field's own rule); a bare Escape
// abandons the edit the same way - the buffer reverts, and the
// arbitration's copy of the key is spent, so the next Escape closes
// the overlay behind the field.
//
// Tab onto the field selects the whole spelling (typing replaces it);
// a click onto it places the caret at the click (the text field's own
// placement, which the arrival frame's select-all defers to).
//
// `step` is the Up/Down increment; 0 (the default) derives it the way
// the family always has - 1% of the range when the field is ranged, 1
// (the int ladder's own) when it is not - and a negative step disables
// the arrows' stepping outright. `min`/`max` bound the commits only
// while min < max; equal (the default) or inverted lays the field
// UNBOUNDED - the honest row for a value with no natural range, where
// the old grid's 0..100 fallback invented one. input_float spells the
// value with `significant` significant digits (%g semantics, 6 by
// default, clamped to 1..9); input_int spells the plain integer. A
// non-finite float spells "nan"/"inf" and never re-commits (the parse
// refuses its own spelling); the row is otherwise inert under it, like
// drag_float's NaN guard. `width` lays the row like a button's
// (negative = fill the line); the trailing `disabled` parameter is the
// one-row lock, as everywhere.
bool input_float(GuiContext& gui, LayoutCursor& layout,
                 const ChromeTheme& theme, const char* label, float& value,
                 float min = 0.0f, float max = 0.0f, float step = 0.0f,
                 int significant = 6, float width = -1.0f,
                 bool disabled = false);
bool input_int(GuiContext& gui, LayoutCursor& layout,
               const ChromeTheme& theme, const char* label, int& value,
               int min = 0, int max = 0, int step = 0, float width = -1.0f,
               bool disabled = false);

// The typing filters the numeric fields ride: is_float_insertable
// admits the decimal-float charset (digits, '.', '+', '-', 'e', 'E'),
// is_int_insertable the integer one (digits, '+', '-'). Both are
// text_input_behavior filter parameters, so a custom field narrows its
// own charset the same way. The PARSE stays the authority - a filter
// admits charset, the commit admits format ("1.2.3" passes the filter
// and dies at the parse).
inline bool is_float_insertable(std::uint32_t cp) {
    return (cp >= '0' && cp <= '9') || cp == '.' || cp == '+' || cp == '-' ||
           cp == 'e' || cp == 'E';
}
inline bool is_int_insertable(std::uint32_t cp) {
    return (cp >= '0' && cp <= '9') || cp == '+' || cp == '-';
}

// The parse's own error codes, on ws::core::Error with the "ui"
// category (the theme's own): what a commit refused, for a caller that
// wants to say why. The messages are static literals - the error never
// allocates past its own storage.
enum class InputParseError : std::int32_t {
    empty = 1,        // nothing (or only whitespace) to parse
    not_a_number = 2, // "abc", "inf", "nan", "1e" - strtod refused or spelled non-finite
    trailing = 3,     // "1.2.3", "4px" - text after the number
    out_of_range = 4, // parses but cannot live in the type (the int's overflow)
};

// The strict parse, public and pure (the popup_placement precedent): the
// WHOLE string must be one number - no trailing text, no hex spellings,
// no non-finite values. Leading whitespace is tolerated (strtod's own
// contract); the decimal point is the C locale's, the same locale every
// snprintf spelling in the gui writes. input_parse_int refuses a
// fractional spelling outright ("1.5" is trailing text to an integer),
// and a magnitude past the int's own bounds answers out_of_range.
ws::core::Result<float> input_parse_float(const char* text);
ws::core::Result<int> input_parse_int(const char* text);

// The float spelling, the format half of the pure pair: `value` in
// `significant` significant digits (%g semantics - trailing zeros drop,
// small and large magnitudes go exponential), written into `buf` and
// NUL-terminated within `capacity`. `significant` clamps to 1..9 (the
// float's own honest digits end near 7; 9 leaves room for a caller's
// round-trip tolerance). Returns the bytes written (the pen, excluding
// the terminator), 0 when the capacity refuses anything. A non-finite
// value spells "nan"/"inf" exactly as snprintf does - a display that
// never re-parses.
int input_format_float(char* buf, int capacity, float value, int significant);

// --- the numeric field's split halves ------------------------------------------
// The same seam every family takes: the behavior owns the editing, the
// commit edges and the clamped write; the draw owns the pixels - the
// fitted label at the left, then text_input's own field look over the
// rest of the row (the stock draw delegates to text_input_draw at the
// field rect, so a skin replacing EITHER row owns its pixels whole).
// The state travels public with them: hold NumInputState in the side
// table under your own id and the custom numeric field edits through
// the same moves. input_int's behavior is the integer ladder of the
// same shape (its own filter, parse and spelling); both answer
// NumInputResult, and both draws are one - the buffer already holds
// the spelling, int or float alike.

// The numeric field's persistent half: the text field's own editing
// pair (nested - the caret, the anchor, the blink), plus the spelling
// buffer the pair edits. First touch zeroes the buffer, and the first
// unfocused frame spells the value into it.
inline constexpr int kNumInputCapacity = 64;

struct NumInputState {
    TextInputState edit;  // the field's own pair, the same verbs
    char buf[static_cast<std::size_t>(kNumInputCapacity)] = {};
};

// The behavior's answer: the text field's whole mapped result (the
// caret and selection ride the field rect, not the row) plus the
// commit word. `changed` is the widget's return - the value MOVED this
// frame, by a commit or a step; `edited` (inherited) says the buffer
// alone changed, which a live readout may still want to watch.
struct NumInputResult : TextInputResult {
    bool changed = false;  // the value moved this frame (commit or step)
};

NumInputResult input_float_behavior(GuiContext& gui, WidgetId id,
                                    const Rect& rect, float& value, float min,
                                    float max, float step, int significant,
                                    const char* label,
                                    const ChromeTheme& theme,
                                    bool disabled = false);
NumInputResult input_int_behavior(GuiContext& gui, WidgetId id,
                                  const Rect& rect, int& value, int min,
                                  int max, int step, const char* label,
                                  const ChromeTheme& theme,
                                  bool disabled = false);

// The look half: the fitted label at the row's left share, then the
// field - text_input's stock look over the field rect the behavior
// mapped its caret into. Recomputes the label fit itself (the draw
// owns its own fits); reads no context, and `buf` is read-only.
void input_float_draw(const Rect& rect, const NumInputResult& result,
                      const ChromeTheme& theme, const char* label,
                      const char* buf, int capacity);


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
// table taller than its panel simply flows, and the walk is VIRTUALIZED
// over it: the rows the current hit clip cannot see (a scroll panel's
// viewport, a plain panel's content rect, their intersection when
// nested) neither draw, hit-test nor register. The visible window plus
// ONE sentinel row at each end is all that records, so a
// ten-thousand-row grid costs what its viewport shows, not its row
// count - and since the clip was already hiding those pixels and
// unclaiming those presses, culling the walk changes nothing the user
// can touch. A bare table with no clip pushed walks every row, as it
// always did. The honest limits of v1: Tab wraps within the visible
// window (a culled row never registers), and the arrows rest on a
// row the walk cannot see - a step off the window's edge still lands
// (set_focus needs no registration; a scroll panel pulls the landing
// row into view the same frame), but a row the USER scrolled out of
// view never walks, and its stepping pauses until the scroll returns
// it.
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

// --- the table's split halves ------------------------------------------------
// table's look seam, the same shape every split family takes: the
// composed widget owns the geometry resolution and the grid walk (the
// focus stepping, the pick), the draws own the pixels - a custom grid
// recomputes nothing to repaint the stock look, and a skin replaces
// the header and the row independently.

// The resolved grid a header or row draw walks: the whole table's rect
// (the header at its top), the row height, the caller's column array
// and the stretch width the composed table resolved once. The column
// walk (fixed first, stretch sharing the remainder, clamped at the
// table's right edge) is recomputed by the draws, never stored.
struct TableGeometry {
    Rect rect{};
    float row_height = 0.0f;
    const TableColumn* columns = nullptr;
    int column_count = 0;
    float stretch_width = 0.0f;
};

// What one row's behavior hands its draw: the citizenship fold plus
// the selection display state (the caller's int, read and never
// written - the pick stays the composed walk's). `dimmed` carries the
// disabled scope's whole-table factor: a locked grid dims as one, the
// rows' own faces still ladder by their state.
struct TableRowPaint : WidgetPaint {
    bool selected = false;
    bool dimmed = false;
};

// The header's look: the face bar, the column labels in the cell
// geometry, and the 2-px rule that anchors the grid. `disabled` takes
// the scope's answer so a locked grid's chrome reads dead from its
// top. Chrome - the draw claims nothing.
void table_header_draw(const TableGeometry& geom, bool disabled,
                       const ChromeTheme& theme);

// One row's look: the ring, the bare-list face, the selected row's
// 3-px accent bar, then the row's cells in the column walk. Reads no
// context; `cells` is the row's slice of the caller's row-major array
// (column_count entries, null cells empty).
void table_row_draw(const Rect& row_rect, const TableRowPaint& paint,
                    const char* const* cells, const TableGeometry& geom,
                    const ChromeTheme& theme);

// --- the plot -----------------------------------------------------------------
// The waveform widget: one shared x axis, N borrowed series, and - when
// the data earns it - a second y spine, the shape every scope and
// results panel draws. The call is ONE slot in the parent's flow: it
// fits the axes to the VISIBLE data (the caller owns the visibility
// flags, like every display state here), lays the nice-number grid with
// engineering-notation labels, strokes the series (min/max decimated
// when a long sweep meets a narrow frame, so a 100k-point waveform
// still shows its envelope at ~2 vertices per pixel), and answers the
// hover: the nearest sample to the cursor, for the caller's legend
// readouts. The hands are the scope language: the wheel zooms both
// axes toward the cursor (the plot registers its own wheel claim - the
// innermost rect beats the enclosing scroll panel's), a left-drag
// rubber band zooms to the box, dragging on an axis strip stretches
// that axis alone, a middle or Space+left drag pans, a double-click
// resets to the auto fit, a right-click cancels the gesture in flight.
// The view never leaves the data: every manual window clamps inside
// the padded auto fit (home), the one rule all the gestures share.
//
// The state is the context's, like a scroll panel's offset: the manual
// view and the gesture in flight live in the persistent side table
// under the plot's id, so they survive frames and the plot vanishing
// for frames (a tab that comes back keeps its zoom) with no caller
// storage. The series themselves are BORROWED - `ys` spans re-read
// every frame, the cells/items discipline - and the caller re-describes
// the list each frame, so a same-label plot showing a different run is
// just a different call.
//
// v1 limits, deliberate: mouse-driven gestures only (no touch pan; a
// soft keyboard has no wheel), one x axis shared by every series, the
// legend is the caller's rows (checkboxes and live readouts are app
// chrome, built on selectable and the hover answer), no log axes, no
// markers/annotations, and the stock polyline joins are square (r2d
// strokes line segments; round joins are a canvas-side luxury). The
// crosshair is solid, not dashed, for the same reason.

// A closed interval with the endpoints in order - the plot's own range
// spelling, named in full so a TU holding both this header and
// ws::circsim's results math (the editor, today) reads unambiguously.
struct PlotAxisRange {
    double lo = 0.0;
    double hi = 0.0;
};

// Which y spine a series maps through. The left spine is the plot's
// primary scale; the right one exists only while some VISIBLE series
// claims it (a mixed V/I legend on one scale flattens itself into a
// lie, so each unit earns its own spine, own ticks, own drag strip).
enum class PlotSpine : std::uint8_t { left, right };

// One series, plain data with a borrowed span: `ys` holds
// `sample_count` samples (the plot call's own parameter - every series
// of one call shares the count, one x axis), re-read every frame.
// `color` 0 rides the stock series wheel (8 hues, i mod 8 - data
// colors, not chrome); `visible` is display state the row never
// writes, the caller's to own; `unit` (optional) suffixes the
// tooltip's readout ("V", "A"); `label` (optional) names the tooltip
// row - the legend the caller composes keys its own rows by whatever
// it wants.
struct PlotSeries {
    const char* label = nullptr;
    const double* ys = nullptr;
    std::uint32_t color = 0;
    bool visible = true;
    PlotSpine spine = PlotSpine::left;
    const char* unit = nullptr;
};

// The pure axis math, public so the headless suite (and any caller
// sizing its own readouts) gates the same decisions the plot renders -
// the popup_placement precedent. These mirror ws::circsim's plot.hpp
// contracts (the editor's hand-rolled Results panel rides those); they
// are re-homed here rather than linked because a gui widget must not
// reach into an app tier, and a future shared math tier could unify
// the two spellings without touching either consumer's calls.

// Tick positions covering [lo, hi] at roughly `target` intervals,
// snapped to the 1/2/5 x 10^k ladder every scope walks. Degenerate
// (lo == hi, or either non-finite) answers the single tick {lo};
// inverted or infinite spans answer none (a caller bug labels itself
// by drawing no grid at all); target 0 is treated as 1. The answer
// never exceeds the fixed 32-slot budget (a clamped target cannot ask
// for more), so the walk allocates nothing.
struct PlotTicks {
    std::array<double, 32> ticks{};
    unsigned count = 0;
    const double* begin() const { return ticks.data(); }
    const double* end() const { return ticks.data() + count; }
};

PlotTicks plot_ticks(double lo, double hi, unsigned target);

// SI engineering notation, the scope convention: 4700 -> "4.7k",
// -12.3e-3 -> "-12.3m", 2e-9 -> "2n", 0 -> "0" (never "-0"). Prefixes
// run f p n u m (none) k M G T - ASCII "u" for micro, the SPICE
// spelling; case matters. `digits` is the significant-digit budget
// (1..6, clamped); a mantissa that rounds up through its decade
// ("999.9m") renormalizes to the next prefix rather than printing
// 1000m, and magnitudes beyond the table fall back to plain %g.
// Non-finite inputs answer "nan"/"inf"/"-inf". Writes into `buf`
// (always NUL-terminated, truncated to fit) and returns it - the
// caller-scratch discipline, no allocation mid-frame.
const char* plot_format_eng(char* buf, std::size_t cap, double v,
                            int digits = 3);

// Pads [lo, hi] by `frac` of its span on both sides (default 5%), the
// breathing room a frame wants so extremal points are not ON the
// border. A degenerate lo == hi expands by frac of max(|lo|, 1) - a
// constant line still gets an axis; non-finite endpoints answer {0,
// 1}, the empty-but-sane frame; frac <= 0 is treated as 0.
PlotAxisRange plot_expand_range(double lo, double hi, double frac = 0.05);

// Clamps `view` back inside `home`: non-finite or degenerate answers
// home, a window at or past home's span answers home, otherwise the
// window slides the minimal distance so both ends sit inside - the
// invariant that keeps every manual window a healthy interval inside
// the data.
PlotAxisRange plot_clamp_view(PlotAxisRange view, PlotAxisRange home);

// Zooms `view` about `anchor` (a data coordinate, clamped into the
// view first) by `factor`: > 1 zooms IN (the span shrinks), < 1 out,
// 1 or non-finite or non-positive answers the view unchanged. The
// result is floored at home's span x 1e-6 (below that the axis is
// float mush) and clamped into home, so a wheel spun forever settles,
// never inverts.
PlotAxisRange plot_zoom_range(PlotAxisRange view, PlotAxisRange home,
                              double anchor, double factor);

// Slides `view` by `delta` data units (positive = toward hi) and
// clamps into home - a pan past the data's edge parks flush against
// it. A non-finite delta answers the view unchanged.
PlotAxisRange plot_pan_range(PlotAxisRange view, PlotAxisRange home,
                             double delta);

// --- the plot's split halves -----------------------------------------------
// The behavior/draw seam, the table's own shape: the behavior owns the
// geometry resolution, the auto fit, every gesture and the hover
// answer; the draw owns every pixel over the resolved frame. A custom
// look recomputes nothing to repaint the stock appearance.

// The gesture in flight, and the manual view it rides. Public (the
// ScrollState discipline) so a custom plot surface holds its own in
// the side table under its own id - the split halves take it by
// reference. `kind` none = the resting plot. The press-time captures
// (g*/was_active) exist so a right-click cancel restores the view
// exactly as the press found it, and the axis-drag anchors are the
// data coordinates the press mapped to, kept so the live zoom pivots
// where the drag began however the window moves under it.
enum class PlotGestureKind : std::uint8_t {
    none,
    pan,     // middle or Space+left drag: both axes follow the cursor
    band,    // left drag in the frame: the rubber box lands at release
    zoom_x,  // left drag on the bottom strip: the x axis stretches
    zoom_y,  // left drag on a y strip: that spine stretches
};

struct PlotState {
    // The manual view; inactive = the auto fit. Three windows - x,
    // the left spine, the right spine - each clamped inside its own
    // home by the behavior, never directly writable by a gesture.
    bool view_active = false;
    PlotAxisRange x{};
    PlotAxisRange y{};
    PlotAxisRange y1{};

    // The gesture in flight.
    PlotGestureKind kind = PlotGestureKind::none;
    float from_x = 0.0f;   // the press point, frame px
    float from_y = 0.0f;
    float click_x = 0.0f;  // the last press, for the double-click pair
    float click_y = 0.0f;
    float click_clock = 100.0f;  // seconds since that press (< 0.35: a pair)
    int zoom_axis = 0;           // zoom_y's strip: 0 left, 1 right
    bool was_active = false;     // the press-time view_active (cancel)
    PlotAxisRange gx{};          // press-time captures (cancel restores)
    PlotAxisRange gy{};
    PlotAxisRange gy1{};
    double anchor_x = 0.0;   // the strip drags' data anchors
    double anchor_y = 0.0;
    double anchor_y1 = 0.0;
};

// The resolved frame the behavior hands the draw (and any custom
// look): the placed rect, the inner plot frame with its margins, the
// effective view windows (the manual one clamped, or the auto fit),
// and the axis facts the mapping needs. The screen<->data maps ride
// it: sx/sy/sy1 map data into the frame's pixels, wx/wy/wy1 map the
// cursor back. The spans are positive by construction - the behavior
// only ever resolves windows that passed expand_range/clamp_view, so
// the maps never divide by zero.
struct PlotFrame {
    Rect rect{};    // the whole plot row (what layout.next placed)
    float fx = 0.0f;  // the inner frame's top-left
    float fy = 0.0f;
    float iw = 0.0f;  // the inner frame's size (the series' clip)
    float ih = 0.0f;
    float margin_right = 0.0f;  // 10 px, or 46 with the right spine
    bool has_right = false;     // a visible series rides the right spine
    bool by_index = false;      // the x axis is the sample index (xs null)
    bool x_sorted = false;      // xs strictly increasing (decimation rides)
    float cursor_x = 0.0f;      // the pointer's position, captured by the
    float cursor_y = 0.0f;      // behavior (the band's live corner reads it)
    PlotAxisRange view_x{};     // the effective windows
    PlotAxisRange view_y{};
    PlotAxisRange view_y1{};

    // Screen px -> data, through each spine's own window.
    double wx(float px) const {
        return view_x.lo +
               (static_cast<double>(px) - fx) / iw * (view_x.hi - view_x.lo);
    }
    double wy(float py) const {
        return view_y.hi -
               (static_cast<double>(py) - fy) / ih * (view_y.hi - view_y.lo);
    }
    double wy1(float py) const {
        return view_y1.hi -
               (static_cast<double>(py) - fy) / ih * (view_y1.hi - view_y1.lo);
    }
    // Data -> screen px.
    float sx(double v) const {
        return static_cast<float>(
            fx + (v - view_x.lo) / (view_x.hi - view_x.lo) * iw);
    }
    float sy(double v) const {
        return static_cast<float>(
            fy + (view_y.hi - v) / (view_y.hi - view_y.lo) * ih);
    }
    float sy1(double v) const {
        return static_cast<float>(
            fy + (view_y1.hi - v) / (view_y1.hi - view_y1.lo) * ih);
    }
};

// What the plot answers: the hover (the pointer rests in the frame
// under an open claim layer, and `hover` is the nearest sample to the
// cursor - binary-searched on a sorted axis, scanned otherwise, the
// index-axis rounded - with `hover_x` that sample's x, for the
// caller's legend readouts), and the frame's view news: `view_changed`
// a gesture moved the manual window this frame, `view_reset` a
// double-click dropped back to the auto fit (disjoint by design - a
// reset is not a move). A pointer outside the frame, or a raised
// modal veil, answers the resting plot: hovered false, hover -1.
struct PlotResult {
    bool hovered = false;
    int hover = -1;
    double hover_x = 0.0;
    bool view_changed = false;
    bool view_reset = false;
};

// The interaction half at a placed rect: fits the axes to the visible
// series, resolves the effective view, registers the wheel claim
// (frame plus strips - the innermost rect beats an enclosing scroll
// panel's, and the first notch after the cursor arrives settles
// ownership instead of zooming, the gui's one-frame claim rule), runs
// every gesture against `state` (which the caller holds - the
// composed plot keeps it in the persistent side table under the
// widget's id; a custom surface may hold its own), and resolves the
// hover. Fills `frame` with the resolved geometry, views and cursor
// position; answers the frame's news. Draws nothing. An empty plot
// (count <= 0) or one too narrow to frame (inner size under 40x30)
// resolves a sane-empty frame, claims nothing, hovers nothing.
PlotResult plot_behavior(GuiContext& gui, const Rect& rect,
                         const double* xs, const PlotSeries* series,
                         int series_count, int sample_count,
                         PlotFrame& frame, PlotState& state);

// The look half: the grid and zero rail, the tick labels in
// engineering notation (x below, the left spine right-aligned in its
// margin, the right spine in its own when it exists), the frame
// outline, the series (clipped to the frame, min/max decimated when
// the window is long and the axis is sorted, non-finite samples
// breaking the line), the hover crosshair and per-series markers, the
// floating tooltip (x plus one row per visible series, capped at 8
// with "+N more"; hidden while a gesture flies), the in-flight band
// with its dx/dy readout, the axis name, the manual-view hint, and
// the empty-state message. Reads no context; `state` is read-only
// here (the band's press point and the hint's view_active).
void plot_draw(const Rect& rect, const PlotFrame& frame,
               const PlotSeries* series, int series_count,
               int sample_count, const double* xs,
               const char* x_name, const PlotResult& result,
               const PlotState& state, const ChromeTheme& theme);

// The composed widget: places the row (`height` tall, default 220 -
// the scope strip; `width` lays like a button's, negative = fill the
// line), runs the behavior against the side-table state, paints
// through the painter row (a skin replaces the whole look in one
// override). `xs` is the shared x axis - null rides the sample index
// (0..sample_count-1), the table-without-a-sweep-axis shape;
// `series_count` series walk it, each holding `sample_count` samples
// - the one count every series shares, so the fits, the decimation
// and the hover all speak one length. `x_name` (optional) labels the
// axis, quiet, inside the frame's top-left. A negative or zero
// height lays a degenerate row and draws nothing.
PlotResult plot(GuiContext& gui, LayoutCursor& layout,
                const ChromeTheme& theme, const char* label,
                const double* xs, const PlotSeries* series,
                int series_count, int sample_count,
                const char* x_name = nullptr, float height = 220.0f,
                float width = -1.0f);

// --- menus ------------------------------------------------------------------
// Menus are data-driven: the caller owns a plain array of item structs
// and re-describes it every frame, the same discipline as combo's item
// list - a menu holds no state beyond what the context keeps (the open
// chain, the highlight, the submenu timers), so the same array drives a
// bar, a context menu and a submenu without the caller wiring anything.
// What fires comes back as a MenuEvent pointing at the caller's own
// MenuItem, read it the frame it arrives.

// What a menu entry is.
enum class MenuItemKind { action, check, radio, separator };

// One menu entry, plain data with borrowed strings - the caller's
// bytes, re-read each frame; keep them alive while the menu can draw
// them. `label` carries the '&' mnemonic spelling: the character after
// the first '&' is the mnemonic - underlined where menus draw it, and
// Alt+letter opens the bar title carrying one; "&&" is a literal
// ampersand. `icon` is the optional square an item draws inset, a
// button's. `shortcut` is the right-aligned hint - plain text the host
// supplies ("Ctrl+S"), never interpreted. A non-null `submenu` (with
// its count) makes the item a parent: it opens a nested menu instead of
// firing. `kind` picks the row's shape - action plain, check a box
// like a checkbox's, radio the dot, separator a rule that ignores
// every other field - and `checked` is the check/radio display state,
// the caller's to own. `disabled` lays a dead row no click or key
// reaches; `disabled_reason` is the tooltip a mouse rest shows on it.
struct MenuItem {
    const char* label = nullptr;
    r2d::Sprite icon = {};
    const char* shortcut = nullptr;
    const MenuItem* submenu = nullptr;
    int submenu_count = 0;
    MenuItemKind kind = MenuItemKind::action;
    bool checked = false;
    bool disabled = false;
    const char* disabled_reason = nullptr;
};

// One top-level menu of a bar: its title (an '&' mnemonic allowed,
// like an item's label) and its item list.
struct MenuDesc {
    const char* title = nullptr;
    const MenuItem* items = nullptr;
    int count = 0;
};

// What a menu reports the frame an item activates: the item itself -
// the caller's own MenuItem, by pointer - and the top-level menu that
// held it (menu_bar's index into its MenuDesc array; context_menu
// reports -1, its list having no parent). The event copies nothing.
struct MenuEvent {
    const MenuItem* item = nullptr;
    int menu = -1;
};

// One menu level's hover engine state: the row the pointer rests on
// and the seconds it has continuously rested there. The menus keep
// one per open-chain level; a caller laying its own menu-shaped
// popup keeps its own.
struct MenuHoverState {
    int item = -1;      // the row the pointer rests on (-1: none)
    float rest = 0.0f;  // seconds of continuous rest on that row
};

// What one step of the hover engine ripened.
enum class MenuHoverAction {
    none,           // resting, nothing ripe yet
    open_child,     // a live parent rested past the submenu delay
    show_tooltip,   // a disabled row rested past the tooltip delay
};

// The menus' hover engine, one step: name the row the pointer rests on
// this frame and inject the frame's dt, and the clock follows - a move
// to a new row restarts the rest. `parent` spells a live submenu row,
// `tooltip` a disabled row carrying a reason (mutually exclusive, so at
// most one answer fires); the delays arrive as parameters so the engine
// runs headless - the menu chain passes the theme's menu_open_delay
// and tooltip_delay. The answer repeats every ripe frame: apply it
// through your own state, the way the chain's open levels do, and
// clear the state by assignment ({}) when the pointer leaves every
// row.
MenuHoverAction menu_hover_step(MenuHoverState& hover, int item, float dt,
                                float open_delay, float tooltip_delay,
                                bool parent, bool tooltip);

// --- the menus' split halves --------------------------------------------------
// The chain and the bar own the interaction (the hover engine, the
// open chain, the keyboard walk - deeply stateful, and a skin never
// needs to re-run any of it); these draws own the row looks. A custom
// menu repaints its rows through them; the painters seam (P3) routes
// here.

// One menu level's resolved columns - what every row of the level
// lays out of, computed by the chain (it sizes the window from the
// same numbers). The gutter column exists when any row carries art
// (an icon, a check, a radio), the shortcut column when any row
// carries a hint, the marker room when any row opens a submenu.
struct MenuGeometry {
    float gutter = 0.0f;           // the art column's width (0: none)
    float marker = 0.0f;           // the submenu diamond's size
    float marker_room = 0.0f;      // the diamond's column (0: no parent)
    float shortcut_column = 0.0f;  // the hints' column (0: none)
    float content_width = 0.0f;    // the window's inner width
    float label_budget = 0.0f;     // the label column's fit budget
    float widest_shortcut = 0.0f;  // the hint column's measured width
};

// What one item's behavior hands its draw: the state() fold (items
// are not focusable citizens - the arrow walk is the menu's keyboard -
// so `focused` stays false on the stock chain, but a custom menu may
// drive it), the chain's aim on this row, and whether THIS row's
// submenu stands open.
struct MenuItemPaint : WidgetPaint {
    bool highlighted = false;  // the keyboard/hover aim rests here
    bool child_open = false;   // this row's submenu is open
};

// One menu item's look: the face ladder (dead, pressed, the
// highlight's accent, nothing at rest - the bare-list look), the
// gutter art (the icon, the check box, the radio dot), the label with
// its mnemonic underlined, the right-aligned shortcut hint, and the
// submenu diamond. Reads no context; the label fits recompute here.
void menu_item_draw(const Rect& rect, const MenuItemPaint& paint,
                    const ChromeTheme& theme, const MenuItem& item,
                    const MenuGeometry& geometry);

// The separator's look: the border-colored rule across the row's
// middle - a rule, not a row, nothing else to it.
void menu_item_separator_draw(const Rect& rect, const ChromeTheme& theme);

// What one bar title's behavior hands its draw: the citizenship fold
// plus the open flag (the title's face keys on it - the OPEN title
// wears the popup window's own face, so title and menu read as one
// connected surface).
struct MenuTitlePaint : WidgetPaint {
    bool open = false;  // this title's menu is open
};

// One bar title's look: the ring, the face ladder (press feedback,
// then the open title's panel face, then hover, nothing at rest - text
// on the bar strip), and the centered label with its mnemonic
// underlined. `title` carries the '&' mnemonic spelling.
void menu_title_draw(const Rect& rect, const MenuTitlePaint& paint,
                     const ChromeTheme& theme, const char* title);

// The menu bar: `menus` (count of them) laid as title rows flowing
// left, `open` the caller's index of the open menu (-1 = none, where
// the caller initializes it). The titles are focusable citizens - Tab
// walks them, Enter/Space toggles the focused one - and Alt+mnemonic
// opens the title carrying it from anywhere on the bar. A click opens
// a menu below its title, placed by place_popup; hovering another
// title while one is open switches to it. Items behave as MenuItem
// documents, flush rows; a parent item opens its submenu to the right
// after the mouse rests on it for theme.menu_open_delay (a touch opens
// it at a tap, no delay), and the chain closes on a click outside, on
// Escape - the topmost-overlay rule: the deepest open level first, each
// level claiming its own - or on a choice. The keyboard walks the
// deepest open menu: Up/Down move the highlight wrapping past
// separators and disabled rows, Enter activates the highlighted item,
// Right opens its submenu, Left closes the deepest one - and that menu
// owns the whole cluster: nothing beneath the bar sees those keys, or
// the typing, while a menu is open. A disabled item never highlights
// and never fires; a mouse rest on one shows its `disabled_reason` as
// a tooltip. Returns the activated item the frame it fires (.menu the
// root menu's index) - check, radio and separator rows included in the
// reporting contract: the caller owns the state they display, and a
// separator never fires. v1 limits, deliberate: menu items are not
// focusable citizens (the arrow walk is a menu's keyboard; Tab walks
// only the titles), and a bar does not wrap - count menus to fit the
// strip.
MenuEvent menu_bar(GuiContext& gui, LayoutCursor& layout,
                   const ChromeTheme& theme, const char* label,
                   const MenuDesc* menus, int count, int& open);

// The context menu: the popup that opens on right-click (mouse button
// 1) or a touch long-press - held past theme.long_press_time without
// wandering past theme.drag_threshold, the long-press-is-a-right-click
// rule - inside `region`, at the press point. `open` is the caller's
// bool; where the menu opened lives in the context's persistent state,
// so it holds still while open. The item rows are menu_bar's own
// renderer and rules - the keyboard walk, Escape through the
// arbitration, the click-outside close, the submenu delays - and the
// event reports the fired item with .menu at -1. A right-click while
// one is open moves it; the press that opens a menu never also closes
// it.
MenuEvent context_menu(GuiContext& gui, LayoutCursor& layout,
                       const ChromeTheme& theme, const char* label,
                       const Rect& region, const MenuItem* items,
                       int count, bool& open);

// --- the modal dialog -------------------------------------------------------
// The one-call overlay: a centered modal window the helper owns end to
// end - veil, panel, title, body, buttons, the Enter/Esc semantics -
// so a dialog is one call in the host's frame, not a scope it builds.

// The standard button sets, reading order left to right. `none` lays
// no buttons: the dialog then closes only by its `open` flipped from
// outside, and it takes no Enter and no Escape (nothing to answer
// with; those keys belong to whatever sits in the body).
enum class DialogButtons { none, ok, ok_cancel, yes_no, retry_cancel };

// What a dialog answers: the index of the standard button that fired
// (0 the positive, reading order; -1 when nothing fired this frame)
// and `accepted`, the semantic answer - true when the positive button
// or Enter spoke, false when the negative button or Escape did. A
// caller branching on yes-or-no reads `accepted` and never `button`.
struct DialogEvent {
    int button = -1;
    bool accepted = false;
};

// A modal dialog with a standard button row. `title` draws as the
// heading; `body` (optional) lays rows between it and the buttons - any
// widget calls, run in the dialog's own cursor and id scope, the veil
// trapping Tab inside. The window centers on the frame at `width`; its
// height is the body's extent measured the LAST frame it ran (one
// frame of settle the first time it opens - the immediate-mode answer,
// like a scroll panel's content extent). The buttons are real button
// rows: a click fires them, and Enter answers the positive one
// wherever the focus sits - consumed at the dialog's entry, so nothing
// in the body double-takes the key (Space stays a body widget's own).
// Escape answers the negative one through the same arbitration every
// overlay uses - the frame after the press, and a text field that drops
// its selection first spends the key: one Escape abandons the edit,
// the next closes the dialog. The dialog flips `open` false itself on
// any answer, so the event and the flag agree on the frame it closes -
// read `accepted` that same frame. Button labels are English stock in
// v1 (OK, Cancel, Yes, No, Retry); a themed label set is the seam a
// localization would grow.
DialogEvent modal_dialog(GuiContext& gui, const ChromeTheme& theme,
                         const char* label, const char* title,
                         DialogButtons buttons, bool& open,
                         float width = 360.0f,
                         const std::function<void(GuiContext&, LayoutCursor&,
                                                  const ChromeTheme&)>& body =
                             {});

// --- toasts -----------------------------------------------------------------
// The notification stratum: transient messages that stack above
// EVERYTHING - the toast layer sits above the modal veil, so a toast
// stays visible and its action stays clickable while a dialog blocks
// the world beneath. The host owns the store and pushes from its own
// code; draw_toasts draws it once a frame.

// The severity accent a toast wears - the theme's toast_* faces.
enum class ToastSeverity { info, warning, error };

// One notification, plain data with borrowed strings: the caller's
// bytes, re-read every frame the toast draws - keep them alive past
// clear(). `action` (optional) is a real button the toast lays; its
// click dismisses the toast and reports the slot through draw_toasts'
// return - a click anywhere on the body dismisses SILENTLY, so an
// app's action reply never fires on a mere dismissal. `duration` is
// the seconds a toast lives once shown; zero or less is sticky, and
// only a click or Toasts::dismiss removes it.
struct Toast {
    const char* text = nullptr;
    ToastSeverity severity = ToastSeverity::info;
    const char* action = nullptr;
    float duration = 4.0f;
};

// The host-owned toast store. Fixed slots: push past the cap drops the
// OLDEST toast and returns its slot - the flood policy, warned once
// per process; a notification stack never grows without bound, and the
// drop is the point, not a failure. Slot ids are stable from push to
// dismiss. No GuiContext anywhere in the store: push from wherever the
// app learns the news; only draw_toasts touches the context.
struct Toasts {
    // Enqueues `toast`; returns its slot id.
    int push(const Toast& toast);
    // Removes one toast by its slot id (a stale id is a no-op).
    void dismiss(int slot);
    // Removes every toast.
    void clear();
    // The live count.
    int count() const;

    // draw_toasts ages and lays the slots: the store's one consumer,
    // granted the internals so the public surface stays
    // push/dismiss/clear/count.
    friend int draw_toasts(GuiContext&, const ChromeTheme&, Toasts&, float);

private:
    static constexpr int kMaxToasts = 8;
    struct Slot {
        // Owned strings - the public Toast API uses borrowed pointers
        std::string text;
        std::string action;
        ToastSeverity severity = ToastSeverity::info;
        float duration = 4.0f;
        float age = 0.0f;   // seconds since push, drawn down against duration
        bool live = false;
    };
    std::array<Slot, kMaxToasts> slots_{};
    int live_count_ = 0;
};

// Draws the store's live toasts, bottom-right of the frame, stacking
// upward newest-nearest-the-corner, `bottom_clearance` px clear of the
// bottom edge (a status bar's height). Takes no row in any flow - call
// it from wherever, once a frame. Each toast is a panel wearing its
// severity accent as a left band, its text, and - when it carries one
// - its action button: a real, focusable, clickable button at the
// toast stratum, which sits above the modal veil, so it works while a
// dialog is up. A click on the body dismisses silently; the age runs
// down on the frame clock and the toast vanishes when it runs out.
// Returns the slot whose action fired this frame, -1 when none did.
int draw_toasts(GuiContext& gui, const ChromeTheme& theme, Toasts& toasts,
                float bottom_clearance = 0.0f);

// One toast's chrome: the floating frame (popup_frame_draw) plus the
// severity's accent band down the panel's left edge, the full height.
// The scope's content - the text row, the action button - stays the
// composed draw_toasts' own half; a skin replaces the chrome, the
// citizen rows inside keep working.
void toast_draw(const Rect& bounds, ToastSeverity severity,
                const ChromeTheme& theme);

}  // namespace ws::ui::gui
