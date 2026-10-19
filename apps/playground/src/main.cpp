// Minimal playground: proves out the ws::ui shell end to end (window, Vulkan
// swapchain, ImGui + ImPlot contexts, event system, 2D renderer) and gives
// the workspace a scratch pad. Includes only ws headers - third parties stay
// behind the ui lib. (The Android entry below talks to the NDK platform:
// the app IS the platform boundary, the same reason it owns the font path.)
// The screen-sized demos live in their own files - audio_demo (the sound
// panel + its engine), touch_demo (trails + pinch), net_screen (the net lab,
// which owns its own tick) - this file keeps the frame phases: the gui
// panels, the simulation tick, the 2D/3D worlds, the overlay and the ImGui
// debug window.

#include <ws/ui/app.hpp>
#include <ws/input/event.hpp>
#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/ui/renderer2d.hpp>
#include <ws/ui/widgets.hpp>

#include "audio_demo.hpp"
#include "net_screen.hpp"
#include "touch_demo.hpp"

#include <ws/core/log.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#ifdef __ANDROID__
// NDK headers are C - keep them clear of our -Wpedantic settings (the same
// wrap src/platform/window_android.cpp uses).
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

// One rule for panel geometry, stated once. The gui block draws at these
// rects; the pinch gate reads them back so a two-finger pinch over a panel
// goes to the widgets, not the camera.
struct PanelLayout {
    ws::ui::gui::Rect demo;
    ws::ui::gui::Rect pages;
    ws::ui::gui::Rect sound;
    ws::ui::gui::Rect net;

    static PanelLayout compute(const ws::ui::r2d::FrameSize& frame,
                               float scale,
                               const ws::ui::gui::Rect& net_rect) {
        const float x = frame.width - 244.0f * scale;
        return {
            {x, 24.0f * scale, 220.0f * scale, 312.0f * scale},
            {x, 344.0f * scale, 220.0f * scale, 220.0f * scale},
            {x, frame.height - 152.0f * scale, 220.0f * scale, 128.0f * scale},
            net_rect,
        };
    }

    std::array<ws::ui::gui::Rect, 4> all() const {
        return {demo, pages, sound, net};
    }
};

// The demo panel's popup payload: three choices, one tint each - the
// clip-proof quad at the panel's corner wears the choice, so the menu's
// effect is visible outside the menu.
constexpr const char* kMenuItems[] = {"red", "green", "blue"};
constexpr std::uint32_t kMenuTints[] = {
    ws::ui::r2d::rgba(232, 80, 60), ws::ui::r2d::rgba(90, 200, 90),
    ws::ui::r2d::rgba(90, 150, 255)};

class PlaygroundApp final : public ws::ui::App {
public:
    using ws::ui::App::App;

protected:
    void on_setup() override;
    void on_event(ws::input::Event&) override;
    void on_update() override;

private:
    // Frame phases.
    void draw_gui_panels(const ws::ui::r2d::FrameSize& frame);
    void draw_demo_panel(const ws::ui::gui::Rect& bounds);
    void draw_world_2d();
    void draw_world_3d();
    void draw_overlay();
    void draw_imgui_debug();

    // ---- rendering + camera state ----
    ws::ui::r2d::Camera2D camera_{1000.0f, 600.0f, 0.0f, 1.0f};
    ws::ui::r2d::Camera3D camera3d_{{1000.0f, 100.0f, -1500.0f},
                                    {1000.0f, 600.0f, 0.0f},
                                    {0.0f, -1.0f, 0.0f},
                                    0.7853982f, 1.0f, 5000.0f};
    bool show_3d_ = true;
    bool far_first_ = true;
    ws::ui::r2d::Texture checker_{};
    std::vector<std::uint8_t> checker_bytes_;  // the file's bytes, for the reload demo
    std::vector<ws::ui::r2d::Sprite> icons_;
    float spin_ = 0.0f;

