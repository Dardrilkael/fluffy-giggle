// ws::ui::gui widget functions (buildplan Phase 5+). One function per
// widget: lay out through LayoutCursor, hit-test through GuiContext,
// emit draws through r2d - identity, layout and drawing stay one seam.
// Every color and metric comes in through the ChromeTheme parameter;
// the functions themselves stay stateless.

#include <ws/ui/gui/widgets.hpp>

#include <ws/ui/renderer2d.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace ws::ui::gui {
namespace {

// The label placeholder until r2d has text: a centered bar whose width
// tracks the label length, so sibling buttons stay visually distinct.
// Clamped to the face (theme padding each side) and never negative, so
// a narrow button cannot hand r2d a backwards bar.
float label_bar_width(const char* label, float max_width) {
    const std::size_t chars = label != nullptr ? std::strlen(label) : 0;
    const float wanted = static_cast<float>(chars) * 6.0f;
    return std::max(0.0f, std::min(wanted, max_width));
}

}  // namespace

bool button(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
            const char* label, float width, r2d::Sprite icon) {
    width = std::max(width, 0.0f);  // a negative width would draw a mirrored quad

    // One seam: the id keys the hit-test AND any persistent state; the
    // rect is layout's answer for this frame; the draws land exactly
    // where hit-testing says the widget is.
    const WidgetId id = gui.ids().current(label);
    const Rect rect = layout.next(width, theme.control_height);
    const HitTest hit = gui.hit_test(id, rect);

    std::uint32_t face = theme.control_bg;
    if (hit.pressed) {
        face = theme.control_pressed;  // owned by the press, even if dragged away
    } else if (hit.hovered) {
        face = theme.control_hovered;
    }
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
                         r2d::rgba(255, 255, 255), icon);
        const float used = rect.height + theme.spacing;
        label_x = rect.x + used;
        label_room = std::max(0.0f, rect.width - used);
    }

    // The label bar: half the control height, centered both ways in the
    // room that remains. A zero-width bar (empty label or a degenerate
    // button) draws nothing.
    const float bar_height = rect.height * 0.5f;
    const float bar_width =
        label_bar_width(label, label_room - 2.0f * theme.padding);
    if (bar_width > 0.0f) {
        r2d::draw_quad(label_x + (label_room - bar_width) * 0.5f,
                       rect.y + (rect.height - bar_height) * 0.5f, bar_width,
                       bar_height, theme.text);
    }

    return hit.clicked;
}

bool checkbox(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
              const char* label, bool& value) {
    const WidgetId id = gui.ids().current(label);

    // The row auto-sizes: box, a gap, the label bar (FLT_MAX = the bar
    // never clamps here; an explicit width policy is not worth a knob).
    const float gap = theme.spacing;
    const float bar_width = label_bar_width(label, FLT_MAX);
    const Rect rect =
        layout.next(theme.control_height + gap + bar_width, theme.control_height);
    const HitTest hit = gui.hit_test(id, rect);

    if (hit.clicked) {
        value = !value;  // the caller's bool is the state; a click flips it
    }

    // The box: a control_height square with the same three-state face a
    // button wears, corners rounded by the theme.
    std::uint32_t face = theme.control_bg;
    if (hit.pressed) {
        face = theme.control_pressed;
    } else if (hit.hovered) {
        face = theme.control_hovered;
    }
    const float box = theme.control_height;
    r2d::draw_rounded_quad(rect.x, rect.y, box, box, theme.corner_radius, face);

    // The check: the accent fill, inset past the box's corners so a
    // heavily-rounded theme cannot let the square fill poke outside the
    // arcs - drawn only while on, and only when the box leaves room (tiny
    // control heights clamp to none).
    const float inset = std::max(5.0f, theme.corner_radius);
    const float fill = box - 2.0f * inset;
    if (value && fill > 0.0f) {
        r2d::draw_quad(rect.x + inset, rect.y + inset, fill, fill,
                       theme.control_on);
    }

    // The label bar beside the box, vertically centered.
    if (bar_width > 0.0f) {
        const float bar_height = rect.height * 0.5f;
        r2d::draw_quad(rect.x + box + gap,
                       rect.y + (rect.height - bar_height) * 0.5f, bar_width,
                       bar_height, theme.text);
    }
    return hit.clicked;
}

