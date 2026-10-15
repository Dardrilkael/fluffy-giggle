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
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

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

// drag_float's persistent half (the same side-table discipline): the grab
// in flight - which pointer holds the press, where it started, and the
// value it started from. The anchor is what makes the drag RELATIVE:
// every frame maps the pointer's travel from the press, so no per-frame
// delta bookkeeping exists and a frame skipped mid-drag resumes exactly
// where the finger is.
struct DragState {
    PointerId pointer = kNoPointer;  // the grabbing pointer, while held
    float anchor_x = 0.0f;      // the grab's pointer x
    float anchor_value = 0.0f;  // the value at the grab
};

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

bool radio(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
           const char* label, int& value, int this_option) {
    const WidgetId id = gui.ids().id(label);

    // The row auto-sizes like a checkbox's: dot, a gap, the label's
    // DISPLAY advance ("##" suffixes id the row, never size it).
    const float gap = theme.spacing;
    char scratch[128];
    const LabelFit fit =
        fit_text(label, -1.0f, theme, scratch, sizeof(scratch));
    const float label_width = fit.advance;
    const Rect rect =
        layout.next(theme.control_height + gap + label_width,
                    theme.control_height);
    // Full citizenship, like a checkbox: focusable, state() for the one
    // hit-test plus the face, and the keyboard below.
    gui.register_focusable(id);
    const WidgetResult result = gui.state(id, rect);
    const HitTest& hit = result.hit;

    // Keyboard select: Enter or Space picks the focused radio,
    // edge-only - a click with no pointer involved.
    const bool key_selected =
        gui.is_focused(id) &&
        (gui.input().key_pressed[keys::kEnter] ||
         gui.input().key_pressed[keys::kSpace]);

    // Selecting the selected option is an idempotent write that reports
    // false: true means the selection MOVED to this option this frame.
    const bool was_other = value != this_option;
    if (hit.clicked || key_selected) {
        value = this_option;
    }

    // The focus ring: the on-accent as a 1px outline behind the row, so
    // Tab navigation is legible. Skipped for degenerate rows.
    if (gui.is_focused(id) && rect.width > 0.0f && rect.height > 0.0f) {
        r2d::draw_rounded_quad(rect.x - 1.0f, rect.y - 1.0f,
                               rect.width + 2.0f, rect.height + 2.0f,
                               theme.corner_radius + 1.0f, theme.control_on);
    }

    // The disabled look: the dead face for the dot, the fill and the
    // label dimmed through the one helper.
    const float dim_by =
        result.state == WidgetState::Disabled ? theme.disabled_alpha : 1.0f;

    // The dot: a control_height circle - the radius asks for half the
    // box, exactly what the draw clamps to - with the faces a button
    // wears.
    std::uint32_t face = theme.control_bg;
    if (result.state == WidgetState::Disabled) {
        face = theme.control_disabled;
    } else if (result.state == WidgetState::Pressed) {
        face = theme.control_pressed;
    } else if (result.state == WidgetState::Hovered) {
        face = theme.control_hovered;
    }
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
    return (hit.clicked || key_selected) && was_other;
}

bool toggle_switch(GuiContext& gui, LayoutCursor& layout,
                   const ChromeTheme& theme, const char* label, bool& value) {
    const WidgetId id = gui.ids().id(label);

    // The row auto-sizes like a checkbox's: the pill (twice the height
    // wide, the height tall), a gap, the label's DISPLAY advance.
    const float gap = theme.spacing;
    const float box = theme.control_height;
    const float track_w = 2.0f * box;
    char scratch[128];
    const LabelFit fit =
        fit_text(label, -1.0f, theme, scratch, sizeof(scratch));
    const float label_width = fit.advance;
    const Rect rect = layout.next(track_w + gap + label_width, box);
    gui.register_focusable(id);
    const WidgetResult result = gui.state(id, rect);
    const HitTest& hit = result.hit;

    // Keyboard toggle: Enter or Space flips the focused switch,
    // edge-only - a click with no pointer involved.
    const bool key_toggled =
        gui.is_focused(id) &&
        (gui.input().key_pressed[keys::kEnter] ||
         gui.input().key_pressed[keys::kSpace]);

    if (hit.clicked || key_toggled) {
        value = !value;  // the caller's bool is the state; a click flips it
    }

    // The focus ring: the on-accent as a 1px outline behind the row.
    // Skipped for degenerate rows.
    if (gui.is_focused(id) && rect.width > 0.0f && rect.height > 0.0f) {
        r2d::draw_rounded_quad(rect.x - 1.0f, rect.y - 1.0f,
                               rect.width + 2.0f, rect.height + 2.0f,
                               theme.corner_radius + 1.0f, theme.control_on);
    }

    // The disabled look: the knob and the label dimmed through the one
    // helper; the track's own dead face is the off state's, while the
    // on state dims its accent (below) like the checkbox's fill.
    const float dim_by =
        result.state == WidgetState::Disabled ? theme.disabled_alpha : 1.0f;

    // The pill: the control faces while off; the accent IS the track
    // while on - dimmed under the disabled scope rather than deadened,
    // so the state stays readable. The radius asks for half the height
    // and clamps to exactly that: the full pill.
    std::uint32_t face = theme.control_bg;
    if (result.state == WidgetState::Disabled) {
        face = theme.control_disabled;
    } else if (result.state == WidgetState::Pressed) {
        face = theme.control_pressed;
    } else if (result.state == WidgetState::Hovered) {
        face = theme.control_hovered;
    }
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

    // The label beside the pill, vertically centered - the same draw a
    // checkbox's label gets.
    if (fit.advance > 0.0f) {
        if (theme.font != nullptr) {
            r2d::draw_text(rect.x + track_w + gap,
                           label_top(rect.y, rect.height, theme), fit.text,
                           *theme.font, dim(theme.text, dim_by));
        } else {
            const float bar_height = rect.height * 0.5f;
            r2d::draw_quad(rect.x + track_w + gap,
                           rect.y + (rect.height - bar_height) * 0.5f,
                           fit.advance, bar_height, dim(theme.text, dim_by));
        }
    }
    return hit.clicked || key_toggled;
}