    // ---- signal demo (ImGui) ----
    static constexpr int kMaxSamples = 512;
    std::array<float, kMaxSamples> xs_{};
    std::array<float, kMaxSamples> ys_{};
    float amplitude_ = 0.75f;
    int samples_ = 128;
    bool show_signal_ = true;
    bool show_imgui_ = false;
    bool show_implot_ = false;

    // ---- gui demo state ----
    ws::ui::gui::GuiContext gui_;
    ws::ui::gui::LayoutCursor layout_;
    int gui_clicks_ = 0;
    float gui_spin_rate_ = 0.01f;
    int active_tab_ = 0;
    int row_count_ = 16;
    bool show_modal_ = false;
    bool menu_open_ = false;
    int menu_choice_ = 0;
    bool combo_open_ = false;
    char name_buf_[32] = "playground";
    std::array<bool, 16> rows_{};

    // ---- text-event echo ----
    static constexpr std::size_t kTypedMax = 40;
    std::string typed_;

    // ---- density + theme ----
    float ui_scale_ = 1.0f;
    ws::ui::r2d::Font font_;
    ws::ui::gui::ChromeTheme chrome_ = ws::ui::gui::kDefaultChrome;

    // ---- extracted concerns ----
    playground::AudioDemo audio_;
    playground::TouchDemo touch_;
    playground::NetScreen net_;
};

void PlaygroundApp::on_setup() {
    checker_ = ws::ui::r2d::load_texture("assets/checker.png");
    // The same file as raw bytes: the reload button below bakes them
    // through load_texture_from_memory. A failed read leaves it empty
    // and the button degrades to the white fallback - exactly the
    // contract a missing file gets.
    if (std::ifstream file("assets/checker.png",
                           std::ios::binary | std::ios::ate);
        file) {
        const std::streamoff size = file.tellg();
        if (size > 0) {
            checker_bytes_.resize(static_cast<std::size_t>(size));
            file.seekg(0);
            file.read(reinterpret_cast<char*>(checker_bytes_.data()),
                      static_cast<std::streamsize>(size));
        }
    }
    icons_ = ws::ui::r2d::load_atlas({"assets/icon_blue.png",
                                      "assets/icon_orange.png",
                                      "assets/icon_green.png",
                                      "assets/icon_purple.png"});
    ui_scale_ = ui_scale();
    net_.set_ui_scale(ui_scale_);
    font_ = ws::ui::r2d::load_font(kDefaultFontPath, 16.0f * ui_scale_);
    chrome_ = ws::ui::gui::kDefaultChrome.scaled(ui_scale_);
    chrome_.font = font_.empty() ? nullptr : &font_;
    install_clipboard(gui_);
    audio_.setup();
}

void PlaygroundApp::on_event(ws::input::Event& event) {
    ws::input::EventDispatcher dispatcher{event};
    // GLFW key codes 0-255 match ASCII, so 'D' needs no GLFW header.
    dispatcher.dispatch<ws::input::KeyPressedEvent>(
        [this](const ws::input::KeyPressedEvent& e) {
            if (!e.repeat && e.keycode == 'D') {
                show_imgui_ = !show_imgui_;
                return true;
            }
            return false;
        });
    // The text-event echo: one UTF-32 codepoint per glyph, arrival
    // order. The drawn echo (screen block below) keeps the bake's
    // range - ASCII 32..126, the v1 font has no other glyphs; wider
    // codepoints are observed here but not shown.
    dispatcher.dispatch<ws::input::TextInputEvent>(
        [this](const ws::input::TextInputEvent& e) {
            if (e.codepoint >= 32u && e.codepoint <= 126u) {
                if (typed_.size() >= kTypedMax) {
                    typed_.erase(typed_.begin());
                }
                typed_.push_back(static_cast<char>(e.codepoint));
            }
            return false;  // observed, never consumed
        });
    dispatcher.dispatch<ws::input::WindowResizeEvent>(
        [](const ws::input::WindowResizeEvent& e) {
            ws::core::info("playground: resized to {}x{}", e.width, e.height);
            return true;
        });
}

