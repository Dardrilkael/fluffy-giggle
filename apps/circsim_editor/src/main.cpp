// apps/circsim_editor - stage 3, the editor app (docs/CIRCSIM_PLAN.md).
//
// The host's whole job, the shell_example's shape: a document type
// (the .csim schematic whose every edit rides the undo stack), five
// panels (the schematic canvas, the symbol palette, the property grid,
// the problems list, the netlist view), the command set (the edit
// tools beside the shell's File/Window set) and a one-key settings
// schema. Everything else - menus, toolbar, docking, palette, toasts,
// recents, autosave/crash recovery, session restore, the ordered
// shutdown - is the shell's.
//
// The bare editor keys (R rotate, M mirror, W wire, L label, F fit,
// Delete, Esc) live on the shortcut resolver's PANEL rung: they answer
// only while the Schematic panel is the active panel, and the
// resolver's text rule keeps them quiet while a field holds the
// keyboard (the app reports the text_editing rung while the Properties
// panel saw a focused field last frame - one frame of settle, the same
// cadence every gui claim rides).

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <ws/app/command.hpp>
#include <ws/app/shortcut.hpp>
#include <ws/app/undo.hpp>
#include <ws/input/event.hpp>
#include <ws/input/input.hpp>
#include <ws/jobs/job_system.hpp>
#include <ws/platform/dialogs.hpp>
#include <ws/platform/locations.hpp>
#include <ws/platform/messagebox.hpp>
#include <ws/platform/paths.hpp>
#include <ws/settings/schema.hpp>
#include <ws/settings/settings.hpp>
#include <ws/ui/app.hpp>
#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/job_progress.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/ui/renderer2d.hpp>
#include <ws/ui/shell/errors.hpp>
#include <ws/ui/shell/shell.hpp>

#include "canvas_panel.hpp"
#include "editor.hpp"
#include "results_panel.hpp"
#include "side_panels.hpp"

#include <ws/circsim/overlay.hpp>

#ifdef _WIN32
#include <ws/platform/single_instance.hpp>
#endif

#ifdef __ANDROID__
// The android entry (the playground/chatter shape): native_app_glue's
// handle, the activity's package paths, and a CWD that lands somewhere
// writable. All behind the guard - a desktop TU never sees the NDK.
#include <unistd.h>

#include <android_native_app_glue.h>  // android_app + the android_main contract

#include "android_saf.hpp"  // the SAF bridge (the pickers + the write-through fs)

namespace {

// The glue handle android_main() received, read once by on_setup()
// (the shell needs it through AppConfig; the app TU keeps its own copy
// for the activity paths the locations build reads).
android_app* g_android_app = nullptr;

}  // namespace
#endif

namespace {

namespace gui = ws::ui::gui;
namespace shell = ws::ui::shell;
using gui::GuiContext;
using gui::LayoutCursor;

// The GLFW key codes the app's own chords ride (the workspace's plain
// int convention).
constexpr int kKeyR = 82;
constexpr int kKeyM = 77;
constexpr int kKeyW = 87;
constexpr int kKeyL = 76;
constexpr int kKeyF = 70;
constexpr int kKeyY = 89;  // Ctrl+Y - the standard redo chord
constexpr int kKeyZ = 90;
constexpr int kKeyC = 67;  // Ctrl+C - copy (the panel rung: a focused
                          // text field keeps its own selection chords)
constexpr int kKeyV = 86;  // Ctrl+V - paste
constexpr int kKeyD = 68;  // Ctrl+D - duplicate
constexpr int kKeyP = 80;  // P - the probe tool's arm key
constexpr int kF1 = 290;
// F8 = 297 (GLFW_KEY_F8): the problems walk's chord pair. Spelled
// here rather than ws::input::keys because the input lib only names
// the keys the GUI itself gives meaning to (the app TU's own rule,
// the kF1 beside it).
constexpr int kF8 = 297;
constexpr int kLeftSuper = 343;   // GLFW_KEY_LEFT_SUPER
constexpr int kRightSuper = 347;  // GLFW_KEY_RIGHT_SUPER

// --- the font faces, resolved at the platform boundary (the gallery's
// rule: the app picks them, the first candidate that exists answers).

#ifdef __ANDROID__
constexpr const char* kUiFontPath = "/system/fonts/Roboto-Regular.ttf";
constexpr const char* kExtraFontPath =
    "/system/fonts/NotoSansCJK-Regular.ttc";
constexpr const char* kMonoFontPath = "/system/fonts/DroidSansMono.ttf";
#elif defined(_WIN32)
constexpr const char* kUiFontPath = "C:/Windows/Fonts/segoeui.ttf";
constexpr const char* kExtraFontPath = "C:/Windows/Fonts/msyh.ttc";
constexpr const char* kMonoFontPath = "C:/Windows/Fonts/consola.ttf";
#else
// A posix dev box: dejavu (Debian/Fedora), then liberation (RH/
// Ubuntu). A missing face degrades to the fontless label bars by
// design - the app still builds and runs for a look.
constexpr const char* kUiFontPath =
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
constexpr const char* kExtraFontPath =
    "/usr/share/fonts/truetype/chinese/NotoSansSC-Regular.ttf";
constexpr const char* kMonoFontPath =
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";
#endif

[[nodiscard]] bool file_exists(const char* path) {
    return std::filesystem::exists(path);
}

[[nodiscard]] const char* font_or(const char* primary, const char* fallback) {
    return file_exists(primary) ? primary : fallback;
}

// The bake pair follows the ACTIVE chrome's brightness (the P1b rule):
// the dark stock's light-on-dark text wants the boldening pair, the
// light stock's dark-on-light text the near-identity one. The shell's
// theme is a live setting, so the fonts re-bake when it flips.
[[nodiscard]] ws::ui::r2d::FontBakeOptions bake_options_for(bool dark) {
    return dark ? ws::ui::r2d::FontBakeOptions::dark_chrome()
                : ws::ui::r2d::FontBakeOptions::light_chrome();
}

// The UI face: the BUNDLED Inter by default (the stock look, identical
// on every platform - no system font walk), with a path override for
// custom fonts: WS_FONT_PATH wins when it is set and loadable, the
// bundled face answers otherwise. The caller's options carry the
// theme-brightness pick.
[[nodiscard]] ws::ui::r2d::Font load_ui_font(
    float pixel_height, const ws::ui::r2d::FontBakeOptions& options) {
#if defined(_WIN32)
  char* custom = nullptr;
  size_t len = 0;
  _dupenv_s(&custom, &len, "WS_FONT_PATH");
  const char* custom_c = custom;
#else
  const char* custom_c = std::getenv("WS_FONT_PATH");
#endif
  if (custom_c) {
    ws::ui::r2d::Font loaded =
        ws::ui::r2d::load_font(custom_c, pixel_height, options);
    if (!loaded.empty()) {
#if defined(_WIN32)
      free(custom);
#endif
      return loaded;
    }
    // A dead override falls through to the bundled face.
  }
#if defined(_WIN32)
  free(custom);
#endif
  return ws::ui::r2d::load_default_font(pixel_height, options);
}

// The mono face, the same override ladder for the data runs (the
// netlist, the problems rows, the measurements): WS_MONO_FONT_PATH
// wins when it is set and loadable, and the BUNDLED JetBrains Mono
// answers otherwise - the stock look on every platform, the same rule
// load_ui_font gives the UI face (the pre-typography pass walked the
// system mono paths instead, so a box without one silently lost its
// columns).
[[nodiscard]] ws::ui::r2d::Font load_mono_font(
    float pixel_height, const ws::ui::r2d::FontBakeOptions& options) {
#if defined(_WIN32)
  char* custom = nullptr;
  size_t len = 0;
  _dupenv_s(&custom, &len, "WS_MONO_FONT_PATH");
  const char* custom_c = custom;
#else
  const char* custom_c = std::getenv("WS_MONO_FONT_PATH");
#endif
  if (custom_c) {
    ws::ui::r2d::Font loaded =
        ws::ui::r2d::load_font(custom_c, pixel_height, options);
    if (!loaded.empty()) {
#if defined(_WIN32)
      free(custom);
#endif
      return loaded;
    }
    // A dead override falls through to the bundled face.
  }
#if defined(_WIN32)
  free(custom);
#endif
  return ws::ui::r2d::load_default_mono_font(pixel_height, options);
}

// --- the app ----------------------------------------------------------------

class CircsimApp final : public ws::ui::App {
  public:
    explicit CircsimApp(ws::ui::AppConfig config)
        : ws::ui::App(std::move(config)) {}

    // The ordered exit: the shell's shutdown (jobs cancelled, panels
    // closed, state files written) runs while every member is still
    // live, then the final drain lands the last completions.
    ~CircsimApp() override {
        if (shell_ != nullptr) {
            (void)shell_->shutdown();
        }
        jobs_.drain();
    }

