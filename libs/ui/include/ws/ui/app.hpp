#pragma once

#include <memory>
#include <string>
#include <string_view>

#ifdef __ANDROID__
struct android_app;  // opaque native_app_glue handle; never defined in a
                     // public header (no NDK include needed to read this)
#endif

namespace ws::input {
class Event;  // the event family lives in ws::input (stage 3a): the shell
              // queues these between frames and delivers each to on_event()
}  // namespace ws::input

namespace ws::jobs {
class JobSystem;  // the frame drain's target (stage 8): installed, never
                 // owned - no ws::jobs header needed to read this one
}  // namespace ws::jobs

namespace ws::ui {

struct AppConfig {
    std::string title = "ws app";
    int width = 1280;   // initial window size in screen coordinates
    int height = 720;   // (ignored on Android - the OS owns the window)
    bool vsync = true;  // false: prefer mailbox/immediate present mode
#ifdef __ANDROID__
    // The native_app_glue handle android_main() received; the shell renders
    // to its ANativeWindow and pumps its looper. Null = run() fails fast.
    android_app* app_handle = nullptr;
#endif
};

class RenderCommandQueue;

namespace gui {
class GuiContext;  // the clipboard seam's target (installed, never owned)
}  // namespace gui

// The standard system pointers (stage 12's cursor shape). The arrow is
// the OS default; the rest are the shapes every desktop shares. A host
// without a cursor API keeps the default (Android's glue exposes none -
// a USB mouse there carries the system pointer).
enum class CursorShape {
    arrow,         // the default pointer - clears any shape set earlier
    text,          // the I-beam, over editable text
    crosshair,     // pixel-precise picking
    hand,          // a clickable thing
    resize_ew,     // a horizontal resize edge
    resize_ns,     // a vertical resize edge
    resize_all,    // the four-way move
    not_allowed,   // the operation will not work here
};

// Generic application shell: window + Vulkan context + Dear ImGui (docking)
// + ImPlot, with a Hazel-style event system and a render command queue.
// Apps subclass and override on_setup()/on_event()/on_update().
//
// All Vulkan/GLFW state is hidden behind a pimpl; no Vk* or GLFW types leak
// into this header on purpose - engine libs and apps never see them either.
// Drawing goes through <ws/ui/widgets.hpp>; apps never include third-party
// headers.
class App {
public:
    explicit App(AppConfig config = {});
    virtual ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    // Creates the window and GPU context, runs the main loop until the window
    // closes, shuts everything down. Returns 0 on success, 1 if startup failed
    // (no Vulkan loader/device, etc. - details are logged).
    int run();

    // The queue this app submits non-widget GPU work into (overlays,
    // offscreen passes - anything beyond ImGui's own draw lists). The shell
    // swaps and executes it while recording the frame's command buffer, in
    // submission order. Include <ws/ui/render_command_queue.hpp> to use it.
    RenderCommandQueue& command_queue();

    // Wires the gui's clipboard seam to this app's platform window - the
    // pair the gui text_input's Ctrl+C/X/V ride. Call once after the
    // window exists (on_setup() is the natural point); a host with no
    // clipboard wiring (Android today) leaves the seam unset and the
    // shortcuts no-op. Include <ws/ui/gui/gui_context.hpp> for the type.
    void install_clipboard(gui::GuiContext& gui);

    // Installs the job system whose marshal queue the shell drains once
    // per frame, right after the platform events land and BEFORE
    // on_update() runs. Every progress delivery and completion the
    // system marshalled therefore runs on the main thread, between
    // frames, where callbacks may touch gui state directly (the Toasts
    // store, an app-owned JobProgress mirror) with no locks - the
    // single-thread guarantee the gui helpers in
    // <ws/ui/gui/job_progress.hpp> are built on. The system is
    // caller-owned: install once from on_setup() and keep it alive
    // past run() (or drain it by hand before destroying it - the shell
    // never drains after the loop). Installing nothing leaves the
    // shell draining nothing. Include <ws/jobs/job_system.hpp> for the
    // type.
    void install_job_system(ws::jobs::JobSystem& jobs);

    // The OS's dark-mode answer for this app, live at call time: true
    // when the user's system theme is dark - Windows answers from the
    // Personalize registry value, Android from the configuration's
    // night mode, and a platform without a probe answers false (light).
    // Neither platform offers a change event the shell could forward,
    // and callers ask rarely (a stock theme picked once at startup,
    // re-checked on focus at most), so the answer is a live OS read per
    // call, never cached here - poll it per frame and you pay a
    // registry/config read per frame. Valid from on_setup() on; false
    // before run(), the headless default (the same answer a probe-less
    // platform gives).
    bool prefers_dark() const;

    // Hands `title` to the platform window. The intended caller is the
    // composed answer of ws::platform's window_title(app, document,
    // dirty) - the stage-12 split of labor: ws::platform composes the
    // string (pure, headless) and this seam is where it meets the OS
    // window ui owns. No-op before run() and on a host whose platform
    // owns the label (Android: the activity's label is manifest-bound).
    void set_title(std::string_view title);

    // One of the standard system pointers over this window (see
    // CursorShape). No-op before run() and on a host without a cursor
    // API. The shape persists until the next call - set CursorShape::
    // arrow to hand the default back.
    void set_cursor(CursorShape shape);

protected:
    // Called once after the context and ImGui are ready, before the first
    // frame. Safe to create ImGui/ImPlot objects here.
    virtual void on_setup() {}

    // Called once per queued input/window event, synchronously on the main
    // thread, before on_update() for that frame. Events ImGui already
    // consumed (mouse over a widget, text input) arrive with handled == true;
    // EventDispatcher skips those, so handlers rarely check the flag.
    // Include <ws/input/event.hpp> for the event types. Default is empty.
    virtual void on_event(ws::input::Event&) {}

    // Called once per frame, between NewFrame and Render. Draw widgets /
    // plots here and submit command-queue work. Do NOT call ImGui::Render()
    // or the backend NewFrame functions - the shell handles the frame
    // lifecycle.
    virtual void on_update() {}

    // The platform's ui scale, for touch-density: Android reports the
    // display density (160 dpi = 1.0), desktop the window content scale.
    // Multiply gui theme metrics (ChromeTheme::scaled), font pixel sizes
    // and panel rects by it, so the gui is finger-sized on a phone; 1.0
    // on a 100%-scaling desktop changes nothing. Valid from on_setup()
    // on; 1.0 before run().
    float ui_scale() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ws::ui
