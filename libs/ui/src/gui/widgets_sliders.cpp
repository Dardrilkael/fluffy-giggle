// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the analog rows - slider_float and slider_int over one shared
// behavior/draw pair (the keyboard step is the one difference, and it
// rides the behavior's parameters), and drag_float, whose grab state
// rides the DragState side table. Each widget composes its split halves
// in order: the ladder places the row, the behavior owns the
// interaction, the draw owns the pixels. The shared row idioms live in
// widgets_detail.hpp.

#include "widgets_detail.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace ws::ui::gui {

bool slider_float(GuiContext& gui, LayoutCursor& layout,
                  const ChromeTheme& theme, const char* label, float& value,
                  float min, float max, float width, bool disabled) {
    // The float keyboard step: 1% of the range per arrow press.
    const RowLayout row = begin_widget_row(gui, layout, theme, label, width);
    const SliderResult result =
        slider_behavior(gui, row.id, row.rect, value, min, max,
                        (max - min) / 100.0f, disabled);
    slider_draw(row.rect, result, theme, label, value, min, max);
    return result.changed;
}

bool slider_int(GuiContext& gui, LayoutCursor& layout,
                const ChromeTheme& theme, const char* label, int& value,
                int min, int max, float width, bool disabled) {
    float v = static_cast<float>(value);
    // The int keyboard step: 1 per arrow press.
    const RowLayout row = begin_widget_row(gui, layout, theme, label, width);
    const SliderResult result =
        slider_behavior(gui, row.id, row.rect, v, static_cast<float>(min),
                        static_cast<float>(max), 1.0f, disabled);
    slider_draw(row.rect, result, theme, label, v, static_cast<float>(min),
                static_cast<float>(max));
    // Written only on a real change: a huge int that rounds through float
    // must not silently rewrite itself on idle frames.
    if (result.changed) {
        value = static_cast<int>(std::lround(v));
    }
    return result.changed;
}

SliderResult slider_behavior(GuiContext& gui, WidgetId id, const Rect& rect,
                             float& value, float min, float max,
                             float key_step, bool disabled,
                             const HitShape& hit_shape) {
    SliderResult result{
        begin_widget_paint(gui, id, rect, disabled, hit_shape)};
    const HitTest& hit = result.hit;

    // Click-to-set and drag are one rule: while this slider owns the
    // press and that pointer is down, the value tracks THE POINTER THAT
    // GRABBED IT (hit.pointer - its own finger, never the mouse), so two
    // sliders drag independently. The press frame maps immediately -
    // that jump IS click-to-set; hold frames keep mapping - that is the
    // drag. A same-frame press+release never sees the pointer down, so
    // hit.clicked carries the click-to-set for it (the contact spends
    // its release frame in the table, so the lookup still finds it), and
    // re-mapping a normal release is a no-op: same pointer, same value.
    const ws::input::PointerView* owner = gui.pointer(hit.pointer);
    const bool tracking =
        (hit.pressed && owner != nullptr && owner->down) || hit.clicked;
    if (tracking && owner != nullptr && rect.width > 0.0f) {
        const float span = max - min;
        const float t =
            span > 0.0f ? (owner->x - rect.x) / rect.width : 0.0f;
        const float mapped =
            min + std::min(std::max(t, 0.0f), 1.0f) * span;
        if (mapped != value) {
            value = mapped;
            result.changed = true;
        }
    }

    // Keyboard: the arrows step the focused slider - one key_step per
    // press, clamped to the range; only a real move reports changed.
    // The focus read rides the fold begin_widget_paint captured after
    // state(): nothing between there and here moves it.
    if (result.focused &&
        (gui.input().key_pressed[ws::input::keys::kLeft] ||
         gui.input().key_pressed[ws::input::keys::kRight])) {
        const float step =
            gui.input().key_pressed[ws::input::keys::kRight] ? key_step
                                                              : -key_step;
        const float next = std::min(std::max(value + step, min), max);
        if (next != value) {
            value = next;
            result.changed = true;
        }
    }
    return result;
}

