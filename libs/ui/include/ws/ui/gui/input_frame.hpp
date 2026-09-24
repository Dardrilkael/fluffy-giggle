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
// codes ws::ui events already carry.
struct InputFrame {
    // GLFW mouse buttons run 0..7.
    static constexpr int kMaxMouseButtons = 8;
    // Covers GLFW_KEY_LAST (348) with headroom.
    static constexpr int kMaxKeys = 400;

    // Advances to the next frame: clears the per-frame edges (pressed/
    // released, scroll, key presses, text input) and keeps held state and
    // position. The shell calls this once per frame at the top of the
    // pump, before this frame's input starts accumulating.
    void next_frame();

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
