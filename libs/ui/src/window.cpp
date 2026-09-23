// The only place GLFW's C callback surface exists. Each callback builds the
// matching ws::ui::Event and hands it to the shell's single forwarder.
//
// ImGui's GLFW backend chains to every callback it also installs (cursor
// pos / mouse button / scroll / key): it runs first and calls ours after
// updating its own io state, so the shell can consult io.WantCapture* when
// the event arrives. WindowSize and WindowClose are ours alone - the
// backend installs neither.

#include "window.h"

#include <GLFW/glfw3.h>

namespace ws::ui {
namespace {

WindowEventSink* sink_for(GLFWwindow* window) {
    return static_cast<WindowEventSink*>(glfwGetWindowUserPointer(window));
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
// framebuffer-size query when this arrives.
void on_window_size(GLFWwindow* window, int width, int height) {
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
}

void on_mouse_button(GLFWwindow* window, int button, int action, int mods) {
    (void)mods;
    if (action == GLFW_PRESS) {
        emit(window, MouseButtonPressedEvent{button});
    } else {
        emit(window, MouseButtonReleasedEvent{button});
    }
}

void on_cursor_pos(GLFWwindow* window, double x, double y) {
    emit(window, MouseMovedEvent{static_cast<float>(x), static_cast<float>(y)});
}

void on_scroll(GLFWwindow* window, double x, double y) {
    emit(window, MouseScrolledEvent{static_cast<float>(x), static_cast<float>(y)});
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
}

}  // namespace ws::ui
