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

// The label placeholder while the theme carries no font: a centered bar
// whose width tracks the label length, so sibling buttons stay visually
// distinct. Clamped to the face (theme padding each side) and never
// negative, so a narrow button cannot hand r2d a backwards bar.
float label_bar_width(const char* label, float max_width) {
    const std::size_t chars = label != nullptr ? std::strlen(label) : 0;
    const float wanted = static_cast<float>(chars) * 6.0f;
    return std::max(0.0f, std::min(wanted, max_width));
}

// A label's advance width: the theme font's honest measure, or (fontless)
// the bar placeholder's per-char estimate. UNclamped - each caller owns
// its own fit budget (the checkbox row sizes to it, buttons clamp).
float label_advance(const char* label, const ChromeTheme& theme) {
    if (theme.font != nullptr) {
        return theme.font->measure_text(label);
    }
    const std::size_t chars = label != nullptr ? std::strlen(label) : 0;
    return static_cast<float>(chars) * 6.0f;
}

// Where a label's line box starts vertically: the font's line height
// centered on the row (draw_text's y is the box top, not a baseline), or
// (fontless) half the row - the bar centering the bar branch does inline.
float label_top(float row_top, float row_height, const ChromeTheme& theme) {
    const float h = theme.font != nullptr ? theme.font->line_height()
                                          : row_height * 0.5f;
    return row_top + (row_height - h) * 0.5f;
}

// A scroll region's persistent state (Phase 3 side-table entry): how far
// the content is scrolled up (offset), and the scroll range's top as
// measured at the LAST end_scroll_panel (max_offset = content_height -
// viewport_height). Begin clamps against the stale max, end refreshes
// it - the immediate-mode one-frame lag, kept in one struct so the two
// floats share one side-table entry per panel.
struct ScrollState {
    float offset = 0.0f;
    float max_offset = 0.0f;
};

// The panel chrome begin_panel and begin_scroll_panel share: a 1px
// border-colored quad behind the panel face inset by 1. Drawn BEFORE the
// clip push, so the frame never clips against the content it contains.
// Degenerate panels clamp their face to nothing instead of drawing
// backwards.
void panel_chrome(const Rect& bounds, const ChromeTheme& theme) {
    r2d::draw_quad(bounds.x, bounds.y, bounds.width, bounds.height,
                   theme.border);
    r2d::draw_quad(bounds.x + 1.0f, bounds.y + 1.0f,
                   std::max(0.0f, bounds.width - 2.0f),
                   std::max(0.0f, bounds.height - 2.0f), theme.panel_bg);
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

    // The label: the theme font's glyphs centered in the room that
    // remains, or (fontless) the v1 bar. Text cannot truncate, so a label
    // wider than its room drops rather than overflowing the face.
    if (theme.font != nullptr) {
        const float advance = theme.font->measure_text(label);
        const float room = std::max(0.0f, label_room - 2.0f * theme.padding);
        if (advance > 0.0f && advance <= room) {
            r2d::draw_text(label_x + (label_room - advance) * 0.5f,
                           label_top(rect.y, rect.height, theme), label,
                           *theme.font, theme.text);
        }
    } else {
        // The bar: half the control height, centered both ways in the
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
    }

    return hit.clicked;
}

