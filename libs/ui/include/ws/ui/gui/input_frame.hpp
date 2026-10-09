#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace ws::ui::gui {

// One frame's input as a snapshot. The platform layer (the shell's GLFW
// callbacks) accumulates raw events between frames and publishes exactly
// one InputFrame per frame; widgets read only the snapshot, never live
// callbacks - so every widget in a frame sees identical input, and a
// frame's decisions cannot change mid-frame.
//
// Mouse coordinates are framebuffer pixels with the origin top-left (the
// space of r2d screen mode and Rect), not GLFW screen coordinates - the
// platform layer scales while filling. Button and key codes are the GLFW
// codes ws::ui events already carry. Touch contacts arrive through the
// touch_* methods, keyed by the platform's per-contact id (Android's
// multi-touch path; desktop GLFW synthesizes none).
//
// The mouse exists for the gui only while mouse_present (below): the
// platform sets the flag when it sees the device - cursor move, button,
// cursor enter - and clears it when the cursor leaves the window. A
// touch-only device therefore lists touches only, so mouse_x/y sitting
// at (0,0) (or mirroring a lifted finger) can never hover anything.

// Pointer identity, a strong struct on purpose: a texture id, a body
// slot or a widget index can never be passed where a pointer goes (nor
// a pointer into those APIs). It wraps the platform's per-contact id
// (Android's small ints) plus the two sentinels below.
struct PointerId {
    std::uint32_t value;

    bool operator==(const PointerId&) const = default;
};

// Pointer-id sentinels. Platform touch ids are small per-contact ints and
// can never collide with these two; wherever pointers are listed, the
// mouse ids as kMousePointerId - when it is listed at all, which is
// InputFrame::mouse_present's call - and "no pointer" is kNoPointer.
inline constexpr PointerId kMousePointerId{0xFFFFFFFEu};
inline constexpr PointerId kNoPointer{0xFFFFFFFFu};

// One touch contact in the frame's table. Entries stay through their
// release frame (down=false, released=true) so a widget can still see who
// lifted; next_frame() drops them after.
struct TouchPoint {
    PointerId id = kNoPointer;  // platform contact id, stable while held
    float x = 0.0f;  // framebuffer px, top-left origin (mouse space)
    float y = 0.0f;
    bool down = false;
    bool pressed = false;    // contact began this frame
    bool released = false;   // contact ended this frame
    bool cancelled = false;  // the OS aborted it (never a click)
};

// One pointer as the gui consumes it: the mouse and the touch contacts
// flattened into one uniform list - widgets never special-case where a
// pointer came from.
struct PointerView {
    PointerId id = kNoPointer;
    float x = 0.0f;
    float y = 0.0f;
    bool down = false;
    bool pressed = false;
    bool released = false;
};

// The GLFW key codes the gui itself gives meaning to: Tab cycles focus,
// Shift+Tab cycles back, Enter and Space activate the focused widget
// (button, checkbox, tab), and the arrows step the focused slider. Apps
// keep using raw GLFW numbers for everything else (the arrays in
// InputFrame are indexed by them); these exist so the gui core and its
// widgets can never disagree about the spelling of the few keys with
// built-in behavior.
namespace keys {
inline constexpr int kTab = 258;          // GLFW_KEY_TAB
inline constexpr int kEnter = 257;        // GLFW_KEY_ENTER
inline constexpr int kSpace = 32;         // GLFW_KEY_SPACE
inline constexpr int kLeftShift = 340;     // GLFW_KEY_LEFT_SHIFT
inline constexpr int kRightShift = 341;    // GLFW_KEY_RIGHT_SHIFT
inline constexpr int kRight = 262;         // GLFW_KEY_RIGHT
inline constexpr int kLeft = 263;          // GLFW_KEY_LEFT
}  // namespace keys

struct InputFrame {
    // GLFW mouse buttons run 0..7.
    static constexpr int kMaxMouseButtons = 8;
    // Covers GLFW_KEY_LAST (348) with headroom.
    static constexpr int kMaxKeys = 400;
    // Simultaneous touch contacts tracked per frame. The 9th+ finger drops
    // at the platform layer (warned once) - the same cap policy as the
    // rest of this table.
    static constexpr int kMaxTouches = 8;
    // The mouse plus every touch: the capacity of the flat pointer list.
    static constexpr int kMaxPointers = 1 + kMaxTouches;

    // Advances to the next frame: clears the per-frame edges (pressed/
    // released, scroll, key presses, text input) and keeps held state and
    // position. Released touches leave the table (order of the survivors
    // preserved); held ones keep their slot. The shell calls this once per
    // frame at the top of the pump, before this frame's input starts
    // accumulating.
    void next_frame();

