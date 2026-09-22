#pragma once

#include <memory>
#include <string>

namespace ws::ui {

struct AppConfig {
    std::string title = "ws app";
    int width = 1280;   // initial window size in screen coordinates
    int height = 720;
    bool vsync = true;  // false: prefer mailbox/immediate present mode
};

// Generic application shell: window + Vulkan context + Dear ImGui (docking)
// + ImPlot. Apps subclass and override on_frame() to draw their UI.
//
// All Vulkan/GLFW state is hidden behind a pimpl; no Vk* or GLFW types leak
// into this header on purpose - engine libs and apps never see them either
// (they only include <imgui.h> / <implot.h> for drawing).
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

protected:
    // Called once after the context and ImGui are ready, before the first
    // frame. Safe to create ImGui/ImPlot objects here.
    virtual void on_setup() {}

    // Called once per frame. Draw ImGui widgets / ImPlot plots here.
    // Do NOT call ImGui::Render() or the backend NewFrame functions - the
    // shell handles the frame lifecycle.
    virtual void on_frame() {}

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ws::ui
