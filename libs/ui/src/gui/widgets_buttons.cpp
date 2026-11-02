// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the press-and-choose rows - button, checkbox, radio, toggle_switch.
// Each widget composes its split halves in order: the ladder places the
// row (begin_widget_row), the behavior owns the interaction, the draw
// owns the pixels - the public pair is the same seam a custom look uses.
// Every color and metric rides the ChromeTheme; the shared row idioms
// live in widgets_detail.hpp. P6: the button's variants resolve through
// the theme's tokens, the checkbox's mark rides the built-in icon set,
// and the static paint rects snap to the pixel grid.

#include "widgets_detail.hpp"

#include <algorithm>
#include <cstdint>

namespace ws::ui::gui {

bool button(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
            const char* label, float width, r2d::Sprite icon, bool disabled,
            ButtonStyle style) {
    const RowLayout row = begin_widget_row(gui, layout, theme, label, width);
    ButtonResult result =
        button_behavior(gui, row.id, row.rect, disabled, {}, style);
    fold_hover_t(gui, row.id, result, theme);  // P4: the face traveler
    paint(theme, &WidgetPainters::button, &button_draw, row.rect,
          result, theme, label, icon);
    return result.activated();
}

ButtonResult button_behavior(GuiContext& gui, WidgetId id, const Rect& rect,
                             bool disabled, const HitShape& hit_shape,
                             ButtonStyle style) {
    ButtonResult result{
        begin_widget_paint(gui, id, rect, disabled, hit_shape)};
    // Keyboard activation: Enter or Space fires the focused button, like
    // a click with no pointer involved. Edge-only (the frame the key went
    // down) and only while THIS button holds the focus.
    result.clicked = result.hit.clicked;
    result.key_clicked = key_activated(gui, id);
    result.style = style;  // the look half's variant, the behavior's carry
    return result;
}

// The variant faces (P6), resolved through tokens only: secondary is the
// plain ladder (control_face_of, the P4 blend included), primary walks
// the accent family, ghost draws NO face at rest (a bare-list row) and
// the hover/press faces live, and danger walks the danger family. The
// focused-at-rest ghost takes the plain background - a transparent body
// would flood with the ring behind it, the same exposed-area rule the
// partial bodies follow. nullopt: draw no face at all.
inline std::optional<std::uint32_t> button_face_of(
    const ButtonResult& result, const ChromeTheme& theme) {
    if (result.state == WidgetState::Disabled) {
        return theme.control_disabled;
    }
    switch (result.style) {
        case ButtonStyle::primary: {
            if (result.state == WidgetState::Pressed) {
                return theme.control_pressed;
            }
            if (result.state == WidgetState::Hovered &&
                result.hover_t >= 0.0f) {
                return mix_colors(theme.control_on, theme.accent_hover,
                                  result.hover_t);
            }
            if (result.state == WidgetState::Hovered) {
                return theme.accent_hover;
            }
            return theme.control_on;
        }
        case ButtonStyle::ghost: {
            if (result.state == WidgetState::Pressed) {
                return theme.control_pressed;
            }
            if (result.state == WidgetState::Hovered) {
                // The hover face fades IN on the traveler when one
                // speaks (the bare-list spelling - there is no resting
                // face to blend from).
                if (result.hover_t >= 0.0f) {
                    return fade_alpha(theme.control_hovered, result.hover_t);
                }
                return theme.control_hovered;
            }
            if (result.focused) {
                return theme.control_bg;  // the ring's backing, like the
                                          // ladder's focused-at-rest rule
            }
            return std::nullopt;
        }
        case ButtonStyle::danger: {
            if (result.state == WidgetState::Pressed) {
                return theme.danger_pressed;
            }
            if (result.state == WidgetState::Hovered &&
                result.hover_t >= 0.0f) {
                return mix_colors(theme.danger, theme.danger_hover,
                                  result.hover_t);
            }
            if (result.state == WidgetState::Hovered) {
                return theme.danger_hover;
            }
            return theme.danger;
        }
        case ButtonStyle::secondary:
        default:
            return control_face_of(result, theme);
    }
}

// The variant labels: the accent- and danger-faced buttons wear the
// on-accent label (the contrast pair the theme checks); the rest draw
// `text`.
inline std::uint32_t button_label_of(ButtonStyle style,
                                     const ChromeTheme& theme) {
    return style == ButtonStyle::primary || style == ButtonStyle::danger
               ? theme.text_on_accent
               : theme.text;
}

void button_draw(const Rect& rect, const ButtonResult& result,
                 const ChromeTheme& theme, const char* label,
                 r2d::Sprite icon) {
    // The focus ring: the on-accent outline behind the face, so Tab
    // navigation is legible. Skipped for degenerate rects - a
    // zero-width face would show the ring with nothing in it.
    ring_draw(result.focused, rect, theme);

    // The disabled look: the dead face for the control, everything else
    // (the icon tint, the label) dimmed through the theme's one helper -
    // 1.0 is the identity for the live states.
    const float dim_by = dim_factor(result.state, theme);

    // The face: the variant ladder (nullopt = the ghost's transparent
    // rest - the row IS the parent chrome until the pointer arrives).
    // The static rect snaps (P6); the ring's expansion stays the
    // behavior rect's own so hit and paint agree at the edges.
    const std::optional<std::uint32_t> face = button_face_of(result, theme);
    const Rect snapped = snap_rect(rect);
    if (face.has_value()) {
        r2d::draw_rounded_quad(snapped.x, snapped.y, snapped.width,
                               snapped.height, theme.corner_radius, *face);
    }

    // The optional icon: a square the control is tall minus the corner
    // insets, so it clears the rounded arcs; white tint keeps the asset's
    // own colors. The sentinel (page 0) draws none. The label bar centers
    // in whatever room the icon leaves.
    float label_x = rect.x;
    float label_room = rect.width;
    if (icon.page != 0) {
        const float inset = std::min(theme.corner_radius, rect.height * 0.5f);
        const float side = rect.height - 2.0f * inset;
        r2d::draw_sprite(snap_v(rect.x + inset), snap_v(rect.y + inset), side,
                         side, dim(r2d::rgba(255, 255, 255), dim_by), icon);
        const float used = rect.height + theme.spacing;
        label_x = rect.x + used;
        label_room = std::max(0.0f, rect.width - used);
    }

    // The label: fitted to the room the icon leaves (minus the face's
    // padding each side), centered - the variant's label color, the
    // draw_label origin snapped. A "##" suffix ids the button but
    // never draws; a too-wide label truncates to "..." inside the face
    // instead of dropping.
    char scratch[128];
    const LabelFit fit =
        fit_text(label, std::max(0.0f, label_room - 2.0f * theme.padding),
                 theme, scratch, sizeof(scratch));
    draw_label(label_x + (label_room - fit.advance) * 0.5f, rect.y,
               rect.height, fit, dim(button_label_of(result.style, theme),
                                     dim_by),
               theme);
}

bool checkbox(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
              const char* label, bool& value, bool disabled) {
    // The row auto-sizes: box, a gap, the label's DISPLAY advance - the
    // font's measure or the fontless per-char estimate, "##" suffixes
    // cut (they id the row, never size it); never clamped, so a label
    // always sizes its own row.
    const float gap = theme.spacing;
    char scratch[128];
    const LabelFit fit =
        fit_text(label, -1.0f, theme, scratch, sizeof(scratch));
    const RowLayout row = begin_widget_row(gui, layout, theme, label,
                                           theme.control_height + gap +
                                               fit.advance);
    CheckboxResult result =
        checkbox_behavior(gui, row.id, row.rect, value, disabled);
    fold_hover_t(gui, row.id, result, theme);  // P4: the face traveler
    paint(theme, &WidgetPainters::checkbox, &checkbox_draw, row.rect,
          result, theme, label, value);
    return result.toggled;
}

CheckboxResult checkbox_behavior(GuiContext& gui, WidgetId id,
                                 const Rect& rect, bool& value,
                                 bool disabled, const HitShape& hit_shape) {
    CheckboxResult result{
        begin_widget_paint(gui, id, rect, disabled, hit_shape)};
    result.toggled = result.hit.clicked || key_activated(gui, id);
    if (result.toggled) {
        value = !value;  // the caller's bool is the state; a click flips it
    }
    return result;
}

void checkbox_draw(const Rect& rect, const CheckboxResult& result,
                   const ChromeTheme& theme, const char* label,
                   bool checked) {
    const float gap = theme.spacing;
    const float box = theme.control_height;

    // The focus ring: behind the BOX - the body the checkbox paints -
    // not the row: the label's half of the row is exposed, and the solid
    // ring draw would flood it with the accent the moment focus lands.
    ring_draw(result.focused, {rect.x, rect.y, box, box}, theme);

    // The disabled look: the dead face for the box, the fill and the
    // label dimmed through the one helper - 1.0 is the identity for the
    // live states.
    const float dim_by = dim_factor(result.state, theme);

    // The box: a control_height square with the same faces a button
    // wears, corners rounded by the theme - the snapped square (P6).
    const std::uint32_t face = control_face_of(result, theme);
    const Rect snapped = snap_rect({rect.x, rect.y, box, box});
    r2d::draw_rounded_quad(snapped.x, snapped.y, snapped.width,
                           snapped.height, theme.corner_radius, face);

    // The check (P6): the accent fill as ever, then the built-in check
    // glyph on top in the on-accent label color - the state reads as a
    // mark, not a solid block, when the theme's font carries the set.
    // The sentinel sprite (fontless, or a skin that answers nothing)
    // degrades to the pre-icon square: today's look, byte for byte.
    const float inset = std::max(theme.check_inset, theme.corner_radius);
    const float fill = box - 2.0f * inset;
    if (checked && fill > 0.0f) {
        r2d::draw_quad(snapped.x + inset, snapped.y + inset, fill, fill,
                       dim(theme.control_on, dim_by));
        const r2d::Sprite mark = icon_sprite_of(theme, Icon::check);
        if (mark.page != 0) {
            const float mark_side =
                std::max(1.0f, fill - theme.label_inset);
            const float mark_x =
                snapped.x + inset + (fill - mark_side) * 0.5f;
            const float mark_y =
                snapped.y + inset + (fill - mark_side) * 0.5f;
            draw_icon(mark_x, mark_y, mark_side, mark,
                      dim(theme.text_on_accent, dim_by));
        }
    }

    // The label beside the box, vertically centered: the theme font's
    // line box, or (fontless) the v1 bar at the fitted advance. The row
    // sized itself to that advance, so no truncation case can fire here
    // - the -1 fit IS the draw's answer.
    char scratch[128];
    const LabelFit fit =
        fit_text(label, -1.0f, theme, scratch, sizeof(scratch));
    draw_label(rect.x + box + gap, rect.y, rect.height, fit,
               dim(theme.text, dim_by), theme);
}

bool radio(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
           const char* label, int& value, int this_option, bool disabled) {
    // The row auto-sizes like a checkbox's: dot, a gap, the label's
    // DISPLAY advance ("##" suffixes id the row, never size it).
    const float gap = theme.spacing;
    char scratch[128];
    const LabelFit fit =
        fit_text(label, -1.0f, theme, scratch, sizeof(scratch));
    const RowLayout row = begin_widget_row(gui, layout, theme, label,
                                           theme.control_height + gap +
                                               fit.advance);
    RadioResult result =
        radio_behavior(gui, row.id, row.rect, value, this_option, disabled);
    fold_hover_t(gui, row.id, result, theme);  // P4: the face traveler
    paint(theme, &WidgetPainters::radio, &radio_draw, row.rect, result,
          theme, label, value == this_option);
    return result.moved;
}

RadioResult radio_behavior(GuiContext& gui, WidgetId id, const Rect& rect,
                           int& value, int this_option, bool disabled,
                           const HitShape& hit_shape) {
    RadioResult result{
        begin_widget_paint(gui, id, rect, disabled, hit_shape)};
    // Selecting the selected option is an idempotent write that reports
    // false: true means the selection MOVED to this option this frame.
    const bool select = result.hit.clicked || key_activated(gui, id);
    const bool was_other = value != this_option;
    if (select) {
        value = this_option;
    }
    result.moved = select && was_other;
    return result;
}

void radio_draw(const Rect& rect, const RadioResult& result,
                const ChromeTheme& theme, const char* label, bool selected) {
    const float gap = theme.spacing;
    const float box = theme.control_height;

    // The focus ring: behind the DOT, asked for at the dot's own
    // half-height so the ring curves with it - the same exposed-label
    // rule the checkbox's box follows.
    ring_draw(result.focused, {rect.x, rect.y, box, box}, theme, box * 0.5f);

    // The disabled look: the dead face for the dot, the fill and the
    // label dimmed through the one helper.
    const float dim_by = dim_factor(result.state, theme);

    // The dot: a control_height circle - the radius asks for half the
    // box, exactly what the draw clamps to - with the faces a button
    // wears; the snapped square (P6).
    const std::uint32_t face = control_face_of(result, theme);
    const Rect snapped = snap_rect({rect.x, rect.y, box, box});
    r2d::draw_rounded_quad(snapped.x, snapped.y, snapped.width,
                           snapped.height, box * 0.5f, face);

    // The selection: an accent circle inset by check_inset alone - the
    // dot is all corner, no straight edge for a fill to poke past, so
    // the checkbox's corner-radius clearance has nothing to clear.
    // Drawn only while this option holds the value, and only when the
    // dot leaves room.
    const float fill = box - 2.0f * theme.check_inset;
    if (selected && fill > 0.0f) {
        r2d::draw_rounded_quad(snapped.x + theme.check_inset,
                               snapped.y + theme.check_inset, fill, fill,
                               fill * 0.5f, dim(theme.control_on, dim_by));
    }

    // The label beside the dot - the same draw a checkbox's label gets.
    char scratch[128];
    const LabelFit fit =
        fit_text(label, -1.0f, theme, scratch, sizeof(scratch));
    draw_label(rect.x + box + gap, rect.y, rect.height, fit,
               dim(theme.text, dim_by), theme);
}

bool toggle_switch(GuiContext& gui, LayoutCursor& layout,
                   const ChromeTheme& theme, const char* label, bool& value,
                   bool disabled) {
    // The row auto-sizes like a checkbox's: the pill (twice the height
    // wide, the height tall), a gap, the label's DISPLAY advance.
    const float gap = theme.spacing;
    const float box = theme.control_height;
    const float track_w = 2.0f * box;
    char scratch[128];
    const LabelFit fit =
        fit_text(label, -1.0f, theme, scratch, sizeof(scratch));
    const RowLayout row =
        begin_widget_row(gui, layout, theme, label, track_w + gap + fit.advance);
    ToggleSwitchResult result =
        toggle_switch_behavior(gui, row.id, row.rect, value, disabled);
    fold_hover_t(gui, row.id, result, theme);  // P4: the face traveler
    // P4: the knob traveler - off's end to on's end under the same id
    // (a distinct side-table type; no collision with the face blend).
    result.travel = gui.animate_toward(row.id, value ? 1.0f : 0.0f,
                                       motion_duration(theme));
    paint(theme, &WidgetPainters::toggle_switch, &toggle_switch_draw,
          row.rect, result, theme, label, value);
    return result.toggled;
}

ToggleSwitchResult toggle_switch_behavior(GuiContext& gui, WidgetId id,
                                          const Rect& rect, bool& value,
                                          bool disabled,
                                          const HitShape& hit_shape) {
    ToggleSwitchResult result{
        begin_widget_paint(gui, id, rect, disabled, hit_shape)};
    result.toggled = result.hit.clicked || key_activated(gui, id);
    if (result.toggled) {
        value = !value;  // the caller's bool is the state; a click flips it
    }
    return result;
}

void toggle_switch_draw(const Rect& rect, const ToggleSwitchResult& result,
                        const ChromeTheme& theme, const char* label,
                        bool on) {
    const float gap = theme.spacing;
    const float box = theme.control_height;
    const float track_w = 2.0f * box;

    // The focus ring: behind the PILL, asked for at the pill's own
    // half-height so the ring curves with its ends - the same
    // exposed-label rule the checkbox's box follows.
    ring_draw(result.focused, {rect.x, rect.y, track_w, box}, theme,
              box * 0.5f);

    // The disabled look: the knob and the label dimmed through the one
    // helper; the track's own dead face is the off state's, while the
    // on state dims its accent (below) like the checkbox's fill.
    const float dim_by = dim_factor(result.state, theme);

    // The pill: the control faces while off; the accent IS the track
    // while on - dimmed under the disabled scope rather than deadened,
    // so the state stays readable. The radius is the pill role (P5): a
    // themed radius_pill, else the track's own half-height - the full
    // pill the stock has always drawn. The static track snaps (P6).
    std::uint32_t face = control_face_of(result, theme);
    if (on) {
        face = dim(theme.control_on, dim_by);
    }
    const Rect snapped = snap_rect({rect.x, rect.y, track_w, box});
    r2d::draw_rounded_quad(snapped.x, snapped.y, snapped.width,
                           snapped.height, pill_or_half(theme, box * 0.5f),
                           face);

    // The knob: a near-white circle (the text color - the bright head
    // reads on both faces) filling the pill's height minus the check
    // inset, traveling between its ends on the result's P4 traveler
    // (snapped when no animation speaks). The KNOB stays unsnapped
    // while it travels (a moving element, the detail header's rule).
    const float knob = box - 2.0f * theme.check_inset;
    if (knob > 0.0f) {
        // P4: the knob travels on the result's traveler - snapped to
        // its end when no animation speaks (-1) or motion is off (the
        // traveler reads exactly 0 or 1 then, and mix_colors is exact
        // at its ends).
        const float at = result.travel >= 0.0f
                             ? result.travel
                             : (on ? 1.0f : 0.0f);
        const float span = track_w - 2.0f * theme.check_inset - knob;
        const float x = rect.x + theme.check_inset + at * span;
        r2d::draw_rounded_quad(x, snapped.y + theme.check_inset, knob, knob,
                               knob * 0.5f, dim(theme.text, dim_by));
    }

    // The label beside the pill - the same draw a checkbox's label gets.
    char scratch[128];
    const LabelFit fit =
        fit_text(label, -1.0f, theme, scratch, sizeof(scratch));
    draw_label(rect.x + track_w + gap, rect.y, rect.height, fit,
               dim(theme.text, dim_by), theme);
}

}  // namespace ws::ui::gui