  protected:
    void on_setup() override {
        ws::core::warn("circsim: on_setup() starting");
        install_clipboard(gui_);
        ws::core::warn("circsim: install_clipboard done");
        install_job_system(jobs_);
        ws::core::warn("circsim: install_job_system done");

        // The faces (the shell_example's shape): the BUNDLED Inter
        // (regular + semibold) baked at the brightness pick, the CJK
        // carrier stacked under it, and the netlist panel's mono face
        // beside them.
        scale_ = ui_scale();
        ws::core::warn("circsim: ui_scale={}", scale_);
        rebuild_fonts();
        ws::core::warn("circsim: rebuild_fonts done");

        // The settings schema: the shell reads shell.theme itself.
        declare_settings();
        ws::core::warn("circsim: declare_settings done");

        // The shell.
        shell::ShellConfig config;
        config.app_name = "circsim editor";
#ifdef __ANDROID__
        // The activity's package dirs - the locations header's android
        // contract: the app TU installs LocationRoots (there is no
        // headless truth to compute; os_location refuses). internal-
        // DataPath is the app's private files dir (the shell's state
        // files, the autosaves), externalDataPath the user-visible one
        // (Documents answers it: "the folder saved files default to",
        // MTP-visible over USB when the SAF picker is not involved).
        const ws::platform::LocationRoots roots = [handle = g_android_app] {
            ws::platform::LocationRoots out;
            const char* const internal =
                handle != nullptr &&
                        handle->activity->internalDataPath != nullptr
                    ? handle->activity->internalDataPath
                    : ".";
            const char* const external =
                handle != nullptr &&
                        handle->activity->externalDataPath != nullptr
                    ? handle->activity->externalDataPath
                    : nullptr;
            const std::string app_dir =
                ws::platform::join_path(internal, "circsim editor");
            out.config = app_dir;
            out.data = app_dir;
            out.cache = ws::platform::join_path(app_dir, "cache");
            out.documents =
                external != nullptr ? std::string{external}
                                    : ws::platform::join_path(app_dir,
                                                              "Documents");
            out.temp = out.cache;
            return out;
        }();
        const ws::platform::Locations where{roots};
#else
        const ws::platform::Locations where{"circsim editor"};
#endif
        if (where.ensure(ws::platform::Location::data)) {
            const auto dir = where.get(ws::platform::Location::data);
            if (dir.has_value()) {
                config.config_dir =
                    ws::platform::join_path(dir.value(), "shell");
            }
        }
        if (config.config_dir.empty()) {
            config.config_dir = ".";  // the degenerate fallback
        }
        config.fs = &doc_fs();
#ifdef _WIN32
        dialogs_ = ws::platform::make_file_dialogs();
        config.dialogs = dialogs_.get();
#elif defined(__ANDROID__)
        // The SAF bridge's pickers, when init wired them; the
        // documented refusal otherwise (a failed bridge init means the
        // APK is broken - the Open/Save commands honestly no-op).
        if (ws::platform::FileDialogs* saf = editor::saf::dialogs()) {
            config.dialogs = saf;
        } else {
            dialogs_ =
                std::make_unique<ws::platform::UnavailableFileDialogs>();
            config.dialogs = dialogs_.get();
        }
#else
        // The posix dev box: the documented refusal (the workspace's
        // desktop is Windows; the Unavailable* class is the same answer
        // a failed SAF bridge gets).
        dialogs_ = std::make_unique<ws::platform::UnavailableFileDialogs>();
        config.dialogs = dialogs_.get();
#endif
        config.jobs = &jobs_;
        shell_ = std::make_unique<shell::AppShell>(config, commands_,
                                                   resolver_, *settings_);

        // The document factory: every open path is a .csim schematic.
        shell_->set_document_factory(
            [this](const std::string& path)
                -> ws::core::Result<std::unique_ptr<shell::Document>> {
                if (!path.empty()) {
                    const std::string lower = [&] {
                        std::string out(path);
                        std::transform(out.begin(), out.end(), out.begin(),
                                       [](unsigned char c) {
                                           return static_cast<char>(
                                               std::tolower(c));
                                       });
                        return out;
                    }();
                    // ends_with, not a sized compare: the stage-3 check
                    // compared the last 4 chars with ".csim" (5 chars)
                    // - never equal, so EVERY open and session restore
                    // was refused. Caught by the stage-4 visual gate.
                    if (!lower.ends_with(".csim")) {
                        return ws::core::unexpected<ws::core::Error>(
                            ws::core::make_error(
                                static_cast<std::int32_t>(
                                    shell::ShellError::document),
                                "circsim editor",
                                "'" + path + "' is not a .csim schematic"));
                    }
                }
                return std::make_unique<editor::SchematicDocument>(
                    path, doc_fs());
            });

        register_panels();
        register_commands();
        shell_->set_menu_specs({
            {"&File", "File"},
            {"&Edit", "Edit"},
            {"&Place", "Place"},
            {"&Circuit", "Circuit"},
            {"&View", "View"},
            {"&Help", "Help"},
        });
        shell_->set_toolbar_commands({
            "shell.new_document",
            "shell.open_document",
            "shell.save_document",
            "app.undo",
            "app.redo",
            "app.delete",
            "app.rotate_cw",
            "app.mirror",
            "app.wire",
            "circuit.erc",
            "circuit.run",
        });
        // The toolbar's icons: the chrome actions wear the built-in
        // set's standard glyphs (floppy, arrows, bin...) with their
        // name + chord in the tooltip; circuit.run keeps its text
        // button - Simulate is the one act that deserves the word.
        shell_->set_toolbar_icons({
            {"shell.new_document", gui::Icon::document},
            {"shell.open_document", gui::Icon::folder},
            {"shell.save_document", gui::Icon::save},
            {"app.undo", gui::Icon::undo},
            {"app.redo", gui::Icon::redo},
            {"app.delete", gui::Icon::trash},
            {"app.rotate_cw", gui::Icon::rotate},
            {"app.mirror", gui::Icon::mirror},
            {"app.wire", gui::Icon::wire},
            {"circuit.erc", gui::Icon::clipboard_check},
        });
        shell_->set_status_job_provider([this] { return status_jobs(); });

        // The editor wiring.
        editor_.shell = shell_.get();
        editor_.jobs = &jobs_;
        editor_.toasts = &shell_->toasts();
        editor_.font = font_.empty() ? nullptr : &font_;
        editor_.font_mono = font_mono_.empty() ? nullptr : &font_mono_;
        editor_.set_status = [this](std::string text) {
            shell_->set_status(std::move(text));
        };
        editor_.export_results = [this](bool json) { export_run(json); };
        // The Netlist panel's Copy button needs no wiring of its own:
        // it rides the gui's clipboard seam, which install_clipboard
        // above wired to this window (GLFW's set on desktop; Android's
        // reaches the ClipboardManager through the JNI - either way the
        // seam is installed, and the button answers).

        (void)shell_->startup();

        // First run: the default layout (a returning run restores what
        // it saved).
        if (shell_->panel_host().instances().empty()) {
            (void)shell_->panel_host().open(shell_->panels(), "canvas");
            (void)shell_->panel_host().open(shell_->panels(), "palette");
            (void)shell_->panel_host().open(shell_->panels(), "properties");
            (void)shell_->panel_host().open(shell_->panels(), "problems");
        }
        if (shell_->documents().count() == 0) {
            (void)shell_->documents().new_document();
        }
        watch_active_document(true);
    }

    // The files the OS dropped: straight into the shell (the drop IS an
    // open, the shell's own rule).
    void on_event(ws::input::Event& event) override {
        if (event.type() == ws::input::EventType::FileDropped) {
            const auto& dropped =
                static_cast<const ws::input::FileDroppedEvent&>(event);
            shell_->handle_dropped_files(dropped.paths);
        }
    }

    void on_update() override {
        ws::core::warn("circsim: on_update() starting");
        namespace r2d = ws::ui::r2d;

        // The autorun probe (debug): CIRCSIM_AUTORUN_SECS fires the
        // simulate command N seconds in, so the simulate path can be
        // reproduced headless - the same command the Ctrl+Enter chord
        // and the Circuit menu both ride. Harmless without the
        // variable set.
        if (!autorun_checked_) {
            autorun_checked_ = true;
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)  // the debug probe reads an env var
#endif
            if (const char* env = std::getenv("CIRCSIM_AUTORUN_SECS")) {
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
                autorun_secs_ = static_cast<float>(std::atof(env));
            }
            ws::core::warn("circsim: autorun {}",
                           autorun_secs_ > 0.0f ? "armed" : "off");
        }
        if (autorun_secs_ > 0.0f) {
            const auto now = std::chrono::steady_clock::now();
            if (autorun_start_.time_since_epoch().count() == 0) {
                autorun_start_ = now;
            } else if (std::chrono::duration<float>(now - autorun_start_)
                           .count() >= autorun_secs_) {
                autorun_start_ = now;
                ++autorun_fires_;
                ws::core::warn("circsim: autorun firing circuit.run #{}",
                               autorun_fires_);
                const ws::core::Status st = commands_.execute("circuit.run");
                ws::core::warn("circsim: autorun circuit.run #{} -> {}",
                               autorun_fires_,
                               st.has_value() ? "ok" : "failed");
            }
        }

