// Minimal playground: proves out the ws::ui shell end to end (window, Vulkan
// swapchain, ImGui + ImPlot contexts, event system, 2D renderer) and gives
// the workspace a scratch pad. Includes only ws headers - third parties stay
// behind the ui lib.

#include <ws/ui/app.hpp>
#include <ws/ui/event.hpp>
#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/renderer2d.hpp>
#include <ws/ui/widgets.hpp>

#include <ws/core/log.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

class PlaygroundApp final : public ws::ui::App {
public:
    using ws::ui::App::App;

protected:
    void on_setup() override {
        // Relative to the exe's working directory - the build copies
        // assets/ next to the binary. A miss just logs and every checker
        // quad renders white (the documented fallback).
        checker_ = ws::ui::r2d::load_texture("assets/checker.png");
        // One atlas call packs all four icons into a single page texture;
        // the sprite draws below then share one batch material. A missing
        // file degrades that entry to Sprite{} (white), like load_texture.
        icons_ = ws::ui::r2d::load_atlas({"assets/icon_blue.png",
                                          "assets/icon_orange.png",
                                          "assets/icon_green.png",
                                          "assets/icon_purple.png"});
    }

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
        namespace r2d = ws::ui::r2d;

        // --- gui module: input snapshot (Phase 1) ---
        // The shell accumulates raw input into one InputFrame per frame
        // and publishes it here; the context adopts the copy. The live
        // values show in the window below - widgets arrive in Phase 5.
        gui_.new_frame(ws::ui::gui::frame_input());
        gui_.end_frame();

        // --- 2D world, through the camera ---
        r2d::begin(camera_);
        // 20x12 grid of 100px cells: a few hundred flat quads that batch
        // into a single chunk (one material).
        for (int gy = 0; gy < 12; ++gy) {
            for (int gx = 0; gx < 20; ++gx) {
                const std::uint32_t cell = (gx + gy) % 2 == 0
                                               ? r2d::rgba(52, 52, 64)
                                               : r2d::rgba(40, 40, 50);
                r2d::draw_quad(gx * 100.0f, gy * 100.0f, 100.0f, 100.0f, cell);
            }
        }
        // Textured quads (tint = vertex color * texel), one of them rotating
        // around its center.
        r2d::draw_quad(150.0f, 150.0f, 200.0f, 200.0f, r2d::rgba(255, 255, 255),
                       checker_);
        r2d::draw_quad(420.0f, 260.0f, 160.0f, 160.0f, r2d::rgba(120, 200, 255),
                       checker_);
        r2d::draw_quad(760.0f, 520.0f, 160.0f, 160.0f, r2d::rgba(255, 255, 255),
                       checker_, spin_);
        // Lines are CPU-expanded into quads - same batch, any thickness.
        r2d::draw_line(0.0f, 0.0f, 2000.0f, 1200.0f, 2.0f,
                       r2d::rgba(255, 200, 60));
        r2d::draw_line(2000.0f, 0.0f, 0.0f, 1200.0f, 6.0f,
                       r2d::rgba(90, 200, 255));
        r2d::draw_line(1000.0f, 0.0f, 1000.0f, 1200.0f, 12.0f,
                       r2d::rgba(255, 90, 120, 128));
        // Atlas sprites: sub-rects of one packed page texture. However
        // interleaved with the draws above, these four stay in one batch
        // segment - the whole page is one material.
        for (std::size_t i = 0; i < icons_.size(); ++i) {
            const r2d::FrameSize native = r2d::sprite_size(icons_[i]);
            // Missing/failed entries report {0, 0}: draw a visible white
            // square instead of nothing (the fallback contract).
            const float w = native.width > 0.0f ? native.width : 32.0f;
            const float h = native.height > 0.0f ? native.height : 32.0f;
            const float scale = (i == 3) ? 3.0f : 1.0f;  // magnified: gutters hold
            r2d::draw_sprite(1100.0f + static_cast<float>(i) * 130.0f, 300.0f,
                             w * scale, h * scale,
                             i == 1 ? r2d::rgba(255, 230, 180)
                                    : r2d::rgba(255, 255, 255),
                             icons_[i], i == 1 ? spin_ : 0.0f);
        }
        r2d::end();

        // --- screen layer: pixels from the top-left, no camera ---
        r2d::begin();
        const r2d::FrameSize frame = r2d::frame_size();
        constexpr float inset = 8.0f;
        const std::uint32_t border = r2d::rgba(255, 255, 255, 96);
        r2d::draw_line(inset, inset, frame.width - inset, inset, 2.0f, border);
        r2d::draw_line(frame.width - inset, inset, frame.width - inset,
                       frame.height - inset, 2.0f, border);
        r2d::draw_line(frame.width - inset, frame.height - inset, inset,
                       frame.height - inset, 2.0f, border);
        r2d::draw_line(inset, frame.height - inset, inset, inset, 2.0f, border);
        r2d::end();

        spin_ = std::fmod(spin_ + 0.01f, 6.2831853f);

        if (ws::ui::begin_window("ws playground")) {
            ws::ui::text("shell is alive - every widget here goes through ws::ui");
            ws::ui::text("press D to toggle the demo window (events, not polling)");

            // Live proof of the gui input snapshot (Phase 1): raw input
            // straight from the shell, unaffected by these ImGui widgets.
            const ws::ui::gui::InputFrame& in = gui_.input();
            char line[96];
            std::snprintf(line, sizeof(line),
                          "gui mouse %.0f, %.0f px   down L%d M%d R%d",
                          in.mouse_x, in.mouse_y, in.mouse_down[0],
                          in.mouse_down[1], in.mouse_down[2]);
            ws::ui::text(line);
            std::snprintf(line, sizeof(line), "pressed L%d R%d   scroll %.1f, %.1f",
                          in.mouse_pressed[0], in.mouse_pressed[2], in.scroll_dx,
                          in.scroll_dy);
            ws::ui::text(line);
            std::snprintf(line, sizeof(line),
                          "key 'D' down %d   text chars this frame: %zu",
                          in.key_down['D'], in.text_input.size());
            ws::ui::text(line);

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

            ws::ui::separator();
            ws::ui::text("2D camera (drives the world layer)");
            ws::ui::slider_float("cam x", &camera_.x, -2000.0f, 2000.0f);
            ws::ui::slider_float("cam y", &camera_.y, -2000.0f, 2000.0f);
            ws::ui::slider_float("cam zoom", &camera_.zoom, 0.1f, 4.0f);
            ws::ui::slider_float("cam rotation", &camera_.rotation,
                                 -3.14159265f, 3.14159265f);
            ws::ui::text("atlas: the 4 icons right of the grid share one texture");
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
    ws::ui::r2d::Camera2D camera_{1000.0f, 600.0f, 0.0f, 1.0f};
    ws::ui::r2d::Texture checker_{};
    std::vector<ws::ui::r2d::Sprite> icons_;
    float spin_ = 0.0f;
    std::array<float, kMaxSamples> xs_{};
    std::array<float, kMaxSamples> ys_{};
    float amplitude_ = 0.75f;
    int samples_ = 128;
    bool show_signal_ = true;
    bool show_imgui_ = false;
    bool show_implot_ = false;
    ws::ui::gui::GuiContext gui_;  // gui module consumer; fed the live snapshot
};

}  // namespace

int main() {
    ws::ui::AppConfig config;
    config.title = "ws playground";
    return PlaygroundApp{config}.run();
}