void slider_draw(const Rect& rect, const SliderResult& result,
                 const ChromeTheme& theme, const char* label, float value,
                 float min, float max) {
    // The rail: a half-height strip down the middle of the row, wearing
    // the same faces a button wears, ends rounded by the theme.
    const std::uint32_t face = control_face(result.state, theme);
    const float rail_height = rect.height * 0.5f;
    const float rail_y = rect.y + (rect.height - rail_height) * 0.5f;

    // The focus ring: the on-accent as a 1px outline behind the RAIL -
    // the body the slider paints - not the row, whose area above and
    // below the rail is exposed: the ring draw is solid, masked to an
    // outline by the body, and a row-sized ring would read as the whole
    // slider going green the moment a click lands focus on it.
    ring_draw(result.focused, {rect.x, rail_y, rect.width, rail_height},
              theme);

    // The disabled look: the dead face for the rail, the fill, the
    // handle and the label dimmed through the one helper - 1.0 is the
    // identity for the live states.
    const float dim_by = dim_factor(result.state, theme);

    r2d::draw_rounded_quad(rect.x, rail_y, rect.width, rail_height,
                           theme.corner_radius, face);

    // The handle position reflects the VALUE (the state), not the pointer:
    // an out-of-range caller value just parks the handle at its edge.
    // span <= 0 parks it at the left rather than dividing by zero.
    const float span = max - min;
    const float t = span > 0.0f ? (value - min) / span : 0.0f;
    const float handle_width = theme.slider_handle_width;
    const float travel = std::max(0.0f, rect.width - handle_width);
    const float handle_x =
        rect.x + std::min(std::max(t, 0.0f), 1.0f) * travel;

    // The fill: rail left edge to the handle, in the on-accent - rounded
    // to match the rail's left end. Its exposed sides inset a hard 1 px
    // (a literal like the focus ring's 1): the SDF shades a half-pixel
    // antialiasing ramp past the ideal edge, and a fill flush with the
    // rail paints that ramp on the rail's own outline - the accent
    // reading as escaped from the track. The right end keeps the flush
    // edge; it hides under the handle, which is full height and opaque.
    const float fill_w = handle_x - rect.x - 1.0f;
    if (fill_w > 0.0f) {
        r2d::draw_rounded_quad(rect.x + 1.0f, rail_y + 1.0f, fill_w,
                               rail_height - 2.0f, theme.corner_radius,
                               dim(theme.control_on, dim_by));
    }
    // The handle itself: full control height, over rail and fill.
    r2d::draw_quad(handle_x, rect.y, handle_width, rect.height,
                   dim(theme.text, dim_by));

    // The label, right-aligned over the rail: fitted to the width minus
    // two insets (one each side of the label). A "##" suffix never
    // draws; a too-wide label truncates to "..." instead of spilling
    // backwards past the slider.
    char scratch[128];
    const LabelFit fit =
        fit_text(label,
                 std::max(0.0f, rect.width - 2.0f * theme.label_inset), theme,
                 scratch, sizeof(scratch));
    draw_label(rect.x + rect.width - fit.advance - theme.label_inset, rect.y,
               rect.height, fit, dim(theme.text, dim_by), theme);
}

bool drag_float(GuiContext& gui, LayoutCursor& layout,
               const ChromeTheme& theme, const char* label, float& value,
               float min, float max, float speed, float width,
               bool disabled) {
    const RowLayout row = begin_widget_row(gui, layout, theme, label, width);
    const DragFloatResult result =
        drag_float_behavior(gui, row.id, row.rect, value, min, max, speed,
                            disabled);
    drag_float_draw(row.rect, result, theme, label, value);
    return result.changed;
}

