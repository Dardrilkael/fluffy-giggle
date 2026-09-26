// Minimal playground: proves out the ws::ui shell end to end (window, Vulkan
// swapchain, ImGui + ImPlot contexts, event system, 2D renderer) and gives
// the workspace a scratch pad. Includes only ws headers - third parties stay
// behind the ui lib. (The Android entry below talks to the NDK platform:
// the app IS the platform boundary, the same reason it owns the font path.)

#include <ws/ui/app.hpp>
#include <ws/ui/event.hpp>
#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/ui/renderer2d.hpp>
#include <ws/ui/widgets.hpp>

#include "net_screen.hpp"

#include <ws/core/log.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#ifdef __ANDROID__
// NDK headers are C - keep them clear of our -Wpedantic settings (the same
// wrap src/window_android.cpp uses).
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <android/asset_manager.h>    // unpack the APK's assets at startup
#include <android_native_app_glue.h>  // android_app + the android_main contract
#include <sys/stat.h>                 // mkdir for the unpack target dir
#include <unistd.h>                   // chdir
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

namespace {

// The system UI face - the APP picks it, not the ui lib (r2d takes
// caller-resolved paths; font-directory knowledge lives in this TU, the
// platform boundary, and never inside the lib). One constant per TU, per
// the house rule. Roboto ships on every Android device; a missing face
// degrades to the fontless v1 bars, same as a missing Windows face.
#ifdef __ANDROID__
constexpr const char* kDefaultFontPath = "/system/fonts/Roboto-Regular.ttf";
#else
constexpr const char* kDefaultFontPath = "C:/Windows/Fonts/segoeui.ttf";
#endif

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
        // One font, one bake (the v1 rule): Segoe UI at 16 px, wired into
        // the theme the gui panels below read. A failed load degrades to a
        // fontless theme - the panels keep their v1 bars, never go dark.
        font_ = ws::ui::r2d::load_font(kDefaultFontPath, 16.0f);
        chrome_.font = font_.empty() ? nullptr : &font_;
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

        // --- gui module: input snapshot + the first real widget (Phases 1+5) ---
        // The shell accumulates raw input into one InputFrame per frame
        // and publishes it here; the context adopts the copy, widgets
        // consume it between new_frame and end_frame.
        gui_.new_frame(ws::ui::gui::frame_input());

        // A gui panel in screen space (framebuffer px, top-left origin),
        // drawn through r2d above the world quads. The panel widget draws
        // the chrome, clips its children to the content rect, scopes their
        // ids and flows them through the cursor. Every widget below drives
        // a real piece of app state - the same state the ImGui window's
        // controls below show - and the click counter is ours to keep.
        r2d::set_layer(10);
        r2d::begin();
        const r2d::FrameSize gui_frame = r2d::frame_size();
        const ws::ui::gui::Rect content = ws::ui::gui::begin_panel(
            gui_, layout_, chrome_, "demo",
            {gui_frame.width - 244.0f, 24.0f, 220.0f, 176.0f});
        if (ws::ui::gui::button(gui_, layout_, chrome_,
                                "clicks", 120.0f,
                                icons_.empty() ? r2d::Sprite{}
                                               : icons_[0])) {
            ++gui_clicks_;
        }
        ws::ui::gui::checkbox(gui_, layout_, chrome_,
                              "signal", show_signal_);
        ws::ui::gui::slider_float(gui_, layout_, chrome_,
                                  "amplitude", amplitude_, 0.0f, 1.0f);
        ws::ui::gui::slider_int(gui_, layout_, chrome_,
                                "samples", samples_, 4, kMaxSamples);
        ws::ui::gui::slider_float(gui_, layout_, chrome_,
                                  "spin", gui_spin_rate_, 0.0f, 0.2f);
        // Clip proof: this quad pokes past the panel's bottom-right corner
        // by 40 px each way - the scissor must cut it at the content edge.
        r2d::draw_quad(content.x + content.width - 8.0f,
                       content.y + content.height - 8.0f, 48.0f, 48.0f,
                       r2d::rgba(255, 120, 60));
        ws::ui::gui::end_panel(gui_, layout_);

