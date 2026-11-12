// The input family's out-of-line half: InputFrame's touch table (the
// ws::ui platform callbacks fill it - window_glfw/window_android call the
// touch_* writers with stable per-contact ids), next_frame()'s advance,
// the primary-touch -> mouse bridge the writers maintain, and the
// frame_input() glue (set_active_input points the getter at the shell's
// live accumulator while ws::ui's App::run() is active). Pure CPU, no
// platform headers - the tests drive it headless.

#include <ws/input/input.hpp>

#include <algorithm>
#include <array>

namespace ws::input {
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

// --- the IME composition channel's writers (G1.2) -----------------------------

namespace {

// The shared spelling of begin/update: adopt the preedit string (a null
// reads as empty), clamp the advisory caret span into it, order the ends.
void preedit_spell(Preedit& preedit, const char* utf8, int lo, int hi) {
    preedit.active = true;
    preedit.text = utf8 != nullptr ? utf8 : "";
    const int len = static_cast<int>(preedit.text.size());
    lo = std::clamp(lo, 0, len);
    hi = std::clamp(hi, 0, len);
    if (lo > hi) {
        std::swap(lo, hi);
    }
    preedit.caret_lo = lo;
    preedit.caret_hi = hi;
}

}  // namespace

void InputFrame::composition_begin(const char* utf8, int caret_lo,
                                   int caret_hi) {
    preedit_spell(preedit, utf8, caret_lo, caret_hi);
}

void InputFrame::composition_update(const char* utf8, int caret_lo,
                                    int caret_hi) {
    // Begin's own semantics by design: an update with no begin stands as
    // one (the header contract - platforms differ on the first report).
    preedit_spell(preedit, utf8, caret_lo, caret_hi);
}

void InputFrame::composition_end(const char* committed_utf8) {
    // The held half clears NOW - the underline is gone the very frame the
    // IME let go, not one frame later - and the commit becomes the edge
    // the gui consumes this frame. Empty text is the cancel spelling.
    preedit.active = false;
    preedit.text.clear();
    preedit.caret_lo = 0;
    preedit.caret_hi = 0;
    preedit.commit = committed_utf8 != nullptr ? committed_utf8 : "";
}

void InputFrame::next_frame() {
    mouse_pressed.fill(false);
    mouse_released.fill(false);
    scroll_dx = 0.0f;
    scroll_dy = 0.0f;
    key_pressed.fill(false);
    key_repeated.fill(false);
    text_input.clear();
    // The composition's EDGE half clears with the rest; the held half
    // (text, caret span, active) deliberately survives - see Preedit's
    // own doc block. An end() is what clears the held half, at the
    // moment the IME actually let go.
    preedit.commit.clear();
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

// --- frame_input: the shell's accumulator (set_active_input installs) --------

namespace {

// Process-wide input state (the shell's tap, the only global this lib
// owns). Single-threaded BY DESIGN: the immediate-mode loop runs on the
// shell's main thread only, so these are plain globals on purpose - not a
// data race waiting to be "fixed" with atomics.

// The shell's accumulator while ws::ui's App::run() is active; null before
// startup and after shutdown.
InputFrame* g_active_input = nullptr;

// The answer frame_input() gives without a running app - a valid empty
// frame, so callers never see a dangling or partially filled snapshot.
const InputFrame& empty_input() {
    static const InputFrame empty;
    return empty;
}

}  // namespace

void set_active_input(InputFrame* input) {
    g_active_input = input;
}

const InputFrame& frame_input() {
    return g_active_input != nullptr ? *g_active_input : empty_input();
}

}  // namespace ws::input
