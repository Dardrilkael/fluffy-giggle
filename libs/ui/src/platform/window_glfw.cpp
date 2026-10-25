// The desktop half of the platform seam (see src/platform/window.h): GLFW owns the
// window, this TU owns GLFW. The only place GLFW's C callback surface
// exists; each callback builds the matching ws::input::Event and hands it to
// the shell's single forwarder; the same callbacks also tap raw input into
// the gui library's ws::input::InputFrame (WindowEventSink::input) - a second,
// deliberately unfiltered consumer, since the gui library hit-tests for
// itself (see <ws/input/input.hpp>).
//
// ImGui's GLFW backend installs its own callbacks over ours and chains to
// ours at the top of each of its own, BEFORE its io updates - so when our
// callback runs, io does not yet reflect the current event. io.WantCapture*
// still answers correctly because those flags were recomputed at the last
// NewFrame (see imgui_wants() in src/app/app.cpp). WindowSize and WindowClose
// are ours alone - the backend installs neither.
//
// The cursor callbacks also own ws::input::InputFrame::mouse_present - the gui's
// "is there a mouse at all": a move, a button or the cursor entering sets
// it (and samples the position, so a first click lands where the user
// sees it, not at (0,0)); the cursor leaving clears it, and the gui's
// pointer list drops the mouse that frame.

#include "platform/window.h"

#include <ws/core/log.hpp>