        // The lazy CJK bake: a small batch per frame, both fonts.
        (void)r2d::font_bake_pending(font_, 8u);
        (void)r2d::font_bake_pending(font_mono_, 8u);
        ws::core::warn("circsim: font_bake_pending done");

        // The active document watch (a switch re-points the editor and
        // clears every per-document surface).
        watch_active_document(false);
        ws::core::warn("circsim: watch_active_document done");

        // The chord loop, before the frame (the gallery's rule): the
        // rung is the text_editing one while a field held the keyboard
        // last frame - the resolver's text rule then keeps the panel
        // rung's bare keys (R/M/W/L/F/Del) out of the field, while the
        // control-carrying chords (undo, run) still answer.
        const ws::input::InputFrame& frame_in = ws::input::frame_input();
        resolve_chords(frame_in);
        ws::core::warn("circsim: resolve_chords done");

        ws::core::warn("circsim: gui_.begin_frame");
        gui_.begin_frame(frame_in);
        ws::core::warn("circsim: r2d::begin");
        r2d::begin();
        ws::core::warn("circsim: r2d::frame_size");
        const r2d::FrameSize frame = r2d::frame_size();
        ws::core::warn("circsim: gui_.begin_layer");
        gui_.begin_layer(gui::Layer::base);
        ws::core::warn("circsim: shell_->theme().scaled");
        gui::ChromeTheme theme = shell_->theme().scaled(scale_);
        ws::core::warn("circsim: gui::is_dark");
        const bool dark = gui::is_dark(theme);
        ws::core::warn("circsim: dark={}", dark);
        if (dark != font_dark_) {
            font_dark_ = dark;
            rebuild_fonts();
        }
        theme.font = font_.empty() ? nullptr : &font_;
        theme.font_strong = font_strong_.empty() ? nullptr : &font_strong_;
        theme.font_mono = font_mono_.empty() ? nullptr : &font_mono_;
        ws::core::warn("circsim: shell_->draw");
        shell_->draw(gui_, theme,
                     gui::Rect{0.0f, 0.0f, frame.width, frame.height});
        ws::core::warn("circsim: gui_.end_layer");
        gui_.end_layer();
        ws::core::warn("circsim: r2d::end");
        r2d::end();
        ws::core::warn("circsim: r2d::set_layer(0)");
        r2d::set_layer(0);
        ws::core::warn("circsim: gui_.end_frame");
        gui_.end_frame();

        // The composed title, handed to the OS window when it changes.
        const std::string title = shell_->window_title();
        if (title != last_title_) {
            set_title(title);
            last_title_ = title;
        }

#ifdef __ANDROID__
        // A save whose push to the picked SAF location was refused:
        // the local save stood (the write-through seam's own policy),
        // so this is a warning toast, never an error one - drained
        // here, between frames, where the toast store is safe to touch.
        if (const std::optional<std::string> refused =
                editor::saf::take_push_error()) {
            editor_.toast("saved locally - the picked location refused "
                          "the write: " + *refused,
                          gui::ToastSeverity::warning);
        }
#endif
        ws::core::warn("circsim: on_update() done");
    }

  private:
    // The faces, re-baked on demand: the bundled Inter (regular + the
    // semibold HEADING bake, a step larger than the body - the font
    // hierarchy's own pair) at the brightness pick, the bundled
    // JetBrains Mono beside them, the past-ASCII carriers stacked under
    // each. A theme flip calls this again - a bake is a snapshot of its
    // options.
    void rebuild_fonts() {
        const ws::ui::r2d::FontBakeOptions bake = bake_options_for(font_dark_);
        font_ = load_ui_font(16.0f * scale_, bake);
        if (!font_.empty()) {
            // The primary face re-stacks for its own past-ASCII glyphs
            // (Latin-1 supplements), then the Han carrier under it.
            (void)ws::ui::r2d::font_add_face(
                font_, font_or(kUiFontPath, kExtraFontPath), 0);
            (void)ws::ui::r2d::font_add_face(font_, kExtraFontPath, 1);
        }
        // The heading face: the semibold at 18 px against the body's
        // 16 - "bold and slightly larger", the typography brief's own
        // hierarchy rung. gui::heading() follows whatever size this
        // bake carries; one font stays one (weight, size) pair.
        font_strong_ = ws::ui::r2d::load_default_font(
            ws::ui::r2d::DefaultFontWeight::semibold, 18.0f * scale_, bake);
        if (!font_strong_.empty()) {
            // The strong face answers the same carriers - and the same
            // face-0 fallback as font_ above (a box whose dejavu is
            // missing must not build a strong face whose Latin-1
            // supplement rides the CJK carrier instead: the two faces
            // would disagree about every past-ASCII glyph).
            (void)ws::ui::r2d::font_add_face(
                font_strong_, font_or(kUiFontPath, kExtraFontPath), 0);
            (void)ws::ui::r2d::font_add_face(font_strong_, kExtraFontPath, 1);
        }
        // The data face (netlist, problems, measurements): the bundled
        // JetBrains Mono through the same override ladder as the UI
        // face, the system mono stacked under it for its past-ASCII
        // glyphs, the Han carrier under that.
        font_mono_ = load_mono_font(15.0f * scale_, bake);
        if (!font_mono_.empty()) {
            (void)ws::ui::r2d::font_add_face(
                font_mono_, font_or(kMonoFontPath, kExtraFontPath), 0);
            (void)ws::ui::r2d::font_add_face(font_mono_, kExtraFontPath, 1);
        }
    }

    void declare_settings() {
        using ws::settings::SettingDesc;
        using ws::settings::SettingType;
        using ws::settings::SettingValue;
        SettingDesc theme{"shell.theme", SettingType::text,
                          SettingValue::text("dark"), {},
                          "the chrome: dark or light", "shell"};
        (void)schema_.declare(std::move(theme));
        settings_ = std::make_unique<ws::settings::SettingsStore>(schema_);
    }

    void register_panels() {
        // The schematic canvas - scoped_shortcuts is the point: the
        // panel rung is live while it is the active panel, which is
        // where the editor's bare keys bind.
        shell::PanelDesc canvas;
        canvas.id = "canvas";
        canvas.title = "Schematic";
        canvas.placement.area = shell::DockPlacement::Area::center;
        canvas.scoped_shortcuts = true;
        canvas.draw = [this](GuiContext& g, LayoutCursor& l,
                             const gui::ChromeTheme& t,
                             const shell::PanelInstance&) {
            editor::draw_canvas_panel(editor_, g, l, t);
        };
        (void)shell_->panels().add(std::move(canvas));

        shell::PanelDesc palette;
        palette.id = "palette";
        palette.title = "Palette";
        palette.placement.area = shell::DockPlacement::Area::left;
        palette.placement.weight = 0.14f;  // ~14% of the window
        palette.draw = [this](GuiContext& g, LayoutCursor& l,
                              const gui::ChromeTheme& t,
                              const shell::PanelInstance&) {
            editor::draw_palette_panel(editor_, g, l, t);
        };
        (void)shell_->panels().add(std::move(palette));

        shell::PanelDesc properties;
        properties.id = "properties";
        properties.title = "Properties";
        properties.placement.area = shell::DockPlacement::Area::right;
        // Of the canvas leaf (86% after the palette): ~24% overall,
        // leaving the schematic ~62%.
        properties.placement.weight = 0.28f;
        properties.draw = [this](GuiContext& g, LayoutCursor& l,
                                 const gui::ChromeTheme& t,
                                 const shell::PanelInstance&) {
            editor::draw_properties_panel(editor_, g, l, t);
        };
        (void)shell_->panels().add(std::move(properties));

        shell::PanelDesc problems;
        problems.id = "problems";
        problems.title = "Problems";
        problems.placement.area = shell::DockPlacement::Area::bottom;
        problems.placement.at_root = true;  // full width under all columns
        problems.placement.weight = 0.26f;
        problems.draw = [this](GuiContext& g, LayoutCursor& l,
                               const gui::ChromeTheme& t,
                               const shell::PanelInstance&) {
            editor::draw_problems_panel(editor_, g, l, t);
        };
        (void)shell_->panels().add(std::move(problems));

        shell::PanelDesc netlist;
        netlist.id = "netlist";
        netlist.title = "Netlist";
        netlist.placement.area = shell::DockPlacement::Area::center;  // tab
        // with Problems (the last leaf: the bottom strip)
        netlist.draw = [this](GuiContext& g, LayoutCursor& l,
                              const gui::ChromeTheme& t,
                              const shell::PanelInstance&) {
            editor::draw_netlist_panel(editor_, g, l, t);
        };
        (void)shell_->panels().add(std::move(netlist));

        shell::PanelDesc results;
        results.id = "results";
        results.title = "Results";
        results.placement.area = shell::DockPlacement::Area::center;  // tab
        results.draw = [this](GuiContext& g, LayoutCursor& l,
                             const gui::ChromeTheme& t,
                             const shell::PanelInstance&) {
            editor::draw_results_panel(editor_, g, l, t);
        };
        (void)shell_->panels().add(std::move(results));
    }