        // Phase 9: a tabbed page under the demo panel. The strip selects
        // (the caller's int is the state); "list" is a scrollable checkbox
        // column whose offset lives in the gui context - it survives tab
        // flips - and "tune" edits the list itself, live: shrinking the
        // row count shrinks the scroll range one frame later.
        const ws::ui::gui::Rect page = ws::ui::gui::begin_panel(
            gui_, layout_, chrome_, "pages",
            {gui_frame.width - 244.0f, 208.0f, 220.0f, 220.0f});
        static const char* kPageTabs[] = {"list", "tune"};
        (void)ws::ui::gui::tab_bar(gui_, layout_, chrome_,
                                   "views", kPageTabs, 2, active_tab_,
                                   page.width * 0.5f);
        if (active_tab_ == 0) {
            const float below = chrome_.control_height +
                                chrome_.spacing;
            (void)ws::ui::gui::begin_scroll_panel(
                gui_, layout_, chrome_, "list",
                {page.x, page.y + below, page.width, page.height - below});
            for (int i = 0; i < row_count_; ++i) {
                gui_.ids().push(i);  // loop rows id by index (one label)
                ws::ui::gui::checkbox(
                    gui_, layout_, chrome_, "row",
                    rows_[static_cast<std::size_t>(i)]);
                gui_.ids().pop();
            }
            ws::ui::gui::end_scroll_panel(gui_, layout_,
                                          chrome_);
        } else {
            ws::ui::gui::slider_int(gui_, layout_, chrome_,
                                    "rows", row_count_, 1, 16, page.width);
            if (ws::ui::gui::button(gui_, layout_, chrome_,
                                    "clear", page.width)) {
                rows_.fill(false);
            }
        }
        ws::ui::gui::end_panel(gui_, layout_);
        // The Phase 10 real screen's gui half, bottom-left: the tab strip
        // plus sliders/button (drive) and the scrollable node list
        // (inspect) that steer the world-space net below.
        net_.draw_panel(gui_, layout_, chrome_, gui_frame);
        r2d::end();
        r2d::set_layer(0);

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
        // Rounded quads, 9-sliced from the builtin mask: this one magnifies
        // the baked 8px arcs to a 24px radius; the tint multiplies the white
        // mask, so any flat color comes out of the one texture.
        r2d::draw_rounded_quad(250.0f, 700.0f, 160.0f, 90.0f, 24.0f,
                               r2d::rgba(90, 168, 110));
        // Radius past half the shorter side clamps: this renders as a
        // fully-round-end pill, not overlapping corners.
        r2d::draw_rounded_quad(450.0f, 700.0f, 40.0f, 40.0f, 30.0f,
                               r2d::rgba(72, 132, 192));
        // Atlas sprites: sub-rects of one packed page texture. However
        // interleaved with the draws above, these four stay in one batch
        // segment - one page is one texture-table slot, and texture is
        // not a sort key, so painter order rules.
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
        // The Phase 10 real screen's world half: the net graph, drawn
        // last so its nodes sit over the demo lines that cross the area.
        // The gui block earlier in this frame already applied the panel's
        // edits, so the forward pass here reflects them with no lag.
        net_.draw_world();
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

        spin_ = std::fmod(spin_ + gui_spin_rate_, 6.2831853f);

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
            std::snprintf(line, sizeof(line),
                          "gui button clicks: %d (the panel, top right)",
                          gui_clicks_);
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
    ws::ui::gui::LayoutCursor layout_;  // one per screen; the pass repeats each frame
    int gui_clicks_ = 0;
    float gui_spin_rate_ = 0.01f;  // driven by the gui panel's "spin" slider
    int active_tab_ = 0;           // Phase 9: the page panel's selected tab
    int row_count_ = 16;           // the "list" tab's rows (edited in "tune")
    std::array<bool, 16> rows_{};   // the "list" tab's checkbox states
    playground::NetScreen net_;    // Phase 10: the real screen (world + panel)
    ws::ui::r2d::Font font_;      // one bake, one face - outlives the theme use
    ws::ui::gui::ChromeTheme chrome_ = ws::ui::gui::kDefaultChrome;  // + font_
};

}  // namespace

#ifdef __ANDROID__

// Android entry. native_app_glue calls android_main() on the app's own
// thread and hands over the handle the shell needs (AppConfig::app_handle).
// The APK's assets/ unpack into the activity's files dir once per launch,
// then chdir() makes every CWD-relative load in the app (assets/*.png,
// imgui.ini) resolve there - so the app logic and the shell stay exactly
// the desktop code, with zero knowledge of Android packaging.

namespace {

// stb_image and imgui.ini want real files, so copy every top-level APK
// asset out of the asset manager into <filesDir>/assets/. A miss degrades
// exactly like a missing file on desktop: white textures, one log line.
void unpack_assets(android_app* app) {
    AAssetManager* assets = app->activity->assetManager;
    const char* base = app->activity->internalDataPath;
    if (assets == nullptr || base == nullptr) {
        return;
    }
    AAssetDir* dir = AAssetManager_openDir(assets, "");
    if (dir == nullptr) {
        return;
    }
    // EEXIST is fine - the dir survives launches; its files overwrite.
    mkdir((std::string(base) + "/assets").c_str(), 0755);
    while (const char* name = AAssetDir_getNextFileName(dir)) {
        AAsset* asset = AAssetManager_open(assets, name, AASSET_MODE_STREAMING);
        if (asset == nullptr) {
            continue;
        }
        std::FILE* out =
            std::fopen((std::string(base) + "/assets/" + name).c_str(), "wb");
        if (out == nullptr) {
            ws::core::error("playground: cannot unpack asset {}", name);
            AAsset_close(asset);
            continue;
        }
        char buffer[65536];
        int read = 0;
        while ((read = AAsset_read(asset, buffer, sizeof(buffer))) > 0) {
            if (std::fwrite(buffer, 1, static_cast<std::size_t>(read), out) !=
                static_cast<std::size_t>(read)) {
                ws::core::error("playground: short write unpacking {}", name);
                break;
            }
        }
        std::fclose(out);
        AAsset_close(asset);
    }
    AAssetDir_close(dir);
}

}  // namespace

// The glue header declares android_main inside its extern "C" block; the
// definition must match or the glue's C call site would not resolve.
extern "C" void android_main(android_app* app) {
    unpack_assets(app);
    chdir(app->activity->internalDataPath);
    ws::ui::AppConfig config;
    config.title = "ws playground";
    config.app_handle = app;
    PlaygroundApp{config}.run();
}

#else

int main() {
    ws::ui::AppConfig config;
    config.title = "ws playground";
    return PlaygroundApp{config}.run();
}

#endif
