// Internal to ws::ui - never visible to apps or other libs. This is the one
// translation unit allowed to touch GLFW's C callback surface; the shell
// (src/app.cpp) consumes events through WindowEventSink only.
#pragma once

#include <functional>

#include <ws/ui/event.hpp>
#include <ws/ui/gui/input_frame.hpp>

struct GLFWwindow;  // opaque GLFW handle; GLFW types never reach headers

namespace ws::ui {

// Every GLFW callback funnels its Event through this single sink. The shell
// owns the sink (stable address, installed via the window user pointer,
// which the ImGui GLFW backend does not use - it keeps its state in
// io.BackendPlatformUserData instead).
//
// `input` is the parallel gui-library tap: the same raw callbacks
// accumulate one ws::ui::gui::InputFrame per frame (src/window.cpp),
// independent of the event stream - so the gui snapshot stays unfiltered
// even while ImGui (or the app's on_event) consumes the same physical
// input. The gui library hit-tests for itself. Null until the shell wires
// it; every callback null-checks.
struct WindowEventSink {
    std::function<void(Event&)> forward;
    gui::InputFrame* input = nullptr;
};

// Installs the window/input callbacks on `window`; every one of them builds
// the matching ws::ui::Event and forwards it to sink->forward (and taps the
// gui input snapshot through sink->input). The callbacks themselves are
// file-local in window.cpp - deliberately never declared in any header.
// Call before ImGui_ImplGlfw_InitForVulkan(window, true): the backend then
// chains ours from the top of its own callbacks, before its io updates.
void install_window_callbacks(GLFWwindow* window, WindowEventSink* sink);

// Installs the process-wide GLFW error callback (file-local in window.cpp,
// logging through ws::core). Must run before glfwInit() so that init
// failures are logged too.
void install_glfw_error_callback();

}  // namespace ws::ui