    // --- the command set ----------------------------------------------------

    void register_commands() {
        using ws::app::Command;
        using ws::app::Mod;

        const auto add = [this](Command command) {
            (void)commands_.add(std::move(command));
        };

        Command undo_cmd;
        undo_cmd.id = "app.undo";
        undo_cmd.label = "Undo";
        undo_cmd.category = "Edit";
        undo_cmd.shortcut = {kKeyZ, Mod::control};
        undo_cmd.enabled = [this] {
            return editor_.doc != nullptr && editor_.doc->undo().can_undo();
        };
        undo_cmd.execute = [this](std::string_view) {
            if (editor_.doc != nullptr) {
                (void)editor_.doc->undo().undo();
            }
        };
        add(std::move(undo_cmd));

        Command redo_cmd;
        redo_cmd.id = "app.redo";
        redo_cmd.label = "Redo";
        redo_cmd.category = "Edit";
        // Ctrl+Y, the standard redo chord (the declared default the
        // menus and the toolbar tooltip hint); Ctrl+Shift+Z rides
        // beside it as the alias below - the text editor's habit keeps
        // answering.
        redo_cmd.shortcut = {kKeyY, Mod::control};
        redo_cmd.enabled = [this] {
            return editor_.doc != nullptr && editor_.doc->undo().can_redo();
        };
        redo_cmd.execute = [this](std::string_view) {
            if (editor_.doc != nullptr) {
                (void)editor_.doc->undo().redo();
            }
        };
        add(std::move(redo_cmd));

        Command delete_cmd;
        delete_cmd.id = "app.delete";
        delete_cmd.label = "Delete Selection";
        delete_cmd.category = "Edit";
        // The bare editor keys carry no DECLARED shortcut: they bind on
        // the resolver's PANEL rung below (a declared chord would seed
        // the global rung through install_defaults, where it would
        // answer in every panel and fight the text rule).
        delete_cmd.enabled = [this] {
            return editor_.doc != nullptr &&
                   !editor_.selection_set().empty();
        };
        delete_cmd.execute = [this](std::string_view) { delete_selection(); };
        add(std::move(delete_cmd));

        // The group edit's trio. The chords bind on the PANEL rung
        // (like Del), so a focused text field keeps its own Ctrl+C/V -
        // the schematic's clipboard only answers while the Schematic
        // panel owns the keys.
        Command copy_cmd;
        copy_cmd.id = "app.copy";
        copy_cmd.label = "Copy";
        copy_cmd.category = "Edit";
        copy_cmd.enabled = [this] {
            return editor_.doc != nullptr &&
                   !editor_.selection_set().empty();
        };
        copy_cmd.execute = [this](std::string_view) {
            if (editor_.copy_selection()) {
                const auto& clip = editor_.clipboard;
                editor_.status("Copied " +
                               std::to_string(clip.parts.size() +
                                              clip.wires.size() +
                                              clip.junctions.size() +
                                              clip.labels.size()) +
                               " objects");
            }
        };
        add(std::move(copy_cmd));

        Command paste_cmd;
        paste_cmd.id = "app.paste";
        paste_cmd.label = "Paste";
        paste_cmd.category = "Edit";
        paste_cmd.enabled = [this] {
            return editor_.doc != nullptr && !editor_.clipboard.empty();
        };
        paste_cmd.execute = [this](std::string_view) {
            // The view center carries the paste: the clipboard's own
            // anchor rides it, so Ctrl+V lands the copy in sight (the
            // canvas's cursor-relative rule, parked where the eye is).
            const std::vector<editor::Selection> pasted =
                editor_.paste_clipboard(editor_.view.center);
            if (!pasted.empty()) {
                editor_.status("Pasted " + std::to_string(pasted.size()) +
                               " objects");
            }
        };
        add(std::move(paste_cmd));

        Command duplicate_cmd;
        duplicate_cmd.id = "app.duplicate";
        duplicate_cmd.label = "Duplicate";
        duplicate_cmd.category = "Edit";
        duplicate_cmd.enabled = [this] {
            return editor_.doc != nullptr &&
                   !editor_.selection_set().empty();
        };
        duplicate_cmd.execute = [this](std::string_view) {
            const std::vector<editor::Selection> pasted =
                editor_.duplicate_selection();
            if (!pasted.empty()) {
                editor_.status("Duplicated " +
                               std::to_string(pasted.size()) + " objects");
            }
        };
        add(std::move(duplicate_cmd));

        Command rotate_cw;
        rotate_cw.id = "app.rotate_cw";
        rotate_cw.label = "Rotate Clockwise";
        rotate_cw.category = "Edit";
        rotate_cw.enabled = [this] { return rotatable(); };
        rotate_cw.execute = [this](std::string_view) { rotate(90, "Rotate"); };
        add(std::move(rotate_cw));

        Command rotate_ccw;
        rotate_ccw.id = "app.rotate_ccw";
        rotate_ccw.label = "Rotate Counter-Clockwise";
        rotate_ccw.category = "Edit";
        rotate_ccw.enabled = [this] { return rotatable(); };
        rotate_ccw.execute = [this](std::string_view) {
            rotate(-90, "Rotate");
        };
        add(std::move(rotate_ccw));

        Command mirror_cmd;
        mirror_cmd.id = "app.mirror";
        mirror_cmd.label = "Mirror";
        mirror_cmd.category = "Edit";
        mirror_cmd.enabled = [this] { return rotatable(); };
        mirror_cmd.execute = [this](std::string_view) {
            if (editor_.tool == editor::Tool::Place) {
                editor_.ghost_mirror = !editor_.ghost_mirror;
                return;
            }
            const ws::circsim::Part* p = selected_part();
            if (p != nullptr) {
                editor_.transform_part(p->id, p->rot, !p->mirror, "Mirror");
            }
        };
        add(std::move(mirror_cmd));

        Command wire_cmd;
        wire_cmd.id = "app.wire";
        wire_cmd.label = "Place Wire";
        wire_cmd.category = "Place";
        wire_cmd.enabled = [this] { return editor_.doc != nullptr; };
        wire_cmd.execute = [this](std::string_view) {
            editor_.tool = editor::Tool::Wire;
            editor_.wire_draft = editor::Editor::WireDraft{};
        };
        add(std::move(wire_cmd));

        Command label_cmd;
        label_cmd.id = "app.label";
        label_cmd.label = "Place Net Label";
        label_cmd.category = "Place";
        label_cmd.enabled = [this] { return editor_.doc != nullptr; };
        label_cmd.execute = [this](std::string_view) {
            editor_.tool = editor::Tool::Label;
            editor_.wire_draft = editor::Editor::WireDraft{};
        };
        add(std::move(label_cmd));

        Command probe_cmd;
        probe_cmd.id = "app.probe";
        probe_cmd.label = "Probe Tool";
        probe_cmd.category = "Place";
        probe_cmd.enabled = [this] { return editor_.doc != nullptr; };
        probe_cmd.execute = [this](std::string_view) {
            editor_.tool = editor::Tool::Probe;
            editor_.wire_draft = editor::Editor::WireDraft{};
        };
        add(std::move(probe_cmd));

        Command escape_cmd;
        escape_cmd.id = "app.escape";
        escape_cmd.label = "Cancel Tool";
        escape_cmd.category = "Place";
        escape_cmd.execute = [this](std::string_view) {
            // The right click's staging: the in-flight box cancels
            // first, then the draft, the tool disarms, the selection
            // clears last.
            if (editor_.box.active) {
                editor_.box = editor::BoxSelect{};
            } else if (editor_.wire_draft.active) {
                editor_.wire_draft = editor::Editor::WireDraft{};
            } else if (editor_.tool != editor::Tool::Select) {
                editor_.tool = editor::Tool::Select;
            } else {
                editor_.sel.clear();
                editor_.multi.clear();
            }
        };
        add(std::move(escape_cmd));

        Command fit_cmd;
        fit_cmd.id = "view.fit";
        fit_cmd.label = "Zoom to Fit";
        fit_cmd.category = "View";
        fit_cmd.enabled = [this] { return editor_.schem() != nullptr; };
        fit_cmd.execute = [this](std::string_view) {
            const std::optional<ws::circsim::Bounds> b =
                editor_.content_bounds();
            if (b.has_value()) {
                editor_.view.fit(*b, 4.0);
            }
        };
        add(std::move(fit_cmd));

        Command erc_cmd;
        erc_cmd.id = "circuit.erc";
        erc_cmd.label = "Run ERC Check";
        erc_cmd.category = "Circuit";
        erc_cmd.enabled = [this] { return editor_.schem() != nullptr; };
        erc_cmd.execute = [this](std::string_view) {
            editor_.run_erc();
            editor_.status(editor_.problems.header);
        };
        add(std::move(erc_cmd));

        // The problems walk's pair (F8 / Shift+F8, the IDE-standard
        // next/previous finding). The declared chords seed the GLOBAL
        // rung through install_defaults, so they answer in every
        // panel - and the resolver's text rule keeps both out of a
        // focused field (a bare and a shift-only chord never fire
        // from below the text_editing rung), which is the chord
        // loop's own text_field_focused respect. The act is the
        // panel's step_problem: the same walk Prev/Next and a row
        // click ride, so the chords cannot drift from the buttons.
        Command next_finding;
        next_finding.id = "problems.next";
        next_finding.label = "Next Finding";
        next_finding.category = "View";
        next_finding.shortcut = {kF8, Mod::none};
        next_finding.enabled = [this] {
            return editor::problems_any_visible(editor_);
        };
        next_finding.execute = [this](std::string_view) {
            (void)editor::step_problem(editor_, true);
        };
        add(std::move(next_finding));

        Command prev_finding;
        prev_finding.id = "problems.previous";
        prev_finding.label = "Previous Finding";
        prev_finding.category = "View";
        prev_finding.shortcut = {kF8, Mod::shift};
        prev_finding.enabled = [this] {
            return editor::problems_any_visible(editor_);
        };
        prev_finding.execute = [this](std::string_view) {
            (void)editor::step_problem(editor_, false);
        };
        add(std::move(prev_finding));

        Command netlist_cmd;
        netlist_cmd.id = "circuit.netlist";
        netlist_cmd.label = "Show Netlist";
        netlist_cmd.category = "Circuit";
        netlist_cmd.enabled = [this] { return editor_.schem() != nullptr; };
        netlist_cmd.execute = [this](std::string_view) {
            editor_.refresh_netlist();
            if (!shell_->panel_host().is_open("netlist")) {
                (void)shell_->panel_host().open(shell_->panels(), "netlist");
            }
        };
        add(std::move(netlist_cmd));

        Command run_cmd;
        run_cmd.id = "circuit.run";
        run_cmd.label = "Simulate";
        run_cmd.category = "Circuit";
        run_cmd.shortcut = {ws::input::keys::kEnter, Mod::control};
        run_cmd.enabled = [this] {
            return editor_.schem() != nullptr && !run_job_.busy();
        };
        run_cmd.execute = [this](std::string_view) { submit_run(); };
        add(std::move(run_cmd));

        Command results_cmd;
        results_cmd.id = "circuit.results";
        results_cmd.label = "Show Results";
        results_cmd.category = "Circuit";
        results_cmd.enabled = [this] { return editor_.doc != nullptr; };
        results_cmd.execute = [this](std::string_view) {
            if (!shell_->panel_host().is_open("results")) {
                (void)shell_->panel_host().open(shell_->panels(), "results");
            }
        };
        add(std::move(results_cmd));

        Command csv_cmd;
        csv_cmd.id = "circuit.export_csv";
        csv_cmd.label = "Export Results CSV";
        csv_cmd.category = "Circuit";
        csv_cmd.enabled = [this] { return editor_.results.has; };
        csv_cmd.execute = [this](std::string_view) { export_run(false); };
        add(std::move(csv_cmd));

        Command json_cmd;
        json_cmd.id = "circuit.export_json";
        json_cmd.label = "Export Results JSON";
        json_cmd.category = "Circuit";
        json_cmd.enabled = [this] { return editor_.results.has; };
        json_cmd.execute = [this](std::string_view) { export_run(true); };
        add(std::move(json_cmd));

        Command about_cmd;
        about_cmd.id = "app.about";
        about_cmd.label = "About circsim editor";
        about_cmd.category = "Help";
        about_cmd.shortcut = {kF1, Mod::none};
        about_cmd.execute = [this](std::string_view) {
            // The message rides the editor's toast pair, whose
            // toast_text_ deque gives every toast its own stable
            // bytes (editor.hpp's borrowed-bytes contract): a second
            // About inside the first toast's life re-points nothing -
            // the app-owned about_text_ this replaced could be
            // reassigned under a still-live toast, dangling its
            // message.
            editor_.toast(
                "circsim editor - the ws::circsim stage-3 schematic "
                "editor over ws::ui::shell - panel keys (R rotate, M "
                "mirror, W wire, L label, P probe, F fit, Del, "
                "Ctrl+C/V/D copy/paste/duplicate) answer one frame "
                "after a text field gives the keyboard back: leave the "
                "field, then press",
                gui::ToastSeverity::info);
        };
        add(std::move(about_cmd));

        // The declared chords seed the GLOBAL rung (the control-carrying
        // set - undo/redo/run/about); the editor's bare keys bind on
        // the PANEL rung, where they answer only while the Schematic
        // panel is active and never inside a focused field.
        (void)resolver_.install_defaults(commands_,
                                         ws::app::KeyContext::global);
        // The redo alias: Ctrl+Shift+Z answers wherever Ctrl+Y does
        // (install_defaults seeded only the declared chord; a second
        // binding on the same command is the alias, not a conflict).
        (void)resolver_.bind({kKeyZ, Mod::control | Mod::shift}, "app.redo",
                             ws::app::KeyContext::global);
        const auto panel = [this](int key, const char* id) {
            (void)resolver_.bind({key, ws::app::Mod::none}, id,
                                 ws::app::KeyContext::panel);
        };
        panel(kKeyR, "app.rotate_cw");
        panel(ws::input::keys::kDelete, "app.delete");
        panel(kKeyM, "app.mirror");
        panel(kKeyW, "app.wire");
        panel(kKeyL, "app.label");
        panel(kKeyP, "app.probe");
        panel(kKeyF, "view.fit");
        panel(ws::input::keys::kEscape, "app.escape");
        // The clipboard trio carries control (the standard chords),
        // still on the PANEL rung: a focused text field keeps its own
        // copy/paste, the schematic's answers only while the canvas
        // owns the keys.
        (void)resolver_.bind({kKeyC, ws::app::Mod::control}, "app.copy",
                             ws::app::KeyContext::panel);
        (void)resolver_.bind({kKeyV, ws::app::Mod::control}, "app.paste",
                             ws::app::KeyContext::panel);
        (void)resolver_.bind({kKeyD, ws::app::Mod::control}, "app.duplicate",
                             ws::app::KeyContext::panel);
        // Shift+R (rotate ccw) carries shift, which the text rule lets
        // through at the panel rung only when no field is focused -
        // correct: it is not a typing key.
        (void)resolver_.bind({kKeyR, ws::app::Mod::shift}, "app.rotate_ccw",
                             ws::app::KeyContext::panel);
    }