    // --- touch table -----------------------------------------------------------
    // The platform layer's ONLY writers (besides tests): touch_down appends
    // an entry, touch_move updates one, touch_up marks the release edge -
    // the entry leaves at next_frame(), so the release frame still sees it.
    // All three also maintain the primary-touch -> mouse bridge (below);
    // none of them touches mouse_present - that flag is the platform's own
    // statement about a real mouse, and a finger is not one.
    //
    // Returns the entry's slot, or -1 when the table is full (the caller
    // drops the contact and warns once). One down per contact is the
    // platform contract; a repeat updates in place instead of duplicating.
    // A same-frame id reuse - touch_up then touch_down on one id before
    // next_frame(), which Android's low pointer ids make common - keeps
    // the lift's edges: the slot reads pressed+released with down, the
    // flags the gui needs to report the lift while the new contact claims
    // fresh. The position is the new contact's: one frame snapshots
    // last-write position, order-exact edges (the mouse fields have the
    // same rule).
    int touch_down(PointerId id, float x, float y);
    // Unknown ids are a no-op (the platform never moves an unreported
    // contact).
    void touch_move(PointerId id, float x, float y);
    // `cancelled` marks an OS-aborted gesture (palm, system takeover) -
    // the release edge still fires.
    void touch_up(PointerId id, float x, float y, bool cancelled);
    // The contact currently mirrored as the mouse, kNoPointer when none.
    // Platform layers emit the bridge's Mouse* events off this.
    PointerId bridged_touch() const { return bridged_touch_id_; }

    // --- held state ------------------------------------------------------------
    float mouse_x = 0.0f;  // framebuffer pixels, top-left origin
    float mouse_y = 0.0f;
    // A mouse exists, as far as the gui is concerned. The platform owns
    // this flag: it sets it whenever the device is seen - a cursor move, a
    // button, the cursor entering, each also sampling the position - and
    // clears it when the cursor leaves the window. While false the mouse
    // is not a pointer at all: gather_pointers lists the touches only,
    // pointer_pos(kMousePointerId) answers false and the wheel is dead, so
    // a touch-only device (where these fields mirror the last finger, or
    // sit at (0,0) before any) can never show hover at the top-left.
    // Held state, not an edge - next_frame() keeps it; the touch bridge
    // never writes it.
    bool mouse_present = false;
    std::array<bool, kMaxMouseButtons> mouse_down{};
    std::array<bool, kMaxKeys> key_down{};

    // --- the frame clock (the shell writes it; next_frame keeps it) ------------
    // Seconds of wall time from the previous pump top to this one - the
    // frame's own clock, for anything that must move with real time (a
    // caret's blink). The shell writes it once per iteration, right
    // before this frame's input starts accumulating; a blocked
    // iteration (hidden window, surface loss) reports the whole block -
    // an honest clock, and an app that wants it smooth filters its own
    // way. next_frame() deliberately does NOT clear it (unlike every
    // edge below): the shell's next write is the refresh. A hand-built
    // frame defaults to 0; without a running app frame_input()'s empty
    // snapshot answers 0 too.
    float dt_seconds = 0.0f;

    // --- edges this frame (cleared by next_frame) ------------------------------
    std::array<bool, kMaxMouseButtons> mouse_pressed{};
    std::array<bool, kMaxMouseButtons> mouse_released{};
    float scroll_dx = 0.0f;  // the frame's deltas, summed
    float scroll_dy = 0.0f;
    std::array<bool, kMaxKeys> key_pressed{};  // key went down this frame
    // UTF-32 codepoints typed this frame, in arrival order.
    std::vector<std::uint32_t> text_input;

    // --- touch contacts (the multi-touch table) --------------------------------
    // Live contacts in arrival order; a released-this-frame entry stays
    // until next_frame() drops it. touch_count never exceeds kMaxTouches -
    // touch_down enforces the cap and direct writers must respect it.
    std::array<TouchPoint, kMaxTouches> touches{};
    int touch_count = 0;

    // --- unified pointer view --------------------------------------------------
    // The frame's pointers as one flat list: [0] is the mouse (id
    // kMousePointerId) while mouse_present, the touches follow in table
    // order - a touch-only device lists the touches and nothing else. A
    // touch exists while down or in its release frame - no hover without
    // contact. The return value is the entry count; slots past it read as
    // kNoPointer.
    int gather_pointers(std::array<PointerView, kMaxPointers>& out) const;
    // Pointer `id`'s position (the mouse or any touch); false when no such
    // pointer is in this frame - the absent mouse included. The out params
    // are optional (pure lookup).
    bool pointer_pos(PointerId id, float* x, float* y) const;

private:
    // The primary-touch -> mouse bridge: the FIRST contact of a gesture
    // mirrors the mouse (position, button 0 and its edges) so single-finger
    // use, the gui's mouse path and ImGui keep working unchanged. No
    // promotion - when it lifts, the mouse stays released until a new
    // gesture's first contact bridges. touch_up clears it; next_frame()
    // self-heals if its contact is already gone. The mirror is raw fields
    // only: mouse_present stays the platform's call, so a lifting finger
    // can never unplug a real mouse.
    PointerId bridged_touch_id_ = kNoPointer;
};

// The shell's snapshot for the frame being rendered, valid during
// on_update() - hand it to GuiContext::begin_frame(). Filled by the platform
// layer from RAW input (the GLFW callbacks on desktop, the NativeActivity
// pump on Android): deliberately not the app-facing event stream, so the
// snapshot stays unfiltered even while
// ImGui consumes the same physical input - the gui library hit-tests for
// itself. Outside a running app: an empty frame (the drop-safe contract,
// like r2d::frame_size()).
const InputFrame& frame_input();

}  // namespace ws::ui::gui