bool checkbox(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
              const char* label, bool& value) {
    const WidgetId id = gui.ids().current(label);

    // The row auto-sizes: box, a gap, the label (its advance - the font's
    // measure or the fontless per-char estimate; never clamped, so a
    // label always sizes its own row).
    const float gap = theme.spacing;
    const float label_width = label_advance(label, theme);
    const Rect rect =
        layout.next(theme.control_height + gap + label_width, theme.control_height);
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

    // The label beside the box, vertically centered: the theme font's
    // line box, or (fontless) the v1 bar. The row already sized itself
    // to the advance, so the label always fits - no truncation case.
    if (theme.font != nullptr) {
        if (label_width > 0.0f) {
            r2d::draw_text(rect.x + box + gap,
                           label_top(rect.y, rect.height, theme), label,
                           *theme.font, theme.text);
        }
    } else if (label_width > 0.0f) {
        const float bar_height = rect.height * 0.5f;
        r2d::draw_quad(rect.x + box + gap,
                       rect.y + (rect.height - bar_height) * 0.5f, label_width,
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
    // press and that pointer is down, the value tracks THE POINTER THAT
    // GRABBED IT (hit.pointer - its own finger, never the mouse), so two
    // sliders drag independently. The press frame maps immediately -
    // that jump IS click-to-set; hold frames keep mapping - that is the
    // drag. A same-frame press+release never sees the pointer down, so
    // hit.clicked carries the click-to-set for it (the contact spends
    // its release frame in the table, so the lookup still finds it), and
    // re-mapping a normal release is a no-op: same pointer, same value.
    bool changed = false;
    const PointerView* owner = gui.pointer(hit.pointer);
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

    // The label, right-aligned over the rail: the theme font's glyphs, or
    // (fontless) the v1 bar (clamped to the rail's budget). Text cannot
    // truncate, so a label wider than the rail drops instead of spilling
    // backwards past the slider.
    if (theme.font != nullptr) {
        const float advance = theme.font->measure_text(label);
        if (advance > 0.0f && advance <= rect.width - 4.0f) {
            r2d::draw_text(rect.x + rect.width - advance - 2.0f,
                           label_top(rect.y, rect.height, theme), label,
                           *theme.font, theme.text);
        }
    } else {
        const float bar_width = label_bar_width(label, rect.width - 4.0f);
        if (bar_width > 0.0f) {
            const float bar_height = rect.height * 0.5f;
            r2d::draw_quad(rect.x + rect.width - bar_width - 2.0f,
                           rect.y + (rect.height - bar_height) * 0.5f, bar_width,
                           bar_height, theme.text);
        }
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

bool tab_bar(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
             const char* label, const char* const* tabs, int count,
             int& active, float width) {
    width = std::max(width, 0.0f);  // like button: no mirrored quads

    bool changed = false;
    // The strip's own scope: each tab ids by index under it, so duplicate
    // tab labels in one strip stay distinct widgets (the loop-children
    // discipline, without stringifying).
    gui.ids().push(label);
    for (int i = 0; i < count; ++i) {
        gui.ids().push(i);
        const Rect rect = layout.next(width, theme.control_height);
        const HitTest hit = gui.hit_test(gui.ids().current("tab"), rect);

        // Face priority: press feedback even on the selected tab, then
        // the selection accent, then hover, then idle.
        std::uint32_t face = theme.control_bg;
        if (hit.pressed) {
            face = theme.control_pressed;
        } else if (i == active) {
            face = theme.control_on;
        } else if (hit.hovered) {
            face = theme.control_hovered;
        }
        r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                               theme.corner_radius, face);

        // The tab's label, centered like a button's: the theme font's
        // glyphs, or (fontless) the v1 bar. Same fit rule as a button -
        // an over-long label drops instead of overflowing the tab.
        const char* tab_label = tabs != nullptr ? tabs[i] : nullptr;
        if (theme.font != nullptr) {
            const float advance = theme.font->measure_text(tab_label);
            const float room = std::max(0.0f,
                                        rect.width - 2.0f * theme.padding);
            if (advance > 0.0f && advance <= room) {
                r2d::draw_text(rect.x + (rect.width - advance) * 0.5f,
                               label_top(rect.y, rect.height, theme),
                               tab_label, *theme.font, theme.text);
            }
        } else {
            const float bar_width = label_bar_width(tab_label,
                                                    rect.width -
                                                        2.0f * theme.padding);
            if (bar_width > 0.0f) {
                const float bar_height = rect.height * 0.5f;
                r2d::draw_quad(rect.x + (rect.width - bar_width) * 0.5f,
                               rect.y + (rect.height - bar_height) * 0.5f,
                               bar_width, bar_height, theme.text);
            }
        }

        // A full click moves the selection; clicking the already-selected
        // tab is a no-op that reports nothing.
        if (hit.clicked && i != active) {
            active = i;
            changed = true;
        }
        gui.ids().pop();
        if (i + 1 < count) {
            layout.same_line();  // one line: the next tab beside this one
        }
    }
    gui.ids().pop();
    return changed;
}

Rect begin_panel(GuiContext& gui, LayoutCursor& layout,
                 const ChromeTheme& theme, const char* label,
                 const Rect& bounds) {
    // Chrome first, BEFORE the clip push, so the frame and face never clip
    // against the content rect they contain (see panel_chrome).
    panel_chrome(bounds, theme);

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

Rect begin_scroll_panel(GuiContext& gui, LayoutCursor& layout,
                        const ChromeTheme& theme, const char* label,
                        const Rect& bounds) {
    // The same chrome as begin_panel, before any clip.
    panel_chrome(bounds, theme);

    // The viewport: bounds inset by the padding, clamped empty when the
    // bounds cannot hold it (a backwards rect must not reach r2d). This
    // is the UNSCROLLED window the content shows through - the clip and
    // the returned rect both use it, unshifted.
    const Rect viewport{bounds.x + theme.padding, bounds.y + theme.padding,
                        std::max(0.0f, bounds.width - 2.0f * theme.padding),
                        std::max(0.0f, bounds.height - 2.0f * theme.padding)};

    // The scroll state keys INSIDE the panel's own scope, so the matching
    // end_scroll_panel re-derives the same entry without the caller
    // threading a handle through (begin pushes, end pops - both read
    // while the scope is live).
    gui.ids().push(label);
    const WidgetId scroll_id = gui.ids().current("scroll");
    ScrollState& scroll = gui.persistent_state<ScrollState>(scroll_id);

    // The wheel moves the offset while the pointer is inside the viewport.
    // Positive dy is wheel-up - toward EARLIER content - so the offset
    // (content shifted up) decreases. Clamped against the range measured
    // at the last end_scroll_panel: this frame's content does not exist
    // yet, and a fresh panel has no range at all.
    if (contains(viewport, gui.input().mouse_x, gui.input().mouse_y) &&
        gui.input().scroll_dy != 0.0f) {
        scroll.offset -= gui.input().scroll_dy * theme.scroll_speed;
    }
    scroll.offset =
        std::min(std::max(scroll.offset, 0.0f), scroll.max_offset);

    // Children flow inside a VIRTUAL panel, shifted up by the offset: the
    // rects layout hands out are already at their scrolled positions, so
    // widgets draw AND hit-test there with no per-widget knowledge - while
    // the clip stays at the unscrolled viewport, cutting exactly what
    // pokes past the edge. Push order (scope, layout, clip) is reversed
    // by end_scroll_panel.
    (void)layout.begin_panel({bounds.x, bounds.y - scroll.offset,
                              bounds.width,
                              bounds.height + scroll.offset},
                             theme.padding);
    r2d::push_clip(viewport.x, viewport.y, viewport.width, viewport.height);
    return viewport;
}

void end_scroll_panel(GuiContext& gui, LayoutCursor& layout,
                      const ChromeTheme& theme) {
    // The same entry begin_scroll_panel keyed, re-derived inside the
    // still-pushed scope.
    const WidgetId scroll_id = gui.ids().current("scroll");
    ScrollState& scroll = gui.persistent_state<ScrollState>(scroll_id);

    // The real viewport: this frame's (virtual, shifted) content rect,
    // shifted back down by the offset it was opened with. With no panel
    // open (caller bug) panel_content() is empty and the math decays
    // harmlessly, like every underflow pop.
    Rect viewport = layout.panel_content();
    viewport.y += scroll.offset;
    viewport.width = std::max(0.0f, viewport.width);
    viewport.height = std::max(0.0f, viewport.height - scroll.offset);

    // This frame's placements size the scroll range the NEXT frame clamps
    // against - a content that shrank pulls the offset in one frame later.
    const float extent = layout.flow_extent();
    scroll.max_offset = std::max(0.0f, extent - viewport.height);
    scroll.offset =
        std::min(std::max(scroll.offset, 0.0f), scroll.max_offset);

    // The thumb, after all children (painter order puts it over the
    // content) but still inside the clip. Drawn only while there is room
    // to scroll: a fitted panel wears no dead chrome. Proportional to the
    // visible share, with a floor so a huge content cannot shrink it to
    // nothing.
    if (scroll.max_offset > 0.0f && viewport.width > 0.0f &&
        viewport.height > 0.0f) {
        const float track = viewport.height;
        const float thumb_width = 4.0f;
        float thumb_height =
            track * track / (track + scroll.max_offset);
        thumb_height = std::min(std::max(thumb_height, 12.0f), track);
        const float thumb_y =
            viewport.y +
            (scroll.offset / scroll.max_offset) * (track - thumb_height);
        r2d::draw_rounded_quad(viewport.x + viewport.width - thumb_width,
                               thumb_y, thumb_width, thumb_height,
                               theme.corner_radius, theme.border);
    }

    // The reverse of begin_scroll_panel's pushes.
    r2d::pop_clip();
    layout.end_panel();
    gui.ids().pop();
}

}  // namespace ws::ui::gui