    // --- the command bodies ---------------------------------------------------

    [[nodiscard]] const ws::circsim::Part* selected_part() const {
        if (editor_.doc == nullptr || editor_.schem() == nullptr ||
            editor_.sel.kind != editor::Selection::Kind::Part) {
            return nullptr;
        }
        return editor_.schem()->find_part(editor_.sel.id);
    }

    [[nodiscard]] bool rotatable() const {
        if (editor_.doc == nullptr) {
            return false;
        }
        return editor_.tool == editor::Tool::Place ||
               selected_part() != nullptr;
    }

    void rotate(int degrees, const char* name) {
        if (editor_.tool == editor::Tool::Place) {
            editor_.ghost_rot =
                ((editor_.ghost_rot + degrees) % 360 + 360) % 360;
            return;
        }
        const ws::circsim::Part* p = selected_part();
        if (p != nullptr) {
            const int rot = ((p->rot + degrees) % 360 + 360) % 360;
            editor_.transform_part(p->id, rot, p->mirror, name);
        }
    }

    void delete_selection() {
        if (editor_.doc == nullptr || editor_.schem() == nullptr) {
            return;
        }
        // The working set as ONE undo step: the box's own payoff. A
        // lone anchor keeps the single-entity path it always had (the
        // junction rule - a lone junction dies through its first wire
        // - lives there).
        const std::vector<editor::Selection> set =
            editor_.selection_set();
        if (set.size() > 1) {
            std::vector<std::uint32_t> parts;
            std::vector<std::uint32_t> wires;
            std::vector<std::uint32_t> labels;
            for (const editor::Selection& m : set) {
                switch (m.kind) {
                    case editor::Selection::Kind::Part:
                        parts.push_back(m.id);
                        break;
                    case editor::Selection::Kind::Wire:
                        wires.push_back(m.id);
                        break;
                    case editor::Selection::Kind::Label:
                        labels.push_back(m.id);
                        break;
                    case editor::Selection::Kind::Junction:
                    case editor::Selection::Kind::None:
                        break;  // a junction dies with its wires
                }
            }
            if (editor::push_delete_set(*editor_.doc, parts, wires,
                                        labels)) {
                editor_.sel.clear();
                editor_.multi.clear();
            }
            return;
        }
        switch (editor_.sel.kind) {
            case editor::Selection::Kind::Part:
                editor::push_delete_part(*editor_.doc, editor_.sel.id);
                break;
            case editor::Selection::Kind::Wire:
                editor::push_delete_wire(*editor_.doc, editor_.sel.id);
                break;
            case editor::Selection::Kind::Label:
                editor::push_delete_label(*editor_.doc, editor_.sel.id);
                break;
            case editor::Selection::Kind::Junction:
                // A junction delete is the first wire through it (the
                // properties panel's rule - a lone junction is nothing).
                // Find the id first: the command mutates the deque the
                // walk would still be holding.
                {
                    std::optional<std::uint32_t> through;
                    for (const ws::circsim::Wire& w :
                         editor_.schem()->wires()) {
                        const ws::circsim::JunctionRef* a =
                            std::get_if<ws::circsim::JunctionRef>(&w.a);
                        const ws::circsim::JunctionRef* b =
                            std::get_if<ws::circsim::JunctionRef>(&w.b);
                        if ((a != nullptr && a->junction == editor_.sel.id) ||
                            (b != nullptr && b->junction == editor_.sel.id)) {
                            through = w.id;
                            break;
                        }
                    }
                    if (through.has_value()) {
                        editor::push_delete_wire(*editor_.doc, *through);
                    }
                }
                break;
            case editor::Selection::Kind::None:
                return;
        }
        editor_.sel.clear();
        editor_.multi.clear();
    }

