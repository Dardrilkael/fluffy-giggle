// Internal to ws::ui - never visible to apps or other libs. This header is
// the windowing-platform seam: the shell (src/app/app.cpp) talks to the window
// system ONLY through WindowHost, and exactly one implementation compiles
// per platform - src/platform/window_glfw.cpp on desktop, src/platform/window_android.cpp on
// Android (native_app_glue). GLFW's C callback surface and Android's
// AInputEvent/lifecycle translation both live behind it.
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <ws/input/event.hpp>
#include <ws/input/input.hpp>
#include <ws/ui/app.hpp>
#include <ws/ui/gui/gui_context.hpp>

#include <volk.h>  // internal header: Vk types appear in the seam signature
                   // and never reach apps - that surface is <ws/ui/app.hpp>.

namespace ws::ui {

// Every platform event funnels its ws::input::Event through this single sink; the
// callbacks that feed it are file-local in the platform TU.
//
// `input` is the parallel gui-library tap: the same raw events accumulate
// one ws::input::InputFrame per frame, independent of the event stream -
// so the gui snapshot stays unfiltered even while ImGui (or the app's
// on_event) consumes the same physical input. The gui library hit-tests for
// itself. Null until the shell wires it; every source null-checks.
struct WindowEventSink {
    std::function<void(ws::input::Event&)> forward;
    ws::input::InputFrame* input = nullptr;
};

// Everything the shell needs from the windowing platform, and nothing more.
// Desktop specifics (hints, glfwInit/Terminate, callback chaining) and
// Android specifics (the surface-loss lifecycle: the OS hands the window
// over at APP_CMD_INIT_WINDOW and takes it back at APP_CMD_TERM_WINDOW, so
// "window ready" is a runtime STATE there, not a startup fact) both stay
// behind this interface.
class WindowHost {
public:
    virtual ~WindowHost() = default;

    // One-time platform init: desktop initializes GLFW and opens the window
    // (installing our callbacks before any ImGui backend, which then chains
    // to them); Android records the android_app handle from
    // AppConfig::app_handle and installs the cmd/input callbacks. `sink`
    // must outlive the host.
    virtual bool init(WindowEventSink& sink, const AppConfig& config) = 0;

    // Instance extensions surface creation needs (VK_KHR_surface plus the
    // platform surface extension).
    virtual std::vector<const char*> instance_extensions() const = 0;

    // Creates the Vulkan surface against the current window. Valid only
    // while window_ready() - the caller owns the returned handle.
    virtual bool create_surface(VkInstance instance, VkSurfaceKHR* out) const = 0;

    // Pumps OS messages, non-blocking; events fire through the sink during
    // the call. Desktop: glfwPollEvents. Android: one ALooper_pollOnce(0)
    // round - present/acquire pacing throttles the render loop, not this.
    virtual void pump() = 0;

    // Blocks until a window exists again or the platform asked to stop.
    // Desktop: unreachable (window_ready() never goes false after init);
    // Android: ALooper_pollOnce(-1) - surface-loss sleep, not a spin, so an
    // app with no window costs no CPU.
    virtual void wait_for_window() = 0;

    // True when the loop should exit. Desktop: the close button. Android:
    // destroyRequested - back button and ANativeActivity_finish both
    // arrive there through the normal OS sequence.
    virtual bool should_close() const = 0;

    // Asks the platform to wind the app down (desktop: set the close flag;
    // Android: ANativeActivity_finish, after which the OS delivers the
    // full term-window/destroy sequence through pump()/wait_for_window()).
    // The shell's exception paths use this instead of platform teardown.
    virtual void request_stop() = 0;

    // True while a usable window/surface exists. Desktop: always true after
    // init. Android: false before the first APP_CMD_INIT_WINDOW and between
    // APP_CMD_TERM_WINDOW and the next INIT - the shell must not touch the
    // swapchain or the surface then (it detaches instead).
    virtual bool window_ready() const = 0;

    // True when frames must not render (minimized). Desktop: iconified.
    // Android: never false-in-a-ready-window (a lost window reports through
    // window_ready() instead).
    virtual bool is_hidden() const = 0;

    // Framebuffer size in pixels; the swapchain sizes itself against this.
    virtual void framebuffer_size(int* width, int* height) const = 0;

    // The platform's ui scale: how many gui-space pixels one
    // density-independent "point" is. Android: display density / 160 (a
    // 280 dpi phone reports 1.75); desktop: the GLFW content scale (1.0
    // at a 100%-scaling display). Apps reach it through App::ui_scale().
    virtual float ui_scale() const = 0;

    // The platform's dark-mode answer, live at call time: true when the
    // OS theme (or its app-theme spelling) is dark. The base default is
    // the light answer - the stub for hosts with no probe. GLFW has no
    // theme API, so the desktop override reads the Windows Personalize
    // registry key (other desktops answer light for now); Android reads
    // the night mask of the glue's live AConfiguration. No platform
    // surfaces a change event the shell could forward, and callers ask
    // rarely (a theme picked once at startup), so this reads the OS per
    // call - a hot poller caches at its own cadence. Apps reach it
    // through App::prefers_dark().
    virtual bool prefers_dark() const { return false; }

    // --- ImGui platform backend (GLFW / Android) ----------------------------
    // init installs the backend against the current window. Android calls
    // it once per attach (each INIT_WINDOW can hand over a NEW
    // ANativeWindow), so shutdown is symmetric with init; desktop calls the
    // pair exactly once. The ImGui context must already exist - the shell
    // creates it before the first attach.
    virtual bool imgui_platform_init() = 0;
    virtual void imgui_platform_new_frame() = 0;
    virtual void imgui_platform_shutdown() = 0;

    // The gui clipboard seam's platform half: pushes this host's
    // get/set pair onto the app's GuiContext (text_input's Ctrl+C/X/V
    // ride it; App::install_clipboard is the app-side spelling). The
    // base default installs nothing - the unset seam, "no clipboard",
    // which is the honest answer on a host without wiring (Android
    // today: a JNI hop is a later item) - so a platform opts in by
    // overriding, like every other capability here.
    virtual void install_clipboard(gui::GuiContext& gui) { (void)gui; }
};

// The platform host for this build: window_glfw.cpp on desktop,
// window_android.cpp on Android. Never returns null; on Android a null
// AppConfig::app_handle makes init() fail (logged there, not here).
std::unique_ptr<WindowHost> make_window_host();

}  // namespace ws::ui