DragFloatResult drag_float_behavior(GuiContext& gui, WidgetId id,
                                    const Rect& rect, float& value,
                                    float min, float max, float speed,
                                    bool disabled, const HitShape& hit_shape) {
    DragFloatResult result{
        begin_widget_paint(gui, id, rect, disabled, hit_shape)};
    const HitTest& hit = result.hit;

    // The anchor: persistent state under the widget id, so the grab
    // survives frames (and clear_persistent_state evicts it - the
    // same side-table discipline as the scroll state).
    DragState& drag = gui.persistent_state<DragState>(id);

    // Grab-and-drag, not click-to-set: a press inside GRABS (the value
    // does not jump - unlike a slider there is nothing to click-to-set),
    // and while that pointer holds the press the value maps its travel
    // from the ANCHOR, so no per-frame delta bookkeeping exists and a
    // frame skipped mid-drag resumes exactly where the finger is. The
    // sweep outlives the rect (hit.pressed stays true through the hold,
    // press started inside), and a same-frame press+release never sees
    // the pointer down - a plain click grabs and lifts, moving nothing.
    const ws::input::PointerView* owner = gui.pointer(hit.pointer);
    if (hit.pressed && owner != nullptr && owner->down) {
        if (drag.pointer == ws::input::kNoPointer) {
            drag.pointer = hit.pointer;
            drag.anchor_x = owner->x;
            drag.anchor_value = value;
        }
        if (drag.pointer == hit.pointer) {
            const float mapped =
                std::min(std::max(drag.anchor_value +
                                      (owner->x - drag.anchor_x) * speed,
                                  min),
                         max);
            // The NaN guard: a poisoned anchor (or caller value) maps to
            // NaN, which never equals itself - the drag refuses rather
            // than writing garbage.
            if (mapped == mapped && mapped != value) {
                value = mapped;
                result.changed = true;
            }
        }
    } else {
        // Lift, cancel, or a pointer that never really pressed us: the
        // grab ends, so the next press anchors fresh.
        drag.pointer = ws::input::kNoPointer;
    }

    // Keyboard: the arrows step the focused row - 1% of the range per
    // press, like a slider; only a real move reports changed, and the
    // same NaN guard holds. The focus read rides the fold
    // begin_widget_paint captured after state().
    if (result.focused &&
        (gui.input().key_pressed[ws::input::keys::kLeft] ||
         gui.input().key_pressed[ws::input::keys::kRight])) {
        const float step = (max - min) / 100.0f;
        const float delta =
            gui.input().key_pressed[ws::input::keys::kRight] ? step : -step;
        const float next = std::min(std::max(value + delta, min), max);
        if (next == next && next != value) {
            value = next;
            result.changed = true;
        }
    }
    return result;
}

void drag_float_draw(const Rect& rect, const DragFloatResult& result,
                     const ChromeTheme& theme, const char* label,
                     float value) {
    // The focus ring: the on-accent as a 1px outline behind the row, so
    // Tab navigation is legible. Skipped for degenerate rows.
    ring_draw(result.focused, rect, theme);

    // The disabled look: the dead face for the row, the value label
    // dimmed through the one helper.
    const float dim_by = dim_factor(result.state, theme);

    // The row: the faces a button wears, rounded by the theme - a drag
    // value reads as a control, not a text line.
    const std::uint32_t face = control_face(result.state, theme);
    r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                           theme.corner_radius, face);

    // The combined "label: value" line (three decimals, the v1 spelling):
    // the display cut first - a "##" suffix ids without drawing - then one
    // snprintf into a local buffer; a label-less row shows the value
    // alone. Fit_text truncates to the row's room (one padding each
    // side), and the fit draws left-aligned like every widget label: the
    // theme font, or the fontless bar at the fitted advance.
    char value_buf[64];
    const std::string_view display = label_display(label);
    if (display.empty()) {
        std::snprintf(value_buf, sizeof(value_buf), "%.3f", value);
    } else {
        std::snprintf(value_buf, sizeof(value_buf), "%.*s: %.3f",
                      static_cast<int>(display.size()), display.data(),
                      value);
    }
    char scratch[128];
    const LabelFit fit =
        fit_text(value_buf, std::max(0.0f, rect.width - 2.0f * theme.padding),
                 theme, scratch, sizeof(scratch));
    draw_label(rect.x + theme.padding, rect.y, rect.height, fit,
               dim(theme.text, dim_by), theme);
}

}  // namespace ws::ui::gui
