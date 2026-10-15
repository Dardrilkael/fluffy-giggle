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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

// The sound panel rides ws::audio on both platforms - the same OpenAL
// Soft pimpl everywhere, Oboe->AAudio under it on Android. Nothing audio
// is gated anymore; only the entry split at the bottom of this file is.
#include <numbers>
#include <ws/audio/engine.hpp>

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

// The top-right gui column's two panel rects, stated once: the gui block
// lays the panels there, and the pinch gate below treats those pixels as
// the widgets' - a two-finger pinch over the gui must not move the world.
// (The net panel's rect answers the same contract through
// NetScreen::panel_bounds().) Margins and sizes multiply by the ui scale
// (App::ui_scale(), captured at setup): the touch-density rule - the
// theme metrics grow by the same factor, so the scaled rects keep
// fitting the scaled widgets.
ws::ui::gui::Rect demo_panel_rect(float frame_width, float scale) {
    return {frame_width - 244.0f * scale, 24.0f * scale,
            220.0f * scale, 312.0f * scale};
}
ws::ui::gui::Rect pages_panel_rect(float frame_width, float scale) {
    return {frame_width - 244.0f * scale, 344.0f * scale,
            220.0f * scale, 220.0f * scale};
}

// The sound panel's rect, bottom-right and anchored to the frame the way
// the net screen's is bottom-left (net_screen.cpp) - the mirror position
// keeps it clear of the top-right demo column at any window size. The
// pinch gate below treats these pixels as the widgets', same contract.
ws::ui::gui::Rect sound_panel_rect(float frame_width, float frame_height,
                                   float scale) {
    return {frame_width - 244.0f * scale, frame_height - 152.0f * scale,
            220.0f * scale, 128.0f * scale};
}

// The demo panel's popup payload: three choices, one tint each - the
// clip-proof quad at the panel's corner wears the choice, so the menu's
// effect is visible outside the menu.
constexpr const char* kMenuItems[] = {"red", "green", "blue"};
constexpr std::uint32_t kMenuTints[] = {
    ws::ui::r2d::rgba(232, 80, 60), ws::ui::r2d::rgba(90, 200, 90),
    ws::ui::r2d::rgba(90, 150, 255)};

// The panel's payload: the wavplayer arpeggio synthesized in place (the
// SoundDesc path - the engine copies on load), so the demo ships zero
// audio files. C-major up and back down, 0.23 s per note, linear decay.
constexpr std::uint32_t kAudioRate = 44'100;  // Hz
constexpr std::uint32_t kAudioNoteFrames = kAudioRate * 23 / 100;
constexpr double kAudioMelody[] = {523.25, 659.25, 783.99, 1046.50,
                                   783.99, 659.25, 523.25};
constexpr auto kAudioNoteCount =
    sizeof(kAudioMelody) / sizeof(kAudioMelody[0]);

