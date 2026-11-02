#pragma once

// The private layer shared by every widget family file (see
// src/gui/CLAUDE.md): the per-widget persistent state structs and the
// row idioms each citizen widget walks, lifted whole from the old
// gui_widgets.cpp anonymous namespace. Internal to the gui src/ tree -
// never under include/ - and inline, so the one spelling links from
// every family TU.

#include <ws/ui/gui/widgets.hpp>
#include <ws/ui/gui/painters.hpp>

#include <ws/ui/renderer2d.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>
#include <utility>

namespace ws::ui::gui {

// --- the painter routing fold ------------------------------------------------
// The one indirection every composed widget walks (P3's seam): the
// theme's row when a skin installed a table AND filled that row, the
// stock draw otherwise. A null table and a null row are the same
// answer - the stock look - so partial skins are legal and the stock
// path stays a predictable never-taken branch. The static assertion
// pins the pair: `stock` must name exactly the function type its row
// holds, so the two spellings cannot drift apart at a call site.
template <typename Row, typename Stock, typename... Args>
inline void paint(const ChromeTheme& theme, Row WidgetPainters::*row,
                  Stock stock, Args&&... args) {
    static_assert(std::is_same_v<Row, Stock>,
                  "the painter row and the stock draw must agree");
    const WidgetPainters* table = theme.painters;
    if (table != nullptr && (table->*row) != nullptr) {
        (table->*row)(std::forward<Args>(args)...);
    } else {
        stock(std::forward<Args>(args)...);
    }
}

// --- P4's motion spelling ----------------------------------------------------
// The duration a traveler runs: the theme's one knob, gated by its own
// switch - 0 (INSTANT) whenever animation is off, so every traveler
// below collapses to the exact pre-P4 value by construction.
inline float motion_duration(const ChromeTheme& theme) {
    return theme.anim_enabled ? theme.anim_duration : 0.0f;
}

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

// The rect a themed shadow needs around `bounds` (P5): the window grown
// by the blur on every side and the vertical drop below, or the bounds
// themselves when the theme's shadow tokens are zero - the one spelling
// every overlay scope opens its clip escape on, so a shadow paints past
// the window rect and a shadowless theme clips exactly as before.
inline Rect shadow_bounds(const Rect& bounds, const ChromeTheme& theme) {
    const float blur = theme.shadow_blur;
    if (blur <= 0.0f) {
        return bounds;
    }
    const float drop = std::max(0.0f, theme.shadow_offset_y);
    return {bounds.x - blur, bounds.y - blur, bounds.width + 2.0f * blur,
            bounds.height + 2.0f * blur + drop};
}

// The panel chrome begin_panel and begin_scroll_panel share: the border
// as a rounded outline (P5) - the outer rounded quad in the border face,
// the inner one inset by border_width in the panel face - at the theme's
// PANEL radius. A sub-pixel radius renders the same squares the old
// straight-quad pair did (draw_rounded_quad's own clamp), so one spelling
// covers square and rounded skins alike. Degenerate panels clamp their
// face to nothing instead of drawing backwards.
inline void panel_chrome(const Rect& bounds, const ChromeTheme& theme) {
    r2d::draw_rounded_outline(bounds.x, bounds.y, bounds.width, bounds.height,
                              theme.panel_radius(),
                              std::max(1.0f, theme.border_width),
                              theme.border, theme.panel_bg);
}

// The overlay panel scope every stratum-raised window shares - the
// popup scope, the modal's panel half, and the menu windows: AFTER the
// caller pushes its layer (and the modal paints its veil), this pushes
// the clip ESCAPE at the bounds - r2d and the hit mirror push-for-push,
// so a window hanging past its host panel's edge draws AND hit-tests
// there - then the chrome (popup_frame_draw, the split look seam every
// floating window wears), the content panel (`spacing` is the one
// caller difference: the gui's own overlays flow their theme's, menu
// windows lay their rows flush), the keyless wheel claim (an open
// overlay eats the wheel over the panel beneath) and the content clip
// pair. pop_overlay_panel pops exactly this, in reverse; the layer
// push and its end stay with the caller (each end guards its own
// stratum before popping anything).
inline void push_overlay_panel(GuiContext& gui, LayoutCursor& layout,
                               const ChromeTheme& theme, const char* label,
                               const Rect& bounds, float spacing) {
    // The clip ESCAPE opens on the shadow's bounds, not the panel's (P5):
    // a themed shadow spreads past the window rect, and the escape exists
    // exactly so a floating window may paint past its host's clip. The
    // HIT escape stays at the bounds - a shadow is not a target.
    const Rect escape = shadow_bounds(bounds, theme);
    r2d::push_clip_escape(escape.x, escape.y, escape.width, escape.height);
    gui.push_hit_clip_escape(bounds);
    paint(theme, &WidgetPainters::popup_frame, &popup_frame_draw, bounds,
          theme);
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

// The focus ring: the on-accent outline behind `body` - the rect the
// widget itself paints - so Tab navigation is legible. The draw is a
// SOLID rounded quad expanded by the theme's focus_ring_width that the
// body then masks to a ring (the behind-body form: a true stroke would
// need the body's face to repaint its middle, which the ring must not
// know), so body must be exactly what covers the row: a full-face row
// (a button's) passes the row rect; a partial one (the slider's rail,
// the checkbox's box) passes the part - the row's exposed area would
// otherwise flood with the accent the moment focus lands. `radius` is
// the body's own rounding; the circle and pill bodies pass their
// half-height. The width is the theme's focus_ring_width (P5, was a
// hardcoded 1 - the stock default keeps every ring at today's weight);
// the expansion grows with it so a thicker ring reads as a thicker
// ring, not a halo. Skipped for degenerate rects - a zero-width body
// would show the ring with nothing in it.
//
// ring_draw is the draw half's spelling: focus arrives as a value the
// behavior already captured, so a look never reads the context.
// focus_ring (the unsplit widgets' convenience) delegates here.
inline void ring_draw(bool focused, const Rect& body, const ChromeTheme& theme,
                      float radius) {
    if (focused && body.width > 0.0f && body.height > 0.0f) {
        const float w = std::max(1.0f, theme.focus_ring_width);
        r2d::draw_rounded_quad(body.x - w, body.y - w, body.width + 2.0f * w,
                               body.height + 2.0f * w, radius + w,
                               theme.control_on);
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

// --- pixel snapping (P6) ------------------------------------------------------
// The stock painters' alignment: static rects and 1px-class strokes snap
// to whole device pixels so a scaled theme's fractional metrics
// (control_height 28 at 1.3 scale = 36.4) land on the pixel grid instead
// of smearing the SDF's antialiasing over every edge. PAINT ONLY - the
// behavior halves never snap (the hit rects stay fractional; only the
// pixels round), and the ANIMATED values (the toggle knob's travel x, a
// slider handle mid-drag) stay unsnapped: a moving element that jumps
// pixel to pixel reads as stepping, and the SDF's AA is the softer
// answer while something moves.

// Half-up rounding, the one spelling (std::round's banker's-rounding
// ties would make 36.5 land differently at 36 and 37).
inline float snap_v(float v) { return std::floor(v + 0.5f); }

// The rect snap: origin rounds, the edges round in place and the size
// re-derives from them - a snapped rect keeps its snapped edges crisp
// (rounding x and width independently could grow the rect by a pixel on
// the far side).
inline Rect snap_rect(const Rect& r) {
    const float x = snap_v(r.x);
    const float y = snap_v(r.y);
    const float x2 = snap_v(r.x + r.width);
    const float y2 = snap_v(r.y + r.height);
    return {x, y, std::max(0.0f, x2 - x), std::max(0.0f, y2 - y)};
}

// The pill shapes' radius (P5): the theme's radius_pill when a skin set
// one, else the shape's OWN half-height - the full pill, the stock look.
// (ChromeTheme::pill_radius() falls back to corner_radius, the radius
// ROLES' shared rule; the toggle track and the scrollbar thumb predate
// that fallback and stay full pills unless a theme says otherwise.)
inline float pill_or_half(const ChromeTheme& theme, float half_height) {
    return theme.radius_pill >= 0.0f ? theme.radius_pill : half_height;
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

// The channel-wise blend of two packed colors at t in [0, 1]: exact at
// both ends (t = 0 answers a byte-for-byte, t = 1 answers b), so the
// blend never shifts the instant look.
inline std::uint32_t mix_colors(std::uint32_t a, std::uint32_t b, float t) {
    if (t <= 0.0f) {
        return a;
    }
    if (t >= 1.0f) {
        return b;
    }
    const auto channel = [](std::uint32_t ca, std::uint32_t cb, float k) {
        const float blended =
            static_cast<float>(ca) +
            (static_cast<float>(cb) - static_cast<float>(ca)) * k;
        return static_cast<std::uint32_t>(blended + 0.5f);
    };
    const std::uint32_t r = channel(a >> 24, b >> 24, t);
    const std::uint32_t g = channel((a >> 16) & 0xFFu, (b >> 16) & 0xFFu, t);
    const std::uint32_t bl = channel((a >> 8) & 0xFFu, (b >> 8) & 0xFFu, t);
    const std::uint32_t al = channel(a & 0xFFu, b & 0xFFu, t);
    return (r << 24) | (g << 16) | (bl << 8) | al;
}

// The alpha-only fade of a packed color at t in [0, 1]: exact at both
// ends, the channel the "face fades IN from transparent" travelers use
// (a bare-list row blends nothing - its hover face arrives).
inline std::uint32_t fade_alpha(std::uint32_t color, float t) {
    if (t <= 0.0f) {
        return color & 0xFFFFFF00u;  // fully transparent, hue kept
    }
    if (t >= 1.0f) {
        return color;
    }
    const float a = static_cast<float>(color & 0xFFu) * t;
    return (color & 0xFFFFFF00u) | static_cast<std::uint32_t>(a + 0.5f);
}

// The overlay scopes' fade bookkeeping (P4): one TYPE for the side
// table, shared by the open and closed branches of every overlay scope
// (two same-named local structs would be two distinct types - two
// tables - and the closed branch's re-arm would never reach the open
// branch's state).
struct OverlayFade {
    bool was_open = false;
};

// The overlay fade's tint: white with the alpha scaled by t - a pure
// fade, exact at both ends (t = 1 is the identity tint, t = 0 hides
// everything - the traveler's first frame draws nothing, the fade-in).
inline std::uint32_t fade_tint(float t) {
    if (t <= 0.0f) {
        return 0xFFFFFF00u;
    }
    if (t >= 1.0f) {
        return 0xFFFFFFFFu;
    }
    const std::uint32_t a = static_cast<std::uint32_t>(t * 255.0f + 0.5f);
    return 0xFFFFFF00u | a;
}

// The control face a row wears, P4's blend included: the plain ladder
// when no animation speaks (hover_t < 0 - hand-built paints, disabled
// themes), the resting face blended toward its hover face otherwise.
// Pressed and dead stay INSTANT whatever the traveler says - latency
// beats smoothness on the states that confirm a touch.
inline std::uint32_t control_face_of(const WidgetPaint& paint,
                                     const ChromeTheme& theme) {
    if (paint.hover_t < 0.0f || paint.state == WidgetState::Pressed ||
        paint.state == WidgetState::Disabled) {
        return control_face(paint.state, theme);
    }
    return mix_colors(theme.control_bg, theme.control_hovered, paint.hover_t);
}

// The bare-list face a row wears, P4's fade included: nothing at rest
// (as ever), the hover face fading IN on the traveler when animation
// speaks. Pressed and dead stay instant; the disabled scope's dim still
// rides the caller's dim_by.
inline std::optional<std::uint32_t> list_face_of(const WidgetPaint& paint,
                                                 const ChromeTheme& theme) {
    if (paint.hover_t >= 0.0f && paint.state == WidgetState::Hovered) {
        return fade_alpha(theme.control_hovered, paint.hover_t);
    }
    return list_face(paint.state, theme);
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
    // The label's origin snaps (P6): a fractional row center must not
    // smear every glyph over a pixel boundary - the box top snaps, in
    // whichever branch draws.
    x = snap_v(x);
    if (theme.font != nullptr) {
        r2d::draw_text(x, snap_v(label_top(row_top, row_height, theme)),
                       fit.text, *theme.font, color);
    } else {
        const float bar_height = row_height * 0.5f;
        r2d::draw_quad(x, snap_v(row_top + (row_height - bar_height) * 0.5f),
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
// claim latches inside it. Deliberately theme-free (the behavior
// contract): P4's face traveler folds in the COMPOSED widget, which
// owns the theme - see fold_hover_t below.
inline WidgetPaint begin_widget_paint(GuiContext& gui, WidgetId id,
                                      const Rect& rect, bool disabled,
                                      const HitShape& hit_shape = {}) {
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

// P4's face traveler, the composed widget's fold: the hover blend value
// under the behavior's id, INSTANT (0 or 1 exactly) whenever the theme's
// motion is off. A distinct side-table type per traveler, so a widget's
// own state and its hover blend never collide under one id.
inline void fold_hover_t(GuiContext& gui, WidgetId id, WidgetPaint& paint,
                         const ChromeTheme& theme) {
    paint.hover_t = gui.animate_toward(
        id,
        paint.state == WidgetState::Hovered || paint.state == WidgetState::Pressed
            ? 1.0f
            : 0.0f,
        motion_duration(theme));
}

}  // namespace ws::ui::gui
