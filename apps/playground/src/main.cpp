// Minimal playground: proves out the ws::ui shell end to end (window, Vulkan
// swapchain, ImGui + ImPlot contexts, event system) and gives the workspace
// a scratch pad. Includes only ws headers - third parties stay behind the
// ui lib.

#include <ws/ui/app.hpp>
#include <ws/ui/event.hpp>
#include <ws/ui/widgets.hpp>

#include <ws/core/log.hpp>

#include <array>
#include <cmath>

namespace {

class PlaygroundApp final : public ws::ui::App {
public:
    using ws::ui::App::App;

protected:
    void on_event(ws::ui::Event& event) override {
        ws::ui::EventDispatcher dispatcher{event};
        // GLFW key codes 0-255 match ASCII, so 'D' needs no GLFW header.
        dispatcher.dispatch<ws::ui::KeyPressedEvent>(
            [this](const ws::ui::KeyPressedEvent& e) {
                if (!e.repeat && e.keycode == 'D') {
                    show_imgui_ = !show_imgui_;
                    return true;
                }
                return false;
            });
        dispatcher.dispatch<ws::ui::WindowResizeEvent>(
            [](const ws::ui::WindowResizeEvent& e) {
                ws::core::info("playground: resized to {}x{}", e.width, e.height);
                return true;
            });
    }

    void on_update() override {
        if (ws::ui::begin_window("ws playground")) {
            ws::ui::text("shell is alive - every widget here goes through ws::ui");
            ws::ui::text("press D to toggle the demo window (events, not polling)");
            if (ws::ui::button("imgui demo")) {
                show_imgui_ = !show_imgui_;
            }
            ws::ui::same_line();
            if (ws::ui::button("implot demo")) {
                show_implot_ = !show_implot_;
            }
            ws::ui::separator();
            ws::ui::checkbox("show signal", &show_signal_);
            ws::ui::slider_float("amplitude", &amplitude_, 0.0f, 1.0f);
            ws::ui::slider_int("samples", &samples_, 4, kMaxSamples);

            if (show_signal_ && ws::ui::begin_plot("signal", -1.0f, 160.0f)) {
                for (int i = 0; i < samples_; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(samples_ - 1);
                    xs_[i] = t;
                    ys_[i] = amplitude_ * std::sin(t * 6.2831853f);
                }
                ws::ui::plot_line("sin", xs_.data(), ys_.data(), samples_);
                ws::ui::end_plot();
            }
        }
        ws::ui::end_window();

        if (show_imgui_) {
            ws::ui::show_imgui_demo(&show_imgui_);
        }
        if (show_implot_) {
            ws::ui::show_implot_demo(&show_implot_);
        }
    }

private:
    static constexpr int kMaxSamples = 512;
    std::array<float, kMaxSamples> xs_{};
    std::array<float, kMaxSamples> ys_{};
    float amplitude_ = 0.75f;
    int samples_ = 128;
    bool show_signal_ = true;
    bool show_imgui_ = false;
    bool show_implot_ = false;
};

}  // namespace

int main() {
    ws::ui::AppConfig config;
    config.title = "ws playground";
    return PlaygroundApp{config}.run();
}