void PlaygroundApp::on_update() {
    namespace r2d = ws::ui::r2d;

    gui_.begin_frame(ws::input::frame_input());

    // Gui block: panels first so draw_panel can publish net_.panel_bounds()
    // for the layout query below.
    r2d::begin();
    gui_.begin_layer(ws::ui::gui::Layer::base);
    const r2d::FrameSize gui_frame = r2d::frame_size();
    draw_gui_panels(gui_frame);
    gui_.end_layer();
    r2d::end();
    r2d::set_layer(0);
    gui_.end_frame();

    // Simulation tick (advances NetScreen's clock, trains, evaluates, probes).
    net_.tick();

    // Input consumers: trails, pinch (blocked by any gui panel), audio pump.
    const auto panels = PanelLayout::compute(gui_frame, ui_scale_,
                                             net_.panel_bounds());
    const auto& in = ws::input::frame_input();
    touch_.update_trails(in);
    touch_.apply_pinch(in, camera_, panels.all());
    audio_.tick();

    // Render.
    draw_world_2d();
    if (show_3d_) draw_world_3d();
    draw_overlay();

    spin_ = std::fmod(spin_ + gui_spin_rate_, 6.2831853f);

    draw_imgui_debug();
}

void PlaygroundApp::draw_gui_panels(const ws::ui::r2d::FrameSize& frame) {
    namespace gui = ws::ui::gui;

    // Modal first (chrome-first: the veil paints frame-wide and the panels
    // below must be click-dead on the very frame it opens).
    if (gui::begin_modal(gui_, layout_, chrome_, "confirm",
            {frame.width * 0.5f - 140.0f * ui_scale_,
             frame.height * 0.5f - 60.0f * ui_scale_,
             280.0f * ui_scale_, 120.0f * ui_scale_},
            show_modal_)) {
        gui::text(layout_, chrome_, "the modal veil", -1.0f);
        if (gui::button(gui_, layout_, chrome_, "close", -1.0f)) {
            show_modal_ = false;
        }
        gui::end_modal(gui_, layout_);
    }

    const auto panels = PanelLayout::compute(frame, ui_scale_, net_.panel_bounds());
    draw_demo_panel(panels.demo);

    // Pages panel: tabbed list / tune.
    {
        const gui::Rect page = gui::begin_panel(gui_, layout_, chrome_, "pages",
                                                 panels.pages);
        static const char* kPageTabs[] = {"list", "tune"};
        (void)gui::tab_bar(gui_, layout_, chrome_, "views", kPageTabs, 2,
                            active_tab_, page.width * 0.5f);
        if (active_tab_ == 0) {
            const float below = chrome_.control_height + chrome_.spacing;
            (void)gui::begin_scroll_panel(
                gui_, layout_, chrome_, "list",
                {page.x, page.y + below, page.width, page.height - below});
            for (int i = 0; i < row_count_; ++i) {
                gui_.ids().push(i);
                gui::checkbox(gui_, layout_, chrome_, "row",
                              rows_[static_cast<std::size_t>(i)]);
                gui_.ids().pop();
            }
            gui::end_scroll_panel(gui_, layout_, chrome_);
        } else {
            gui::slider_int(gui_, layout_, chrome_, "rows", row_count_, 1, 16,
                             page.width);
            if (gui::button(gui_, layout_, chrome_, "clear", page.width)) {
                rows_.fill(false);
            }
        }
        gui::end_panel(gui_, layout_);
    }

    net_.draw_panel(gui_, layout_, chrome_, frame);
    audio_.draw_panel(gui_, layout_, chrome_, panels.sound);
}

