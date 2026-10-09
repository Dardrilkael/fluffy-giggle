// InputFrame's out-of-line half: the touch table the platform callbacks
// fill (window_glfw/window_android call the touch_* writers with stable
// per-contact ids), next_frame()'s advance, and the primary-touch ->
// mouse bridge the writers maintain. Pure CPU, no platform headers - the
// tests drive it headless.

#include <ws/ui/gui/input_frame.hpp>

#include <algorithm>
#include <array>

namespace ws::ui::gui {
namespace {

// --- InputFrame touch table ---------------------------------------------------

// Scan the touch table by contact id - the platform keys every touch_* call
// by id, never by slot (ids are stable per contact; slots shuffle as
// earlier fingers lift). -1 when the id is not in the table. Clamped to
// kMaxTouches so a stray touch_count cannot walk out of the array.
int find_touch(const InputFrame& frame, PointerId id) {
    const int count = std::min(frame.touch_count, InputFrame::kMaxTouches);
    for (int i = 0; i < count; ++i) {
        if (frame.touches[static_cast<std::size_t>(i)].id == id) {
            return i;
        }
    }
    return -1;
}

}  // namespace

// --- InputFrame: one frame's snapshot (the shell fills, next_frame advances) ----

void InputFrame::next_frame() {
    mouse_pressed.fill(false);
    mouse_released.fill(false);
    scroll_dx = 0.0f;
    scroll_dy = 0.0f;
    key_pressed.fill(false);
    text_input.clear();
    // Touch edges clear and lifted contacts leave the table now - their
    // release frame is over. The survivors keep their arrival order (that
    // order is the stable pointer order across frames).
    int kept = 0;
    const int count = std::min(touch_count, kMaxTouches);
    for (int i = 0; i < count; ++i) {
        if (!touches[static_cast<std::size_t>(i)].down) {
            continue;  // lifted last frame: dropped
        }
        TouchPoint& point = touches[static_cast<std::size_t>(kept)];
        point = touches[static_cast<std::size_t>(i)];
        point.pressed = false;
        point.released = false;
        point.cancelled = false;
        ++kept;
    }
    touch_count = kept;
    // Self-heal: a bridge whose contact is already gone (only reachable by
    // direct table surgery - touch_up clears it first) stops mirroring.
    if (bridged_touch_id_ != kNoPointer &&
        find_touch(*this, bridged_touch_id_) < 0) {
        bridged_touch_id_ = kNoPointer;
    }
}

int InputFrame::touch_down(PointerId id, float x, float y) {
    int slot = find_touch(*this, id);
    if (slot < 0) {
        if (touch_count >= kMaxTouches) {
            return -1;  // the cap contract: the caller drops + warns once
        }
        slot = touch_count++;
        TouchPoint& fresh = touches[static_cast<std::size_t>(slot)];
        fresh.id = id;
        fresh.released = false;   // a slot above the compacted count still
        fresh.cancelled = false;  // holds a dropped contact's bytes - a new
                                  // contact must not inherit its edges
    }
    TouchPoint& point = touches[static_cast<std::size_t>(slot)];
    point.x = x;
    point.y = y;
    point.down = true;
    point.pressed = true;
    // released/cancelled are deliberately NOT written on a found slot: a
    // same-frame id reuse (a finger lifts and the platform hands its id to
    // a new contact before next_frame() - Android recycles low ids fast)
    // must not destroy the lift's edges. The slot then reads
    // pressed+released with down - up-then-down, the flags the gui needs
    // to report the old lifetime's release while the new contact claims
    // fresh. A same-frame tap is down-then-up (pressed+released, down
    // false) and stays exactly that; next_frame() clears every edge with
    // the rest. The position, though, is the new contact's - one frame
    // snapshots last-write position but order-exact edges, the same rule
    // the mouse fields below live by (the bridge never cleared
    // mouse_released here either).
    // The primary-touch -> mouse bridge: the first contact of a gesture
    // mirrors the mouse so single-finger use and ImGui keep working
    // unchanged; later contacts only track.
    // The primary-touch -> mouse bridge: the first contact of a gesture
    // mirrors the mouse so single-finger use and ImGui keep working
    // unchanged; later contacts only track.
    if (bridged_touch_id_ == kNoPointer) {
        bridged_touch_id_ = id;
    }
    if (bridged_touch_id_ == id) {
        mouse_x = x;
        mouse_y = y;
        mouse_down[0] = true;
        mouse_pressed[0] = true;
    }
    return slot;
}

void InputFrame::touch_move(PointerId id, float x, float y) {
    const int slot = find_touch(*this, id);
    if (slot < 0) {
        return;  // unknown id: no-op (never a reported contact)
    }
    TouchPoint& point = touches[static_cast<std::size_t>(slot)];
    point.x = x;
    point.y = y;
    if (bridged_touch_id_ == id) {
        mouse_x = x;  // the bridge follows its contact's position
        mouse_y = y;
    }
}

void InputFrame::touch_up(PointerId id, float x, float y, bool cancelled) {
    const int slot = find_touch(*this, id);
    if (slot < 0) {
        return;
    }
    TouchPoint& point = touches[static_cast<std::size_t>(slot)];
    point.x = x;
    point.y = y;
    point.down = false;
    point.released = true;
    point.cancelled = cancelled;
    if (bridged_touch_id_ == id) {
        // Cancellation mirrors the lift but must never produce a click.
        mouse_x = x;
        mouse_y = y;
        mouse_down[0] = false;
        mouse_released[0] = true;
        bridged_touch_id_ = kNoPointer;  // no promotion: the mouse stays
                                         // released until a new gesture
    }
}

int InputFrame::gather_pointers(
    std::array<PointerView, kMaxPointers>& out) const {
    out.fill(PointerView{});
    // The mouse is a pointer only while the platform says one is there;
    // absent, the list is exactly the touches (a touch-only device) and
    // nothing reports hover from mouse_x/y.
    int count = 0;
    if (mouse_present) {
        PointerView& mouse = out[0];
        mouse.id = kMousePointerId;
        mouse.x = mouse_x;
        mouse.y = mouse_y;
        mouse.down = mouse_down[0];
        mouse.pressed = mouse_pressed[0];
        mouse.released = mouse_released[0];
        count = 1;
    }
    const int touch_total = std::min(touch_count, kMaxTouches);
    for (int i = 0; i < touch_total; ++i) {
        const TouchPoint& point = touches[static_cast<std::size_t>(i)];
        PointerView& view = out[static_cast<std::size_t>(count)];
        view.id = point.id;
        view.x = point.x;
        view.y = point.y;
        view.down = point.down;
        view.pressed = point.pressed;
        view.released = point.released;
        ++count;
    }
    return count;
}

bool InputFrame::pointer_pos(PointerId id, float* x, float* y) const {
    if (id == kMousePointerId) {
        if (!mouse_present) {
            return false;  // not a pointer this frame
        }
        if (x != nullptr) {
            *x = mouse_x;
        }
        if (y != nullptr) {
            *y = mouse_y;
        }
        return true;
    }
    const int slot = find_touch(*this, id);
    if (slot < 0) {
        return false;
    }
    const TouchPoint& point = touches[static_cast<std::size_t>(slot)];
    if (x != nullptr) {
        *x = point.x;
    }
    if (y != nullptr) {
        *y = point.y;
    }
    return true;
}

}  // namespace ws::ui::gui
