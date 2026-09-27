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

// Pointer-id sentinels. Platform touch ids are small per-contact ints and
// can never collide with these two; wherever pointers are listed, the
// mouse always ids as kMousePointerId and "no pointer" is kNoPointer.
inline constexpr std::uint32_t kMousePointerId = 0xFFFFFFFEu;
inline constexpr std::uint32_t kNoPointer = 0xFFFFFFFFu;

// One touch contact in the frame's table. Entries stay through their
// release frame (down=false, released=true) so a widget can still see who
// lifted; next_frame() drops them after.
struct TouchPoint {
    std::uint32_t id = kNoPointer;  // platform contact id, stable while held
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
    std::uint32_t id = kNoPointer;
    float x = 0.0f;
    float y = 0.0f;
    bool down = false;
    bool pressed = false;
    bool released = false;
};

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
    // All three also maintain the primary-touch -> mouse bridge (below).
    //
    // Returns the entry's slot, or -1 when the table is full (the caller
    // drops the contact and warns once). One down per contact is the
    // platform contract; a repeat updates in place instead of duplicating.
    int touch_down(std::uint32_t id, float x, float y);
    // Unknown ids are a no-op (the platform never moves an unreported
    // contact).
    void touch_move(std::uint32_t id, float x, float y);
    // `cancelled` marks an OS-aborted gesture (palm, system takeover) -
    // the release edge still fires.
    void touch_up(std::uint32_t id, float x, float y, bool cancelled);
    // The contact currently mirrored as the mouse, kNoPointer when none.
    // Platform layers emit the bridge's Mouse* events off this.
    std::uint32_t bridged_touch() const { return bridged_touch_id_; }

    // --- held state ------------------------------------------------------------
    float mouse_x = 0.0f;  // framebuffer pixels, top-left origin
    float mouse_y = 0.0f;
    std::array<bool, kMaxMouseButtons> mouse_down{};
    std::array<bool, kMaxKeys> key_down{};

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
    // The frame's pointers as one flat list: [0] is always the mouse (id
    // kMousePointerId), the touches follow in table order. A touch exists
    // while down or in its release frame - no hover without contact. The
    // return value is the entry count; slots past it read as kNoPointer.
    int gather_pointers(std::array<PointerView, kMaxPointers>& out) const;
    // Pointer `id`'s position (the mouse or any touch); false when no such
    // pointer is in this frame. The out params are optional (pure lookup).
    bool pointer_pos(std::uint32_t id, float* x, float* y) const;

private:
    // The primary-touch -> mouse bridge: the FIRST contact of a gesture
    // mirrors the mouse (position, button 0 and its edges) so single-finger
    // use, the gui's mouse path and ImGui keep working unchanged. No
    // promotion - when it lifts, the mouse stays released until a new
    // gesture's first contact bridges. touch_up clears it; next_frame()
    // self-heals if its contact is already gone.
    std::uint32_t bridged_touch_id_ = kNoPointer;
};

// The shell's snapshot for the frame being rendered, valid during
// on_update() - hand it to GuiContext::new_frame(). Filled by the platform
// layer from RAW GLFW input (src/window.cpp): deliberately not the
// app-facing event stream, so the snapshot stays unfiltered even while
// ImGui consumes the same physical input - the gui library hit-tests for
// itself. Outside a running app: an empty frame (the drop-safe contract,
// like r2d::frame_size()).
const InputFrame& frame_input();

}  // namespace ws::ui::gui