void PlaygroundApp::draw_demo_panel(const ws::ui::gui::Rect& bounds) {
    namespace gui = ws::ui::gui;
    namespace r2d = ws::ui::r2d;
    const gui::Rect content = gui::begin_panel(gui_, layout_, chrome_, "demo",
                                                bounds);

    // M18's popup demo, the combo idiom in miniature: the menu button
    // IS the anchor (a press on it while open reaches the button -
    // the anchor exemption keeps the popup, the caller's own toggle
    // closes it), the dropdown hangs below it over the rows there,
    // and the caller places both rects by hand - library-dumb
    // placement, the arithmetic the combo row at the panel's bottom
    // now owns. A choice closes the menu and retints the clip-proof
    // quad at the panel's corner.
    if (gui::button(gui_, layout_, chrome_, "menu",
                    120.0f * ui_scale_)) {
        menu_open_ = !menu_open_;
    }
    const gui::Rect menu_anchor{content.x, content.y,
                                120.0f * ui_scale_,
                                chrome_.control_height};
    const float menu_h = chrome_.padding * 2.0f +
                         chrome_.control_height * 3.0f +
                         chrome_.spacing * 2.0f;
    const gui::Rect menu_bounds{
        content.x, content.y + chrome_.control_height,
        140.0f * ui_scale_, menu_h};
    if (gui::begin_popup(gui_, layout_, chrome_, "menu",
                         menu_anchor, menu_bounds, menu_open_)) {
        for (int i = 0; i < 3; ++i) {
            gui_.ids().push(i);  // one label, three ids
            if (gui::selectable(gui_, layout_, chrome_,
                                kMenuItems[static_cast<std::size_t>(i)],
                                menu_choice_ == i, -1.0f)) {
                menu_choice_ = i;
                menu_open_ = false;  // a choice closes: caller's rule
                combo_open_ = false;  // one choice, one open dropdown
            }
            gui_.ids().pop();
        }
        gui::end_popup(gui_, layout_);
    }
    const gui::WidgetId clicks_w = gui_.ids().id("clicks");
    if (gui::button(gui_, layout_, chrome_,
                    "clicks", 120.0f * ui_scale_,
                    icons_.empty() ? r2d::Sprite{}
                                   : icons_[0])) {
        ++gui_clicks_;
    }
    // M18's tooltip demo: the hint follows the cursor while the
    // clicks button is hot - chrome at the tooltip stratum that
    // claims nothing, so the button beneath keeps its hover and
    // its clicks. The bounds carry the hint's WHOLE box: the scope
    // is a padded panel (the cursor starts inset by theme.padding)
    // around one control_height text row, so anything shorter
    // clips the line to its top sliver.
    const float hint_w =
        chrome_.font != nullptr
            ? chrome_.font->measure_text("the click counter")
            : 150.0f * ui_scale_;
    if (gui::begin_tooltip(
            gui_, layout_, chrome_, "hint",
            {gui_.input().mouse_x + 14.0f * ui_scale_,
             gui_.input().mouse_y + 18.0f * ui_scale_,
             hint_w + 2.0f * chrome_.padding,
             chrome_.control_height + 2.0f * chrome_.padding},
            gui_.is_hot(clicks_w))) {
        gui::text(layout_, chrome_, "the click counter",
                  -1.0f);
        gui::end_tooltip(gui_, layout_);
    }
    // The modal demo's opener: flips the bool begin_modal above reads
    // next frame (the veil latches then, starving every panel below
    // it - including this one).
    if (gui::button(gui_, layout_, chrome_, "veil",
                    120.0f * ui_scale_)) {
        show_modal_ = true;
    }
    gui::checkbox(gui_, layout_, chrome_,
                  "signal", show_signal_);
    gui::slider_float(gui_, layout_, chrome_,
                      "amplitude", amplitude_, 0.0f, 1.0f,
                      120.0f * ui_scale_);
    gui::slider_int(gui_, layout_, chrome_,
                    "samples", samples_, 4, kMaxSamples,
                    120.0f * ui_scale_);
    gui::slider_float(gui_, layout_, chrome_,
                      "spin", gui_spin_rate_, 0.0f, 0.2f,
                      120.0f * ui_scale_);
    // M19's combo, the same choice as one widget: the row owns the
    // placement the raw popup above leaves to the caller - the
    // dropdown it opens hangs past the panel's bottom edge, the clip
    // escape in daily wear, flipping above or clamping when the
    // frame lacks room - and the shared choice retints the quad from
    // either control. The two dropdowns close each other: a pick is
    // one answer, and a click outside one's bounds is the popup's
    // own outside-press close.
    if (gui::combo(gui_, layout_, chrome_, "tint", kMenuItems, 3,
                   menu_choice_, combo_open_,
                   120.0f * ui_scale_)) {
        menu_open_ = false;  // the choice moved: the menu follows suit
    }
    // M20's text input, the widget arc's editing rung: the buffer is the
    // caller's, the caret/selection state lives in the gui context keyed
    // by the row's id, and the frame clock spends its tick on the caret
    // blink. Ctrl+C/X/V ride the clipboard seam install_clipboard wired
    // at setup. Enter is deliberately not the widget's - the app reads
    // the focused id plus the key edge itself, so the row below stays
    // free for a caller rule; here that rule is an echo to the log.
    (void)gui::text_input(gui_, layout_, chrome_, "name", name_buf_,
                          static_cast<int>(sizeof(name_buf_)),
                          120.0f * ui_scale_);
    if (gui_.is_focused(gui_.ids().id("name")) &&
        gui_.input().key_pressed[ws::input::keys::kEnter]) {
        ws::core::info("playground: name \"{}\"", name_buf_);
    }
    // Clip proof: this quad pokes past the panel's bottom-right corner
    // by 40 px each way - the scissor must cut it at the content edge.
    // Its tint is the popup demo's choice (red/green/blue above).
    r2d::draw_quad(content.x + content.width - 8.0f,
                   content.y + content.height - 8.0f, 48.0f, 48.0f,
                   kMenuTints[static_cast<std::size_t>(menu_choice_)]);

    gui::end_panel(gui_, layout_);
}

