// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the press-and-choose rows - button, checkbox, radio, toggle_switch.
// Each widget lays out through LayoutCursor, hit-tests through
// GuiContext and draws through r2d - identity, layout and drawing
// stay one seam - and every color and metric rides the ChromeTheme;
// the shared row idioms live in widgets_detail.hpp.

#include "widgets_detail.hpp"

#include <algorithm>
#include <cstdint>

namespace ws::ui::gui {

bool button(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
            const char* label, float width, r2d::Sprite icon) {
    const Row row = begin_row(gui, layout, theme, label, width);
    const WidgetId id = row.id;
    const Rect& rect = row.rect;
    const WidgetResult& result = row.result;
    const HitTest& hit = result.hit;

    // Keyboard activation: Enter or Space fires the focused button, like
    // a click with no pointer involved. Edge-only (the frame the key went
    // down) and only while THIS button holds the focus.
    const bool key_clicked = key_activated(gui, id);

    // The focus ring: the on-accent as a 1px outline behind the face, so
    // Tab navigation is legible. Skipped for degenerate rects - a
    // zero-width face would show the ring with nothing in it.
    focus_ring(gui, id, rect, theme);

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

    return hit.clicked || key_clicked;
}

bool checkbox(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
              const char* label, bool& value) {
    // The row auto-sizes: box, a gap, the label's DISPLAY advance - the
    // font's measure or the fontless per-char estimate, "##" suffixes
    // cut (they id the row, never size it); never clamped, so a label
    // always sizes its own row. One fit serves the sizing AND the draw:
    // the row is exactly the advance wide, so the fitted text always
    // fits.
    const float gap = theme.spacing;
    char scratch[128];
    const LabelFit fit =
        fit_text(label, -1.0f, theme, scratch, sizeof(scratch));
    const Row row = begin_row(gui, layout, theme, label,
                              theme.control_height + gap + fit.advance);
    const WidgetId id = row.id;
    const Rect& rect = row.rect;
    const WidgetResult& result = row.result;
    const HitTest& hit = result.hit;

    // Keyboard toggle: Enter or Space flips the focused checkbox,
    // edge-only - a click with no pointer involved.
    const bool key_toggled = key_activated(gui, id);

    if (hit.clicked || key_toggled) {
        value = !value;  // the caller's bool is the state; a click flips it
    }

    // The focus ring: the on-accent as a 1px outline behind the row, so
    // Tab navigation is legible. Skipped for degenerate rows.
    focus_ring(gui, id, rect, theme);

    // The disabled look: the dead face for the box, the fill and the
    // label dimmed through the one helper - 1.0 is the identity for the
    // live states.
    const float dim_by = dim_factor(result.state, theme);

    // The box: a control_height square with the same faces a button
    // wears, corners rounded by the theme.
    const std::uint32_t face = control_face(result.state, theme);
    const float box = theme.control_height;
    r2d::draw_rounded_quad(rect.x, rect.y, box, box, theme.corner_radius, face);

    // The check: the accent fill, inset past the box's corners so a
    // heavily-rounded theme cannot let the square fill poke outside the
    // arcs - drawn only while on, and only when the box leaves room (tiny
    // control heights clamp to none).
    const float inset = std::max(theme.check_inset, theme.corner_radius);
    const float fill = box - 2.0f * inset;
    if (value && fill > 0.0f) {
        r2d::draw_quad(rect.x + inset, rect.y + inset, fill, fill,
                       dim(theme.control_on, dim_by));
    }

    // The label beside the box, vertically centered: the theme font's
    // line box, or (fontless) the v1 bar at the fitted advance. The row
    // already sized itself to that advance, so no truncation case can
    // fire here - the -1 fit above WAS the draw's answer.
    draw_label(rect.x + box + gap, rect.y, rect.height, fit,
               dim(theme.text, dim_by), theme);
    return hit.clicked || key_toggled;
}

bool radio(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
           const char* label, int& value, int this_option) {
    // The row auto-sizes like a checkbox's: dot, a gap, the label's
    // DISPLAY advance ("##" suffixes id the row, never size it).
    const float gap = theme.spacing;
    char scratch[128];
    const LabelFit fit =
        fit_text(label, -1.0f, theme, scratch, sizeof(scratch));
    const Row row = begin_row(gui, layout, theme, label,
                              theme.control_height + gap + fit.advance);
    const WidgetId id = row.id;
    const Rect& rect = row.rect;
    const WidgetResult& result = row.result;
    const HitTest& hit = result.hit;

    // Keyboard select: Enter or Space picks the focused radio,
    // edge-only - a click with no pointer involved.
    const bool key_selected = key_activated(gui, id);

    // Selecting the selected option is an idempotent write that reports
    // false: true means the selection MOVED to this option this frame.
    const bool was_other = value != this_option;
    if (hit.clicked || key_selected) {
        value = this_option;
    }

    // The focus ring: the on-accent as a 1px outline behind the row, so
    // Tab navigation is legible. Skipped for degenerate rows.
    focus_ring(gui, id, rect, theme);

    // The disabled look: the dead face for the dot, the fill and the
    // label dimmed through the one helper.
    const float dim_by = dim_factor(result.state, theme);

    // The dot: a control_height circle - the radius asks for half the
    // box, exactly what the draw clamps to - with the faces a button
    // wears.
    const std::uint32_t face = control_face(result.state, theme);
    const float box = theme.control_height;
    r2d::draw_rounded_quad(rect.x, rect.y, box, box, box * 0.5f, face);

    // The selection: an accent circle inset by check_inset alone - the
    // dot is all corner, no straight edge for a fill to poke past, so
    // the checkbox's corner-radius clearance has nothing to clear.
    // Drawn only while this option holds the value, and only when the
    // dot leaves room.
    const float fill = box - 2.0f * theme.check_inset;
    if (value == this_option && fill > 0.0f) {
        r2d::draw_rounded_quad(rect.x + theme.check_inset,
                               rect.y + theme.check_inset, fill, fill,
                               fill * 0.5f, dim(theme.control_on, dim_by));
    }

    // The label beside the dot, vertically centered - the same draw a
    // checkbox's label gets.
    draw_label(rect.x + box + gap, rect.y, rect.height, fit,
               dim(theme.text, dim_by), theme);
    return (hit.clicked || key_selected) && was_other;
}

bool toggle_switch(GuiContext& gui, LayoutCursor& layout,
                   const ChromeTheme& theme, const char* label, bool& value) {
    // The row auto-sizes like a checkbox's: the pill (twice the height
    // wide, the height tall), a gap, the label's DISPLAY advance.
    const float gap = theme.spacing;
    const float box = theme.control_height;
    const float track_w = 2.0f * box;
    char scratch[128];
    const LabelFit fit =
        fit_text(label, -1.0f, theme, scratch, sizeof(scratch));
    const Row row = begin_row(gui, layout, theme, label,
                              track_w + gap + fit.advance);
    const WidgetId id = row.id;
    const Rect& rect = row.rect;
    const WidgetResult& result = row.result;
    const HitTest& hit = result.hit;

    // Keyboard toggle: Enter or Space flips the focused switch,
    // edge-only - a click with no pointer involved.
    const bool key_toggled = key_activated(gui, id);

    if (hit.clicked || key_toggled) {
        value = !value;  // the caller's bool is the state; a click flips it
    }

    // The focus ring: the on-accent as a 1px outline behind the row.
    // Skipped for degenerate rows.
    focus_ring(gui, id, rect, theme);

    // The disabled look: the knob and the label dimmed through the one
    // helper; the track's own dead face is the off state's, while the
    // on state dims its accent (below) like the checkbox's fill.
    const float dim_by = dim_factor(result.state, theme);

    // The pill: the control faces while off; the accent IS the track
    // while on - dimmed under the disabled scope rather than deadened,
    // so the state stays readable. The radius asks for half the height
    // and clamps to exactly that: the full pill.
    std::uint32_t face = control_face(result.state, theme);
    if (value) {
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
            value ? rect.x + track_w - theme.check_inset - knob
                  : rect.x + theme.check_inset;
        r2d::draw_rounded_quad(x, rect.y + theme.check_inset, knob, knob,
                               knob * 0.5f, dim(theme.text, dim_by));
    }

    // The label beside the pill - the same draw a checkbox's label gets.
    draw_label(rect.x + track_w + gap, rect.y, rect.height, fit,
               dim(theme.text, dim_by), theme);
    return hit.clicked || key_toggled;
}

}  // namespace ws::ui::gui
