// The desktop half of the platform seam (see src/platform/window.h): GLFW owns the
// window, this TU owns GLFW. The only place GLFW's C callback surface
// exists; each callback builds the matching ws::ui::Event and hands it to
// the shell's single forwarder; the same callbacks also tap raw input into
// the gui library's InputFrame (WindowEventSink::input) - a second,
// deliberately unfiltered consumer, since the gui library hit-tests for
// itself (see <ws/ui/input.hpp>).
//
// ImGui's GLFW backend installs its own callbacks over ours and chains to
// ours at the top of each of its own, BEFORE its io updates - so when our
// callback runs, io does not yet reflect the current event. io.WantCapture*
// still answers correctly because those flags were recomputed at the last
// NewFrame (see imgui_wants() in src/app/app.cpp). WindowSize and WindowClose
// are ours alone - the backend installs neither.
//
// The cursor callbacks also own InputFrame::mouse_present - the gui's
// "is there a mouse at all": a move, a button or the cursor entering sets
// it (and samples the position, so a first click lands where the user
// sees it, not at (0,0)); the cursor leaving clears it, and the gui's
// pointer list drops the mouse that frame.

#include "platform/window.h"

#include <ws/core/log.hpp>

#include <imgui_impl_glfw.h>

#define GLFW_INCLUDE_NONE
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

// The shared tail of every "the mouse was seen" tap: scales a screen-space
// position (GLFW's coordinates) into the gui's framebuffer pixels (r2d
// screen mode) - the ratio is 1:1 whenever the two match and drifts under
// DPI scaling; a degenerate window size keeps the last position rather
// than dividing by zero - and marks the mouse present.
void tap_cursor(GLFWwindow* window, gui::InputFrame* input, double x, double y) {
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
    input->mouse_present = true;
}

void on_mouse_button(GLFWwindow* window, int button, int action, int mods) {
    (void)mods;
    if (action == GLFW_PRESS) {
        emit(window, MouseButtonPressedEvent{button});
    } else {
        emit(window, MouseButtonReleasedEvent{button});
    }
    if (gui::InputFrame* input = input_for(window)) {
        // The click can be the device's first appearance (no move since the
        // window opened): sample, so the press lands at the real cursor
        // and not at the (0,0) default.
        double x = 0.0;
        double y = 0.0;
        glfwGetCursorPos(window, &x, &y);
        tap_cursor(window, input, x, y);
        if (button >= 0 && button < gui::InputFrame::kMaxMouseButtons) {
            const bool down = (action == GLFW_PRESS);
            input->mouse_down[button] = down;
            if (down) {
                input->mouse_pressed[button] = true;
            } else {
                input->mouse_released[button] = true;
            }
        }
    }
}

void on_cursor_pos(GLFWwindow* window, double x, double y) {
    emit(window, MouseMovedEvent{static_cast<float>(x), static_cast<float>(y)});
    if (gui::InputFrame* input = input_for(window)) {
        tap_cursor(window, input, x, y);
    }
}