void PlaygroundApp::draw_world_2d() {
    namespace r2d = ws::ui::r2d;
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
    // The net lab's world half, drawn last so its card sits over whatever
    // the demo collage puts underneath - the gui block earlier in this
    // frame already applied the panel's edits, and tick() above advanced
    // the training, so the picture reflects both with no lag.
    net_.draw_world();
    r2d::end();
}

void PlaygroundApp::draw_world_3d() {
    namespace r2d = ws::ui::r2d;
    // One begin(Camera3D) block = one kind: its chunks flush through
    // the depth-tested pipeline while everything above stays 2D
    // painter's order. The block is an epoch, so it renders after the
    // 2D world and under the screen border below.
    r2d::begin(camera3d_);
    // Ground grid on the world's z=0 plane - the same x/y extents
    // the 2D camera above shows, now with visible convergence.
    // Thickness is WORLD units here: ~2 px at the grid's depth.
    for (int gx = 0; gx <= 10; ++gx) {
        const float x = gx * 200.0f;
        r2d::draw_line(x, 0.0f, 0.0f, x, 1200.0f, 0.0f, 4.0f,
                       r2d::rgba(80, 80, 100));
    }
    for (int gy = 0; gy <= 6; ++gy) {
        const float y = gy * 200.0f;
        r2d::draw_line(0.0f, y, 0.0f, 2000.0f, y, 0.0f, 4.0f,
                       r2d::rgba(80, 80, 100));
    }
    // Occlusion proof - the whole point of the depth buffer. Both
    // billboards sit ON the eye->target axis (the red one at 2/3
    // of the way to the target), so they project to the same
    // screen point; red is nearer and must cover the overlap in
    // EITHER submission order. The far_first toggle flips the
    // order live: the picture must not change. Blue is wider so
    // it peeks out around red and makes the overlap visible.
    if (far_first_) {
        r2d::draw_quad(1000.0f, 600.0f, 0.0f, 700.0f, 700.0f,
                       r2d::rgba(80, 120, 255));
        r2d::draw_quad(1000.0f, 433.0f, -500.0f, 300.0f, 300.0f,
                       r2d::rgba(255, 60, 60));
    } else {
        r2d::draw_quad(1000.0f, 433.0f, -500.0f, 300.0f, 300.0f,
                       r2d::rgba(255, 60, 60));
        r2d::draw_quad(1000.0f, 600.0f, 0.0f, 700.0f, 700.0f,
                       r2d::rgba(80, 120, 255));
    }
    // Parallax set: a sprite billboard floating nearer than the
    // grid, and a world-space line running INTO the scene - its
    // visible part starts mid-frame and recedes toward the
    // vanishing point, all depth-test behavior.
    r2d::draw_sprite(1450.0f, 350.0f, -400.0f, 240.0f, 240.0f,
                     r2d::rgba(140, 200, 255),
                     icons_.empty() ? r2d::Sprite{} : icons_[2]);
    r2d::draw_line(1700.0f, 950.0f, -700.0f, 1700.0f, 950.0f, 500.0f,
                   24.0f, r2d::rgba(255, 200, 60));
    r2d::end();
}

