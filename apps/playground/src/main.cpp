// Minimal playground: proves out the ws::ui shell end to end (window, Vulkan
// swapchain, ImGui + ImPlot contexts) and gives the workspace a scratch pad.

#include <ws/ui/app.hpp>

#include <imgui.h>
#include <implot.h>

namespace {

class PlaygroundApp final : public ws::ui::App {
public:
    using ws::ui::App::App;

protected:
    void on_frame() override {
        ImGui::ShowDemoWindow(&show_imgui_);
        ImPlot::ShowDemoWindow(&show_implot_);
    }

private:
    bool show_imgui_ = true;
    bool show_implot_ = true;
};

}  // namespace

int main() {
    ws::ui::AppConfig config;
    config.title = "ws playground";
    return PlaygroundApp{config}.run();
}
