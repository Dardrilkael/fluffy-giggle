// ws::gestures - the arbiter implementation. One file, one state
// machine, no dependencies past ws::input. Everything here is pure
// arithmetic over the frame's touch table; the only memory is the
// Contact vector and the pinch anchor (both reset() clears).

#include <ws/gestures/arbiter.hpp>

#include <algorithm>
#include <cmath>

namespace ws::gestures {

namespace {

constexpr double kEps = 1e-9;

}  // namespace

GestureActions TouchArbiter::pass(const ws::input::InputFrame& frame) {
    GestureActions out;

    // --- 1. fold the frame's touch table into the contact book -------
    // New presses append; live contacts update position and accumulate
    // hold time; lifts mark lifted/cancelled. The table keeps released
    // contacts through this frame (ws::input's rule), so press, move
    // and lift edges are all readable right here, in order.
    for (int i = 0; i < frame.touch_count && i < ws::input::InputFrame::kMaxTouches; ++i) {
        const ws::input::TouchPoint& t = frame.touches[i];
        if (t.id == ws::input::kNoPointer) {
            continue;
        }
        Contact* c = nullptr;
        for (Contact& cand : contacts_) {
            if (cand.id == t.id) {
                c = &cand;
                break;
            }
        }
        if (c == nullptr) {
            // A fresh press. (The platform's same-frame id reuse keeps
            // the lift's edges in ONE slot - ws::input's documented
            // carry - so a fresh id here is genuinely fresh.)
            Contact fresh;
            fresh.id = t.id;
            fresh.start_x = t.x;
            fresh.start_y = t.y;
            fresh.x = t.x;
            fresh.y = t.y;
            fresh.pan_ref_x = t.x;
            fresh.pan_ref_y = t.y;
            if (t.cancelled) {
                // A contact that arrives already cancelled (the platform
                // reported press and cancel in one frame): never a
                // gesture participant.
                fresh.cancelled = true;
                fresh.lifted = true;
            }
            contacts_.push_back(fresh);
            continue;
        }
        // The existing contact's update.
        c->x = t.x;
        c->y = t.y;
        if (t.cancelled) {
            c->cancelled = true;
        }
        if (t.released) {
            c->lifted = true;
            // A lifted contact keeps its last position; a genuinely
            // fresh press on the same id next frame is a new Contact
            // (the erasure below drops lifted ones at pass end).
        }
    }

    // Hold time: every live (not lifted) contact ages by the frame's dt.
    for (Contact& c : contacts_) {
        if (!c.lifted) {
            c.held_s += frame.dt_seconds;
            // Slop: measured from the press point, Euclidean, once.
            const double dx = static_cast<double>(c.x) - c.start_x;
            const double dy = static_cast<double>(c.y) - c.start_y;
            if (!c.past_slop &&
                std::hypot(dx, dy) >
                    static_cast<double>(policy.touch_slop_px)) {
                c.past_slop = true;
            }
        }
    }

    // --- 2. the verdict ------------------------------------------------
    // Partitions, cheapest first:
    //   * every contact cancelled -> Cancelled/Idle (never a click);
    //   * any contact past slop while 2+ live -> Pinch (drag folds in);
    //   * 2+ live contacts -> Pinch (even inside slop: a second finger
    //     landing is the pinch's own start - the deferred click dies);
    //   * one live contact past slop -> Drag;
    //   * one live stationary contact past the tap window -> LongPress;
    //   * one live stationary contact inside the window -> Touching;
    //   * the LAST contact lifted this frame, never past slop, never
    //     cancelled, inside the window -> the TAP (the deferred click
    //     lands at lift, not at press);
    //   * no live contacts, gesture unresolved -> Idle.
    const auto live = [&](const Contact& c) { return !c.lifted; };
    const int live_count = static_cast<int>(
        std::count_if(contacts_.begin(), contacts_.end(), live));

    // Pinch math lives here ONCE: the two oldest live contacts (the
    // press order the table carries) define the span and the mid.
    const Contact* finger_a = nullptr;
    const Contact* finger_b = nullptr;
    for (const Contact& c : contacts_) {
        if (!live(c) || c.cancelled) {
            continue;
        }
        if (finger_a == nullptr) {
            finger_a = &c;
        } else if (finger_b == nullptr) {
            finger_b = &c;
            break;  // a third finger joins the same pinch (the two
                    // oldest are the gesture; extra fingers are noise)
        }
    }

    if (live_count == 0) {
        // Every contact is gone: the gesture resolves or was aborted.
        const bool any_cancel = std::any_of(
            contacts_.begin(), contacts_.end(),
            [](const Contact& c) { return c.cancelled; });
        // The tap: exactly one contact, lifted this frame, never past
        // slop, never cancelled, held within the window. A multi-contact
        // gesture lifting is never a tap (a pinch that ended), and a
        // slop-crossing drag lifting is a drag END, not a click.
        if (contacts_.size() == 1u) {
            const Contact& c = contacts_.front();
            if (c.lifted && !c.cancelled && !c.past_slop &&
                !c.pinch_member &&
                c.held_s <= policy.tap_window_s) {
                out.has_tap = true;
                out.tap.x = c.x;
                out.tap.y = c.y;
                state_ = GestureState::Idle;
                anchor_span_px_ = 0.0;
                contacts_.clear();
                return out;
            }
        }
        // A cancelled gesture: nothing clicks, nothing drags.
        out.cancelled = any_cancel;
        state_ = GestureState::Idle;
        anchor_span_px_ = 0.0;
        contacts_.clear();
        return out;
    }

    // The pinch: two live contacts (press-order). The FIRST pinch frame
    // only anchors (span now, mid now - no zoom, no pan); later frames
    // answer span_now/anchor_span as the zoom factor and the mid drift
    // as the pan. A fresh pinch (the anchor was spent by a resolution)
    // re-anchors the same way, so the first step of every pinch is
    // jitter-free by construction.
    if (finger_a != nullptr && finger_b != nullptr) {
        state_ = GestureState::Pinch;
        // Every live contact rode this pinch: none of them may ever
        // resolve to a tap, whichever lifts first or last (the exact
        // leak the "first finger of a pinch is never a click" contract
        // guards - a lone survivor lifting after its partner is still
        // the pinch's end, not a fresh tap).
        for (Contact& c : contacts_) {
            if (!c.lifted) {
                c.pinch_member = true;
            }
        }
        const double span = std::hypot(
            static_cast<double>(finger_b->x) - finger_a->x,
            static_cast<double>(finger_b->y) - finger_a->y);
        const float mid_x = (finger_a->x + finger_b->x) * 0.5f;
        const float mid_y = (finger_a->y + finger_b->y) * 0.5f;

        // A past-slop single contact that becomes a pinch: its deferred
        // drag resolves into the pinch (the fingers move together now -
        // the drag action is dead, the click was never born).
        if (anchor_span_px_ <= kEps) {
            // The anchor frame: the span and mid the next frame's steps
            // measure against. No action on the anchor frame itself -
            // the first step of every pinch is jitter-free by
            // construction (there is nothing to step from yet).
            anchor_span_px_ = span;
            anchor_mid_x_ = mid_x;
            anchor_mid_y_ = mid_y;
        } else {
            // The two actions are INDEPENDENT per frame: the span's
            // ratio gates the zoom (a tremble under the ratio is not a
            // zoom), the mid's drift pans (a pure translation - span
            // unchanged - is a pan the frame must answer, not a zoom's
            // passenger). Both measure against the last frame's anchor
            // and both re-anchor every frame, so a series of small
            // steps compounds exactly (the same per-frame compounding
            // contract the wheel's accumulated factor carries in
            // CanvasView::feed_scroll).
            const double ratio =
                anchor_span_px_ > kEps ? span / anchor_span_px_ : 1.0;
            if (std::fabs(ratio - 1.0) >= policy.pinch_anchor_ratio) {
                out.has_zoom = true;
                out.zoom.factor = ratio;
                out.zoom.anchor_x = mid_x;
                out.zoom.anchor_y = mid_y;
            }
            const float dx = mid_x - anchor_mid_x_;
            const float dy = mid_y - anchor_mid_y_;
            if (std::fabs(dx) > 1e-3f || std::fabs(dy) > 1e-3f) {
                out.has_pan = true;
                out.pan.dx = dx;
                out.pan.dy = dy;
            }
            anchor_span_px_ = span;
            anchor_mid_x_ = mid_x;
            anchor_mid_y_ = mid_y;
        }
        // Lifted contacts in a pinch frame: their folds are gone with
        // the gesture (a pinch never resolves into a click or a drag).
        // Drop them from the book now so the next pass's partitions
        // see only the live.
        contacts_.erase(
            std::remove_if(contacts_.begin(), contacts_.end(),
                           [](const Contact& c) { return c.lifted; }),
            contacts_.end());
        return out;
    }

    // One live contact (the only other shape with a contact alive).
    // (Three+ live contacts with only one non-cancelled: the cancelled
    // ones fold away below; the survivor is a one-finger gesture.)
    Contact* sole = nullptr;
    for (Contact& c : contacts_) {
        if (live(c)) {
            sole = &c;
            break;
        }
    }
    // Drop lifted contacts (their verdicts resolved or died above).
    contacts_.erase(
        std::remove_if(contacts_.begin(), contacts_.end(),
                       [](const Contact& c) { return c.lifted; }),
        contacts_.end());
    if (sole == nullptr) {
        state_ = GestureState::Idle;
        anchor_span_px_ = 0.0;
        return out;
    }

    // A pinch survivor (its partner lifted, this finger still down):
    // the gesture is the pinch's own taper - not a fresh anything. It
    // sits frozen (no drag arms, no tap can ever land) until it lifts
    // or a new finger re-pairs. Without this the pinch's own finger
    // motion (past slop from the pinch's spread) would arm a drag with
    // the gesture's leftovers - the mid-pinch tool-arming leak, the
    // same class the pinch_member tap gate closes.
    if (sole->pinch_member) {
        state_ = GestureState::Touching;
        return out;
    }

    if (sole->past_slop) {
        state_ = GestureState::Drag;
        out.has_drag = true;
        out.drag.pointer = sole->id;
        out.drag.x = sole->x;
        out.drag.y = sole->y;
        // The slop crossing point: where the drag ARMED. Kept stable
        // across frames (the press + slop offset), so the consumer's
        // grab offset is honest from the first drag frame on.
        const double dx = static_cast<double>(sole->x) - sole->start_x;
        const double dy = static_cast<double>(sole->y) - sole->start_y;
        const double len = std::max(std::hypot(dx, dy), 1e-9);
        const double k =
            static_cast<double>(policy.touch_slop_px) / len;
        out.drag.from_x = static_cast<float>(
            sole->start_x + dx * k);
        out.drag.from_y = static_cast<float>(
            sole->start_y + dy * k);
        out.drag.press_x = sole->start_x;
        out.drag.press_y = sole->start_y;
        if (policy.one_finger_pan) {
            // A map-style surface: the drag IS the pan. The step is
            // this finger's move since the last drag frame the arbiter
            // reported (pan_ref_ - its own memory; a frame with no move
            // event answers no pan, and the frame's coalesced position
            // is where the finger IS, not where it WAS).
            const float pan_dx = sole->x - sole->pan_ref_x;
            const float pan_dy = sole->y - sole->pan_ref_y;
            if (pan_dx != 0.0f || pan_dy != 0.0f) {
                out.has_pan = true;
                out.pan.dx = pan_dx;
                out.pan.dy = pan_dy;
            }
        }
        sole->pan_ref_x = sole->x;
        sole->pan_ref_y = sole->y;
        return out;
    }

    if (sole->held_s > policy.tap_window_s) {
        // Stationary, held past the window: a long-press. Announce once
        // (the crossing frame), then stay quiet - the consumer's own
        // state machine handles what a long-press means.
        if (state_ != GestureState::LongPress) {
            out.long_press = true;
        }
        state_ = GestureState::LongPress;
        return out;
    }

    state_ = GestureState::Touching;
    return out;
}

void TouchArbiter::reset() {
    contacts_.clear();
    anchor_span_px_ = 0.0;
    state_ = GestureState::Idle;
}

}  // namespace ws::gestures
