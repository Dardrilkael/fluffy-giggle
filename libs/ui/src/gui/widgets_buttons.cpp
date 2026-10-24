// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the press-and-choose rows - button, checkbox, radio, toggle_switch.
// Each widget composes its split halves in order: the ladder places the
// row (begin_widget_row), the behavior owns the interaction, the draw
// owns the pixels - the public pair is the same seam a custom look uses.
// Every color and metric rides the ChromeTheme; the shared row idioms
// live in widgets_detail.hpp.

#include "widgets_detail.hpp"

#include <algorithm>
#include <cstdint>

namespace ws::ui::gui {

bool button(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
            const char* label, float width, r2d::Sprite icon,
            bool disabled) {
    const RowLayout row = begin_widget_row(gui, layout, theme, label, width);
    const ButtonResult result =
        button_behavior(gui, row.id, row.rect, disabled);
    button_draw(row.rect, result, theme, label, icon);
    return result.activated();
}

ButtonResult button_behavior(GuiContext& gui, WidgetId id, const Rect& rect,
                             bool disabled, const HitShape& hit_shape) {
    ButtonResult result{
        begin_widget_paint(gui, id, rect, disabled, hit_shape)};
    // Keyboard activation: Enter or Space fires the focused button, like
    // a click with no pointer involved. Edge-only (the frame the key went
    // down) and only while THIS button holds the focus.
    result.clicked = result.hit.clicked;
    result.key_clicked = key_activated(gui, id);
    return result;
}

void button_draw(const Rect& rect, const ButtonResult& result,
                 const ChromeTheme& theme, const char* label,
                 r2d::Sprite icon) {
    // The focus ring: the on-accent as a 1px outline behind the face, so
    // Tab navigation is legible. Skipped for degenerate rects - a
    // zero-width face would show the ring with nothing in it.
    ring_draw(result.focused, rect, theme);

    // The disabled look: the dead face for the control, everything else
    // (the icon tint, the label) dimmed through the theme's one helper -
    // 1.0 is the identity for the live states.
    const float dim_by = dim_factor(result.state, theme);

    const std::uint32_t face = control_face(result.state, theme);
    r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                           theme.corner_radius, face);

    // The optional icon: a square the control is tall minus the corner
    // insets, so it clears the rounded arcs; white tint keeps the asset's
    // own colors. The sentinel (page 0) draws none. The label bar centers
    // in whatever room the icon leaves.
    float label_x = rect.x;
    float label_room = rect.width;
    if (icon.page != 0) {
        const float inset = std::min(theme.corner_radius, rect.height * 0.5f);
        const float side = rect.height - 2.0f * inset;
        r2d::draw_sprite(rect.x + inset, rect.y + inset, side, side,
                         dim(r2d::rgba(255, 255, 255), dim_by), icon);
        const float used = rect.height + theme.spacing;
        label_x = rect.x + used;
        label_room = std::max(0.0f, rect.width - used);
    }

    // The label: fitted to the room the icon leaves (minus the face's
    // padding each side), centered. A "##" suffix ids the button but
    // never draws; a too-wide label truncates to "..." inside the face
    // instead of dropping.
    char scratch[128];
    const LabelFit fit =
        fit_text(label, std::max(0.0f, label_room - 2.0f * theme.padding),
                 theme, scratch, sizeof(scratch));
    draw_label(label_x + (label_room - fit.advance) * 0.5f, rect.y,
               rect.height, fit, dim(theme.text, dim_by), theme);
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
    const CheckboxResult result =
        checkbox_behavior(gui, row.id, row.rect, value, disabled);
    checkbox_draw(row.rect, result, theme, label, value);
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
    // wears, corners rounded by the theme.
    const std::uint32_t face = control_face(result.state, theme);
    r2d::draw_rounded_quad(rect.x, rect.y, box, box, theme.corner_radius, face);

    // The check: the accent fill, inset past the box's corners so a
    // heavily-rounded theme cannot let the square fill poke outside the
    // arcs - drawn only while on, and only when the box leaves room
    // (tiny control heights clamp to none).
    const float inset = std::max(theme.check_inset, theme.corner_radius);
    const float fill = box - 2.0f * inset;
    if (checked && fill > 0.0f) {
        r2d::draw_quad(rect.x + inset, rect.y + inset, fill, fill,
                       dim(theme.control_on, dim_by));
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
    const RadioResult result =
        radio_behavior(gui, row.id, row.rect, value, this_option, disabled);
    radio_draw(row.rect, result, theme, label, value == this_option);
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
    // wears.
    const std::uint32_t face = control_face(result.state, theme);
    r2d::draw_rounded_quad(rect.x, rect.y, box, box, box * 0.5f, face);

    // The selection: an accent circle inset by check_inset alone - the
    // dot is all corner, no straight edge for a fill to poke past, so
    // the checkbox's corner-radius clearance has nothing to clear.
    // Drawn only while this option holds the value, and only when the
    // dot leaves room.
    const float fill = box - 2.0f * theme.check_inset;
    if (selected && fill > 0.0f) {
        r2d::draw_rounded_quad(rect.x + theme.check_inset,
                               rect.y + theme.check_inset, fill, fill,
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
    const ToggleSwitchResult result =
        toggle_switch_behavior(gui, row.id, row.rect, value, disabled);
    toggle_switch_draw(row.rect, result, theme, label, value);
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
    // so the state stays readable. The radius asks for half the height
    // and clamps to exactly that: the full pill.
    std::uint32_t face = control_face(result.state, theme);
    if (on) {
        face = dim(theme.control_on, dim_by);
    }
    r2d::draw_rounded_quad(rect.x, rect.y, track_w, box, box * 0.5f, face);

    // The knob: a near-white circle (the text color - the bright head
    // reads on both faces) filling the pill's height minus the check
    // inset, SNAPPED to its end - off left, on right. No travel
    // animation in v1: the frame clock's first consumer, the text
    // input's caret blink, spends the tick on typing instead.
    const float knob = box - 2.0f * theme.check_inset;
    if (knob > 0.0f) {
        const float x =
            on ? rect.x + track_w - theme.check_inset - knob
               : rect.x + theme.check_inset;
        r2d::draw_rounded_quad(x, rect.y + theme.check_inset, knob, knob,
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
