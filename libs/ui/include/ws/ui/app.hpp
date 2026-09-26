#pragma once

#include <memory>
#include <string>

#ifdef __ANDROID__
struct android_app;  // opaque native_app_glue handle; never defined in a
                     // public header (no NDK include needed to read this)
#endif

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

class Event;
class RenderCommandQueue;

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

protected:
    // Called once after the context and ImGui are ready, before the first
    // frame. Safe to create ImGui/ImPlot objects here.
    virtual void on_setup() {}

    // Called once per queued input/window event, synchronously on the main
    // thread, before on_update() for that frame. Events ImGui already
    // consumed (mouse over a widget, text input) arrive with handled == true;
    // EventDispatcher skips those, so handlers rarely check the flag.
    // Include <ws/ui/event.hpp> for the event types. Default is empty.
    virtual void on_event(Event&) {}

    // Called once per frame, between NewFrame and Render. Draw widgets /
    // plots here and submit command-queue work. Do NOT call ImGui::Render()
    // or the backend NewFrame functions - the shell handles the frame
    // lifecycle.
    virtual void on_update() {}

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ws::ui
