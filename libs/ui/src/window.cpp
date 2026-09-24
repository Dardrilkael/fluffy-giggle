// The only place GLFW's C callback surface exists. Each callback builds the
// matching ws::ui::Event and hands it to the shell's single forwarder; the
// same callbacks also tap raw input into the gui library's InputFrame
// (WindowEventSink::input) - a second, deliberately unfiltered consumer,
// since the gui library hit-tests for itself (see <ws/ui/gui/input_frame.hpp>).
//
// ImGui's GLFW backend installs its own callbacks over ours and chains to
// ours at the top of each of its own, BEFORE its io updates - so when our
// callback runs, io does not yet reflect the current event. io.WantCapture*
// still answers correctly because those flags were recomputed at the last
// NewFrame (see imgui_wants() in src/app.cpp). WindowSize and WindowClose
// are ours alone - the backend installs neither.

#include "window.h"

#include <ws/core/log.hpp>

#include <GLFW/glfw3.h>

namespace ws::ui {
namespace {

WindowEventSink* sink_for(GLFWwindow* window) {
    return static_cast<WindowEventSink*>(glfwGetWindowUserPointer(window));
}

// The gui input tap for this window, when the shell installed one.
gui::InputFrame* input_for(GLFWwindow* window) {
    WindowEventSink* sink = sink_for(window);
    return (sink != nullptr && sink->input != nullptr) ? sink->input : nullptr;
}

// Forwards `event` unless the window has no sink (or a broken one). Events
// are built here on the callback's stack; the shell copies what it keeps.
template <typename E>
void emit(GLFWwindow* window, E&& event) {
    if (WindowEventSink* sink = sink_for(window); sink && sink->forward) {
        sink->forward(event);
    }
}

void on_window_close(GLFWwindow* window) {
    // GLFW already sets the close flag before invoking this callback; set it
    // again so the intent is explicit and the loop cannot miss it.
    glfwSetWindowShouldClose(window, GLFW_TRUE);
    emit(window, WindowCloseEvent{});
}

// Size in screen coordinates; the shell recreates the swapchain off its own
// framebuffer-size query when this arrives. A zero size means minimized (at
// least on Windows): an app can size nothing against it, and the restore
// that follows reports the real size - so it is not delivered.
void on_window_size(GLFWwindow* window, int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    emit(window, WindowResizeEvent{width, height});
}

void on_key(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)scancode;
    (void)mods;
    switch (action) {
        case GLFW_PRESS:
            emit(window, KeyPressedEvent{key, false});
            break;
        case GLFW_RELEASE:
            emit(window, KeyReleasedEvent{key});
            break;
        case GLFW_REPEAT:
            emit(window, KeyPressedEvent{key, true});
            break;
        default:
            break;
    }
    // gui tap: held state on every transition, pressed edge on PRESS only -
    // key repeat is a widget policy, not an input fact, so the edge stays
    // pure. Out-of-range codes (GLFW_KEY_UNKNOWN) drop.
    if (gui::InputFrame* input = input_for(window);
        input != nullptr && key >= 0 && key < gui::InputFrame::kMaxKeys) {
        input->key_down[key] = (action != GLFW_RELEASE);
        if (action == GLFW_PRESS) {
            input->key_pressed[key] = true;
        }
    }
}

void on_mouse_button(GLFWwindow* window, int button, int action, int mods) {
    (void)mods;
    if (action == GLFW_PRESS) {
        emit(window, MouseButtonPressedEvent{button});
    } else {
        emit(window, MouseButtonReleasedEvent{button});
    }
    if (gui::InputFrame* input = input_for(window);
        input != nullptr && button >= 0 &&
        button < gui::InputFrame::kMaxMouseButtons) {
        const bool down = (action == GLFW_PRESS);
        input->mouse_down[button] = down;
        if (down) {
            input->mouse_pressed[button] = true;
        } else {
            input->mouse_released[button] = true;
        }
    }
}

void on_cursor_pos(GLFWwindow* window, double x, double y) {
    emit(window, MouseMovedEvent{static_cast<float>(x), static_cast<float>(y)});
    if (gui::InputFrame* input = input_for(window)) {
        // GLFW reports screen coordinates; gui space is framebuffer pixels
        // (r2d screen mode). The ratio is 1:1 whenever the two match and
        // drifts under DPI scaling. A degenerate window size keeps the
        // last position rather than dividing by zero.
        int fb_width = 0;
        int fb_height = 0;
        int win_width = 0;
        int win_height = 0;
        glfwGetFramebufferSize(window, &fb_width, &fb_height);
        glfwGetWindowSize(window, &win_width, &win_height);
        if (win_width > 0 && win_height > 0) {
            input->mouse_x =
                static_cast<float>(x) *
                (static_cast<float>(fb_width) / static_cast<float>(win_width));
            input->mouse_y =
                static_cast<float>(y) *
                (static_cast<float>(fb_height) / static_cast<float>(win_height));
        }
    }
}

void on_scroll(GLFWwindow* window, double x, double y) {
    emit(window, MouseScrolledEvent{static_cast<float>(x), static_cast<float>(y)});
    if (gui::InputFrame* input = input_for(window)) {
        // Deltas, not positions - sum, or ticks arriving between frames
        // would be lost (same rule as the event path's coalescing).
        input->scroll_dx += static_cast<float>(x);
        input->scroll_dy += static_cast<float>(y);
    }
}

// Text input has no Event counterpart (the event system models keys, not
// glyphs), so codepoints go straight into the gui frame. The ImGui backend
// installs its own char callback and chains to ours first, so its text
// widgets keep working unchanged.
void on_char(GLFWwindow* window, unsigned int codepoint) {
    if (gui::InputFrame* input = input_for(window)) {
        input->text_input.push_back(codepoint);
    }
}

// Process-wide, not window-scoped: installed before glfwInit() (by the
// shell, via install_glfw_error_callback()) so init failures get logged.
void glfw_error(int code, const char* description) {
    core::error("ui: GLFW error {}: {}", code, description);
}

}  // namespace

void install_window_callbacks(GLFWwindow* window, WindowEventSink* sink) {
    glfwSetWindowUserPointer(window, sink);
    glfwSetWindowCloseCallback(window, on_window_close);
    glfwSetWindowSizeCallback(window, on_window_size);
    glfwSetKeyCallback(window, on_key);
    glfwSetMouseButtonCallback(window, on_mouse_button);
    glfwSetCursorPosCallback(window, on_cursor_pos);
    glfwSetScrollCallback(window, on_scroll);
    glfwSetCharCallback(window, on_char);
}

void install_glfw_error_callback() {
    glfwSetErrorCallback(&glfw_error);
}

}  // namespace ws::ui