    // --- the simulate job -------------------------------------------------------

    void submit_run() {
        ws::core::warn("circsim: submit_run enter (busy={}, doc={})",
                       run_job_.busy(), editor_.doc != nullptr);
        if (run_job_.busy() || editor_.doc == nullptr) {
            ws::core::warn("circsim: submit_run early-out");
            return;
        }
        // The job owns a snapshot (the schematic and the symbol table
        // are value types; the run never sees the live document).
        ws::circsim::Schematic schem = editor_.doc->schem;
        ws::circsim::SymbolLibrary lib = editor_.library;
        ws::core::warn("circsim: submit_run snapshot copied");
        // The version THIS run was submitted against - the staleness
        // stamp the delivery must answer with. Reading the schematic's
        // version at delivery instead would bless every edit that
        // landed while the job was in flight: a run against a
        // schematic the user has already moved on from would show as
        // fresh. The snapshot rides the job's completion, so it is the
        // version the results and the Problems header both key to.
        const std::uint64_t submitted = editor_.schem()->version();
        run_progress_.reset();
        ws::jobs::JobCallbacks<ws::circsim::RunOutcome> callbacks =
            run_progress_.callbacks<ws::circsim::RunOutcome>();
        callbacks.on_complete =
            [this, submitted](ws::jobs::JobHandle<ws::circsim::RunOutcome>
                                  handle) {
                deliver_run(handle.take(), submitted);
            };
        run_job_ = jobs_.submit<ws::circsim::RunOutcome>(
            [schem = std::move(schem),
             lib = std::move(lib)](ws::jobs::JobContext&)
                -> ws::core::Result<ws::circsim::RunOutcome> {
                ws::core::warn("circsim: worker run_schematic enter");
                const auto r =
                    ws::circsim::run_schematic(schem, nullptr, {}, &lib);
                ws::core::warn("circsim: worker run_schematic done "
                               "(run_has={})",
                               r.run.has_value());
                return r;
            },
            ws::jobs::Priority::normal, {}, std::move(callbacks));
        ws::core::warn("circsim: submit_run submitted (busy={})",
                       run_job_.busy());
    }

    void deliver_run(ws::core::Result<ws::circsim::RunOutcome> result,
                     std::uint64_t submitted_version) {
        ws::core::warn("circsim: deliver_run enter (has_value={}, "
                       "version={})",
                       result.has_value(), submitted_version);
        if (editor_.doc == nullptr || !result.has_value()) {
            if (!result.has_value()) {
                editor_.toast("Simulation job failed: " +
                                  result.error().message,
                              gui::ToastSeverity::error);
            }
            return;
        }
        ws::circsim::RunOutcome& outcome = *result;
        // The problems carry both halves of a failure: the net diags
        // plus the mapped run error (run_schematic's own contract).
        // The run's net build and back-map go to BOTH caches: the
        // Problems panel's locate seam reads its copy, and the
        // Results cache keeps the originals the canvas paints from
        // (wire voltages, the LED glow) - an ERC pass can replace the
        // Problems copy without ever dimming the canvas.
        editor_.problems.nets = outcome.nets;
        editor_.problems.nets_valid = true;
        editor_.problems.map = outcome.map;
        editor_.problems.map_valid = true;
        const std::size_t count = outcome.problems.size();
        ws::core::warn("circsim: deliver_run problems={} run_has={}", count,
                       outcome.run.has_value());
        editor_.refresh_problems(
            true, std::move(outcome.problems),
            outcome.run.has_value() ? "run" : "run (failed)",
            submitted_version);
        ws::core::warn("circsim: deliver_run refresh_problems done");
        if (outcome.run.has_value()) {
            // The Results panel's half of the delivery: the run itself
            // (tables + measurements) keyed to the version it was
            // SUBMITTED against - the snapshot the job carried, not
            // the live document's version (an edit that landed mid-run
            // must leave the result stale) - the view reset to the
            // first table, and the panel opened - the simulate act
            // ends with the waveforms on screen.
            editor_.results.has = true;
            editor_.results.version = submitted_version;
            editor_.results.run = std::move(*outcome.run);
            // The canvas's own half: the run's net build and back-map
            // (the originals - the Problems cache holds its copy).
            editor_.results.nets = std::move(outcome.nets);
            editor_.results.map = std::move(outcome.map);
            editor_.playback = editor::Playback{};
            editor_.transport_scrub = false;
            editor_.results_scrub = false;  // the row's drag dies with the run
            // The park rides the sweep table's OWN axis (seconds for a
            // .tran, the swept source's end value for a .dc) - the
            // settled state, the reading an .op user means by "the
            // answer". One press plays the whole sweep across the
            // canvas.
            if (const ws::spice::Table* sweep =
                    editor_.canvas_sweep_table();
                sweep != nullptr) {
                const std::span<const double> axis =
                    ws::circsim::overlay_sweep_axis(*sweep);
                if (!axis.empty()) {
                    editor_.playback.t = axis.back();
                }
            }
            editor_.results.table = 0;
            editor_.results.visible_for = editor::ResultsCache::kNoTable;
            editor_.results.visible.clear();
            editor_.results.x_sorted_for = editor::ResultsCache::kNoTable;
            editor_.results.hover = -1;
            // A fresh run's waveforms start on the auto fit: the old
            // plot's zoom/pan window and any in-flight gesture belong
            // to numbers that no longer exist.
            editor_.results.view = editor::PlotView{};
            editor_.results.view_for = editor::ResultsCache::kNoTable;
            editor_.results.gesture = editor::PlotGesture{};
            ws::core::warn("circsim: deliver_run results set, opening panel");
            if (!shell_->panel_host().is_open("results")) {
                (void)shell_->panel_host().open(shell_->panels(), "results");
            }
            ws::core::warn("circsim: deliver_run panel open done");

            std::size_t rows = 0;
            for (const ws::spice::Table& t : editor_.results.run.results.tables) {
                rows += t.row_count();
            }
            char summary[192];
            std::snprintf(summary, sizeof summary,
                          "Simulation finished: %zu table%s, %zu row%s, %zu "
                          "measurement%s, %zu fourier%s",
                          editor_.results.run.results.tables.size(),
                          editor_.results.run.results.tables.size() == 1
                              ? ""
                              : "s",
                          rows, rows == 1 ? "" : "s",
                          editor_.results.run.measurements.size(),
                          editor_.results.run.measurements.size() == 1
                              ? ""
                              : "s",
                          editor_.results.run.fouriers.size(),
                          editor_.results.run.fouriers.size() == 1 ? "" : "s");
            editor_.toast(summary, gui::ToastSeverity::info);
            editor_.status(count == 0 ? summary
                                      : editor_.problems.header);
        } else {
            editor_.toast("Simulation failed: " +
                              outcome.run.error().message,
                          gui::ToastSeverity::error);
            editor_.status(editor_.problems.header);
        }
        ws::core::warn("circsim: deliver_run done");
    }