bool slider_float(GuiContext& gui, LayoutCursor& layout,
                  const ChromeTheme& theme, const char* label, float& value,
                  float min, float max, float width) {
    width = std::max(width, 0.0f);  // a negative width would draw a mirrored quad
    const WidgetId id = gui.ids().current(label);
    const Rect rect = layout.next(width, theme.control_height);
    const HitTest hit = gui.hit_test(id, rect);

    // Click-to-set and drag are one rule: while this slider owns the
    // press and the button is down, the value tracks the pointer. The
    // press frame maps immediately - that jump IS click-to-set; hold
    // frames keep mapping - that is the drag. A same-frame press+release
    // never sees the button down, so hit.clicked carries the click-to-set
    // for it (and re-mapping a normal release is a no-op: same pointer,
    // same value).
    bool changed = false;
    const bool tracking =
        (hit.pressed && gui.input().mouse_down[0]) || hit.clicked;
    if (tracking && rect.width > 0.0f) {
        const float span = max - min;
        const float t = span > 0.0f
                            ? (gui.input().mouse_x - rect.x) / rect.width
                            : 0.0f;
        const float mapped =
            min + std::min(std::max(t, 0.0f), 1.0f) * span;
        if (mapped != value) {
            value = mapped;
            changed = true;
        }
    }

    // The rail: a half-height strip down the middle of the row, wearing
    // the same three-state face, ends rounded by the theme.
    std::uint32_t face = theme.control_bg;
    if (hit.pressed) {
        face = theme.control_pressed;
    } else if (hit.hovered) {
        face = theme.control_hovered;
    }
    const float rail_height = rect.height * 0.5f;
    const float rail_y = rect.y + (rect.height - rail_height) * 0.5f;
    r2d::draw_rounded_quad(rect.x, rail_y, rect.width, rail_height,
                           theme.corner_radius, face);

    // The handle position reflects the VALUE (the state), not the pointer:
    // an out-of-range caller value just parks the handle at its edge.
    // span <= 0 parks it at the left rather than dividing by zero.
    const float span = max - min;
    const float t = span > 0.0f ? (value - min) / span : 0.0f;
    const float handle_width = 10.0f;
    const float travel = std::max(0.0f, rect.width - handle_width);
    const float handle_x =
        rect.x + std::min(std::max(t, 0.0f), 1.0f) * travel;

    // The fill: rail left edge to the handle, in the on-accent - rounded
    // to match the rail's left end (its right end hides under the handle,
    // which is full height and opaque).
    if (handle_x > rect.x) {
        r2d::draw_rounded_quad(rect.x, rail_y, handle_x - rect.x, rail_height,
                               theme.corner_radius, theme.control_on);
    }
    // The handle itself: full control height, over rail and fill.
    r2d::draw_quad(handle_x, rect.y, handle_width, rect.height, theme.text);

    // The label bar, right-aligned over the rail.
    const float bar_width = label_bar_width(label, rect.width - 4.0f);
    if (bar_width > 0.0f) {
        const float bar_height = rect.height * 0.5f;
        r2d::draw_quad(rect.x + rect.width - bar_width - 2.0f,
                       rect.y + (rect.height - bar_height) * 0.5f, bar_width,
                       bar_height, theme.text);
    }
    return changed;
}

bool slider_int(GuiContext& gui, LayoutCursor& layout,
                const ChromeTheme& theme, const char* label, int& value,
                int min, int max, float width) {
    float v = static_cast<float>(value);
    const bool changed = slider_float(gui, layout, theme, label, v,
                                      static_cast<float>(min),
                                      static_cast<float>(max), width);
    // Written only on a real change: a huge int that rounds through float
    // must not silently rewrite itself on idle frames.
    if (changed) {
        value = static_cast<int>(std::lround(v));
    }
    return changed;
}

Rect begin_panel(GuiContext& gui, LayoutCursor& layout,
                 const ChromeTheme& theme, const char* label,
                 const Rect& bounds) {
    // Chrome first, BEFORE the clip push, so the frame and face never clip
    // against the content rect they contain: a 1px border-colored quad
    // behind the panel face inset by 1. Degenerate panels clamp their
    // face to nothing instead of drawing backwards.
    r2d::draw_quad(bounds.x, bounds.y, bounds.width, bounds.height,
                   theme.border);
    r2d::draw_quad(bounds.x + 1.0f, bounds.y + 1.0f,
                   std::max(0.0f, bounds.width - 2.0f),
                   std::max(0.0f, bounds.height - 2.0f), theme.panel_bg);

    // Then the three pushes: the layout panel (it computes the content
    // rect), the clip at that rect (children cannot draw outside), and
    // the id scope (children id under the panel). A degenerate content
    // rect - an overflowed panel or bounds smaller than the padding -
    // pushes an empty clip: honestly nothing, rather than children
    // leaking outside.
    const Rect content = layout.begin_panel(bounds, theme.padding);
    r2d::push_clip(content.x, content.y, content.width, content.height);
    gui.ids().push(label);
    return content;
}

void end_panel(GuiContext& gui, LayoutCursor& layout) {
    // Exactly the reverse of begin_panel; each pop is a no-op against a
    // push that dropped (headless) or overflowed (the balanced clamps).
    gui.ids().pop();
    r2d::pop_clip();
    layout.end_panel();
}

}  // namespace ws::ui::gui