// The cursor crossing the window edge. Entering is the device being seen -
// sample and mark present, exactly like a move (a press can beat the first
// move callback). Leaving unplugs the mouse: the gui's pointer list drops
// it that frame, hover dies with it, and a press still held evaporates -
// the release, whenever it lands, reports nowhere.
void on_cursor_enter(GLFWwindow* window, int entered) {
    if (gui::InputFrame* input = input_for(window)) {
        if (entered != GLFW_FALSE) {
            double x = 0.0;
            double y = 0.0;
            glfwGetCursorPos(window, &x, &y);
            tap_cursor(window, input, x, y);
        } else {
            input->mouse_present = false;
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

// Glyphs are input too: one TextInputEvent per codepoint (UTF-32, arrival
// order - the event counterpart of the gui frame's text_input), plus the
// gui frame's own copy. The ImGui backend installs its own char callback
// and chains to ours first, so its text widgets keep working unchanged.
void on_char(GLFWwindow* window, unsigned int codepoint) {
    emit(window, TextInputEvent{codepoint});
    if (gui::InputFrame* input = input_for(window)) {
        input->text_input.push_back(codepoint);
    }
}

// Process-wide, not window-scoped: installed before glfwInit() (by the
// host, via install_glfw_error_callback()) so init failures get logged.
void glfw_error(int code, const char* description) {
    core::error("ui: GLFW error {}: {}", code, description);
}

// Installs the window/input callbacks on `window`; every one of them builds
// the matching ws::ui::Event and forwards it to sink->forward (and taps the
// gui input snapshot through sink->input). The callbacks themselves are
// file-local - deliberately never declared in any header. Called before
// ImGui_ImplGlfw_InitForVulkan(window, true): the backend then chains ours
// from the top of its own callbacks, before its io updates.
void install_window_callbacks(GLFWwindow* window, WindowEventSink* sink) {
    glfwSetWindowUserPointer(window, sink);
    glfwSetWindowCloseCallback(window, on_window_close);
    glfwSetWindowSizeCallback(window, on_window_size);
    glfwSetKeyCallback(window, on_key);
    glfwSetMouseButtonCallback(window, on_mouse_button);
    glfwSetCursorPosCallback(window, on_cursor_pos);
    glfwSetCursorEnterCallback(window, on_cursor_enter);
    glfwSetScrollCallback(window, on_scroll);
    glfwSetCharCallback(window, on_char);
}

// Installs the process-wide GLFW error callback (file-local, logging
// through ws::core). Must run before glfwInit() so that init failures are
// logged too.
void install_glfw_error_callback() {
    glfwSetErrorCallback(&glfw_error);
}

}  // namespace

namespace {

// WindowHost on GLFW: init opens the window, and "window ready" is a
// startup fact that stays true for the whole run - the desktop teardown is
// once, in order, in the destructor.
class GlfwHost final : public WindowHost {
public:
    ~GlfwHost() override {
        if (window_ != nullptr) {
            glfwDestroyWindow(window_);
            window_ = nullptr;
        }
        if (glfw_initialized_) {
            glfwTerminate();
            glfw_initialized_ = false;
        }
    }

    bool init(WindowEventSink& sink, const AppConfig& config) override {
        if (config.width < 1 || config.height < 1) {
            core::error("ui: invalid initial window size {}x{}", config.width,
                        config.height);
            return false;
        }
        install_glfw_error_callback();  // before glfwInit: log init failures too
        if (!glfwInit()) {
            core::error("ui: glfwInit failed");
            return false;
        }
        glfw_initialized_ = true;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window_ = glfwCreateWindow(config.width, config.height, config.title.c_str(),
                                  nullptr, nullptr);
        if (window_ == nullptr) {
            core::error("ui: failed to create window \"{}\"", config.title);
            return false;
        }
        sink_ = &sink;
        install_window_callbacks(window_, sink_);
        return true;
    }

    std::vector<const char*> instance_extensions() const override {
        uint32_t count = 0;
        const char** extensions = glfwGetRequiredInstanceExtensions(&count);
        if (extensions == nullptr) {
            core::error("ui: GLFW reports no Vulkan WSI extensions");
            return {};
        }
        return std::vector<const char*>(extensions, extensions + count);
    }

    bool create_surface(VkInstance instance, VkSurfaceKHR* out) const override {
        return glfwCreateWindowSurface(instance, window_, nullptr, out) == VK_SUCCESS;
    }

    void pump() override { glfwPollEvents(); }

    void wait_for_window() override {
        // Unreachable: window_ready() never goes false on desktop (the
        // shell never calls this without that check).
    }

    bool should_close() const override {
        return glfwWindowShouldClose(window_) == GLFW_TRUE;
    }

    void request_stop() override { glfwSetWindowShouldClose(window_, GLFW_TRUE); }

    bool window_ready() const override { return window_ != nullptr; }

    bool is_hidden() const override {
        return glfwGetWindowAttrib(window_, GLFW_ICONIFIED) == GLFW_TRUE;
    }

    void framebuffer_size(int* width, int* height) const override {
        glfwGetFramebufferSize(window_, width, height);
    }

    float ui_scale() const override {
        // The content scale GLFW recommends for this window - it can
        // change when the window moves between monitors, so this is a
        // per-call query, not an init-time fact. 1.0 at 100% scaling.
        float xs = 1.0f;
        float ys = 1.0f;
        if (window_ != nullptr) {
            glfwGetWindowContentScale(window_, &xs, &ys);
        }
        return xs > 0.0f ? xs : 1.0f;
    }

    bool imgui_platform_init() override {
        return ImGui_ImplGlfw_InitForVulkan(window_, true);
    }

    void imgui_platform_new_frame() override { ImGui_ImplGlfw_NewFrame(); }

    void imgui_platform_shutdown() override { ImGui_ImplGlfw_Shutdown(); }

    // The gui clipboard seam, wired to this window: glfwGetClipboardString
    // returns platform-owned memory valid until the next clipboard call
    // (the seam's exact contract), and the pair captures the host, so it
    // dies with the window - before the app's GuiContext can be gone
    // while the pair still points here, the teardown order guarantees it
    // (the subclass's members, the context among them, die first).
    void install_clipboard(gui::GuiContext& gui) override {
        gui.set_clipboard(
            [this] { return glfwGetClipboardString(window_); },
            [this](const char* text) { glfwSetClipboardString(window_, text); });
    }

private:
    GLFWwindow* window_ = nullptr;
    WindowEventSink* sink_ = nullptr;
    bool glfw_initialized_ = false;
};

}  // namespace

std::unique_ptr<WindowHost> make_window_host() {
    return std::make_unique<GlfwHost>();
}

}  // namespace ws::ui