void PlaygroundApp::draw_overlay() {
    namespace r2d = ws::ui::r2d;
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

    r2d::set_layer(5);
    touch_.draw_overlay();
    if (!typed_.empty()) {
        r2d::draw_text(280.0f * ui_scale_,
                       frame.height - 36.0f * ui_scale_, typed_.c_str(),
                       font_, r2d::rgba(235, 238, 242));
    }
    r2d::set_layer(0);
    r2d::end();
}

void PlaygroundApp::draw_imgui_debug() {
    if (ws::ui::begin_window("ws playground")) {
        ws::ui::text("shell is alive - every widget here goes through ws::ui");
        ws::ui::text("press D to toggle the demo window (events, not polling)");

        // Live proof of the gui input snapshot (Phase 1): raw input
        // straight from the shell, unaffected by these ImGui widgets.
        const ws::input::InputFrame& in = gui_.input();
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
        std::snprintf(line, sizeof(line),
                      "touches %d (trails, pinch-zoom, finger-scroll)",
                      in.touch_count);
        ws::ui::text(line);
        std::snprintf(line, sizeof(line),
                      "audio: %s (sound panel, bottom right)",
                      audio_.is_playing() ? "playing" : "idle");
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
        ws::ui::separator();
        ws::ui::text("3D camera (drives the perspective layer)");
        ws::ui::checkbox("show 3D", &show_3d_);
        ws::ui::slider_float("eye x", &camera3d_.eye[0], -2000.0f,
                             2000.0f);
        ws::ui::slider_float("eye y", &camera3d_.eye[1], -2000.0f,
                             2000.0f);
        ws::ui::slider_float("eye z", &camera3d_.eye[2], -4000.0f, 0.0f);
        ws::ui::slider_float("3D fov", &camera3d_.fov_y, 0.1f, 2.0f);
        ws::ui::checkbox("far billboard first (occlusion proof)",
                         &far_first_);
        ws::ui::text("atlas: the 4 icons right of the grid share one texture");

        // The r2d texture lifecycle, live: the stats block reads the
        // LAST frame's accounting (this frame has not flushed yet), and
        // the button walks the destroy/reload pair end to end - the
        // destroy kills the old handle on the spot (its slot's GPU
        // memory returns once the frames in flight have drained), the
        // reload bakes the file bytes into a fresh slot. Watch the
        // handle move on click and the live-texture count settle back
        // a few frames later, when the retired slot reaches the
        // freelist.
        ws::ui::separator();
        const ws::ui::r2d::Stats st = ws::ui::r2d::stats();
        std::snprintf(line, sizeof(line),
                      "r2d last frame: %u cmds, %u quads, %u chunks, %u verts, %u dropped",
                      st.commands, st.quads, st.chunks, st.vertices,
                      st.dropped_quads);
        ws::ui::text(line);
        std::snprintf(line, sizeof(line), "%u live textures, tier %s%s",
                      st.textures,
                      st.bindless ? "bindless" : "chunked",
                      st.update_after_bind ? ", update-after-bind" : "");
        ws::ui::text(line);
        std::snprintf(line, sizeof(line), "checker handle 0x%08x",
                      static_cast<unsigned>(checker_.id));
        ws::ui::text(line);
        if (ws::ui::button("destroy + reload checker")) {
            ws::ui::r2d::destroy_texture(checker_);
            checker_ = ws::ui::r2d::load_texture_from_memory(
                checker_bytes_.data(), checker_bytes_.size());
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