    // --- the export (the Results panel's buttons and the Circuit menu) ---

    // The path policy: the document's stem with the new extension,
    // beside the schematic it came from; an untitled document exports
    // to the working directory. The write rides the fs seam (atomic
    // by contract), and the body is the writers ws::spice already
    // owns - the same bytes the CLI's --format emits.
    void export_run(bool json) {
        if (!editor_.results.has || editor_.doc == nullptr) {
            return;
        }
        std::string stem = editor_.doc->path();
        if (stem.empty()) {
            stem = "circsim-results";
        } else {
            const std::size_t slash = stem.find_last_of("/\\");
            const std::size_t dot = stem.find_last_of('.');
            if (dot != std::string::npos &&
                (slash == std::string::npos || dot > slash)) {
                stem.resize(dot);
            }
            if (stem.empty() ||
                (slash != std::string::npos && dot == slash + 1u)) {
                stem = "circsim-results";
            }
        }
        const std::string path =
            stem + (json ? ".json" : ".csv");
        const std::string body =
            json ? ws::circsim::results_json(editor_.results.run)
                 : ws::spice::results_csv(editor_.results.run.results);
        const ws::core::Status done = doc_fs().write(path, body);
        if (done.has_value()) {
            editor_.toast("Results exported to " + path,
                          gui::ToastSeverity::info);
        } else {
            editor_.toast("Export failed: " + done.error().message,
                          gui::ToastSeverity::error);
        }
    }

    [[nodiscard]] std::vector<shell::AppShell::StatusJob> status_jobs() {
        std::vector<shell::AppShell::StatusJob> jobs;
        if (run_job_.busy()) {
            shell::AppShell::StatusJob row;
            row.label = "simulate";
            row.fraction = [this] { return run_progress_.fraction(); };
            row.message = [] { return "running"; };
            row.cancel = [this] { run_job_.request_cancel(); };
            jobs.push_back(std::move(row));
        }
        return jobs;
    }

    // --- the active-document watch ------------------------------------------------

    void watch_active_document(bool force) {
        shell::DocumentManager& docs = shell_->documents();
        const int at = docs.active_index();
        if (!force && at == watched_index_ && docs.count() == watched_count_) {
            return;
        }
        watched_index_ = at;
        watched_count_ = docs.count();

        // The old document's listener comes off first (the stack
        // outlives the watch, never the reverse).
        if (editor_.doc != nullptr && undo_listener_ != 0) {
            (void)editor_.doc->undo().remove_listener(undo_listener_);
            undo_listener_ = 0;
        }
        editor_.doc =
            dynamic_cast<editor::SchematicDocument*>(docs.active());
        editor_.document_changed();
        if (editor_.doc != nullptr) {
            const auto listener = editor_.doc->undo().add_listener([this] {
                // An undo/redo landing (or any edit) reloads the
                // Properties field rows next frame.
                editor_.bump_edit_epoch();
            });
            if (listener.has_value()) {
                undo_listener_ = *listener;
            }
        }
    }

    // --- the chord loop -------------------------------------------------------------

    void resolve_chords(const ws::input::InputFrame& frame) {
        if (shell_->palette_open) {
            return;  // the palette owns the keys while it is open
        }
        ws::app::Mod mods = ws::app::Mod::none;
        if (frame.key_down[ws::input::keys::kLeftShift] ||
            frame.key_down[ws::input::keys::kRightShift]) {
            mods = mods | ws::app::Mod::shift;
        }
        if (frame.key_down[ws::input::keys::kLeftControl] ||
            frame.key_down[ws::input::keys::kRightControl]) {
            mods = mods | ws::app::Mod::control;
        }
        if (frame.key_down[ws::input::keys::kLeftAlt] ||
            frame.key_down[ws::input::keys::kRightAlt]) {
            mods = mods | ws::app::Mod::alt;
        }
        if (frame.key_down[kLeftSuper] || frame.key_down[kRightSuper]) {
            mods = mods | ws::app::Mod::super_key;
        }
        static constexpr int kModifierKeys[] = {
            ws::input::keys::kLeftShift,   ws::input::keys::kRightShift,
            ws::input::keys::kLeftControl, ws::input::keys::kRightControl,
            ws::input::keys::kLeftAlt,     ws::input::keys::kRightAlt,
            kLeftSuper,                    kRightSuper,
        };
        // The text_editing rung while a Properties field held the
        // keyboard last frame (the panel sets the flag as it draws):
        // the resolver's text rule keeps the bare panel keys out of
        // the field while the control chords still answer.
        const ws::app::KeyContext rung =
            editor_.text_field_focused
                ? ws::app::KeyContext::text_editing
                : shell_->key_context();
        for (int key = 0; key < ws::input::InputFrame::kMaxKeys; ++key) {
            if (!frame.key_pressed[static_cast<std::size_t>(key)]) {
                continue;
            }
            bool is_modifier = false;
            for (const int mod_key : kModifierKeys) {
                if (key == mod_key) {
                    is_modifier = true;
                    break;
                }
            }
            if (is_modifier) {
                continue;
            }
            const ws::app::Resolution hit = resolver_.resolve({key, mods}, rung);
            if (hit.consumed && !hit.command.empty()) {
                const ws::core::Status done = commands_.execute(hit.command);
                if (!done.has_value()) {
                    // The editor's toast pair again, not an app-owned
                    // string: two different command errors inside one
                    // toast's life would re-point a shared buffer and
                    // dangle the earlier message's bytes.
                    editor_.toast(done.error().message,
                                  gui::ToastSeverity::error, 5.0f);
                }
            }
        }
    }

    // --- the members (order matters: the shell borrows them all) -------------------
    //
    // Destruction runs in REVERSE declaration order, so shell_ is
    // declared LAST: it is destroyed FIRST, while every object it
    // borrows is still alive. The shell's own fallback (~AppShell
    // calls shutdown() when the host never did) touches the borrowed
    // jobs (config.jobs), dialogs, settings, commands, resolver and fs
    // through raw pointers - with the old order (shell_ declared above
    // jobs_/dialogs_/editor_) those died first and the fallback rode
    // dangling pointers. The explicit ~CircsimApp shutdown above still
    // runs first (the ordered exit), but the member order is what
    // makes even the skipped-shutdown path safe. The same rule stacks
    // the rest: the fonts cover editor_ (whose font/font_mono point at
    // them), editor_ covers the shell's panel bodies (they draw through
    // it), and run_job_/run_progress_ sit after shell_ only because
    // their records are shared_ptr-owned and never talk back to the
    // system at destruction - jobs_ itself stays here, under shell_.

    ws::settings::StdFileSystem fs_;

    // The filesystem everything file-shaped rides: the SAF bridge's
    // write-through wrapper on android (a document picked through the
    // Storage Access Framework becomes a real sandbox file whose saves
    // push back to the picked place), this class's own std seam on
    // desktop - and on a failed bridge init too, the honest local-only
    // degrade (state files still write; the pickers refuse).
    [[nodiscard]] ws::settings::FileSystem& doc_fs() {
#ifdef __ANDROID__
        if (ws::settings::FileSystem* const saf = editor::saf::fs()) {
            return *saf;
        }
#endif
        return fs_;
    }

    ws::settings::Schema schema_;
    std::unique_ptr<ws::settings::SettingsStore> settings_;
    ws::app::CommandRegistry commands_;
    ws::app::ShortcutResolver resolver_;

    GuiContext gui_;

    ws::jobs::JobSystem jobs_{2};
    std::unique_ptr<ws::platform::FileDialogs> dialogs_;

    float scale_ = 1.0f;
    ws::ui::r2d::Font font_;
    ws::ui::r2d::Font font_strong_;  // the semibold sibling (headings)
    ws::ui::r2d::Font font_mono_;
    bool font_dark_ = true;  // the brightness the fonts are baked for
                             // (the stock default; a persisted light
                             // shell theme flips it on the first frame)

