// ws::ui::gui widget functions (buildplan Phase 5+). One function per
// widget: lay out through LayoutCursor, hit-test through GuiContext,
// emit draws through r2d - identity, layout and drawing stay one seam.
// Every color and metric comes in through the ChromeTheme parameter;
// the functions themselves stay stateless.

#include <ws/ui/gui/widgets.hpp>

#include <ws/ui/renderer2d.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ws::ui::gui {
namespace {

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
// it - the immediate-mode one-frame lag, kept in one struct so the
// floats share one side-table entry per panel. `thumb` is the thumb
// grab in flight (the pointer holding it and where in the thumb the
// grab landed) - the panel's own drag, beside the flow drag's
// ScrollDragState, keyed in the same entry.
struct ScrollState {
    float offset = 0.0f;
    float max_offset = 0.0f;
    PointerId thumb = kNoPointer;  // the grabbing pointer, while held
    float thumb_grab = 0.0f;  // the grab's offset within the thumb
};

// A scroll region's per-contact drag bookkeeping: a track exists from the
// contact's press inside the viewport until it lifts. While it exists and
// !dragging the press is still the CHILD's (a tap in progress, watched
// against the threshold); dragging means the scroll owns the finger - the
// child's press was cancelled and the offset follows the contact 1:1.
struct ScrollDragTrack {
    PointerId id = kNoPointer;
    float press_y = 0.0f;  // the threshold is measured from the press
    float last_y = 0.0f;  // the previous frame's y, for the drag delta
    bool dragging = false;
};

// One per scroll panel (the persistent side-table discipline): a track per
// live contact, so two fingers can drag one region and one lifting never
// confuses the survivor's bookkeeping.
struct ScrollDragState {
    std::array<ScrollDragTrack, InputFrame::kMaxTouches> tracks{};
};

// The contact's track: its own slot, or a free one for a fresh press.
// Null only when the table is somehow full, which the sweep in
// begin_scroll_panel makes unreachable (free slots always cover the
// untracked contacts: live contacts are capped at the track count).
ScrollDragTrack* drag_track_for(ScrollDragState& drag, PointerId id) {
    for (ScrollDragTrack& track : drag.tracks) {
        if (track.id == id) {
            return &track;
        }
    }
    for (ScrollDragTrack& track : drag.tracks) {
        if (track.id == kNoPointer) {
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
void panel_chrome(const Rect& bounds, const ChromeTheme& theme) {
    r2d::draw_quad(bounds.x, bounds.y, bounds.width, bounds.height,
                   theme.border);
    r2d::draw_quad(bounds.x + 1.0f, bounds.y + 1.0f,
                   std::max(0.0f, bounds.width - 2.0f),
                   std::max(0.0f, bounds.height - 2.0f), theme.panel_bg);
}

}  // namespace

// fit_text's body. The display cut first (label_display - "##" id
// suffixes never draw), then the width pass: the whole display when it
// fits, and only a miss truncates - longest fitting prefix + ASCII
// "..." - the cut backed up over UTF-8 continuation bytes so no
// codepoint splits (the fontless theme never draws the text, but the
// answer is a C string any caller could). Both measures ride one
// lambda: the theme font's honest glyph advance, or the per-byte
// estimate the v1 bars size from. The copy lands in the CALLER'S
// scratch - the only buffer fit_text ever writes - and only when
// something changed, so the mid-frame no-allocation rule holds on
// every path.
LabelFit fit_text(const char* label, float max_width,
                  const ChromeTheme& theme, char* scratch,
                  std::size_t scratch_bytes) {
    const std::string_view display = label_display(label);
    if (display.empty()) {
        return {"", 0.0f};  // nothing to say: nothing to draw
    }

    // A marker cut the display when the label continues past it: the
    // view over the label is not null-terminated at the cut, so the
    // display moves into scratch as draw-ready bytes. No scratch, no
    // cut - and nothing drawable.
    const char* text = label;
    if (label[display.size()] != '\0') {
        if (scratch == nullptr || scratch_bytes < display.size() + 1) {
            return {"", 0.0f};
        }
        std::memcpy(scratch, display.data(), display.size());
        scratch[display.size()] = '\0';
        text = scratch;
    }

    // The advance of the display as it stands: the font's measure, or
    // (fontless) the estimate - per byte, the v1 bar's arithmetic.
    const auto measure = [&](const char* s, std::size_t bytes) {
        if (theme.font != nullptr) {
            return theme.font->measure_text(s);
        }
        return static_cast<float>(bytes) * theme.label_char_width;
    };
    const float whole = measure(text, display.size());
    if (max_width < 0.0f || whole <= max_width) {
        return {text, whole};  // fits (or nobody asked for a width)
    }

    // Truncation: prefixes longest-first, each composed in scratch as
    // prefix + "..." and measured whole. The whole display already
    // missed, so the longest candidate cuts at least one byte; k = 0
    // is the bare "..." - the last resort, and past it nothing draws.
    if (scratch == nullptr || scratch_bytes < 4) {
        return {"", 0.0f};  // not even the bare "..." could be built
    }
    const std::size_t cap = scratch_bytes - 1;  // room for the terminator
    std::size_t k_max = display.size() - 1;  // cut at least one byte
    if (k_max > cap - 3) {
        k_max = cap - 3;  // the longest prefix that + "..." still fits
    }
    for (std::size_t k = k_max + 1; k-- > 0;) {
        // UTF-8: back up while the byte at the cut continues the
        // codepoint before it - the prefix keeps whole codepoints
        // (corrupt bytes stay corrupt; utf8_next forgives them).
        while (k > 0 && k < display.size() &&
               (static_cast<unsigned char>(display[k]) & 0xC0u) == 0x80u) {
            --k;
        }
        std::memcpy(scratch, display.data(), k);
        scratch[k] = '.';
        scratch[k + 1] = '.';
        scratch[k + 2] = '.';
        scratch[k + 3] = '\0';
        const float advance = measure(scratch, k + 3);
        if (advance > 0.0f && advance <= max_width) {
            return {scratch, advance};
        }
        if (k == 0) {
            break;  // the bare "..." was the last candidate
        }
    }
    return {"", 0.0f};  // not even "..." fits: nothing drawable
}

bool button(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
            const char* label, float width, r2d::Sprite icon) {
    // Fill-width: a negative width means "the rest of the line" - the one
    // size the caller cannot know before placing, resolved against the
    // cursor before the clamp.
    if (width < 0.0f) {
        width = layout.remaining_width();
    }
    width = std::max(width, 0.0f);  // a negative width would draw a mirrored quad

    // One seam: the id keys the hit-test AND any persistent state; the
    // rect is layout's answer for this frame; the draws land exactly
    // where hit-testing says the widget is.
    const WidgetId id = gui.ids().id(label);
    const Rect rect = layout.next(width, theme.control_height);
    // Buttons are focusable: the registration precedes state() (a press's
    // focus claim reads it), state() runs the one hit-test, and the
    // keyboard gets its say below.
    gui.register_focusable(id);
    const WidgetResult result = gui.state(id, rect);
    const HitTest& hit = result.hit;

    // Keyboard activation: Enter or Space fires the focused button, like
    // a click with no pointer involved. Edge-only (the frame the key went
    // down) and only while THIS button holds the focus.
    const bool key_clicked =
        gui.is_focused(id) &&
        (gui.input().key_pressed[keys::kEnter] ||
         gui.input().key_pressed[keys::kSpace]);

    // The focus ring: the on-accent as a 1px outline behind the face, so
    // Tab navigation is legible. Skipped for degenerate rects - a
    // zero-width face would show the ring with nothing in it.
    if (gui.is_focused(id) && rect.width > 0.0f && rect.height > 0.0f) {
        r2d::draw_rounded_quad(rect.x - 1.0f, rect.y - 1.0f,
                               rect.width + 2.0f, rect.height + 2.0f,
                               theme.corner_radius + 1.0f, theme.control_on);
    }

    // The disabled look: the dead face for the control, everything else
    // (the icon tint, the label) dimmed through the theme's one helper -
    // 1.0 is the identity for the live states.
    const float dim_by =
        result.state == WidgetState::Disabled ? theme.disabled_alpha : 1.0f;

    std::uint32_t face = theme.control_bg;
    if (result.state == WidgetState::Disabled) {
        face = theme.control_disabled;  // the dead face covers idle/hover/press
    } else if (result.state == WidgetState::Pressed) {
        face = theme.control_pressed;  // owned by the press, even if dragged away
    } else if (result.state == WidgetState::Hovered) {
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
                         dim(r2d::rgba(255, 255, 255), dim_by), icon);
        const float used = rect.height + theme.spacing;
        label_x = rect.x + used;
        label_room = std::max(0.0f, rect.width - used);
    }

    // The label: fitted to the room the icon leaves (minus the face's
    // padding each side), centered. One fit serves both themes - the
    // font draws the returned text, the fontless bar draws the returned
    // advance as its width. A "##" suffix ids the button but never
    // draws; a too-wide label truncates to "..." inside the face
    // instead of dropping (the shared advance guard covers the blank
    // measure - nothing drawable draws nothing).
    char scratch[128];
    const LabelFit fit =
        fit_text(label, std::max(0.0f, label_room - 2.0f * theme.padding),
                 theme, scratch, sizeof(scratch));
    if (fit.advance > 0.0f) {
        const float x = label_x + (label_room - fit.advance) * 0.5f;
        if (theme.font != nullptr) {
            r2d::draw_text(x, label_top(rect.y, rect.height, theme), fit.text,
                           *theme.font, dim(theme.text, dim_by));
        } else {
            const float bar_height = rect.height * 0.5f;
            r2d::draw_quad(x, rect.y + (rect.height - bar_height) * 0.5f,
                           fit.advance, bar_height, dim(theme.text, dim_by));
        }
    }

    return hit.clicked || key_clicked;
}

bool checkbox(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
              const char* label, bool& value) {
    const WidgetId id = gui.ids().id(label);

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
    const float label_width = fit.advance;
    const Rect rect =
        layout.next(theme.control_height + gap + label_width, theme.control_height);
    // Full citizenship, like a button: focusable, state() for the one
    // hit-test plus the face, and the keyboard gets its say below.
    gui.register_focusable(id);
    const WidgetResult result = gui.state(id, rect);
    const HitTest& hit = result.hit;

    // Keyboard toggle: Enter or Space flips the focused checkbox,
    // edge-only - a click with no pointer involved.
    const bool key_toggled =
        gui.is_focused(id) &&
        (gui.input().key_pressed[keys::kEnter] ||
         gui.input().key_pressed[keys::kSpace]);

    if (hit.clicked || key_toggled) {
        value = !value;  // the caller's bool is the state; a click flips it
    }

    // The focus ring: the on-accent as a 1px outline behind the row, so
    // Tab navigation is legible. Skipped for degenerate rows.
    if (gui.is_focused(id) && rect.width > 0.0f && rect.height > 0.0f) {
        r2d::draw_rounded_quad(rect.x - 1.0f, rect.y - 1.0f,
                               rect.width + 2.0f, rect.height + 2.0f,
                               theme.corner_radius + 1.0f, theme.control_on);
    }

    // The disabled look: the dead face for the box, the fill and the
    // label dimmed through the one helper - 1.0 is the identity for the
    // live states.
    const float dim_by =
        result.state == WidgetState::Disabled ? theme.disabled_alpha : 1.0f;

    // The box: a control_height square with the same faces a button
    // wears, corners rounded by the theme.
    std::uint32_t face = theme.control_bg;
    if (result.state == WidgetState::Disabled) {
        face = theme.control_disabled;
    } else if (result.state == WidgetState::Pressed) {
        face = theme.control_pressed;
    } else if (result.state == WidgetState::Hovered) {
        face = theme.control_hovered;
    }
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
    if (fit.advance > 0.0f) {
        if (theme.font != nullptr) {
            r2d::draw_text(rect.x + box + gap,
                           label_top(rect.y, rect.height, theme), fit.text,
                           *theme.font, dim(theme.text, dim_by));
        } else {
            const float bar_height = rect.height * 0.5f;
            r2d::draw_quad(rect.x + box + gap,
                           rect.y + (rect.height - bar_height) * 0.5f,
                           fit.advance, bar_height, dim(theme.text, dim_by));
        }
    }
    return hit.clicked || key_toggled;
}

namespace {

// The body both sliders share: they differ only in the keyboard step
// (1% of the range for a float, 1 for an int), which rides through here
// so the public pair keeps its signatures. Citizenship lives here once:
// focusable, state() for the one hit-test plus the face, the arrows step
// the focused slider, and the disabled look dims the fill, the handle
// and the label as one.
bool slider_impl(GuiContext& gui, LayoutCursor& layout,
                 const ChromeTheme& theme, const char* label, float& value,
                 float min, float max, float width, float key_step) {
    // Fill-width, like button: a negative width fills the rest of the
    // line before the clamp keeps the draw sane.
    if (width < 0.0f) {
        width = layout.remaining_width();
    }
    width = std::max(width, 0.0f);  // a negative width would draw a mirrored quad
    const WidgetId id = gui.ids().id(label);
    const Rect rect = layout.next(width, theme.control_height);
    gui.register_focusable(id);
    const WidgetResult result = gui.state(id, rect);
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

    // Keyboard: the arrows step the focused slider - one key_step per
    // press, clamped to the range; only a real move reports changed.
    if (gui.is_focused(id) && (gui.input().key_pressed[keys::kLeft] ||
                               gui.input().key_pressed[keys::kRight])) {
        const float step =
            gui.input().key_pressed[keys::kRight] ? key_step : -key_step;
        const float next = std::min(std::max(value + step, min), max);
        if (next != value) {
            value = next;
            changed = true;
        }
    }

    // The focus ring: the on-accent as a 1px outline behind the row, so
    // Tab navigation is legible. Skipped for degenerate rows.
    if (gui.is_focused(id) && rect.width > 0.0f && rect.height > 0.0f) {
        r2d::draw_rounded_quad(rect.x - 1.0f, rect.y - 1.0f,
                               rect.width + 2.0f, rect.height + 2.0f,
                               theme.corner_radius + 1.0f, theme.control_on);
    }

    // The disabled look: the dead face for the rail, the fill, the
    // handle and the label dimmed through the one helper - 1.0 is the
    // identity for the live states.
    const float dim_by =
        result.state == WidgetState::Disabled ? theme.disabled_alpha : 1.0f;

    // The rail: a half-height strip down the middle of the row, wearing
    // the same faces a button wears, ends rounded by the theme.
    std::uint32_t face = theme.control_bg;
    if (result.state == WidgetState::Disabled) {
        face = theme.control_disabled;
    } else if (result.state == WidgetState::Pressed) {
        face = theme.control_pressed;
    } else if (result.state == WidgetState::Hovered) {
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
    const float handle_width = theme.slider_handle_width;
    const float travel = std::max(0.0f, rect.width - handle_width);
    const float handle_x =
        rect.x + std::min(std::max(t, 0.0f), 1.0f) * travel;

    // The fill: rail left edge to the handle, in the on-accent - rounded
    // to match the rail's left end (its right end hides under the handle,
    // which is full height and opaque).
    if (handle_x > rect.x) {
        r2d::draw_rounded_quad(rect.x, rail_y, handle_x - rect.x, rail_height,
                               theme.corner_radius,
                               dim(theme.control_on, dim_by));
    }
    // The handle itself: full control height, over rail and fill.
    r2d::draw_quad(handle_x, rect.y, handle_width, rect.height,
                   dim(theme.text, dim_by));

    // The label, right-aligned over the rail: fitted to the width minus
    // two insets (one each side of the label), one fit for both themes -
    // the font draws the returned text, the bar draws the advance. A
    // "##" suffix never draws; a too-wide label truncates to "..."
    // instead of spilling backwards past the slider.
    char scratch[128];
    const LabelFit fit =
        fit_text(label,
                 std::max(0.0f, rect.width - 2.0f * theme.label_inset), theme,
                 scratch, sizeof(scratch));
    if (fit.advance > 0.0f) {
        const float x = rect.x + rect.width - fit.advance - theme.label_inset;
        if (theme.font != nullptr) {
            r2d::draw_text(x, label_top(rect.y, rect.height, theme), fit.text,
                           *theme.font, dim(theme.text, dim_by));
        } else {
            const float bar_height = rect.height * 0.5f;
            r2d::draw_quad(x, rect.y + (rect.height - bar_height) * 0.5f,
                           fit.advance, bar_height, dim(theme.text, dim_by));
        }
    }
    return changed;
}

}  // namespace

bool slider_float(GuiContext& gui, LayoutCursor& layout,
                  const ChromeTheme& theme, const char* label, float& value,
                  float min, float max, float width) {
    // The float keyboard step: 1% of the range per arrow press.
    return slider_impl(gui, layout, theme, label, value, min, max, width,
                       (max - min) / 100.0f);
}

bool slider_int(GuiContext& gui, LayoutCursor& layout,
                const ChromeTheme& theme, const char* label, int& value,
                int min, int max, float width) {
    float v = static_cast<float>(value);
    // The int keyboard step: 1 per arrow press.
    const bool changed = slider_impl(gui, layout, theme, label, v,
                                     static_cast<float>(min),
                                     static_cast<float>(max), width, 1.0f);
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
        // Full citizenship, like a button, per tab: each tab registers
        // in strip order (Tab walks the strip left to right), state()
        // runs the one hit-test and derives the face, and Enter or
        // Space selects the focused tab.
        const WidgetId tab_id = gui.ids().id("tab");
        gui.register_focusable(tab_id);
        const WidgetResult result = gui.state(tab_id, rect);
        const HitTest& hit = result.hit;
        const bool key_selected =
            gui.is_focused(tab_id) &&
            (gui.input().key_pressed[keys::kEnter] ||
             gui.input().key_pressed[keys::kSpace]);

        // The focus ring: the on-accent as a 1px outline behind the tab,
        // so Tab navigation is legible. Skipped for degenerate rects.
        if (gui.is_focused(tab_id) && rect.width > 0.0f &&
            rect.height > 0.0f) {
            r2d::draw_rounded_quad(rect.x - 1.0f, rect.y - 1.0f,
                                   rect.width + 2.0f, rect.height + 2.0f,
                                   theme.corner_radius + 1.0f,
                                   theme.control_on);
        }

        // The disabled look: the dead face for an idle tab, the selected
        // tab's accent and the label dimmed through the one helper - the
        // selection stays legible in a dead strip, just faded. 1.0 is
        // the identity for the live states.
        const float dim_by =
            result.state == WidgetState::Disabled ? theme.disabled_alpha
                                                   : 1.0f;

        // Face priority: press feedback even on the selected tab, then
        // the selection accent, then hover, then the dead face, then
        // idle. A dead tab claims nothing, so its pressed/hover branches
        // never fire - the Disabled slot is for the idle one.
        std::uint32_t face = theme.control_bg;
        if (result.state == WidgetState::Pressed) {
            face = theme.control_pressed;
        } else if (i == active) {
            face = dim(theme.control_on, dim_by);
        } else if (result.state == WidgetState::Hovered) {
            face = theme.control_hovered;
        } else if (result.state == WidgetState::Disabled) {
            face = theme.control_disabled;
        }
        r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                               theme.corner_radius, face);

        // The tab's label, centered like a button's: fitted to the tab
        // minus its padding, one fit for both themes - the font draws
        // the returned text, the bar draws the advance. Tab labels
        // never id (the strip ids by INDEX), so a "##" in a tab label
        // is pure display sugar: it cuts and keys nothing.
        const char* tab_label = tabs != nullptr ? tabs[i] : nullptr;
        char scratch[128];
        const LabelFit fit =
            fit_text(tab_label,
                     std::max(0.0f, rect.width - 2.0f * theme.padding), theme,
                     scratch, sizeof(scratch));
        if (fit.advance > 0.0f) {
            const float x = rect.x + (rect.width - fit.advance) * 0.5f;
            if (theme.font != nullptr) {
                r2d::draw_text(x, label_top(rect.y, rect.height, theme),
                               fit.text, *theme.font,
                               dim(theme.text, dim_by));
            } else {
                const float bar_height = rect.height * 0.5f;
                r2d::draw_quad(x, rect.y + (rect.height - bar_height) * 0.5f,
                               fit.advance, bar_height, dim(theme.text, dim_by));
            }
        }

        // A full click or the focused tab's Enter/Space moves the
        // selection; selecting the already-selected tab is a no-op that
        // reports nothing.
        if ((hit.clicked || key_selected) && i != active) {
            active = i;
            changed = true;
        }
        gui.ids().pop();
        if (i + 1 < count) {
            // One line, but FLUSH: the strip is one widget's row, not
            // flow items, so the tabs sit at fixed pitch (the strip is
            // count*width wide, the documented geometry) - the explicit
            // 0 gap overrides the panel's item spacing, which would
            // otherwise gap them.
            layout.same_line(0.0f);
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

    // Then the four pushes: the layout panel (it computes the content
    // rect), the clip at that rect and its hit-testing twin (children
    // cannot draw OR claim outside - what the clip hides cannot win the
    // pointer either), and the id scope (children id under the panel). A
    // degenerate content rect - an overflowed panel or bounds smaller
    // than the padding - pushes an empty clip: honestly nothing, rather
    // than children leaking outside.
    const Rect content = layout.begin_panel(bounds, theme.padding,
                                            theme.spacing);
    // A panel is a wheel claimant too - ALL panels are, scrollable or
    // not: the popup that covers a scroll panel eats the wheel even
    // though it scrolls nothing. Keyless: it claims, never consumes.
    gui.register_wheel_claim(bounds, nullptr);
    r2d::push_clip(content.x, content.y, content.width, content.height);
    gui.push_hit_clip(content);
    gui.ids().push(label);
    return content;
}

void end_panel(GuiContext& gui, LayoutCursor& layout) {
    // Exactly the reverse of begin_panel; each pop is a no-op against a
    // push that dropped (headless) or overflowed (the balanced clamps).
    gui.ids().pop();
    r2d::pop_clip();
    gui.pop_hit_clip();
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
    // while the scope is live). The key itself is RESERVED - the 'R'
    // family a user label can never spell - so "scroll" the widget
    // label and "scroll" the state key are two ids.
    gui.ids().push(label);
    const WidgetId scroll_id = gui.ids().internal_id("scroll");
    ScrollState& scroll = gui.persistent_state<ScrollState>(scroll_id);

    // The wheel claim, with THIS panel's own key (its ScrollState's
    // address - stable for the entry's lifetime): the panel consumes
    // the wheel only when it wins the resolution, so a popup panel
    // over it eats the wheel and nested panels stop double-scrolling.
    // The claim is the whole bounds, not just the viewport: the panel
    // owns its rim as much as its window.
    gui.register_wheel_claim(bounds, &scroll);

    // The wheel moves the offset when this panel owns it - the claim
    // settled LAST frame (the one-frame lag the settle costs: the
    // first notch after the cursor arrives over a panel settles the
    // ownership rather than scrolling) - and only while a mouse is
    // present: the flag gates the whole device, so a touch-only frame
    // (or a cursor that just left the window) never scrolls, whatever
    // mouse_x/y hold. Positive dy is wheel-up - toward EARLIER
    // content - so the offset (content shifted up) decreases. Clamped
    // against the range measured at the last end_scroll_panel: this
    // frame's content does not exist yet, and a fresh panel has no
    // range at all.
    if (gui.input().mouse_present && gui.input().scroll_dy != 0.0f &&
        gui.wheel_won(&scroll)) {
        scroll.offset -= gui.input().scroll_dy * theme.scroll_speed;
    }

    // Touch-drag: a contact that presses inside the viewport is watched -
    // the child it landed on still owns that press (a tap in progress).
    // Past the theme's drag_threshold the scroll takes the finger:
    // cancel_pointer ends the child's press with no click, and from that
    // frame the offset follows the contact 1:1 (finger up = later
    // content) until it lifts. The cancel fires HERE, before any child
    // runs, so the pressed widget sees no press at all from the crossing
    // frame on (known cosmetic limit: a drag-tracking child, a slider,
    // can move by < threshold px before the cancel). The mouse keeps the
    // wheel - a desktop drag is a widget press, not a scroll gesture.
    const WidgetId drag_id = gui.ids().internal_id("drag");
    ScrollDragState& drag = gui.persistent_state<ScrollDragState>(drag_id);
    const InputFrame& input = gui.input();
    const int touch_count =
        std::min(input.touch_count, InputFrame::kMaxTouches);
    // Sweep first: a track whose contact left the frame without a lift (a
    // platform contract violation) frees its slot - the same id-liveness
    // rule the context's own pointer tracks follow.
    for (ScrollDragTrack& track : drag.tracks) {
        if (track.id == kNoPointer) {
            continue;
        }
        bool live = false;
        for (int i = 0; i < touch_count; ++i) {
            live = live || input.touches[static_cast<std::size_t>(i)].id ==
                              track.id;
        }
        if (!live) {
            track = ScrollDragTrack{};
        }
    }
    for (int i = 0; i < touch_count; ++i) {
        const TouchPoint& point = input.touches[static_cast<std::size_t>(i)];
        ScrollDragTrack* track = drag_track_for(drag, point.id);
        if (track == nullptr) {
            continue;  // unreachable: the sweep keeps a slot free
        }
        if (track->id == kNoPointer) {
            // Untracked: arm only on a fresh press inside the viewport,
            // and only while this stratum may take claims at all - the
            // modal veil blocks the raw-input arm exactly as it blocks
            // hit_test's presses, or a scroll under a dialog would drag
            // through it.
            if (point.pressed && viewport.contains(point.x, point.y) &&
                gui.claims_open()) {
                // The arm claim keeps a nested drag singular: both the
                // outer and the inner panel see this press, and the
                // claim_scroll_arm rule (later-or-higher wins) hands it
                // to the innermost panel's begin, which runs after this
                // one. The outer's track lives out the press frame
                // harmless - it cannot cross the threshold the frame it
                // armed - and drops on the next frame's check below.
                gui.claim_scroll_arm(point.id, &scroll);
                track->id = point.id;
                track->press_y = point.y;
                track->last_y = point.y;
                track->dragging = false;
            }
            continue;
        }
        if (gui.scroll_arm_owner(track->id) != &scroll) {
            // Another panel owns this pointer's arm - the innermost
            // begin claimed over this one (later this same frame or the
            // press frame before). The watched track is not this
            // panel's gesture anymore: drop it SILENTLY, so the child
            // the press landed on keeps its tap and the cancel this
            // panel would have fired at the crossing never does.
            *track = ScrollDragTrack{};
            continue;
        }
        if (!point.down) {
            // The lift: a dragging contact's last delta lands with it; a
            // watched one never crossed - the child clicks, the track goes.
            if (track->dragging) {
                scroll.offset -= point.y - track->last_y;
            }
            *track = ScrollDragTrack{};
            continue;
        }
        if (track->dragging) {
            scroll.offset -= point.y - track->last_y;
            track->last_y = point.y;
            continue;
        }
        if (std::abs(point.y - track->press_y) > theme.drag_threshold) {
            track->dragging = true;
            track->last_y = point.y;  // deltas start at the crossing
            gui.cancel_pointer(point.id);  // the scroll owns the finger now
        }
    }
    scroll.offset =
        std::min(std::max(scroll.offset, 0.0f), scroll.max_offset);

    // Children flow inside a VIRTUAL panel, shifted up by the offset: the
    // rects layout hands out are already at their scrolled positions, so
    // widgets draw AND hit-test there with no per-widget knowledge - while
    // the renderer clip and its hit-testing twin stay at the unscrolled
    // viewport, cutting exactly what pokes past the edge from both the
    // pixels and the pointer: a row scrolled out of sight cannot claim a
    // click from whatever sits above or below the panel. Push order
    // (scope, layout, clip) is reversed by end_scroll_panel.
    (void)layout.begin_panel({bounds.x, bounds.y - scroll.offset,
                              bounds.width,
                              bounds.height + scroll.offset},
                             theme.padding, theme.spacing);
    r2d::push_clip(viewport.x, viewport.y, viewport.width, viewport.height);
    gui.push_hit_clip(viewport);
    return viewport;
}

void end_scroll_panel(GuiContext& gui, LayoutCursor& layout,
                      const ChromeTheme& theme) {
    // The same entries begin_scroll_panel keyed, re-derived inside the
    // still-pushed scope: the scroll state the thumb grab rides in, and
    // the flow-drag table the grab must quiet for its pointer. Same
    // reserved keys, same scope - the derivation cannot drift.
    const WidgetId scroll_id = gui.ids().internal_id("scroll");
    ScrollState& scroll = gui.persistent_state<ScrollState>(scroll_id);
    const WidgetId drag_id = gui.ids().internal_id("drag");
    ScrollDragState& drag = gui.persistent_state<ScrollDragState>(drag_id);

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

    // Tab's scroll-into-view: the keyboard arrived on a widget in this
    // panel (Tab at end_frame, the veil-restore hand-back, or set_focus
    // raised the request) and the widget has run, so its drawn bounds
    // are latched. Column overlap is the membership test; the shift is
    // the minimum that pulls the rect into the viewport, below-first
    // (the newly Tabbed thing usually follows the flow). Nested panels
    // each adjust, innermost first - the outer reads the pre-shift
    // latch, so a shift its child panel made the same frame lands a
    // frame late up there; the single-level case (the rule's real
    // shape) has no skew.
    if (gui.focus_into_view_pending()) {
        const Rect* focus = gui.focused_bounds();
        if (focus != nullptr && focus->x < viewport.right() &&
            focus->right() > viewport.x &&
            (focus->y < viewport.y || focus->bottom() > viewport.bottom())) {
            if (focus->y < viewport.y) {
                scroll.offset -= viewport.y - focus->y;  // pull back down
            } else {
                scroll.offset += focus->bottom() - viewport.bottom();
            }
            scroll.offset = std::min(std::max(scroll.offset, 0.0f),
                                    scroll.max_offset);
        }
    }

    // The thumb, after all children (painter order puts it over the
    // content) but still inside the clip. Drawn only while there is room
    // to scroll: a fitted panel wears no dead chrome. Proportional to
    // the visible share, with a floor so a huge content cannot shrink it
    // to nothing.
    //
    // The thumb is also GRABBABLE, and the grabbable band is the whole
    // right gutter (a finger needs more room than the thumb the bar
    // paints): a fresh press there ends any press a child took beneath
    // it - cancel_pointer, fired HERE at end, one frame after the child
    // claimed it, the same cosmetic limit the flow drag's cancel
    // carries - and from that frame the offset maps the pointer 1:1
    // through the track. A press ON the thumb keeps the exact grab
    // offset (the pixel under the finger stays under the finger); a
    // press elsewhere in the gutter jumps the thumb centered under the
    // pointer, the standard scrollbar behavior. The arm claim moves to
    // the thumb with this panel's own key, and the flow-drag track the
    // same press armed at begin dies here: a nested outer panel's
    // watched track must not cross its threshold and cancel this grab
    // mid-flight. The veil gates the NEW grab (claims_open) like it
    // gates every new claim; a grab already in flight is capture and
    // runs to the lift under it, like every drag.
    if (scroll.max_offset > 0.0f && viewport.width > 0.0f &&
        viewport.height > 0.0f) {
        const float track = viewport.height;
        const float thumb_width = theme.scrollbar_thumb_width;
        const float band_width = theme.scrollbar_band_width;  // the grabbable
                                                             // gutter
        float thumb_height =
            track * track / (track + scroll.max_offset);
        thumb_height = std::min(std::max(thumb_height,
                                         theme.scrollbar_min_thumb),
                                track);
        const auto thumb_top_for = [&](float offset) {
            return viewport.y + (offset / scroll.max_offset) *
                                      (track - thumb_height);
        };

        // The fresh press: mouse or any touch, inside the band, on a
        // stratum the veil has not closed.
        const Rect band{viewport.x + viewport.width - band_width, viewport.y,
                        band_width, viewport.height};
        const InputFrame& input = gui.input();
        const auto grab = [&](PointerId pointer, float py) {
            const float thumb_y = thumb_top_for(scroll.offset);
            if (py < thumb_y || py > thumb_y + thumb_height) {
                // Off the thumb: jump it centered under the pointer,
                // this frame - the children already laid out at the old
                // offset shift on the next, like every one-frame scroll.
                scroll.thumb_grab = thumb_height * 0.5f;
                scroll.offset = std::min(
                    std::max((py - scroll.thumb_grab - viewport.y) /
                                 (track - thumb_height) * scroll.max_offset,
                             0.0f),
                    scroll.max_offset);
            } else {
                scroll.thumb_grab = py - thumb_y;  // the pixel under the
                                                   // finger stays put
            }
            scroll.thumb = pointer;
            gui.claim_scroll_arm(pointer, &scroll);
            gui.cancel_pointer(pointer);  // the thumb owns this press now
            ScrollDragTrack* watched = drag_track_for(drag, pointer);
            if (watched != nullptr && watched->id == pointer) {
                *watched = ScrollDragTrack{};  // not the flow drag's press
            }
        };
        if (gui.claims_open()) {
            if (input.mouse_present && input.mouse_pressed[0] &&
                band.contains(input.mouse_x, input.mouse_y)) {
                grab(kMousePointerId, input.mouse_y);
            }
            const int touch_count =
                std::min(input.touch_count, InputFrame::kMaxTouches);
            for (int i = 0; i < touch_count; ++i) {
                const TouchPoint& point =
                    input.touches[static_cast<std::size_t>(i)];
                if (point.pressed && band.contains(point.x, point.y)) {
                    grab(point.id, point.y);
                }
            }
        }

        // The follow: the pointer's own view carries the position, so
        // the mouse, a touch and a bridged finger all drag their grab.
        // A lifted or vanished pointer ends the drag - nothing lands on
        // the release frame.
        if (scroll.thumb != kNoPointer) {
            const PointerView* view = gui.pointer(scroll.thumb);
            if (view == nullptr || !view->down) {
                scroll.thumb = kNoPointer;
            } else {
                scroll.offset = std::min(
                    std::max((view->y - scroll.thumb_grab - viewport.y) /
                                 (track - thumb_height) * scroll.max_offset,
                             0.0f),
                    scroll.max_offset);
            }
        }

        const float thumb_y = thumb_top_for(scroll.offset);
        r2d::draw_rounded_quad(viewport.x + viewport.width - thumb_width,
                               thumb_y, thumb_width, thumb_height,
                               theme.corner_radius, theme.border);
    }

    // The reverse of begin_scroll_panel's pushes.
    r2d::pop_clip();
    gui.pop_hit_clip();
    layout.end_panel();
    gui.ids().pop();
}

}  // namespace ws::ui::gui