#include <imgui_impl_glfw.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
// The OS dark-mode probe (prefers_dark below) reads the Personalize
// registry key, so this platform TU - never an app - carries the one
// windows.h the seam needs. Lean guard + NOMINMAX: this TU is in every
// default desktop build, and windows.h's macros would collide with std.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace ws::ui {
namespace {

WindowEventSink* sink_for(GLFWwindow* window) {
    return static_cast<WindowEventSink*>(glfwGetWindowUserPointer(window));
}

// The gui input tap for this window, when the shell installed one.
ws::input::InputFrame* input_for(GLFWwindow* window) {
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
    emit(window, ws::input::WindowCloseEvent{});
}

// Size in screen coordinates; the shell recreates the swapchain off its own
// framebuffer-size query when this arrives. A zero size means minimized (at
// least on Windows): an app can size nothing against it, and the restore
// that follows reports the real size - so it is not delivered.
void on_window_size(GLFWwindow* window, int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    emit(window, ws::input::WindowResizeEvent{width, height});
}

void on_key(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)scancode;
    (void)mods;
    switch (action) {
        case GLFW_PRESS:
            emit(window, ws::input::KeyPressedEvent{key, false});
            break;
        case GLFW_RELEASE:
            emit(window, ws::input::KeyReleasedEvent{key});
            break;
        case GLFW_REPEAT:
            emit(window, ws::input::KeyPressedEvent{key, true});
            break;
        default:
            break;
    }
    // gui tap: held state on every transition, pressed edge on PRESS only -
    // key repeat is a widget policy, not an input fact, so the edge stays
    // pure. Out-of-range codes (GLFW_KEY_UNKNOWN) drop.
    if (ws::input::InputFrame* input = input_for(window);
        input != nullptr && key >= 0 && key < ws::input::InputFrame::kMaxKeys) {
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
void tap_cursor(GLFWwindow* window, ws::input::InputFrame* input, double x, double y) {
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
        emit(window, ws::input::MouseButtonPressedEvent{button});
    } else {
        emit(window, ws::input::MouseButtonReleasedEvent{button});
    }
    if (ws::input::InputFrame* input = input_for(window)) {
        // The click can be the device's first appearance (no move since the
        // window opened): sample, so the press lands at the real cursor
        // and not at the (0,0) default.
        double x = 0.0;
        double y = 0.0;
        glfwGetCursorPos(window, &x, &y);
        tap_cursor(window, input, x, y);
        if (button >= 0 && button < ws::input::InputFrame::kMaxMouseButtons) {
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
    emit(window, ws::input::MouseMovedEvent{static_cast<float>(x), static_cast<float>(y)});
    if (ws::input::InputFrame* input = input_for(window)) {
        tap_cursor(window, input, x, y);
    }
}

// The cursor crossing the window edge. Entering is the device being seen -
// sample and mark present, exactly like a move (a press can beat the first
// move callback). Leaving unplugs the mouse: the gui's pointer list drops
// it that frame, hover dies with it, and a press still held evaporates -
// the release, whenever it lands, reports nowhere.
void on_cursor_enter(GLFWwindow* window, int entered) {
    if (ws::input::InputFrame* input = input_for(window)) {
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
    emit(window, ws::input::MouseScrolledEvent{static_cast<float>(x), static_cast<float>(y)});
    if (ws::input::InputFrame* input = input_for(window)) {
        // Deltas, not positions - sum, or ticks arriving between frames
        // would be lost (same rule as the event path's coalescing).
        input->scroll_dx += static_cast<float>(x);
        input->scroll_dy += static_cast<float>(y);
    }
}

// Glyphs are input too: one ws::input::TextInputEvent per codepoint (UTF-32, arrival
// order - the event counterpart of the gui frame's text_input), plus the
// gui frame's own copy. The ImGui backend installs its own char callback
// and chains to ours first, so its text widgets keep working unchanged.
void on_char(GLFWwindow* window, unsigned int codepoint) {
    emit(window, ws::input::TextInputEvent{codepoint});
    if (ws::input::InputFrame* input = input_for(window)) {
        input->text_input.push_back(codepoint);
    }
}

// Files dragged onto the window (stage 12's drag-drop-as-events): one
// ws::input::FileDroppedEvent per drop, never coalesced. GLFW hands the
// paths as utf-8 valid only inside this callback, so they are copied
// into the event before it leaves - the one event type that owns heap,
// human-rare by nature. The OS reports no drop position, so paths are
// the whole payload. The ImGui backend installs no drop callback of
// its own, so ours stands alone (had it one, the install order would
// chain it like every other callback here).
void on_drop(GLFWwindow* window, int count, const char** paths) {
    std::vector<std::string> dropped;
    dropped.reserve(count > 0 ? static_cast<std::size_t>(count) : 0u);
    for (int i = 0; i < count; ++i) {
        if (paths[i] != nullptr) {
            dropped.emplace_back(paths[i]);
        }
    }
    emit(window, ws::input::FileDroppedEvent{std::move(dropped)});
}

// Process-wide, not window-scoped: installed before glfwInit() (by the
// host, via install_glfw_error_callback()) so init failures get logged.
void glfw_error(int code, const char* description) {
    core::error("ui: GLFW error {}: {}", code, description);
}

// Installs the window/input callbacks on `window`; every one of them builds
// the matching ws::input::Event and forwards it to sink->forward (and taps the
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
    glfwSetDropCallback(window, on_drop);
}

// Installs the process-wide GLFW error callback (file-local, logging
// through ws::core). Must run before glfwInit() so that init failures are
// logged too.
void install_glfw_error_callback() {
    glfwSetErrorCallback(&glfw_error);
}

}  // namespace

namespace {

// The GLFW spelling of one CursorShape (see <ws/ui/app.hpp>). Cursor
// creation needs no window, so these are pure enum-to-enum.
int glfw_cursor_shape(CursorShape shape) {
    switch (shape) {
        case CursorShape::text: return GLFW_IBEAM_CURSOR;
        case CursorShape::crosshair: return GLFW_CROSSHAIR_CURSOR;
        case CursorShape::hand: return GLFW_HAND_CURSOR;
        case CursorShape::resize_ew: return GLFW_HRESIZE_CURSOR;
        case CursorShape::resize_ns: return GLFW_VRESIZE_CURSOR;
        case CursorShape::resize_all: return GLFW_RESIZE_ALL_CURSOR;
        case CursorShape::not_allowed: return GLFW_NOT_ALLOWED_CURSOR;
        case CursorShape::arrow: break;
    }
    return GLFW_ARROW_CURSOR;
}

// WindowHost on GLFW: init opens the window, and "window ready" is a
// startup fact that stays true for the whole run - the desktop teardown is
// once, in order, in the destructor.
class GlfwHost final : public WindowHost {
public:
    ~GlfwHost() override {
        if (window_ != nullptr) {
            glfwSetCursor(window_, nullptr);
            glfwDestroyWindow(window_);
            window_ = nullptr;
        }
        // The lazily-created standard cursors die before the library
        // does (glfwTerminate invalidates them).
        for (GLFWcursor*& cursor : cursors_) {
            if (cursor != nullptr) {
                glfwDestroyCursor(cursor);
                cursor = nullptr;
            }
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

    bool prefers_dark() const override {
#ifdef _WIN32
        // The Personalize key Windows 1809+ writes: AppsUseLightTheme 0
        // = apps dark, 1 = apps light. GLFW exposes no theme API and no
        // change event, so this is a live read per call - callers ask
        // rarely (a theme picked once at startup, re-checked on focus at
        // most), and a hot poller caches at its own cadence. A missing
        // key (pre-1809 Windows, or the value deleted) and a failed
        // read both keep the light default: absent preference is not a
        // dark demand.
        DWORD apps_light = 1;
        DWORD size = sizeof(apps_light);
        const LSTATUS status =
            RegGetValueA(HKEY_CURRENT_USER,
                         "Software\\Microsoft\\Windows\\CurrentVersion\\"
                         "Themes\\Personalize",
                         "AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr,
                         &apps_light, &size);
        if (status != ERROR_SUCCESS) {
            return false;
        }
        return apps_light == 0;
#else
        // No desktop dark-mode probe on this platform yet - the seam's
        // documented stub (the base default, spelled out where a reader
        // of this TU looks for the platform's answer).
        return false;
#endif
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

    // The stage-12 seam halves (see window.h): the composed title meets
    // the OS window here, and the standard pointers come from GLFW's
    // own cursor factory. set_title takes utf-8, the exact spelling
    // ws::platform's composer answers, so it crosses verbatim.
    void set_title(std::string_view title) override {
        if (window_ != nullptr) {
            glfwSetWindowTitle(window_, std::string{title}.c_str());
        }
    }

    void set_cursor(CursorShape shape) override {
        // The arrow IS the window default: a null cursor hands the
        // default back, so it needs no created object. Every other
        // shape maps to one standard cursor, created on first use and
        // shared for the process lifetime (the cache below).
        if (window_ == nullptr) {
            return;
        }
        if (shape == CursorShape::arrow) {
            glfwSetCursor(window_, nullptr);
            return;
        }
        const std::size_t slot = static_cast<std::size_t>(shape);
        if (cursors_[slot] == nullptr) {
            cursors_[slot] = glfwCreateStandardCursor(glfw_cursor_shape(shape));
            if (cursors_[slot] == nullptr) {
                core::warn("ui: the standard cursor could not be created");
                return;
            }
        }
        glfwSetCursor(window_, cursors_[slot]);
    }

private:
    GLFWwindow* window_ = nullptr;
    WindowEventSink* sink_ = nullptr;
    bool glfw_initialized_ = false;
    // One standard cursor per non-arrow shape, created lazily and
    // destroyed in the destructor before glfwTerminate. Indexed by
    // CursorShape's value - arrow (0) never gets a slot used.
    std::array<GLFWcursor*, static_cast<std::size_t>(CursorShape::not_allowed) + 1>
        cursors_{};
};

}  // namespace

std::unique_ptr<WindowHost> make_window_host() {
    return std::make_unique<GlfwHost>();
}

}  // namespace ws::ui