bool collapsing_header(GuiContext& gui, LayoutCursor& layout,
                       const ChromeTheme& theme, const char* label,
                       bool& open, float width) {
    // Fill-width, like button: a header wants its container's line.
    if (width < 0.0f) {
        width = layout.remaining_width();
    }
    width = std::max(width, 0.0f);  // a negative width would draw a mirrored quad

    const WidgetId id = gui.ids().id(label);
    const Rect rect = layout.next(width, theme.control_height);
    // Full citizenship, like a checkbox: focusable, state() for the one
    // hit-test plus the face, and the keyboard below.
    gui.register_focusable(id);
    const WidgetResult result = gui.state(id, rect);
    const HitTest& hit = result.hit;

    // Keyboard toggle: Enter or Space flips the focused header,
    // edge-only - a click with no pointer involved.
    const bool key_toggled =
        gui.is_focused(id) &&
        (gui.input().key_pressed[keys::kEnter] ||
         gui.input().key_pressed[keys::kSpace]);

    if (hit.clicked || key_toggled) {
        open = !open;  // the caller's bool is the state; a click flips it
    }

    // The focus ring: the on-accent as a 1px outline behind the row, so
    // Tab navigation is legible. Skipped for degenerate rows.
    if (gui.is_focused(id) && rect.width > 0.0f && rect.height > 0.0f) {
        r2d::draw_rounded_quad(rect.x - 1.0f, rect.y - 1.0f,
                               rect.width + 2.0f, rect.height + 2.0f,
                               theme.corner_radius + 1.0f, theme.control_on);
    }

    // The disabled look: the dead face for the row, the marker and the
    // label dimmed through the one helper.
    const float dim_by =
        result.state == WidgetState::Disabled ? theme.disabled_alpha : 1.0f;

    // The row: the faces a button wears, rounded by the theme.
    std::uint32_t face = theme.control_bg;
    if (result.state == WidgetState::Disabled) {
        face = theme.control_disabled;
    } else if (result.state == WidgetState::Pressed) {
        face = theme.control_pressed;
    } else if (result.state == WidgetState::Hovered) {
        face = theme.control_hovered;
    }
    r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                           theme.corner_radius, face);

    // The marker: a square inset like a checkbox's fill - the faint
    // border color closed, the accent open. The state at a glance. The
    // inset clears the row's rounded corners the same way the checkbox
    // fill clears its box's (a heavily-rounded theme cannot let the
    // square poke past the arcs).
    const float box = theme.control_height;
    const float inset = std::max(theme.check_inset, theme.corner_radius);
    const float marker = box - 2.0f * inset;
    if (marker > 0.0f) {
        r2d::draw_quad(rect.x + inset, rect.y + inset, marker, marker,
                       open ? dim(theme.control_on, dim_by) : theme.border);
    }

    // The label beside the marker, like a checkbox's - LEFT-aligned: a
    // header reads from the left. Fitted to the room the marker leaves
    // (minus a face inset each side).
    const float gap = theme.spacing;
    char scratch[128];
    const LabelFit fit =
        fit_text(label, std::max(0.0f, rect.width - box - gap -
                                          2.0f * theme.label_inset),
                 theme, scratch, sizeof(scratch));
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
    // The answer is the state AFTER the flip: the caller's `if` places
    // this frame's children when the click just opened it.
    return open;
}

