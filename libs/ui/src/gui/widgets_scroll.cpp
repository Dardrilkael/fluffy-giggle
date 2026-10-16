// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the scrolling panel - begin/end_scroll_panel with the wheel claim,
// the touch flow-drag and the grabbable thumb (the ScrollState and
// drag-track structs live in widgets_detail.hpp, keyed by the reserved
// internal ids this pair re-derives at each end).

#include "widgets_detail.hpp"

#include <algorithm>
#include <cmath>

namespace ws::ui::gui {

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
    const ws::input::InputFrame& input = gui.input();
    const int touch_count =
        std::min(input.touch_count, ws::input::InputFrame::kMaxTouches);
    // Sweep first: a track whose contact left the frame without a lift (a
    // platform contract violation) frees its slot - the same id-liveness
    // rule the context's own pointer tracks follow.
    for (ScrollDragTrack& track : drag.tracks) {
        if (track.id == ws::input::kNoPointer) {
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
        const ws::input::TouchPoint& point = input.touches[static_cast<std::size_t>(i)];
        ScrollDragTrack* track = drag_track_for(drag, point.id);
        if (track == nullptr) {
            continue;  // unreachable: the sweep keeps a slot free
        }
        if (track->id == ws::input::kNoPointer) {
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
        const ws::input::InputFrame& input = gui.input();
        const auto grab = [&](ws::input::PointerId pointer, float py) {
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
                grab(ws::input::kMousePointerId, input.mouse_y);
            }
            const int touch_count =
                std::min(input.touch_count, ws::input::InputFrame::kMaxTouches);
            for (int i = 0; i < touch_count; ++i) {
                const ws::input::TouchPoint& point =
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
        if (scroll.thumb != ws::input::kNoPointer) {
            const ws::input::PointerView* view = gui.pointer(scroll.thumb);
            if (view == nullptr || !view->down) {
                scroll.thumb = ws::input::kNoPointer;
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