    editor::Editor editor_;

    std::unique_ptr<shell::AppShell> shell_;

    gui::JobProgress run_progress_;
    ws::jobs::JobHandle<ws::circsim::RunOutcome> run_job_{};

    std::string last_title_;
    int watched_index_ = -1;
    std::size_t watched_count_ = 0;
    ws::app::UndoStack::ListenerId undo_listener_ = 0;

    // The autorun probe (debug): fires circuit.run every N seconds.
    bool autorun_checked_ = false;
    int autorun_fires_ = 0;
    float autorun_secs_ = -1.0f;
    std::chrono::steady_clock::time_point autorun_start_{};
};

}  // namespace

#ifdef _WIN32
// The debug crash probe (temporary): on any unhandled exception, print
// the faulting address and the symbolized call tree to stderr - the
// VS-debugger call stack, headless. dbghelp loads at runtime, so the
// link line stays ws::-only. Removed once the simulate crash is fixed.
#include <windows.h>
#include <crtdbg.h>
#include <csignal>
#include <dbghelp.h>

namespace {

using RtlFn = WORD(__stdcall*)(ULONG, ULONG, void**, ULONG*);
using SymInitFn = BOOL(__stdcall*)(HANDLE, PCWSTR, BOOL);
using SymSetOptsFn = DWORD(__stdcall*)(DWORD);
using SymFromAddrFn =
    BOOL(__stdcall*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
using SymLineFn =
    BOOL(__stdcall*)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINEW64);
using SymBaseFn = DWORD64(__stdcall*)(HANDLE, DWORD64);

// The debug CRT's assertion funnel: _STL_VERIFY (iterator debugging)
// and every invalid-parameter call land here before the CRT fast-fails
// (which would bypass the filter below). Print the assert, then raise
// so crash_filter prints the call tree at the assert site.
void invalid_param_handler(const wchar_t* expr, const wchar_t* func,
                           const wchar_t* file, unsigned line, uintptr_t) {
    std::fprintf(stderr,
                 "=== INVALID PARAM: expr=%ls func=%ls file=%ls line=%u "
                 "===\n",
                 expr != nullptr ? expr : L"?",
                 func != nullptr ? func : L"?",
                 file != nullptr ? file : L"?", line);
    std::fflush(stderr);
    RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

void abort_handler(int) {
    std::fprintf(stderr, "=== ABORT ===\n");
    std::fflush(stderr);
    RaiseException(0xE0000002, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    static bool in_filter = false;
    if (in_filter) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    in_filter = true;
    std::fprintf(stderr,
                 "=== CRASH: code=0x%08lX addr=%p ===\n",
                 static_cast<unsigned long>(
                     info->ExceptionRecord->ExceptionCode),
                 info->ExceptionRecord->ExceptionAddress);
    if (info->ContextRecord != nullptr) {
        std::fprintf(stderr, "rip=%p rsp=%p rbp=%p\n",
                     reinterpret_cast<void*>(info->ContextRecord->Rip),
                     reinterpret_cast<void*>(info->ContextRecord->Rsp),
                     reinterpret_cast<void*>(info->ContextRecord->Rbp));
    }

    void* frames[40] = {};
    ULONG count = 0;
    const HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    const RtlFn walk = k32 == nullptr
                           ? nullptr
                           : reinterpret_cast<RtlFn>(GetProcAddress(
                                 k32, "RtlCaptureStackBackTrace"));
    if (walk != nullptr) {
        walk(0, 40, frames, &count);  // skip 0: frame 0 is this filter
    }

    const HMODULE dbg = LoadLibraryA("dbghelp.dll");
    if (dbg == nullptr) {
        std::fprintf(stderr, "crash probe: no dbghelp\n");
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const SymFromAddrFn from_addr = reinterpret_cast<SymFromAddrFn>(
        GetProcAddress(dbg, "SymFromAddrW"));
    const SymLineFn line_addr = reinterpret_cast<SymLineFn>(
        GetProcAddress(dbg, "SymGetLineFromAddrW64"));
    const SymBaseFn mod_base = reinterpret_cast<SymBaseFn>(
        GetProcAddress(dbg, "SymGetModuleBase64"));
    const DWORD64 fault =
        reinterpret_cast<DWORD64>(info->ExceptionRecord->ExceptionAddress);
    const HANDLE proc = GetCurrentProcess();
    if (mod_base != nullptr) {
        const DWORD64 fbase = mod_base(proc, fault);
        std::fprintf(stderr, "fault module base=%llX rva=%llX\n", fbase,
                     fbase != 0 ? fault - fbase : 0);
    }
    for (ULONG i = 0; i < count; ++i) {
        const DWORD64 pc = reinterpret_cast<DWORD64>(frames[i]);
        const DWORD64 base = mod_base != nullptr ? mod_base(proc, pc) : 0;
        std::fprintf(stderr, "frame %2lu: %p (module+%llX)",
                     static_cast<unsigned long>(i), frames[i],
                     base != 0 ? pc - base : pc);
        if (from_addr != nullptr) {
            char buf[sizeof(SYMBOL_INFO) + 256] = {};
            SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            if (from_addr(proc, pc, &disp, sym) != 0) {
                std::fprintf(stderr, "  %s+0x%llX", sym->Name,
                             static_cast<unsigned long long>(disp));
            }
        }
        if (line_addr != nullptr) {
            IMAGEHLP_LINEW64 line = {};
            line.SizeOfStruct = sizeof(IMAGEHLP_LINEW64);
            DWORD line_disp = 0;
            if (line_addr(proc, pc, &line_disp, &line) != 0 &&
                line.FileName != nullptr) {
                std::fprintf(stderr, "  [%ls:%lu]",
                             line.FileName,
                             static_cast<unsigned long>(line.LineNumber));
            }
        }
        std::fprintf(stderr, "\n");
    }
    std::fprintf(stderr, "=== CRASH END ===\n");
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

void crash_probe_install() {
    const HMODULE dbg = LoadLibraryA("dbghelp.dll");
    if (dbg != nullptr) {
        const SymSetOptsFn set_opts = reinterpret_cast<SymSetOptsFn>(
            GetProcAddress(dbg, "SymSetOptions"));
        const SymInitFn init =
            reinterpret_cast<SymInitFn>(GetProcAddress(dbg,
                                                       "SymInitializeW"));
        if (set_opts != nullptr) {
            set_opts(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS |
                     SYMOPT_LOAD_LINES);
        }
        if (init != nullptr) {
            init(GetCurrentProcess(), nullptr, TRUE);
        }
    }
    SetUnhandledExceptionFilter(&crash_filter);
    _set_invalid_parameter_handler(&invalid_param_handler);
    std::signal(SIGABRT, &abort_handler);
    ws::core::warn("circsim: crash probe installed");
}

}  // namespace
#endif  // _WIN32

#ifdef __ANDROID__

// Android entry (the playground/chatter shape): native_app_glue calls
// android_main() on the app's own thread and hands over the handle the
// shell needs (AppConfig::app_handle) and the locations build reads
// (the TU-static above). chdir to the activity's files dir so every
// CWD-relative stray - an untitled document's export writes
// "circsim-results.csv" beside the working directory - lands in a
// writable place; the shell's own state goes through LocationRoots,
// never the CWD.
extern "C" void android_main(android_app* app) {
    g_android_app = app;
    chdir(app->activity->internalDataPath);
    // The SAF bridge first: its filesystem and pickers must exist when
    // on_setup() wires the shell's config (a failed init degrades to
    // the refusing pickers and the local-only seam - the bridge logs
    // why, the app still runs).
    (void)editor::saf::init(app);
    ws::ui::AppConfig config;
    config.title = "circsim editor";
    config.app_handle = app;
    CircsimApp editor_app(std::move(config));
    (void)editor_app.run();
}

#else

int main() {
#ifdef _WIN32
    crash_probe_install();
    // The single-instance grab (the shell_example's rule; a posix dev
    // box has no grab TU and skips the nicety).
    const ws::platform::SingleInstance guard{"ws circsim editor"};
    if (!guard.is_primary()) {
        const std::unique_ptr<ws::platform::MessageBoxes> boxes =
            ws::platform::make_message_boxes();
        ws::platform::MessageBoxRequest request;
        request.kind = ws::platform::MessageBoxKind::warning;
        request.buttons = ws::platform::MessageBoxButtons::ok;
        request.title = "circsim editor";
        request.message =
            "another circsim editor is already running - this copy exits";
        (void)boxes->ask(request);
        return 1;
    }
#endif
    ws::ui::AppConfig config;
    config.title = "circsim editor";
    config.width = 1360;
    config.height = 820;
    CircsimApp app(std::move(config));
    return app.run();
}

#endif