std::vector<std::int16_t> synth_arpeggio() {
    std::vector<std::int16_t> out(kAudioNoteCount * kAudioNoteFrames);
    for (std::size_t note = 0; note < kAudioNoteCount; ++note) {
        const double hz = kAudioMelody[note];
        for (std::uint32_t i = 0; i < kAudioNoteFrames; ++i) {
            const double t = static_cast<double>(i) / kAudioRate;
            const double decay =
                1.0 - static_cast<double>(i) / kAudioNoteFrames;
            out[note * kAudioNoteFrames + i] = static_cast<std::int16_t>(
                std::sin(2.0 * std::numbers::pi * hz * t) * decay * 12'000.0);
        }
    }
    return out;
}

// Trail colors keyed by contact id mod 4: a finger keeps one color for
// its whole gesture, two fingers never share one.
constexpr std::uint8_t kTrailPalette[4][3] = {
    {255, 150, 40},   // orange
    {90, 200, 110},   // green
    {80, 160, 255},   // blue
    {200, 110, 255},  // purple
};

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
        // One font, one bake (the v1 rule), at the display's density: the
        // bake height, the theme metrics and the panel rects below all
        // multiply by App::ui_scale() (display density / 160 on Android,
        // content scale on desktop) - a finger-sized gui on a phone, the
        // same pixels on a 100%-scaling desktop. A failed load degrades
        // to a fontless theme - the panels keep their v1 bars, never go
        // dark.
        ui_scale_ = ui_scale();
        net_.set_ui_scale(ui_scale_);
        font_ = ws::ui::r2d::load_font(kDefaultFontPath, 16.0f * ui_scale_);
        chrome_ = ws::ui::gui::kDefaultChrome.scaled(ui_scale_);
        chrome_.font = font_.empty() ? nullptr : &font_;
        // M20's clipboard seam: the gui's text editing (Ctrl+C/X/V) rides a
        // host-installed pair - GLFW's on desktop, deliberately unset on
        // Android (the trio no-ops there, the seam's documented answer).
        // on_setup is the natural point: the window exists, the gui
        // context is ours for the app's whole lifetime.
        install_clipboard(gui_);
        // The sound panel's engine, one for the app's lifetime (the
        // destructor tears it down). A failed device open is degradation,
        // not a dead app: the panel still draws, the play button refuses
        // with one warning, everything else runs untouched.
        if (auto init = audio_engine_.init(); init) {
            auto samples = synth_arpeggio();
            const ws::audio::SoundDesc desc{
                samples.data(), static_cast<std::uint32_t>(samples.size()),
                1, kAudioRate};
            if (auto sound = audio_engine_.load_sound(desc)) {
                arpeggio_ = *sound;
                audio_ok_ = true;
                ws::core::info("playground: audio device open");
            } else {
                ws::core::log_error(sound.error());
            }
        } else {
            ws::core::log_error(init.error());
        }
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
        // The text-event echo: one UTF-32 codepoint per glyph, arrival
        // order. The drawn echo (screen block below) keeps the bake's
        // range - ASCII 32..126, the v1 font has no other glyphs; wider
        // codepoints are observed here but not shown.
        dispatcher.dispatch<ws::ui::TextInputEvent>(
            [this](const ws::ui::TextInputEvent& e) {
                if (e.codepoint >= 32u && e.codepoint <= 126u) {
                    if (typed_.size() >= kTypedMax) {
                        typed_.erase(typed_.begin());
                    }
                    typed_.push_back(static_cast<char>(e.codepoint));
                }
                return false;  // observed, never consumed
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
        // consume it between begin_frame and end_frame.
        gui_.begin_frame(ws::ui::gui::frame_input());

        // A gui panel in screen space (framebuffer px, top-left origin),
        // drawn through r2d above the world quads. The panel widget draws
        // the chrome, clips its children to the content rect, scopes their
        // ids and flows them through the cursor. Every widget below drives
        // a real piece of app state - the same state the ImGui window's
        // controls below show - and the click counter is ours to keep.
        // The three sliders are the multi-pointer demo: on a touch screen
        // two fingers drag two of them at once - each slider follows its
        // own pointer, never the mouse. The gui block opens with a
        // begin_layer scope: the context drives r2d's sort layer AND the
        // hit-test precedence with the same number (M7's one-concept
        // layers), so the panels below draw and claim at Layer::base.
        r2d::begin();
        gui_.begin_layer(ws::ui::gui::Layer::base);
        const r2d::FrameSize gui_frame = r2d::frame_size();

        // M18's modal scope, chrome-first: begin_modal paints the theme's
        // veil over the whole frame and raises the blocking stratum -
        // the veil is frame-wide, it starts here, outlives end_modal, and
        // lifts at the next begin_frame - so opening it before the panels
        // below run is what makes them click-dead under it, on the very
        // frame the modal appears. `open` comes BY VALUE: the modal never
        // closes itself, the dialog's own button flips the caller's bool.
        // The panels' "veil" button opens it; "close" reopens the world.
        if (ws::ui::gui::begin_modal(
                gui_, layout_, chrome_, "confirm",
                {gui_frame.width * 0.5f - 140.0f * ui_scale_,
                 gui_frame.height * 0.5f - 60.0f * ui_scale_,
                 280.0f * ui_scale_, 120.0f * ui_scale_},
                show_modal_)) {
            ws::ui::gui::text(layout_, chrome_, "the modal veil", -1.0f);
            if (ws::ui::gui::button(gui_, layout_, chrome_, "close",
                                    -1.0f)) {
                show_modal_ = false;
            }
            ws::ui::gui::end_modal(gui_, layout_);  // the veil stays up
        }
        const ws::ui::gui::Rect content = ws::ui::gui::begin_panel(
            gui_, layout_, chrome_, "demo",
            demo_panel_rect(gui_frame.width, ui_scale_));
        // M18's popup demo, the combo idiom in miniature: the menu button
        // IS the anchor (a press on it while open reaches the button -
        // the anchor exemption keeps the popup, the caller's own toggle
        // closes it), the dropdown hangs below it over the rows there,
        // and the caller places both rects by hand - library-dumb
        // placement, the arithmetic the combo row at the panel's bottom
        // now owns. A choice closes the menu and retints the clip-proof
        // quad at the panel's corner.
        if (ws::ui::gui::button(gui_, layout_, chrome_, "menu",
                                120.0f * ui_scale_)) {
            menu_open_ = !menu_open_;
        }
        const ws::ui::gui::Rect menu_anchor{content.x, content.y,
                                           120.0f * ui_scale_,
                                           chrome_.control_height};
        const float menu_h = chrome_.padding * 2.0f +
                             chrome_.control_height * 3.0f +
                             chrome_.spacing * 2.0f;
        const ws::ui::gui::Rect menu_bounds{
            content.x, content.y + chrome_.control_height,
            140.0f * ui_scale_, menu_h};
        if (ws::ui::gui::begin_popup(gui_, layout_, chrome_, "menu",
                                     menu_anchor, menu_bounds,
                                     menu_open_)) {
            for (int i = 0; i < 3; ++i) {
                gui_.ids().push(i);  // one label, three ids
                if (ws::ui::gui::selectable(
                        gui_, layout_, chrome_,
                        kMenuItems[static_cast<std::size_t>(i)],
                        menu_choice_ == i, -1.0f)) {
                    menu_choice_ = i;
                    menu_open_ = false;  // a choice closes: caller's rule
                    combo_open_ = false;  // one choice, one open dropdown
                }
                gui_.ids().pop();
            }
            ws::ui::gui::end_popup(gui_, layout_);
        }
        const ws::ui::gui::WidgetId clicks_w = gui_.ids().id("clicks");
        if (ws::ui::gui::button(gui_, layout_, chrome_,
                                "clicks", 120.0f * ui_scale_,
                                icons_.empty() ? r2d::Sprite{}
                                               : icons_[0])) {
            ++gui_clicks_;
        }
        // M18's tooltip demo: the hint follows the cursor while the
        // clicks button is hot - chrome at the tooltip stratum that
        // claims nothing, so the button beneath keeps its hover and
        // its clicks.
        if (ws::ui::gui::begin_tooltip(
                gui_, layout_, chrome_, "hint",
                {gui_.input().mouse_x + 14.0f * ui_scale_,
                 gui_.input().mouse_y + 18.0f * ui_scale_,
                 150.0f * ui_scale_, chrome_.control_height},
                gui_.is_hot(clicks_w))) {
            ws::ui::gui::text(layout_, chrome_, "the click counter",
                              -1.0f);
            ws::ui::gui::end_tooltip(gui_, layout_);
        }
        // The modal demo's opener: flips the bool begin_modal above reads
        // next frame (the veil latches then, starving every panel below
        // it - including this one).
        if (ws::ui::gui::button(gui_, layout_, chrome_, "veil",
                                120.0f * ui_scale_)) {
            show_modal_ = true;
        }
        ws::ui::gui::checkbox(gui_, layout_, chrome_,
                              "signal", show_signal_);
        ws::ui::gui::slider_float(gui_, layout_, chrome_,
                                  "amplitude", amplitude_, 0.0f, 1.0f,
                                  120.0f * ui_scale_);
        ws::ui::gui::slider_int(gui_, layout_, chrome_,
                                "samples", samples_, 4, kMaxSamples,
                                120.0f * ui_scale_);
        ws::ui::gui::slider_float(gui_, layout_, chrome_,
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
        if (ws::ui::gui::combo(gui_, layout_, chrome_, "tint", kMenuItems, 3,
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
        (void)ws::ui::gui::text_input(gui_, layout_, chrome_, "name", name_buf_,
                                      static_cast<int>(sizeof(name_buf_)),
                                      120.0f * ui_scale_);
        if (gui_.is_focused(gui_.ids().id("name")) &&
            gui_.input().key_pressed[ws::ui::gui::keys::kEnter]) {
            ws::core::info("playground: name \"{}\"", name_buf_);
        }
        // Clip proof: this quad pokes past the panel's bottom-right corner
        // by 40 px each way - the scissor must cut it at the content edge.
        // Its tint is the popup demo's choice (red/green/blue above).
        r2d::draw_quad(content.x + content.width - 8.0f,
                       content.y + content.height - 8.0f, 48.0f, 48.0f,
                       kMenuTints[static_cast<std::size_t>(menu_choice_)]);
        ws::ui::gui::end_panel(gui_, layout_);

        // Phase 9: a tabbed page under the demo panel. The strip selects
        // (the caller's int is the state); "list" is a scrollable checkbox
        // column whose offset lives in the gui context - it survives tab
        // flips - and "tune" edits the list itself, live: shrinking the
        // row count shrinks the scroll range one frame later. On touch the
        // list scrolls by finger: past drag_threshold the scroll owns the
        // press (cancel_pointer), so sweeping rows never clicks them.
        const ws::ui::gui::Rect page = ws::ui::gui::begin_panel(
            gui_, layout_, chrome_, "pages",
            pages_panel_rect(gui_frame.width, ui_scale_));
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
        // The ws::audio demo, the net panel's mirror at bottom right. Play
        // is monophonic - it stops whatever is live so the sliders always
        // steer what you actually hear - and starts from the current slider
        // state. gain/pan write through every frame (update_audio below):
        // pan is world-space x, the listener at the origin facing -z, so
        // the source one unit ahead sweeps across the stereo field and its
        // distance attenuation rides along.
        const ws::ui::gui::Rect sound = ws::ui::gui::begin_panel(
            gui_, layout_, chrome_, "sound",
            sound_panel_rect(gui_frame.width, gui_frame.height, ui_scale_));
        if (ws::ui::gui::button(gui_, layout_, chrome_, "play",
                                sound.width)) {
            play_arpeggio();
        }
        // Full content width like the play button above (the pages/net
        // panels do the same): the default-120 rail under a full-width
        // button left the row's right side looking live but click-dead.
        ws::ui::gui::slider_float(gui_, layout_, chrome_, "gain",
                                  audio_gain_, 0.0f, 1.0f, sound.width);
        ws::ui::gui::slider_float(gui_, layout_, chrome_, "pan",
                                  audio_pan_, -4.0f, 4.0f, sound.width);
        ws::ui::gui::end_panel(gui_, layout_);

        gui_.end_layer();  // back to base: the world below re-owns layer 0
        r2d::end();
        r2d::set_layer(0);

        gui_.end_frame();

        // --- multi-touch demo: trails + pinch-zoom off the raw table ---
        // The plan's gesture rule in miniature: the core ships the touch
        // DATA (frame_input().touches, ids stable per contact), the app
        // composes the gesture. This pass is a second consumer of the same
        // snapshot the gui ran on - widgets and app-side gestures coexist
        // without arbitration.
        const ws::ui::gui::InputFrame& raw = ws::ui::gui::frame_input();
        update_touch_trails(raw);
        apply_pinch_zoom(raw);
        // The sound panel's per-frame pump: update() reaps the finished
        // source (clearing the handle below), then the live slider state
        // writes through to whatever is still playing.
        update_audio();

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

        // --- 3D world: the same grid, through the perspective camera ---
        // One begin(Camera3D) block = one kind: its chunks flush through
        // the depth-tested pipeline while everything above stays 2D
        // painter's order. The block is an epoch, so it renders after the
        // 2D world and under the screen border below.
        if (show_3d_) {
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

        // Multi-touch overlay, layer 5: over the world and this border,
        // under the gui panels (10). One fading breadcrumb per finger,
        // colored by contact id; the solid head marks the live contact,
        // and a lifted trail retracts one point per frame until gone.
        // Desktop has no touch table content (mouse-only by decision), so
        // the overlay draws nothing there - the demo is for the Android
        // run; the text echo below works everywhere keys exist.
        r2d::set_layer(5);
        for (const auto& entry : touch_trails_) {
            const TouchTrail& trail = entry.second;
            const std::uint8_t* pal = kTrailPalette[entry.first % 4u];
            const std::size_t n = trail.points.size();
            for (std::size_t i = 0; i < n; ++i) {
                const float toward_head =
                    static_cast<float>(i + 1) / static_cast<float>(n);
                const auto alpha =
                    static_cast<std::uint8_t>(48.0f + 207.0f * toward_head);
                r2d::draw_quad(trail.points[i].first - 5.0f,
                               trail.points[i].second - 5.0f, 10.0f, 10.0f,
                               r2d::rgba(pal[0], pal[1], pal[2], alpha));
            }
            if (trail.down && !trail.points.empty()) {
                r2d::draw_quad(trail.points.back().first - 9.0f,
                               trail.points.back().second - 9.0f, 18.0f, 18.0f,
                               r2d::rgba(pal[0], pal[1], pal[2], 230));
            }
        }
        // The text-event echo, bottom-center (right of the net panel):
        // the last few glyphs typed on either platform's key path. The
        // anchor scales by the same ui factor, keeping it clear of the
        // wider net panel on a dense screen.
        if (!typed_.empty()) {
            r2d::draw_text(280.0f * ui_scale_,
                           frame.height - 36.0f * ui_scale_, typed_.c_str(),
                           font_, r2d::rgba(235, 238, 242));
        }
        r2d::set_layer(0);
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
            std::snprintf(line, sizeof(line),
                          "touches %d (trails, pinch-zoom, finger-scroll)",
                          in.touch_count);
            ws::ui::text(line);
            std::snprintf(line, sizeof(line),
                          "audio: %s (sound panel, bottom right)",
                          audio_engine_.is_playing(audio_source_)
                              ? "playing"
                              : "idle");
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
    // The perspective half: eye above the world's top edge, target the
    // world center - the whole 2000x1200 grid lands in frame (45 deg fov)
    // with visible convergence. near 1 / far 5000 covers the demo span.
    ws::ui::r2d::Camera3D camera3d_{{1000.0f, 100.0f, -1500.0f},
                                   {1000.0f, 600.0f, 0.0f},
                                   {0.0f, -1.0f, 0.0f},
                                   0.7853982f, 1.0f, 5000.0f};
    bool show_3d_ = true;
    // The occlusion proof's live control: flips the billboards' submission
    // order so the depth test's invariance is watchable, not taken on faith.
    bool far_first_ = true;
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
    bool show_modal_ = false;      // M7: the Layer::modal veil demo's open flag
    bool menu_open_ = false;       // M18: the demo panel's popup menu
    int menu_choice_ = 0;          // its chosen tint (indexes kMenuTints)
    bool combo_open_ = false;      // M19: the combo row's dropdown (same tint)
    char name_buf_[32] = "playground";  // M20: the text input row's buffer
    std::array<bool, 16> rows_{};   // the "list" tab's checkbox states
    // Multi-touch demo state: per-contact breadcrumb trails, the pinch
    // bookkeeping, the text echo buffer. All app-side, the gui contract -
    // the core provides the data, the app composes the gesture.
    struct TouchTrail {
        static constexpr std::size_t kMaxPoints = 32;  // ~half a second at 60fps
        std::vector<std::pair<float, float>> points;  // oldest first
        bool down = false;  // the frame still sees the contact
    };
    static constexpr std::size_t kTypedMax = 40;
    std::map<std::uint32_t, TouchTrail> touch_trails_;
    float pinch_span_ = 0.0f;  // last pinch frame's finger span; 0 = none
    std::string typed_;        // the last few typed glyphs (ASCII 32..126)
    playground::NetScreen net_;    // Phase 10: the real screen (world + panel)
    // The platform's density factor, captured at setup: the font bake,
    // the theme metrics and every panel rect above multiply by it.
    float ui_scale_ = 1.0f;
    ws::ui::r2d::Font font_;      // one bake, one face - outlives the theme use
    ws::ui::gui::ChromeTheme chrome_ = ws::ui::gui::kDefaultChrome;  // + font_

    // --- ws::audio demo state (the sound panel, bottom right) ---
    ws::audio::AudioEngine audio_engine_;  // one per app: the ALC context is
                                           // process-global anyway
    ws::audio::SoundHandle arpeggio_{};    // the synthesized melody
    ws::audio::SourceHandle audio_source_{};  // the live one-shot, {} = none
    bool audio_ok_ = false;      // device open + sound loaded; play no-ops
    bool audio_warned_ = false;  // the one refusal log when play no-ops
    float audio_gain_ = 0.5f;    // live slider state, applied while playing
    float audio_pan_ = 0.0f;     // live slider state, the source's world x

    // The play button. Monophonic on purpose: stop whatever is live so the
    // sliders always steer the one source you hear, then start a fresh one
    // from the current slider state. A stop() failure here would mean the
    // bookkeeping above slipped - logged, never fatal.
    void play_arpeggio() {
        if (!audio_ok_) {
            if (!audio_warned_) {
                audio_warned_ = true;
                ws::core::warn("playground: play pressed, audio is disabled");
            }
            return;
        }
        if (audio_source_.valid()) {
            if (auto stopped = audio_engine_.stop(audio_source_); !stopped) {
                ws::core::log_error(stopped.error());
            }
            audio_source_ = {};
        }
        auto source = audio_engine_.play(arpeggio_, audio_gain_);
        if (!source) {
            ws::core::log_error(source.error());
            return;
        }
        audio_source_ = *source;
    }

    // The wavplayer wait loop's interactive twin, once per frame: update()
    // reaps the finished source, then the sliders write through to whatever
    // is still playing. A setter failure clears the handle instead of
    // logging the same error every frame.
    void update_audio() {
        if (!audio_ok_) {
            return;
        }
        if (auto updated = audio_engine_.update(); !updated) {
            ws::core::log_error(updated.error());
            return;
        }
        if (!audio_engine_.is_playing(audio_source_)) {
            audio_source_ = {};  // finished and reaped; {} until the next play
            return;
        }
        if (auto gain = audio_engine_.set_gain(audio_source_, audio_gain_);
            !gain) {
            ws::core::log_error(gain.error());
            audio_source_ = {};
            return;
        }
        if (auto pos = audio_engine_.set_position(audio_source_, audio_pan_,
                                                  0.0f, -1.0f);
            !pos) {
            ws::core::log_error(pos.error());
            audio_source_ = {};
        }
    }

    // --- multi-touch demo helpers (state above) ---
    // Appends each live contact's position to its trail (capped), and the
    // release frame's lift point closes it; a contact absent this frame
    // retracts one point per frame until its trail is gone.
    void update_touch_trails(const ws::ui::gui::InputFrame& in) {
        for (auto& entry : touch_trails_) {
            entry.second.down = false;  // re-marked by the frame below
        }
        for (int i = 0; i < in.touch_count; ++i) {
            const ws::ui::gui::TouchPoint& t =
                in.touches[static_cast<std::size_t>(i)];
            TouchTrail& trail = touch_trails_[t.id.value];
            if (t.down || t.released) {
                trail.points.emplace_back(t.x, t.y);
                if (trail.points.size() > TouchTrail::kMaxPoints) {
                    trail.points.erase(trail.points.begin());
                }
            }
            trail.down = t.down;
        }
        for (auto it = touch_trails_.begin(); it != touch_trails_.end();) {
            if (it->second.down) {
                ++it;
                continue;
            }
            if (!it->second.points.empty()) {
                it->second.points.erase(it->second.points.begin());
            }
            if (it->second.points.empty()) {
                it = touch_trails_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Two fingers off the gui panels = one pinch: the frame-to-frame span
    // between the first two live contacts scales camera_.zoom (clamped to
    // the window slider's range, so the two controls agree). A finger over
    // the gui is the widgets'; a third finger aborts the gesture until it
    // is two again.
    void apply_pinch_zoom(const ws::ui::gui::InputFrame& in) {
        const ws::ui::gui::TouchPoint* live[2] = {nullptr, nullptr};
        int downs = 0;
        for (int i = 0; i < in.touch_count; ++i) {
            const ws::ui::gui::TouchPoint& t =
                in.touches[static_cast<std::size_t>(i)];
            if (!t.down) {
                continue;
            }
            if (downs < 2) {
                live[downs] = &t;
            }
            ++downs;
        }
        const ws::ui::r2d::FrameSize frame = ws::ui::r2d::frame_size();
        bool pinching = downs == 2;
        if (pinching) {
            const ws::ui::gui::Rect panels[4] = {
                demo_panel_rect(frame.width, ui_scale_),
                pages_panel_rect(frame.width, ui_scale_),
                sound_panel_rect(frame.width, frame.height, ui_scale_),
                net_.panel_bounds()};
            for (const ws::ui::gui::TouchPoint* t : live) {
                for (const ws::ui::gui::Rect& panel : panels) {
                    if (panel.contains(t->x, t->y)) {
                        pinching = false;
                    }
                }
            }
        }
        if (!pinching) {
            pinch_span_ = 0.0f;  // re-armed by the next clean two-finger frame
            return;
        }
        const float span =
            std::hypot(live[1]->x - live[0]->x, live[1]->y - live[0]->y);
        if (pinch_span_ > 0.0f && span > 0.0f) {
            camera_.zoom = std::min(
                std::max(camera_.zoom * (span / pinch_span_), 0.1f), 4.0f);
        }
        pinch_span_ = span;
    }
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
