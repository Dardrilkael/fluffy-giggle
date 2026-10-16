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
    ws::input::PointerId thumb = ws::input::kNoPointer;  // the grabbing pointer, while held
    float thumb_grab = 0.0f;  // the grab's offset within the thumb
};

// A scroll region's per-contact drag bookkeeping: a track exists from the
// contact's press inside the viewport until it lifts. While it exists and
// !dragging the press is still the CHILD's (a tap in progress, watched
// against the threshold); dragging means the scroll owns the finger - the
// child's press was cancelled and the offset follows the contact 1:1.
struct ScrollDragTrack {
    ws::input::PointerId id = ws::input::kNoPointer;
    float press_y = 0.0f;  // the threshold is measured from the press
    float last_y = 0.0f;  // the previous frame's y, for the drag delta
    bool dragging = false;
};

// One per scroll panel (the persistent side-table discipline): a track per
// live contact, so two fingers can drag one region and one lifting never
// confuses the survivor's bookkeeping.
struct ScrollDragState {
    std::array<ScrollDragTrack, ws::input::InputFrame::kMaxTouches> tracks{};
};

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

// The typing filter: v1 edits ASCII - one byte, one glyph, the bake's
// whole range. The test takes the CODEPOINT, never a char: cp 300 cast
// to the byte first would alias into ','. The paste path's
// char->unsigned-char widening is exact, so it shares this one filter.
inline bool is_insertable(std::uint32_t cp) {
    return cp >= 32u && cp <= 126u;
}

// text_input's per-frame lens: the persistent caret pair from
// TextInputState plus the caller's buffer geometry as one object, so
// every edit below speaks a verb instead of spelling memmove. Built
// once per frame with the length measured fresh; the frame-end clamp
// writes the pair back into the persistent state. A dead field carries
// room 0, and the verbs' own clamps turn that into a no-op.
struct EditBuffer {
    char* buf = nullptr;  // the caller's buffer (the widget never owns it)
    int room = 0;   // bytes the buffer may hold: capacity minus the NUL
    int len = 0;    // bytes before the terminator, measured this frame
    int caret = 0;   // the editing position; the selection's live end
    int anchor = 0;  // the selection origin; caret == anchor: none

    // The selection as a range, low end first (equal ends: none).
    int sel_lo() const { return std::min(caret, anchor); }
    int sel_hi() const { return std::max(caret, anchor); }
    bool has_selection() const { return caret != anchor; }

    // Both ends onto `at`: the selection dies, the caret stays.
    void collapse(int at) {
        caret = anchor = at;
    }

    // The one eraser every deleter funnels through: [lo, hi) leaves,
    // the tail slides, the terminator follows the new end.
    void erase(int lo, int hi) {
        lo = std::max(0, lo);
        hi = std::min(len, hi);
        if (lo >= hi) {
            return;
        }
        std::memmove(buf + lo, buf + hi, static_cast<std::size_t>(len - hi));
        len -= hi - lo;
        buf[len] = '\0';
    }

    // A selection dies under what replaces it - typing and pasting both
    // spell it; the caret lands where the selection was.
    void erase_selection() {
        if (has_selection()) {
            const int lo = sel_lo();
            erase(lo, sel_hi());
            collapse(lo);
        }
    }

    // n bytes at `at` (clamped into [0, len], never past the room): the
    // tail slides once, the bytes land, the terminator follows. The
    // return is the pen just past the insertion, or -1 when the room
    // refused everything.
    int insert(int at, const char* bytes, int n) {
        at = std::max(0, std::min(len, at));
        n = std::min(n, room - len);
        if (n <= 0) {
            return -1;
        }
        std::memmove(buf + at + n, buf + at,
                     static_cast<std::size_t>(len - at));
        std::memcpy(buf + at, bytes, static_cast<std::size_t>(n));
        len += n;
        buf[len] = '\0';
        return at + n;
    }

    // A paste through the filter: only insertable bytes count, and only
    // as many as the room takes - counted in one pass over the text,
    // then landed in one move, so the buffer never walks per byte and
    // the capacity never splits mid-paste. The return is insert's pen.
    int insert_filtered(int at, const char* text) {
        int take = 0;
        for (const char* p = text; *p != '\0' && len + take < room; ++p) {
            if (is_insertable(static_cast<unsigned char>(*p))) {
                ++take;
            }
        }
        at = std::max(0, std::min(len, at));
        if (take <= 0) {
            return -1;
        }
        std::memmove(buf + at + take, buf + at,
                     static_cast<std::size_t>(len - at));
        int pen = at;
        for (const char* p = text; *p != '\0'; ++p) {
            if (pen - at >= take) {
                break;  // the room's count is landed; the tail never walks
            }
            if (is_insertable(static_cast<unsigned char>(*p))) {
                buf[pen++] = *p;
            }
        }
        len += take;
        buf[len] = '\0';
        return pen;
    }
};

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

// The focus ring: the on-accent as a 1px outline behind the row, so Tab
// navigation is legible. Skipped for degenerate rects - a zero-width
// face would show the ring with nothing in it.
inline void focus_ring(const GuiContext& gui, WidgetId id, const Rect& rect,
                       const ChromeTheme& theme) {
    if (gui.is_focused(id) && rect.width > 0.0f && rect.height > 0.0f) {
        r2d::draw_rounded_quad(rect.x - 1.0f, rect.y - 1.0f,
                               rect.width + 2.0f, rect.height + 2.0f,
                               theme.corner_radius + 1.0f, theme.control_on);
    }
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

// One row's whole citizenship ladder, in the order the frame contract
// requires: the fill-width resolve against the cursor, the id from the
// label (one seam - it keys the hit-test AND any persistent state), the
// placed rect, the focusable registration BEFORE state() (a press's
// focus claim reads the registration), then state()'s one hit-test.
// Every interactive row widget walks these steps in this order; one
// that begins here cannot get it wrong.
struct Row {
    WidgetId id;
    Rect rect;
    WidgetResult result;
};

// The ladder as one call. `width` keeps LayoutCursor's own contract:
// negative fills the rest of the line; the auto-sized rows (checkbox,
// radio, toggle) pass their computed width, which the resolve leaves
// untouched.
inline Row begin_row(GuiContext& gui, LayoutCursor& layout,
                     const ChromeTheme& theme, const char* label,
                     float width) {
    resolve_width(width, layout);
    const WidgetId id = gui.ids().id(label);
    const Rect rect = layout.next(width, theme.control_height);
    gui.register_focusable(id);
    return {id, rect, gui.state(id, rect)};
}

}  // namespace ws::ui::gui
