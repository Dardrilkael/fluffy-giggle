#pragma once

// ws::gestures - the multi-touch arbiter, extracted ONCE so every app
// shares it. This library owns the answer to ONE question:
//
//     Given this frame's raw touch contacts, what gesture is in flight
//     and what should the view do about it?
//
// The problem it exists to solve (the Android reality): a finger is
// NOT a mouse. A first finger touching down to start a pinch looks
// EXACTLY like a click to a mouse-shaped canvas - it presses, it is
// inside the panel, it lifts within a tap window. Only when the SECOND
// finger lands does "this was a pinch all along" become knowable. So a
// touch-capable canvas must DEFER the first finger's press until either
//   * the finger lifts within the tap window, unmoved (a tap: then the
//     deferred click lands), or
//   * the finger moves past slop (a drag: the deferred press arms the
//     drag at the offset position), or
//   * a second finger lands (a pinch: the deferred click is cancelled
//     forever - the first finger of a zoom was never a click), or
//   * the OS cancels the contact (a palm, a system takeover).
//
// The arbiter answers that as a pure per-frame function: feed it the
// InputFrame (ws::input - already the platform-carried multi-touch
// table with stable per-contact ids), read back a tiny ActionList:
//
//     GestureState state();          // Idle | Touching | Drag | Pinch
//     actions.zoom(factor, anchor)   // pinch span ratio, both-finger mid
//     actions.pan(dx, dy)            // two-finger mid drift, or a
//                                    // one-finger pan after slop
//     actions.tap(x, y)              // the deferred click, once, at lift
//     actions.drag(...)              // one-finger drag after slop
//     actions.cancel()               // every contact died without a
//                                    // verdict
//
// No rendering, no ui, no app types - ws::input + std only. The editor
// canvas consumes it (the Schematic panel's pointer handler), the
// ui CanvasView policy and playground demos can later ride the same
// arbiter instead of their own private pinch math (their code is the
// duplication this lib retires).
//
// Determinism is a contract: the same frame sequence answers the same
// actions, every time - the clock rides in on the frame's own
// dt_seconds (never a wall-clock read inside), so a test (or a replay)
// scripts time itself.

#include <ws/input/input.hpp>

#include <cstdint>
#include <vector>

namespace ws::gestures {

// --- the policy ----------------------------------------------------------------

// The tunable half of the contract, spelled as plain data so an app can
// shape it (a map app wants a smaller slop than a schematic editor).
// Defaults are the Android view-system numbers (scaled to framebuffer
// px, not dp - the InputFrame's own space; an app with a dp scale
// multiplies on the way in).
struct GesturePolicy {
    // How far a finger may wander before a touch stops being a tap
    // candidate and becomes a drag, in px. Android's
    // ViewConfiguration touch slop is ~8dp; 24 px is the same intent at
    // a 3x scale without pretending to read dp here.
    float touch_slop_px = 24.0f;
    // How long a stationary finger may stay down and still lift as a
    // tap, seconds. Beyond it the gesture is a long-press (and the
    // deferred click is spent - a long-press is its own gesture, not a
    // late click). Android's tap timeout is ~200ms with a long-press
    // around 500ms; 0.35s splits the difference for a canvas.
    double tap_window_s = 0.35;
    // The span ratio below which a pinch is ignored as jitter (fingers
    // tremble): a fresh pinch anchors only after the span moves at least
    // this much, so the first zoom step is not a random 1.01.
    double pinch_anchor_ratio = 0.02;
    // Whether ONE finger, after slop, pans the view (a map-style
    // surface) or drags canvas content (an editor: the panel decides).
    // The arbiter still REPORTS the drag either way; this only gates
    // the pan action for one-finger moves.
    bool one_finger_pan = false;
};

// --- the actions ----------------------------------------------------------------

// What one frame's arbiter pass wants done to the view. Values are
// screen px in the InputFrame's own space (top-left origin) - the
// consumer maps them through its own camera.
struct ZoomAction {
    double factor = 1.0;      // multiplicative zoom step for this frame
    float anchor_x = 0.0f;    // the pinch's finger-mid, screen px
    float anchor_y = 0.0f;
};
struct PanAction {
    float dx = 0.0f;          // screen px the view should move this frame
    float dy = 0.0f;
};
struct TapAction {
    float x = 0.0f;          // the tap's landing point, screen px
    float y = 0.0f;
};
struct DragAction {
    // One-finger drag after slop: the contact's id (for the app's own
    // drag bookkeeping), the CURRENT position, where the finger was
    // PRESSED (the drag's subject is whatever sat under the press - a
    // mouse drag's own rule: press on the entity, the follow starts at
    // the crossing), and where the slop crossing happened.
    ws::input::PointerId pointer{};
    float x = 0.0f;
    float y = 0.0f;
    float press_x = 0.0f;   // the contact's press position
    float press_y = 0.0f;
    float from_x = 0.0f;    // the slop-crossing position
    float from_y = 0.0f;
};

// The gesture's phase, one entry per frame's verdict.
enum class GestureState : std::uint8_t {
    Idle,      // no contacts; any previous gesture is fully resolved
    Touching,  // contacts down, none past slop yet: NOTHING may click
    Drag,      // one contact past slop: a one-finger drag
    Pinch,     // two+ contacts: pinch-zoom + two-finger pan
    LongPress, // one stationary contact held past the tap window
};

// One frame's whole answer. At most one of each action kind per frame;
// a pinch frame may carry BOTH a zoom and a pan (the span change and
// the mid drift); a drag frame carries a drag (and a pan when the
// policy's one_finger_pan says the surface scrolls instead).
struct GestureActions {
    bool has_zoom = false;
    ZoomAction zoom;
    bool has_pan = false;
    PanAction pan;
    bool has_tap = false;
    TapAction tap;
    bool has_drag = false;
    DragAction drag;
    bool long_press = false;  // the tap window expired this frame
    bool cancelled = false;   // the OS took every contact away
};

// --- the arbiter ------------------------------------------------------------------

// THE state machine. One instance per interactive surface (a canvas, a
// map, a plot). Call once per frame with the frame's InputFrame BEFORE
// the surface's own mouse handling, then:
//
//   * while state() != Idle the surface MUST NOT treat the bridged
//     mouse path as a click source (the InputFrame mirrors the first
//     touch as mouse button 0 - the bridge that makes single-finger
//     taps work in mouse-shaped code - and that mirror is exactly what
//     would turn the first finger of a pinch into a click);
//   * the tap action is the ONLY click the arbiter will ever emit, and
//     it arrives at the finger's LIFT frame, never at its press;
//   * zoom/pan actions are incremental per-frame steps (apply them
//     directly to the camera; the arbiter never needs the camera back).
//
// Feed order is the shell's contract: next_frame() on the InputFrame
// happens once per frame at the pump top (ws::input's rule); the
// arbiter's own pass() reads that frame as-is and keeps its own
// inter-frame memory (positions, span anchor, slop state) - so the
// arbiter is exactly as deterministic as the frame stream.
class TouchArbiter {
public:
    GesturePolicy policy;

