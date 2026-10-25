#pragma once

// The private layer shared by every widget family file (see
// src/gui/CLAUDE.md): the per-widget persistent state structs and the
// row idioms each citizen widget walks, lifted whole from the old
// gui_widgets.cpp anonymous namespace. Internal to the gui src/ tree -
// never under include/ - and inline, so the one spelling links from
// every family TU.

#include <ws/ui/gui/widgets.hpp>

#include <ws/ui/renderer2d.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>

namespace ws::ui::gui {

// Where a label's line box starts vertically: the font's line height
// centered on the row (draw_text's y is the box top, not a baseline), or
// (fontless) half the row - the bar centering the bar branch does inline.
inline float label_top(float row_top, float row_height,
                       const ChromeTheme& theme) {
    const float h = theme.font != nullptr ? theme.font->line_height()
                                          : row_height * 0.5f;
    return row_top + (row_height - h) * 0.5f;
}

// A scroll region's persistent state and drag table (ScrollState,
// ScrollDragTrack, ScrollDragState) live in the public widgets.hpp now -
// the split scroll halves take them by reference, so a custom scrollable
// region holds its own in the side table.

// drag_float's persistent half (the same side-table discipline): the grab
// in flight - which pointer holds the press, where it started, and the
// value it started from. The anchor is what makes the drag RELATIVE:
// every frame maps the pointer's travel from the press, so no per-frame
// delta bookkeeping exists and a frame skipped mid-drag resumes exactly
// where the finger is.
struct DragState {
    ws::input::PointerId pointer = ws::input::kNoPointer;  // the grabbing pointer, while held
    float anchor_x = 0.0f;      // the grab's pointer x
    float anchor_value = 0.0f;  // the value at the grab
};

// The text field's persistent pair (TextInputState), the typing filter
// (is_insertable) and the per-frame edit lens (EditBuffer) live in the
// public widgets.hpp now - the field's split halves take the state by
// reference and speak the lens's verbs, so a custom text field holds
// its own and edits through the same moves.

// The contact's track: its own slot, or a free one for a fresh press.
// Null only when the table is somehow full, which the sweep in
// begin_scroll_panel makes unreachable (free slots always cover the
// untracked contacts: live contacts are capped at the track count).
inline ScrollDragTrack* drag_track_for(ScrollDragState& drag, ws::input::PointerId id) {
    for (ScrollDragTrack& track : drag.tracks) {
        if (track.id == id) {
            return &track;
        }
    }
    for (ScrollDragTrack& track : drag.tracks) {
        if (track.id == ws::input::kNoPointer) {
            return &track;
        }
    }
    return nullptr;
}

// The panel chrome begin_panel and begin_scroll_panel share: a 1px
// border-colored quad behind the panel face inset by 1. Drawn BEFORE the
// clip push, so the frame never clips against the content it contains.
// Degenerate panels clamp their face to nothing instead of drawing
// backwards.
inline void panel_chrome(const Rect& bounds, const ChromeTheme& theme) {
    r2d::draw_quad(bounds.x, bounds.y, bounds.width, bounds.height,
                   theme.border);
    r2d::draw_quad(bounds.x + 1.0f, bounds.y + 1.0f,
                   std::max(0.0f, bounds.width - 2.0f),
                   std::max(0.0f, bounds.height - 2.0f), theme.panel_bg);
}

// The overlay panel scope every stratum-raised window shares - the
// popup scope, the modal's panel half, and the menu windows: AFTER the
// caller pushes its layer (and the modal paints its veil), this pushes
// the clip ESCAPE at the bounds - r2d and the hit mirror push-for-push,
// so a window hanging past its host panel's edge draws AND hit-tests
// there - then the chrome, the content panel (`spacing` is the one
// caller difference: the gui's own overlays flow their theme's, menu
// windows lay their rows flush), the keyless wheel claim (an open
// overlay eats the wheel over the panel beneath) and the content clip
// pair. pop_overlay_panel pops exactly this, in reverse; the layer
// push and its end stay with the caller (each end guards its own
// stratum before popping anything).
inline void push_overlay_panel(GuiContext& gui, LayoutCursor& layout,
                               const ChromeTheme& theme, const char* label,
                               const Rect& bounds, float spacing) {
    r2d::push_clip_escape(bounds.x, bounds.y, bounds.width, bounds.height);
    gui.push_hit_clip_escape(bounds);
    panel_chrome(bounds, theme);
    const Rect content =
        layout.begin_panel(bounds, theme.padding, spacing);
    gui.register_wheel_claim(bounds, nullptr);
    r2d::push_clip(content.x, content.y, content.width, content.height);
    gui.push_hit_clip(content);
    gui.ids().push(label);
}

// push_overlay_panel's matching pop. Unconditional by design: the
// overlay ends all guard on their stratum first, so a stray pop cannot
// eat an outer scope.
inline void pop_overlay_panel(GuiContext& gui, LayoutCursor& layout) {
    gui.ids().pop();
    r2d::pop_clip();
    gui.pop_hit_clip();
    layout.end_panel();
    r2d::pop_clip();
    gui.pop_hit_clip();
}

// --- the row idioms every citizen widget shares ---------------------------
// The same moves, spelled once: the citizenship ladder (begin_row), the
// fill-width resolve, the Enter/Space edge, the focus ring, the disabled
// dim, the face ladder (and its bare-list exception), and the one label
// draw. Every family file reads as the moves; the spelling - and its
// reasoning - lives here, inline beside label_top above.

// Fill-width: a negative width means "the rest of the line" - the one
// size the caller cannot know before placing - resolved against the
// cursor, then clamped so a past-the-edge row never draws mirrored.
inline void resolve_width(float& width, const LayoutCursor& layout) {
    if (width < 0.0f) {
        width = layout.remaining_width();
    }
    width = std::max(width, 0.0f);
}

// Enter or Space on the focused widget: the row's keyboard activation,
// edge-only - a click with no pointer involved, and only while THIS
// widget holds the focus.
inline bool key_activated(const GuiContext& gui, WidgetId id) {
    return gui.is_focused(id) &&
           (gui.input().key_pressed[ws::input::keys::kEnter] ||
            gui.input().key_pressed[ws::input::keys::kSpace]);
}

// The focus ring: the on-accent as a 1px outline behind `body` - the
// rect the widget itself paints - so Tab navigation is legible. The
// draw is a SOLID quad that the body then masks to an outline, so body
// must be exactly what covers the row: a full-face row (a button's)
// passes the row rect; a partial one (the slider's rail, the
// checkbox's box) passes the part - the row's exposed area would
// otherwise flood with the accent the moment focus lands. `radius` is
// the body's own rounding; the circle and pill bodies pass their
// half-height. Skipped for degenerate rects - a zero-width body would
// show the ring with nothing in it.
//
// ring_draw is the draw half's spelling: focus arrives as a value the
// behavior already captured, so a look never reads the context.
// focus_ring (the unsplit widgets' convenience) delegates here.
inline void ring_draw(bool focused, const Rect& body, const ChromeTheme& theme,
                      float radius) {
    if (focused && body.width > 0.0f && body.height > 0.0f) {
        r2d::draw_rounded_quad(body.x - 1.0f, body.y - 1.0f,
                               body.width + 2.0f, body.height + 2.0f,
                               radius + 1.0f, theme.control_on);
    }
}

inline void focus_ring(const GuiContext& gui, WidgetId id, const Rect& body,
                       const ChromeTheme& theme, float radius) {
    ring_draw(gui.is_focused(id), body, theme, radius);
}

// The common spelling: the body wears the theme's control rounding.
inline void ring_draw(bool focused, const Rect& body,
                      const ChromeTheme& theme) {
    ring_draw(focused, body, theme, theme.corner_radius);
}

// The common spelling: the body wears the theme's control rounding.
inline void focus_ring(const GuiContext& gui, WidgetId id, const Rect& body,
                       const ChromeTheme& theme) {
    focus_ring(gui, id, body, theme, theme.corner_radius);
}

// The disabled dim: the theme's alpha under the lock, 1.0 the identity
// for the live states - the one factor every non-face color rides.
inline float dim_factor(WidgetState state, const ChromeTheme& theme) {
    return state == WidgetState::Disabled ? theme.disabled_alpha : 1.0f;
}

// The face a control row wears for a state: the four-way ladder, dead
// first - the dead face covers idle, hover and press (the whole row
// reads locked), and the pressed face owns the row even when the
// pointer drags off it (state()'s rule). A focused-at-rest row takes
// the plain background: the ring is its look, not a face.
inline std::uint32_t control_face(WidgetState state,
                                  const ChromeTheme& theme) {
    if (state == WidgetState::Disabled) {
        return theme.control_disabled;
    }
    if (state == WidgetState::Pressed) {
        return theme.control_pressed;
    }
    if (state == WidgetState::Hovered) {
        return theme.control_hovered;
    }
    return theme.control_bg;
}

// The bare-list face: the ladder minus its resting rung. Selectable
// rows and table rows draw no face at all until hover, press or the
// lock marks them - an untouched row is text on the parent's chrome,
// and a focused one wears the ring, not a face. nullopt: draw nothing.
inline std::optional<std::uint32_t> list_face(WidgetState state,
                                              const ChromeTheme& theme) {
    if (state == WidgetState::Disabled) {
        return theme.control_disabled;
    }
    if (state == WidgetState::Pressed) {
        return theme.control_pressed;
    }
    if (state == WidgetState::Hovered) {
        return theme.control_hovered;
    }
    return std::nullopt;
}

// One label draw for every row: the fitted text through the font's line
// box, or (fontless) the v1 bar at the fitted advance - vertically
// centered like label_top, at the caller's x (centered, left or right
// of the row is the widget's choice). One fit serves both themes: the
// font draws the returned text, the bar the returned advance; nothing
// drawable draws nothing.
inline void draw_label(float x, float row_top, float row_height,
                       const LabelFit& fit, std::uint32_t color,
                       const ChromeTheme& theme) {
    if (fit.advance <= 0.0f) {
        return;
    }
    if (theme.font != nullptr) {
        r2d::draw_text(x, label_top(row_top, row_height, theme), fit.text,
                       *theme.font, color);
    } else {
        const float bar_height = row_height * 0.5f;
        r2d::draw_quad(x, row_top + (row_height - bar_height) * 0.5f,
                       fit.advance, bar_height, color);
    }
}

// --- the split widgets' ladder and fold ------------------------------------
// The behavior/draw seam's two shared spells. A split widget composes
// three moves: begin_widget_row places and identifies, the behavior
// runs the citizenship and the interaction, the draw paints the result.

// The ladder's first half: the fill-width resolve, the id from the
// label, the placed rect - everything before the citizenship. The
// registration and state() belong to the behavior half next, which keeps
// their order (registration BEFORE state(): a press's focus claim reads
// it).
struct RowLayout {
    WidgetId id;
    Rect rect;
};

inline RowLayout begin_widget_row(GuiContext& gui, LayoutCursor& layout,
                                  const ChromeTheme& theme, const char* label,
                                  float width) {
    resolve_width(width, layout);
    const WidgetId id = gui.ids().id(label);
    return {id, layout.next(width, theme.control_height)};
}

// The behavior half's opening fold: the focusable registration (unless
// dead - Tab walks past), state()'s one hit-test, and the focus read the
// draw's ring keys on - taken after state(), because a press's focus
// claim latches inside it.
inline WidgetPaint begin_widget_paint(GuiContext& gui, WidgetId id,
                                      const Rect& rect, bool disabled,
                                      const HitShape& hit_shape) {
    if (!disabled) {
        gui.register_focusable(id);
    }
    WidgetPaint paint;
    const WidgetResult fold = gui.state(id, rect, disabled, hit_shape);
    paint.state = fold.state;
    paint.hit = fold.hit;
    paint.focused = gui.is_focused(id);
    return paint;
}

}  // namespace ws::ui::gui