bool selectable(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
               const char* label, bool selected, float width) {
    // Fill-width, like button: list rows want their container's line.
    if (width < 0.0f) {
        width = layout.remaining_width();
    }
    width = std::max(width, 0.0f);  // a negative width would draw a mirrored quad

    const WidgetId id = gui.ids().id(label);
    const Rect rect = layout.next(width, theme.control_height);
    gui.register_focusable(id);
    const WidgetResult result = gui.state(id, rect);
    const HitTest& hit = result.hit;

    // Keyboard select: Enter or Space reports the focused row, edge-only
    // - a click with no pointer involved. The selection itself stays
    // the caller's to write.
    const bool key_clicked =
        gui.is_focused(id) &&
        (gui.input().key_pressed[keys::kEnter] ||
         gui.input().key_pressed[keys::kSpace]);

    // The focus ring: the on-accent as a 1px outline behind the row.
    // Skipped for degenerate rows.
    if (gui.is_focused(id) && rect.width > 0.0f && rect.height > 0.0f) {
        r2d::draw_rounded_quad(rect.x - 1.0f, rect.y - 1.0f,
                               rect.width + 2.0f, rect.height + 2.0f,
                               theme.corner_radius + 1.0f, theme.control_on);
    }

    const float dim_by =
        result.state == WidgetState::Disabled ? theme.disabled_alpha : 1.0f;

    // The face ladder EXCEPT the plain idle face: an unselected,
    // unhovered row draws nothing at all - the bare list look. Hover,
    // press and the disabled scope take the usual faces.
    if (result.state == WidgetState::Disabled) {
        r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                               theme.corner_radius, theme.control_disabled);
    } else if (result.state == WidgetState::Pressed) {
        r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                               theme.corner_radius, theme.control_pressed);
    } else if (result.state == WidgetState::Hovered) {
        r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                               theme.corner_radius, theme.control_hovered);
    }

    // The selection marker: a 3-px accent bar down the row's left edge -
    // the v1 look, a hardcoded pixel like the separator's 2. It rides
    // every face (or the bare row), dimmed under the disabled scope.
    if (selected && rect.width > 0.0f && rect.height > 0.0f) {
        r2d::draw_quad(rect.x, rect.y, 3.0f, rect.height,
                       dim(theme.control_on, dim_by));
    }

    // The label, left-aligned past the bar's room (selected and
    // unselected rows align), vertically centered: the theme font, or
    // the fontless bar at the fitted advance.
    char scratch[128];
    const LabelFit fit =
        fit_text(label, std::max(0.0f, rect.width - theme.check_inset -
                                          theme.label_inset),
                 theme, scratch, sizeof(scratch));
    if (fit.advance > 0.0f) {
        const float x = rect.x + theme.check_inset;
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

void text(LayoutCursor& layout, const ChromeTheme& theme,
          const char* label, float width) {
    // Chrome: no id, no hit-test, no state - it lays out and draws, and
    // that is all. A caller wanting a faded label dims its own theme
    // copy; the gui does not invent a Disabled state for something that
    // cannot be disabled.
    char scratch[128];
    const LabelFit fit = fit_text(label, width < 0.0f ? -1.0f : width, theme,
                                  scratch, sizeof(scratch));
    // A negative width sizes the row to the label's DISPLAY advance -
    // marker-cut, never truncated, like a checkbox's sizing fit (and a
    // "##" suffix neither ids nor sizes anything here: it just never
    // draws). A fixed width pins the row and truncates the label into
    // it instead. fit_text's own tests own the truncation ladder.
    const Rect rect =
        layout.next(width < 0.0f ? fit.advance : width, theme.control_height);
    if (fit.advance > 0.0f) {
        if (theme.font != nullptr) {
            r2d::draw_text(rect.x, label_top(rect.y, rect.height, theme),
                           fit.text, *theme.font, theme.text);
        } else {
            const float bar_height = rect.height * 0.5f;
            r2d::draw_quad(rect.x, rect.y + (rect.height - bar_height) * 0.5f,
                           fit.advance, bar_height, theme.text);
        }
    }
}

void separator(LayoutCursor& layout, const ChromeTheme& theme, float width) {
    // Fill-width, like button: a negative width means "the rest of the
    // line", resolved against the cursor before the clamp.
    if (width < 0.0f) {
        width = layout.remaining_width();
    }
    width = std::max(width, 0.0f);  // a negative width would draw a mirrored quad
    // A 2-px rule - a hardcoded pixel like the focus ring's 1, not a
    // theme metric. theme.border is the faint line that never competes
    // with content, and the flow's own spacing gives it air on both
    // sides.
    const Rect rect = layout.next(width, 2.0f);
    r2d::draw_quad(rect.x, rect.y, rect.width, rect.height, theme.border);
}

void progress(LayoutCursor& layout, const ChromeTheme& theme,
              float fraction, float width) {
    // Fill-width, like button: a negative width means "the rest of the
    // line", resolved against the cursor before the clamp.
    if (width < 0.0f) {
        width = layout.remaining_width();
    }
    width = std::max(width, 0.0f);  // a negative width would draw a mirrored quad
    // Clamp to [0, 1], NaN-safe: the inverted comparisons catch a NaN
    // (it fails >= 0) and read it as empty rather than letting it
    // through to the draw.
    if (!(fraction >= 0.0f)) {
        fraction = 0.0f;
    }
    if (!(fraction <= 1.0f)) {
        fraction = 1.0f;
    }
    const Rect rect = layout.next(width, theme.control_height);
    r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                           theme.corner_radius, theme.control_bg);
    // The fill: the accent over the track, rounded with the theme; the
    // draw clamps the radius to the fill's own half-height, so a thin
    // sliver reads as a dot rather than an over-rounded bar.
    const float fill = fraction * rect.width;
    if (fill > 0.0f) {
        r2d::draw_rounded_quad(rect.x, rect.y, fill, rect.height,
                               theme.corner_radius, theme.control_on);
    }
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

bool drag_float(GuiContext& gui, LayoutCursor& layout,
               const ChromeTheme& theme, const char* label, float& value,
               float min, float max, float speed, float width) {
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

    // The anchor: persistent state under the widget id, so the grab
    // survives frames (and clear_persistent_state evicts it - the
    // same side-table discipline as the scroll state).
    DragState& drag = gui.persistent_state<DragState>(id);

    bool changed = false;

    // Grab-and-drag, not click-to-set: a press inside GRABS (the value
    // does not jump - unlike a slider there is nothing to click-to-set),
    // and while that pointer holds the press the value maps its travel
    // from the ANCHOR, so no per-frame delta bookkeeping exists and a
    // frame skipped mid-drag resumes exactly where the finger is. The
    // sweep outlives the rect (hit.pressed stays true through the hold,
    // press started inside), and a same-frame press+release never sees
    // the pointer down - a plain click grabs and lifts, moving nothing.
    const PointerView* owner = gui.pointer(hit.pointer);
    if (hit.pressed && owner != nullptr && owner->down) {
        if (drag.pointer == kNoPointer) {
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
                changed = true;
            }
        }
    } else {
        // Lift, cancel, or a pointer that never really pressed us: the
        // grab ends, so the next press anchors fresh.
        drag.pointer = kNoPointer;
    }

    // Keyboard: the arrows step the focused row - 1% of the range per
    // press, like a slider; only a real move reports changed, and the
    // same NaN guard holds.
    if (gui.is_focused(id) && (gui.input().key_pressed[keys::kLeft] ||
                               gui.input().key_pressed[keys::kRight])) {
        const float step = (max - min) / 100.0f;
        const float delta =
            gui.input().key_pressed[keys::kRight] ? step : -step;
        const float next = std::min(std::max(value + delta, min), max);
        if (next == next && next != value) {
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

    // The disabled look: the dead face for the row, the value label
    // dimmed through the one helper.
    const float dim_by =
        result.state == WidgetState::Disabled ? theme.disabled_alpha : 1.0f;

    // The row: the faces a button wears, rounded by the theme - a drag
    // value reads as a control, not a text line.
    std::uint32_t face = theme.control_bg;
    if (result.state == WidgetState::Disabled) {
        face = theme.control_disabled;
    } else if (result.state == WidgetState::Pressed) {
        face = theme.control_pressed;
    } else if (result.state == WidgetState::Hovered) {
        face = theme.control_hovered;
    }
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
    if (fit.advance > 0.0f) {
        const float x = rect.x + theme.padding;
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

bool begin_popup(GuiContext& gui, LayoutCursor& layout,
                 const ChromeTheme& theme, const char* label,
                 const Rect& anchor, const Rect& bounds, bool& open) {
    if (!open) {
        return false;
    }

    // The outside-press close: a fresh press that lands outside the bounds
    // AND outside the anchor flips `open` false and lays nothing this
    // frame - the press carries on to whatever sits beneath (the click
    // passes through; the widget under claims it fresh). The anchor is
    // exempt so the click that hangs the popup on its own control belongs
    // to that control (a combo row toggles itself), not to the close
    // rule. The veil gate applies: under a raised modal veil the popup is
    // inert like every lower stratum - its close rule eats no press the
    // veil owns.
    const InputFrame& input = gui.input();
    if (gui.claims_open()) {
        if (input.mouse_present && input.mouse_pressed[0] &&
            !bounds.contains(input.mouse_x, input.mouse_y) &&
            !anchor.contains(input.mouse_x, input.mouse_y)) {
            open = false;
            return false;
        }
        const int touch_count =
            std::min(input.touch_count, InputFrame::kMaxTouches);
        for (int i = 0; i < touch_count; ++i) {
            const TouchPoint& point =
                input.touches[static_cast<std::size_t>(i)];
            if (point.pressed && !bounds.contains(point.x, point.y) &&
                !anchor.contains(point.x, point.y)) {
                open = false;
                return false;
            }
        }
    }

    // The stratum, then the panel scope. The clip pair ESCAPES at the
    // bounds, r2d side and hit side at the same rect like every panel
    // clip: the popup hangs off a widget inside a panel (whose clip pair
    // is active at this call), and it must draw AND hit-test past its
    // host's hole - a dropdown at the host's bottom edge would shear off
    // and go unclickable there otherwise. The content pair then nests
    // against the escaped bounds as usual.
    gui.begin_layer(Layer::popup);
    r2d::push_clip_escape(bounds.x, bounds.y, bounds.width, bounds.height);
    gui.push_hit_clip_escape(bounds);
    panel_chrome(bounds, theme);
    const Rect content =
        layout.begin_panel(bounds, theme.padding, theme.spacing);
    // Keyless wheel claim, begin_panel's: claims to eat, never consume -
    // a popup over a scroll panel takes the wheel even though it scrolls
    // nothing.
    gui.register_wheel_claim(bounds, nullptr);
    r2d::push_clip(content.x, content.y, content.width, content.height);
    gui.push_hit_clip(content);
    gui.ids().push(label);
    return open;
}

void end_popup(GuiContext& gui, LayoutCursor& layout) {
    // Drop-safe through the layer: a begin_popup that answered false
    // pushed nothing, and this guard keeps a stray end from eating an
    // outer scope.
    if (gui.current_layer() != Layer::popup) {
        return;
    }
    // Exactly the reverse of begin_popup's pushes.
    gui.ids().pop();
    r2d::pop_clip();
    gui.pop_hit_clip();
    layout.end_panel();
    r2d::pop_clip();
    gui.pop_hit_clip();
    gui.end_layer();
}

bool begin_tooltip(GuiContext& gui, LayoutCursor& layout,
                   const ChromeTheme& theme, const char* label,
                   const Rect& bounds, bool show) {
    if (!show) {
        return false;
    }

    // The tooltip stratum and the panel scope, with the clip escape on
    // the r2d side only: the hint draws past any panel it hovers over.
    // NO hit clip, no wheel claim - nothing in a tooltip is meant to
    // claim, and a caller who puts a live widget in one asked for exactly
    // what they get (it claims at the tooltip stratum, unclipped).
    gui.begin_layer(Layer::tooltip);
    r2d::push_clip_escape(bounds.x, bounds.y, bounds.width, bounds.height);
    panel_chrome(bounds, theme);
    layout.begin_panel(bounds, theme.padding, theme.spacing);
    gui.ids().push(label);
    return show;
}

void end_tooltip(GuiContext& gui, LayoutCursor& layout) {
    // Drop-safe through the layer, like end_popup.
    if (gui.current_layer() != Layer::tooltip) {
        return;
    }
    gui.ids().pop();
    layout.end_panel();
    r2d::pop_clip();
    gui.end_layer();
}

bool begin_modal(GuiContext& gui, LayoutCursor& layout,
                 const ChromeTheme& theme, const char* label,
                 const Rect& bounds, bool open) {
    if (!open) {
        return false;
    }

    gui.begin_layer(Layer::modal);

    // The visible veil - what the input rules already enforce, painted.
    // Drawn first inside the stratum (above every lower-layer chrome,
    // before this scope's own panel) and under its own FULLSCREEN clip
    // escape: the modal may open while a host panel's clip is active, and
    // the veil covers the whole frame, not the host's content rect.
    // Headless the frame size is zero - the escape normalizes to empty
    // and the veil drops, the ordinary r2d contract.
    const r2d::FrameSize frame = r2d::frame_size();
    r2d::push_clip_escape(0.0f, 0.0f, frame.width, frame.height);
    r2d::draw_quad(0.0f, 0.0f, frame.width, frame.height, theme.veil);
    r2d::pop_clip();

    // Then the panel scope, the same shape as a popup's - the clip pair
    // escapes at the bounds, the content pair nests inside - minus the
    // close rule: blocking IS the point. The dialog's own buttons flip
    // the caller's bool.
    r2d::push_clip_escape(bounds.x, bounds.y, bounds.width, bounds.height);
    gui.push_hit_clip_escape(bounds);
    panel_chrome(bounds, theme);
    const Rect content =
        layout.begin_panel(bounds, theme.padding, theme.spacing);
    gui.register_wheel_claim(bounds, nullptr);
    r2d::push_clip(content.x, content.y, content.width, content.height);
    gui.push_hit_clip(content);
    gui.ids().push(label);
    return open;
}

void end_modal(GuiContext& gui, LayoutCursor& layout) {
    // Drop-safe through the layer, like end_popup. Note the veil the
    // scope raised outlives this end - frame-wide, lifts at the next
    // begin_frame (the begin_layer rules).
    if (gui.current_layer() != Layer::modal) {
        return;
    }
    gui.ids().pop();
    r2d::pop_clip();
    gui.pop_hit_clip();
    layout.end_panel();
    r2d::pop_clip();
    gui.pop_hit_clip();
    gui.end_layer();
}

bool combo(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
           const char* label, const char* const* items, int count,
           int& selected, bool& open, float width) {
    // Fill-width, like button: a combo row wants its container's line.
    if (width < 0.0f) {
        width = layout.remaining_width();
    }
    width = std::max(width, 0.0f);  // a negative width would draw a mirrored quad

    const WidgetId id = gui.ids().id(label);
    const Rect rect = layout.next(width, theme.control_height);
    // Full citizenship, like every row widget: focusable, state() for the
    // one hit-test plus the faces.
    gui.register_focusable(id);
    const WidgetResult result = gui.state(id, rect);
    const HitTest& hit = result.hit;

    // The empty combo: nothing to choose, nothing to open - a bare row
    // that never pretends otherwise (and never touches the caller's
    // bool, so a later refill starts closed).
    const bool empty = count <= 0 || items == nullptr;
    if (empty) {
        open = false;
    }

    // The row's click and keyboard toggle: Enter/Space on the focused
    // row flips it too, like a header.
    const bool key_toggled =
        gui.is_focused(id) && !empty &&
        (gui.input().key_pressed[keys::kEnter] ||
         gui.input().key_pressed[keys::kSpace]);
    if (!empty && (hit.clicked || key_toggled)) {
        open = !open;  // the caller's bool is the popup's state
    }

    // The focus ring: the on-accent as a 1px outline behind the row, so
    // Tab navigation is legible. Skipped for degenerate rows.
    if (gui.is_focused(id) && rect.width > 0.0f && rect.height > 0.0f) {
        r2d::draw_rounded_quad(rect.x - 1.0f, rect.y - 1.0f,
                               rect.width + 2.0f, rect.height + 2.0f,
                               theme.corner_radius + 1.0f, theme.control_on);
    }

    // The disabled look: the dead face for the row, the marker and the
    // label dimmed through the one helper.
    const float dim_by =
        result.state == WidgetState::Disabled ? theme.disabled_alpha : 1.0f;

    // The row: the faces a button wears, rounded by the theme.
    std::uint32_t face = theme.control_bg;
    if (result.state == WidgetState::Disabled) {
        face = theme.control_disabled;
    } else if (result.state == WidgetState::Pressed) {
        face = theme.control_pressed;
    } else if (result.state == WidgetState::Hovered) {
        face = theme.control_hovered;
    }
    r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                           theme.corner_radius, face);

    // The marker at the RIGHT edge - the collapsing_header's marker
    // mirrored: the faint border closed, the accent open. The same
    // square-inset geometry, one gap from the row's right side.
    const float inset = std::max(theme.check_inset, theme.corner_radius);
    const float marker = theme.control_height - 2.0f * inset;
    if (marker > 0.0f) {
        r2d::draw_quad(rect.x + rect.width - inset - marker, rect.y + inset,
                       marker, marker,
                       open ? dim(theme.control_on, dim_by) : theme.border);
    }

    // The "label: item" line, drag_float's shape: the display cut first,
    // then one snprintf - a `selected` out of range (or a label-less
    // combo) shows the item alone, the label alone... a label-less
    // empty combo shows nothing. The marker's band comes off the text
    // budget, a padding each side of what remains.
    char value_buf[64];
    const std::string_view display = label_display(label);
    const bool has_item = !empty && selected >= 0 && selected < count &&
                          items[static_cast<std::size_t>(selected)] != nullptr;
    if (display.empty()) {
        if (has_item) {
            std::snprintf(value_buf, sizeof(value_buf), "%s",
                          items[static_cast<std::size_t>(selected)]);
        } else {
            value_buf[0] = '\0';
        }
    } else if (has_item) {
        std::snprintf(value_buf, sizeof(value_buf), "%.*s: %s",
                      static_cast<int>(display.size()), display.data(),
                      items[static_cast<std::size_t>(selected)]);
    } else {
        std::snprintf(value_buf, sizeof(value_buf), "%.*s",
                      static_cast<int>(display.size()), display.data());
    }
    char scratch[128];
    const LabelFit fit =
        fit_text(value_buf, std::max(0.0f, rect.width - marker - inset -
                                              2.0f * theme.padding),
                 theme, scratch, sizeof(scratch));
    if (fit.advance > 0.0f) {
        const float x = rect.x + theme.padding;
        if (theme.font != nullptr) {
            r2d::draw_text(x, label_top(rect.y, rect.height, theme), fit.text,
                           *theme.font, dim(theme.text, dim_by));
        } else {
            const float bar_height = rect.height * 0.5f;
            r2d::draw_quad(x, rect.y + (rect.height - bar_height) * 0.5f,
                           fit.advance, bar_height, dim(theme.text, dim_by));
        }
    }

    // The dropdown, placed by the widget: below the row while the frame
    // has room, else flipped above, clamped at the frame's edges. The
    // popup's own clip cuts whatever exceeds the clamped bounds - the
    // honest v1 clamp (a frame too short shows what fits).
    if (empty) {
        return false;  // nothing to lay beneath a bare row
    }
    const r2d::FrameSize frame = r2d::frame_size();
    const float items_h = static_cast<float>(count) * theme.control_height +
                          static_cast<float>(std::max(count - 1, 0)) *
                              theme.spacing;
    float needed_h = items_h + 2.0f * theme.padding;
    needed_h = std::min(needed_h, frame.height);  // never taller than the frame
    float y = rect.y + rect.height;                // flush below the row
    if (y + needed_h > frame.height) {
        y = rect.y - needed_h;  // no room below: flip above
    }
    if (y < 0.0f) {
        y = 0.0f;  // no room above either: clamp at the frame's top
    }
    const Rect bounds{rect.x, y, rect.width, needed_h};

    bool changed = false;
    if (begin_popup(gui, layout, theme, label, rect, bounds, open)) {
        for (int i = 0; i < count; ++i) {
            gui.ids().push(i);  // one label, count ids
            const char* item = items[static_cast<std::size_t>(i)];
            const bool picked =
                selectable(gui, layout, theme,
                           item != nullptr ? item : "", i == selected, -1.0f);
            if (picked && i != selected) {
                selected = i;  // the caller's int, written on a real move
                changed = true;
            }
            if (picked) {
                open = false;  // a choice closes: the combo's rule
                // And the focus comes back to the row: the item's id is
                // about to vanish with the dropdown, and the absent-
                // widget rule would leave the next Tab restarting at the
                // screen's first focusable - the row keeps the walk where
                // it was. (The outside-press close needs no help: the
                // press that closed it landed on a focusable beneath,
                // which took the focus by its own rule.)
                gui.set_focus(id);
            }
            gui.ids().pop();
        }
        end_popup(gui, layout);
    }
    return changed;
}

// --- M20: the text input ----------------------------------------------------

bool text_input(GuiContext& gui, LayoutCursor& layout,
                const ChromeTheme& theme, const char* label, char* buf,
                int capacity, float width) {
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
    TextInputState& edit = gui.persistent_state<TextInputState>(id);

    // The caller owns the buffer; the widget owns the editing. `live` is
    // the honest capacity read - a null buf or no room for a byte plus
    // its NUL lays the same row inert (the empty-combo idiom: an
    // uneditable field does not pretend otherwise).
    const bool live = buf != nullptr && capacity >= 2;
    const int room = live ? capacity - 1 : 0;  // bytes the buffer may hold

    // The caller's contract: NUL-terminated within `capacity`. The walk
    // bounds itself at the room anyway, so a buffer past its stated
    // capacity sees its own prefix edited, and every write the widget
    // makes still lands inside the capacity.
    int len = 0;
    if (live) {
        while (len < room && buf[len] != '\0') {
            ++len;
        }
    }

    const InputFrame& in = gui.input();
    // state()'s press edge claims the focus the SAME frame, so a quick
    // tap's own press frame already reads focused here.
    const bool focused = gui.is_focused(id);
    const bool dead = result.state == WidgetState::Disabled;
    const bool can_edit = live && focused && !dead;

    // The advance arithmetic the whole field rides: one codepoint's
    // honest glyph advance (0 outside the bake range - inert bytes sit
    // still), or (fontless) the v1 bar's per-char estimate, the same
    // measure fit_text sizes by.
    const auto cp_advance = [&](unsigned char c) -> float {
        if (theme.font != nullptr) {
            const r2d::Font::Glyph* g = theme.font->glyph(c);
            return g != nullptr ? g->advance : 0.0f;
        }
        return theme.label_char_width;
    };
    const auto char_advance = [&](int i) -> float {
        return cp_advance(static_cast<unsigned char>(buf[i]));
    };
    // The pen x at byte `index`, clamped to the buffer's end.
    const auto x_at = [&](int index) -> float {
        float acc = 0.0f;
        for (int i = 0; i < index && i < len; ++i) {
            acc += char_advance(i);
        }
        return acc;
    };
    // The inverse: an x inside the row names the first byte whose
    // half-advance the click crossed - the standard edit-field
    // placement, clamped to the buffer.
    const auto caret_from_x = [&](float dx) -> int {
        float acc = 0.0f;
        for (int i = 0; i < len; ++i) {
            const float adv = char_advance(i);
            if (dx < acc + adv * 0.5f) {
                return i;
            }
            acc += adv;
        }
        return len;
    };

    const float content_x = rect.x + theme.padding;
    const float content_room = std::max(0.0f, rect.width - 2.0f * theme.padding);

    bool changed = false;

    // The frame clock's first consumer: the phase runs every frame (the
    // clock does), and the resets below rewind it solid - every edit,
    // every caret move, every press, and the focus's own arrival.
    edit.blink += in.dt_seconds;
    if (edit.blink >= 1.0f) {
        edit.blink -= 1.0f;
        if (edit.blink >= 1.0f) {
            edit.blink = 0.0f;  // a hitched frame (a pause, a debugger) restarts
        }
    }
    if (focused && !edit.was_focused) {
        edit.blink = 0.0f;  // arrival shows the caret solid
    }

    // --- the pointer: a press places the caret, a sweep selects ---------
    const PointerView* owner = gui.pointer(hit.pointer);
    const bool hold = hit.pressed && owner != nullptr && owner->down;
    const bool shift = in.key_down[static_cast<std::size_t>(keys::kLeftShift)] ||
                       in.key_down[static_cast<std::size_t>(keys::kRightShift)];
    if (can_edit && hold) {
        const int placed = caret_from_x(owner->x - content_x);
        if (owner->pressed || placed != edit.caret) {
            // Shift at the press extends the standing selection; a plain
            // press collapses the anchor onto the caret, and the sweep
            // that follows re-opens a selection the moment the pointer
            // travels.
            if (owner->pressed && !shift) {
                edit.anchor = placed;
            }
            edit.caret = placed;
            edit.blink = 0.0f;
        }
    }

    // --- the keyboard: caret keys, deleters, typing, the clipboard ------
    if (can_edit) {
        const bool ctrl =
            in.key_down[static_cast<std::size_t>(keys::kLeftControl)] ||
            in.key_down[static_cast<std::size_t>(keys::kRightControl)];

        // The caret keys: Shift extends from the anchor, plain collapses
        // it onto the caret - the rule every editor spells.
        const auto move_caret = [&](int to) {
            edit.caret = to;
            if (!shift) {
                edit.anchor = to;
            }
            edit.blink = 0.0f;
        };
        if (in.key_pressed[static_cast<std::size_t>(keys::kLeft)]) {
            move_caret(std::max(0, edit.caret - 1));
        }
        if (in.key_pressed[static_cast<std::size_t>(keys::kRight)]) {
            move_caret(std::min(len, edit.caret + 1));
        }
        if (in.key_pressed[static_cast<std::size_t>(keys::kHome)]) {
            move_caret(0);
        }
        if (in.key_pressed[static_cast<std::size_t>(keys::kEnd)]) {
            move_caret(len);
        }
        if (ctrl && in.key_pressed[static_cast<std::size_t>(keys::kA)]) {
            edit.anchor = 0;  // select all: the anchor says it
            edit.caret = len;
            edit.blink = 0.0f;
        }
        if (in.key_pressed[static_cast<std::size_t>(keys::kEscape)]) {
            edit.anchor = edit.caret;  // Escape's one job: drop the selection
            edit.blink = 0.0f;
        }

        // The one eraser every deleter funnels through: [lo, hi) leaves,
        // the tail slides, the terminator follows the new end.
        const auto erase_range = [&](int lo, int hi) {
            lo = std::max(0, lo);
            hi = std::min(len, hi);
            if (lo >= hi) {
                return;
            }
            std::memmove(buf + lo, buf + hi,
                         static_cast<std::size_t>(len - hi));
            len -= hi - lo;
            buf[len] = '\0';
        };
        // The selection as a range, low end first (equal ends: none).
        const auto sel_lo = [&] { return std::min(edit.caret, edit.anchor); };
        const auto sel_hi = [&] { return std::max(edit.caret, edit.anchor); };

        // Deleters before typing, when one frame carries both (no arrival
        // order exists between key edges and the codepoint list): the
        // old text leaves, then the new lands at the settled caret.
        if (in.key_pressed[static_cast<std::size_t>(keys::kBackspace)] ||
            in.key_pressed[static_cast<std::size_t>(keys::kDelete)]) {
            const bool forward =
                in.key_pressed[static_cast<std::size_t>(keys::kDelete)];
            if (sel_lo() != sel_hi()) {
                const int lo = sel_lo();
                erase_range(lo, sel_hi());
                edit.caret = edit.anchor = lo;
                changed = true;
            } else if (!forward && edit.caret > 0) {
                erase_range(edit.caret - 1, edit.caret);
                --edit.caret;
                edit.anchor = edit.caret;
                changed = true;
            } else if (forward && edit.caret < len) {
                erase_range(edit.caret, edit.caret + 1);
                edit.anchor = edit.caret;
                changed = true;
            }
            edit.blink = 0.0f;
        }

        // Typing: the frame's codepoints filtered to the bake range (v1
        // edits ASCII). A selection dies under what replaces it, and a
        // full buffer drops the rest of the frame's text.
        for (std::uint32_t cp : in.text_input) {
            if (cp < 32u || cp > 126u) {
                continue;
            }
            if (edit.caret != edit.anchor) {
                const int lo = sel_lo();
                erase_range(lo, sel_hi());
                edit.caret = edit.anchor = lo;
            }
            if (len >= room) {
                break;
            }
            std::memmove(buf + edit.caret + 1, buf + edit.caret,
                         static_cast<std::size_t>(len - edit.caret));
            buf[edit.caret] = static_cast<char>(cp);
            ++len;
            buf[len] = '\0';
            ++edit.caret;
            edit.anchor = edit.caret;
            edit.blink = 0.0f;
            changed = true;
        }

        // The clipboard trio - only a selection copies or cuts, and a
        // paste lands at the caret replacing the selection first, like
        // typing. An unwired seam makes all three no-ops (the context's
        // own contract; Android keeps one today).
        if (ctrl &&
            (in.key_pressed[static_cast<std::size_t>(keys::kC)] ||
             in.key_pressed[static_cast<std::size_t>(keys::kX)]) &&
            sel_lo() != sel_hi()) {
            // The one allocation an explicit copy may make: a mid-buffer
            // range is not NUL-terminated, and the seam speaks C strings.
            gui.set_clipboard_text(
                std::string(buf + sel_lo(), buf + sel_hi()).c_str());
            if (in.key_pressed[static_cast<std::size_t>(keys::kX)]) {
                const int lo = sel_lo();
                erase_range(lo, sel_hi());
                edit.caret = edit.anchor = lo;
                edit.blink = 0.0f;
                changed = true;
            }
        }
        if (ctrl && in.key_pressed[static_cast<std::size_t>(keys::kV)]) {
            const char* pasted = gui.get_clipboard_text();
            if (pasted != nullptr) {
                if (edit.caret != edit.anchor) {
                    const int lo = sel_lo();
                    erase_range(lo, sel_hi());
                    edit.caret = edit.anchor = lo;
                }
                // Two passes: count the bytes that pass the filter AND
                // the room, then insert them in one move - a paste never
                // walks the buffer per byte and never splits capacity.
                int take = 0;
                for (const char* p = pasted;
                     *p != '\0' && len + take < room; ++p) {
                    const unsigned char c = static_cast<unsigned char>(*p);
                    if (c >= 32u && c <= 126u) {
                        ++take;
                    }
                }
                if (take > 0) {
                    std::memmove(buf + edit.caret + take, buf + edit.caret,
                                 static_cast<std::size_t>(len - edit.caret));
                    int at = edit.caret;
                    for (const char* p = pasted; *p != '\0'; ++p) {
                        if (at - edit.caret >= take) {
                            break;
                        }
                        const unsigned char c =
                            static_cast<unsigned char>(*p);
                        if (c < 32u || c > 126u) {
                            continue;
                        }
                        buf[at++] = *p;
                    }
                    len += take;
                    buf[len] = '\0';
                    edit.caret = edit.anchor = at;
                    edit.blink = 0.0f;
                    changed = true;
                }
            }
        }
    }

    // The buffer may have shrunk under the edits (or under the caller's
    // own hand between frames): both ends stay in range.
    edit.caret = std::clamp(edit.caret, 0, len);
    edit.anchor = std::clamp(edit.anchor, 0, len);
    edit.was_focused = focused;

    // --- the draw: ring, face, band, text, caret --------------------------
    if (focused && rect.width > 0.0f && rect.height > 0.0f) {
        r2d::draw_rounded_quad(rect.x - 1.0f, rect.y - 1.0f,
                               rect.width + 2.0f, rect.height + 2.0f,
                               theme.corner_radius + 1.0f, theme.control_on);
    }
    const float dim_by = dead ? theme.disabled_alpha : 1.0f;
    std::uint32_t face = theme.control_bg;
    if (dead) {
        face = theme.control_disabled;
    } else if (result.state == WidgetState::Pressed) {
        face = theme.control_pressed;  // the field reads held while a press owns it
    } else if (result.state == WidgetState::Hovered) {
        face = theme.control_hovered;
    }
    r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                           theme.corner_radius, face);

    // The caret and the selection band ride the line box with a font,
    // the half-height bar without - the same two shapes every widget's
    // label wears.
    float mark_y = rect.y + rect.height * 0.25f;
    float mark_h = rect.height * 0.5f;
    if (theme.font != nullptr) {
        mark_y = label_top(rect.y, rect.height, theme);
        mark_h = theme.font->line_height();
    }

    // The selection band: the accent under the text, the selected tab's
    // face - only while the field holds the focus (a stale selection
    // never bands an unfocused row).
    const int sel_lo_draw = std::min(edit.caret, edit.anchor);
    const int sel_hi_draw = std::max(edit.caret, edit.anchor);
    if (focused && !dead && sel_hi_draw > sel_lo_draw) {
        const float x0 =
            content_x + std::min(x_at(sel_lo_draw), content_room);
        const float x1 =
            content_x + std::min(x_at(sel_hi_draw), content_room);
        if (x1 > x0) {
            r2d::draw_quad(x0, mark_y, x1 - x0, mark_h, theme.control_on);
        }
    }

    if (len > 0) {
        // The content, left-aligned inside the padding. Content is DATA,
        // so fit_text's "##" cut does not apply - the truncation here is
        // the walk itself: the longest prefix + "..." when the whole
        // line misses the row's room (the v1 filter already guarantees
        // one byte per glyph).
        const float whole = x_at(len);
        int visible = len;
        bool ellipsis = false;
        if (whole > content_room) {
            const float dots = cp_advance('.') * 3.0f;
            const float budget = std::max(0.0f, content_room - dots);
            float acc = 0.0f;
            visible = 0;
            for (int i = 0; i < len; ++i) {
                const float adv = char_advance(i);
                if (acc + adv > budget) {
                    break;
                }
                acc += adv;
                visible = i + 1;
            }
            ellipsis = true;
        }
        if (theme.font != nullptr) {
            if (!ellipsis) {
                r2d::draw_text(content_x, label_top(rect.y, rect.height, theme),
                               buf, *theme.font, dim(theme.text, dim_by));
            } else {
                char scratch[256];
                const int keep =
                    std::min(visible, static_cast<int>(sizeof(scratch)) - 4);
                std::memcpy(scratch, buf, static_cast<std::size_t>(keep));
                scratch[keep] = '.';
                scratch[keep + 1] = '.';
                scratch[keep + 2] = '.';
                scratch[keep + 3] = '\0';
                r2d::draw_text(content_x, label_top(rect.y, rect.height, theme),
                               scratch, *theme.font, dim(theme.text, dim_by));
            }
        } else {
            const float w = ellipsis ? std::min(whole, content_room) : whole;
            r2d::draw_quad(content_x, mark_y, w, mark_h,
                           dim(theme.text, dim_by));
        }
    } else {
        // The placeholder: the label dimmed, saying what the field
        // wants - only an empty buffer shows it.
        char scratch[128];
        const LabelFit fit =
            fit_text(label, content_room, theme, scratch, sizeof(scratch));
        if (fit.advance > 0.0f) {
            const std::uint32_t ph =
                dim(dim(theme.text, theme.disabled_alpha), dim_by);
            if (theme.font != nullptr) {
                r2d::draw_text(content_x, label_top(rect.y, rect.height, theme),
                               fit.text, *theme.font, ph);
            } else {
                r2d::draw_quad(content_x, mark_y, fit.advance, mark_h, ph);
            }
        }
    }

    // The caret: a 2px bar in the text color - solid on arrival and
    // through work, half on/half off at rest, clamped to the row's room
    // when the line runs past it (v1 has no horizontal scroll; the
    // caret rides the edge while the keys still edit the hidden tail).
    if (focused && !dead && live && edit.blink < 0.5f) {
        const float cx =
            content_x + std::min(x_at(edit.caret), content_room);
        r2d::draw_quad(cx, mark_y, 2.0f, mark_h, theme.text);
    }
    return changed;
}

// --- M21: the table --------------------------------------------------------

bool table(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
           const char* label, const TableColumn* columns, int column_count,
           const char* const* cells, int row_count,
           int* selected, float width) {
    // Fill-width, like button: a table wants its container's line.
    if (width < 0.0f) {
        width = layout.remaining_width();
    }
    width = std::max(width, 0.0f);  // a negative width would draw a mirrored quad

    // No columns, no grid: a bare slot that pretends nothing, the empty
    // combo's rule - a table with nothing to lay never latches looks.
    if (column_count <= 0 || columns == nullptr) {
        (void)layout.next(width, theme.control_height);
        return false;
    }
    // Null cells = an empty body: the header still shows (the caller's
    // "no results" state is the grid's own chrome), the rows do not.
    if (cells == nullptr) {
        row_count = 0;
    }

    // The geometry: one header row, the 2-px rule (the separator's own
    // look, a hardcoded pixel like the focus ring's 1), then CONTIGUOUS
    // rows - a grid, not a spaced list. The whole table is one slot in
    // the parent's flow; the rows never touch the cursor.
    const float row_h = theme.control_height;
    const float body_h = static_cast<float>(std::max(row_count, 0)) * row_h;
    const Rect rect = layout.next(width, row_h + 2.0f + body_h);

    // The width policy, resolved once: the fixed columns come off the
    // table's width first, the stretch columns (width <= 0) share what
    // remains equally, clamped at zero - never a backwards cell.
    int stretch_count = 0;
    float fixed_sum = 0.0f;
    for (int j = 0; j < column_count; ++j) {
        if (columns[static_cast<std::size_t>(j)].width > 0.0f) {
            fixed_sum += columns[static_cast<std::size_t>(j)].width;
        } else {
            ++stretch_count;
        }
    }
    const float stretch_w =
        stretch_count > 0
            ? std::max(0.0f, (rect.width - fixed_sum) /
                                 static_cast<float>(stretch_count))
            : 0.0f;

    // One cell-draw shared by the header and every row: the fitted text
    // at the column's x, one label_inset clear of each side - the same
    // fitted pass every widget's label takes, so a cell wider than its
    // column truncates with "..." instead of spilling into the next.
    const auto cell_text = [&](const char* cell, float x, float cell_w,
                               float y, std::uint32_t color) {
        if (cell == nullptr || cell_w <= 0.0f) {
            return;
        }
        char scratch[128];
        const LabelFit fit =
            fit_text(cell, std::max(0.0f, cell_w - 2.0f * theme.label_inset),
                     theme, scratch, sizeof(scratch));
        if (fit.advance <= 0.0f) {
            return;
        }
        const float cx = x + theme.label_inset;
        if (theme.font != nullptr) {
            r2d::draw_text(cx, label_top(y, row_h, theme), fit.text,
                           *theme.font, color);
        } else {
            const float bar_h = row_h * 0.5f;
            r2d::draw_quad(cx, y + (row_h - bar_h) * 0.5f, fit.advance,
                           bar_h, color);
        }
    };
    // The column walk's one step: the policy width clamped to what the
    // table has left of itself, so a fixed set that sums past the width
    // clamps the CELLS to the table's own right edge - the walk reads
    // the clamp, so every column past the edge lays at it and draws
    // nothing, never outside the rect.
    const auto column_width = [&](float x, float policy) {
        return std::min(policy, std::max(0.0f, rect.x + rect.width - x));
    };

    // The disabled scope dims the whole table as one - the header is
    // chrome, but a locked grid reads dead from its top. One factor for
    // every non-face color; the rows' faces come from their own state().
    const float dim_by = gui.disabled() ? theme.disabled_alpha : 1.0f;

    // The header: a control face bar, its labels in the cell geometry,
    // then the rule that anchors the grid. Chrome - no id, no
    // hit-test, nothing focusable (a header click claims nothing;
    // sorting is a future seam, not a hidden one).
    r2d::draw_rounded_quad(rect.x, rect.y, rect.width, row_h,
                           theme.corner_radius,
                           gui.disabled() ? theme.control_disabled
                                          : theme.control_bg);
    {
        float x = rect.x;
        for (int j = 0; j < column_count; ++j) {
            const TableColumn& col = columns[static_cast<std::size_t>(j)];
            const float cw =
                column_width(x, col.width > 0.0f ? col.width : stretch_w);
            cell_text(col.label, x, cw, rect.y, dim(theme.text, dim_by));
            x += cw;
        }
    }
    r2d::draw_quad(rect.x, rect.y + row_h, rect.width, 2.0f,
                   dim(theme.border, dim_by));

    // One row's cells in the column walk - the same draw the header's
    // labels take, at the row's own y.
    const auto draw_cells = [&](const char* const* row_cells, float y) {
        float x = rect.x;
        for (int j = 0; j < column_count; ++j) {
            const TableColumn& col = columns[static_cast<std::size_t>(j)];
            const float cw =
                column_width(x, col.width > 0.0f ? col.width : stretch_w);
            cell_text(row_cells[static_cast<std::size_t>(j)], x, cw, y,
                      dim(theme.text, dim_by));
            x += cw;
        }
    };

    // The rows. Interactive ones (a non-null selected) are citizens -
    // selectable's exact look and answer; a null selected lays the same
    // grid as pure chrome, no ids at all (the statics' "cannot interact"
    // spelled by the pointer).
    bool changed = false;
    const float body_top = rect.y + row_h + 2.0f;
    {
        // The rows id under the table's label, then by index - the
        // combo's item discipline, so the same grid works anywhere and
        // two tables differ by label like any widget.
        ScopedId table_scope(gui.ids(), label);
        const auto row_id = [&](int index) {
            ScopedId row_scope(gui.ids(), index);
            return gui.ids().id("row");
        };
        for (int i = 0; i < row_count; ++i) {
            const Rect row_rect{rect.x,
                                body_top + static_cast<float>(i) * row_h,
                                rect.width, row_h};
            const char* const* row_cells =
                cells + static_cast<std::size_t>(i) *
                            static_cast<std::size_t>(column_count);
            if (selected == nullptr) {
                draw_cells(row_cells, row_rect.y);
                continue;
            }

            const WidgetId rid = row_id(i);
            gui.register_focusable(rid);
            const WidgetResult result = gui.state(rid, row_rect);
            const HitTest& hit = result.hit;

            // Up/Down step the focused row, clamped at the grid's ends -
            // the slider's arrow rule (Tab's wrap is the screen's walk,
            // not the grid's). set_focus is immediate, combo's handback
            // idiom: the neighbor's state() still runs this frame, so its
            // ring draws now, and the scroll panel's focus-into-view
            // follows the move for free.
            if (gui.is_focused(rid)) {
                if (gui.input().key_pressed[keys::kUp] && i > 0) {
                    gui.set_focus(row_id(i - 1));
                } else if (gui.input().key_pressed[keys::kDown] &&
                           i + 1 < row_count) {
                    gui.set_focus(row_id(i + 1));
                }
            }

            // Keyboard pick: Enter/Space on the focused row, edge-only -
            // selectable's rule; the selection itself stays the caller's
            // to move.
            const bool key_clicked =
                gui.is_focused(rid) &&
                (gui.input().key_pressed[keys::kEnter] ||
                 gui.input().key_pressed[keys::kSpace]);

            // The focus ring: the on-accent as a 1px outline behind the
            // row - skipped for degenerate rows, like every ring.
            if (gui.is_focused(rid) && row_rect.width > 0.0f &&
                row_rect.height > 0.0f) {
                r2d::draw_rounded_quad(
                    row_rect.x - 1.0f, row_rect.y - 1.0f,
                    row_rect.width + 2.0f, row_rect.height + 2.0f,
                    theme.corner_radius + 1.0f, theme.control_on);
            }

            // The face ladder EXCEPT the plain idle face - the bare list
            // look, selectable's: an unhovered, unselected row is text on
            // the parent's chrome.
            if (result.state == WidgetState::Disabled) {
                r2d::draw_rounded_quad(row_rect.x, row_rect.y, row_rect.width,
                                       row_rect.height, theme.corner_radius,
                                       theme.control_disabled);
            } else if (result.state == WidgetState::Pressed) {
                r2d::draw_rounded_quad(row_rect.x, row_rect.y, row_rect.width,
                                       row_rect.height, theme.corner_radius,
                                       theme.control_pressed);
            } else if (result.state == WidgetState::Hovered) {
                r2d::draw_rounded_quad(row_rect.x, row_rect.y, row_rect.width,
                                       row_rect.height, theme.corner_radius,
                                       theme.control_hovered);
            }

            // The selection marker: selectable's 3-px accent bar down the
            // row's left edge, riding every face, dimmed under the lock.
            // An out-of-range *selected (the caller's own) marks nothing.
            if (*selected == i && row_rect.width > 0.0f &&
                row_rect.height > 0.0f) {
                r2d::draw_quad(row_rect.x, row_rect.y, 3.0f, row_rect.height,
                               dim(theme.control_on, dim_by));
            }

            // The pick: a click or the focused row's key writes the
            // caller's int and answers ONLY on a real move - the radio's
            // idempotence (a re-pick on the current row is a no-op).
            if ((hit.clicked || key_clicked) && *selected != i) {
                *selected = i;
                changed = true;
            }
            draw_cells(row_cells, row_rect.y);
        }
    }
    return changed;
}

}  // namespace ws::ui::gui