    TouchArbiter() = default;

    // One frame's verdict. Pure w.r.t. the world: reads only the frame,
    // writes only its own memory. The InputFrame's touch table carries
    // released contacts through their release frame (ws::input's rule),
    // so the lift's edges are readable the same frame they happen.
    GestureActions pass(const ws::input::InputFrame& frame);

    // The current phase (last pass's verdict).
    [[nodiscard]] GestureState state() const { return state_; }

    // True while the arbiter holds contacts it has not resolved - the
    // surface should suppress its own mouse-click reading of the bridged
    // pointer (the mouse mirror of a touch-in-flight is not a click).
    [[nodiscard]] bool holding() const { return state_ != GestureState::Idle; }

    // Hard reset (a surface losing focus, a tool switch mid-gesture):
    // forgets contacts, answers Idle next pass. Idempotent.
    void reset();

    // The contacts the arbiter is tracking (diagnostics, tests): the
    // live count as of the last pass.
    [[nodiscard]] int contact_count() const { return static_cast<int>(contacts_.size()); }

private:
    struct Contact {
        ws::input::PointerId id{};
        // The press position (slop measures from here).
        float start_x = 0.0f;
        float start_y = 0.0f;
        // Current position.
        float x = 0.0f;
        float y = 0.0f;
        // Accumulated hold time, seconds (frame dt of every frame the
        // contact stays down - the clock rides in on the frame).
        double held_s = 0.0;
        bool past_slop = false;
        // This contact was part of a multi-contact gesture (a pinch):
        // its lift can never resolve to a tap - the gesture spent the
        // deferred-click verdict the moment the second finger landed.
        // (The one-finger path keeps its own tap verdicts; this flag
        // only marks contacts that rode a pinch.)
        bool pinch_member = false;
        // The one-finger pan's between-frame reference: where the finger
        // was when the arbiter last reported it (the pan step each
        // drag frame is x - pan_ref). Updated only on drag frames.
        float pan_ref_x = 0.0f;
        float pan_ref_y = 0.0f;
        // The contact lifted this frame (its tap verdict may pend).
        bool lifted = false;
        // The OS cancelled this contact (never a click).
        bool cancelled = false;
    };

    // The pinch anchor: the span (px) and finger-mid the last pinch
    // frame measured; the next pinch frame's zoom factor is
    // span_now / span_then, and its pan is mid_now - mid_then. Zero
    // span = no anchor yet (a fresh pinch establishes it first).
    double anchor_span_px_ = 0.0;
    float anchor_mid_x_ = 0.0f;
    float anchor_mid_y_ = 0.0f;

    GestureState state_ = GestureState::Idle;
    std::vector<Contact> contacts_;
};

}  // namespace ws::gestures
