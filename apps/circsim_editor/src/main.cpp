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
#include <cctype>
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
#include "side_panels.hpp"

#ifdef _WIN32
#include <ws/platform/single_instance.hpp>
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
constexpr int kKeyZ = 90;
constexpr int kF1 = 290;
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
        install_clipboard(gui_);
        install_job_system(jobs_);

        // The faces: the UI face baked once at the ui scale with the
        // CJK carrier stacked under it, and the netlist panel's mono
        // face (the shell_example's stack shape).
        scale_ = ui_scale();
        font_ = ws::ui::r2d::load_font(font_or(kUiFontPath, kExtraFontPath),
                                       16.0f * scale_);
        if (!font_.empty()) {
            (void)ws::ui::r2d::font_add_face(
                font_, font_or(kUiFontPath, kExtraFontPath), 0);
            (void)ws::ui::r2d::font_add_face(font_, kExtraFontPath, 1);
        }
        font_mono_ = ws::ui::r2d::load_font(kMonoFontPath, 15.0f * scale_);
        if (!font_mono_.empty()) {
            (void)ws::ui::r2d::font_add_face(font_mono_, kMonoFontPath, 0);
            (void)ws::ui::r2d::font_add_face(font_mono_, kExtraFontPath, 1);
        }

        // The settings schema: the shell reads shell.theme itself.
        declare_settings();

        // The shell.
        shell::ShellConfig config;
        config.app_name = "circsim editor";
        const ws::platform::Locations where{"circsim editor"};
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
        config.fs = &fs_;
#ifdef _WIN32
        dialogs_ = ws::platform::make_file_dialogs();
#else
        // The posix dev box: the documented refusal (the workspace's
        // desktop is Windows; the Unavailable* class is the same answer
        // android gets).
        dialogs_ = std::make_unique<ws::platform::UnavailableFileDialogs>();
#endif
        config.dialogs = dialogs_.get();
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
                    if (lower.size() < 5u ||
                        lower.compare(lower.size() - 4u, 4u, ".csim") != 0) {
                        return ws::core::unexpected<ws::core::Error>(
                            ws::core::make_error(
                                static_cast<std::int32_t>(
                                    shell::ShellError::document),
                                "circsim editor",
                                "'" + path + "' is not a .csim schematic"));
                    }
                }
                return std::make_unique<editor::SchematicDocument>(path, fs_);
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
        namespace r2d = ws::ui::r2d;

        // The lazy CJK bake: a small batch per frame, both fonts.
        (void)r2d::font_bake_pending(font_, 8u);
        (void)r2d::font_bake_pending(font_mono_, 8u);

        // The active document watch (a switch re-points the editor and
        // clears every per-document surface).
        watch_active_document(false);

        // The chord loop, before the frame (the gallery's rule): the
        // rung is the text_editing one while a field held the keyboard
        // last frame - the resolver's text rule then keeps the panel
        // rung's bare keys (R/M/W/L/F/Del) out of the field, while the
        // control-carrying chords (undo, run) still answer.
        const ws::input::InputFrame& frame_in = ws::input::frame_input();
        resolve_chords(frame_in);

        gui_.begin_frame(frame_in);
        r2d::begin();
        const r2d::FrameSize frame = r2d::frame_size();
        gui_.begin_layer(gui::Layer::base);
        gui::ChromeTheme theme = shell_->theme().scaled(scale_);
        theme.font = font_.empty() ? nullptr : &font_;
        shell_->draw(gui_, theme,
                     gui::Rect{0.0f, 0.0f, frame.width, frame.height});
        gui_.end_layer();
        r2d::end();
        r2d::set_layer(0);
        gui_.end_frame();

        // The composed title, handed to the OS window when it changes.
        const std::string title = shell_->window_title();
        if (title != last_title_) {
            set_title(title);
            last_title_ = title;
        }
    }

  private:
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
        problems.draw = [this](GuiContext& g, LayoutCursor& l,
                               const gui::ChromeTheme& t,
                               const shell::PanelInstance&) {
            editor::draw_problems_panel(editor_, g, l, t);
        };
        (void)shell_->panels().add(std::move(problems));

        shell::PanelDesc netlist;
        netlist.id = "netlist";
        netlist.title = "Netlist";
        netlist.placement.area = shell::DockPlacement::Area::bottom;
        netlist.draw = [this](GuiContext& g, LayoutCursor& l,
                              const gui::ChromeTheme& t,
                              const shell::PanelInstance&) {
            editor::draw_netlist_panel(editor_, g, l, t);
        };
        (void)shell_->panels().add(std::move(netlist));
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
        redo_cmd.shortcut = {kKeyZ, Mod::control | Mod::shift};
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
                   editor_.sel.kind != editor::Selection::Kind::None;
        };
        delete_cmd.execute = [this](std::string_view) { delete_selection(); };
        add(std::move(delete_cmd));

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
                editor::push_transform_part(*editor_.doc, p->id, p->rot,
                                            !p->mirror, "Mirror");
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
        };
        add(std::move(label_cmd));

        Command escape_cmd;
        escape_cmd.id = "app.escape";
        escape_cmd.label = "Cancel Tool";
        escape_cmd.category = "Place";
        escape_cmd.execute = [this](std::string_view) {
            if (editor_.tool != editor::Tool::Select) {
                editor_.tool = editor::Tool::Select;
                editor_.wire_draft = editor::Editor::WireDraft{};
            } else {
                editor_.sel.clear();
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

        Command about_cmd;
        about_cmd.id = "app.about";
        about_cmd.label = "About circsim editor";
        about_cmd.category = "Help";
        about_cmd.shortcut = {kF1, Mod::none};
        about_cmd.execute = [this](std::string_view) {
            about_text_ =
                "circsim editor - the ws::circsim stage-3 schematic "
                "editor over ws::ui::shell";
            shell_->toasts().push({about_text_.c_str(),
                                   gui::ToastSeverity::info, nullptr, 4.0f});
        };
        add(std::move(about_cmd));

        // The declared chords seed the GLOBAL rung (the control-carrying
        // set - undo/redo/run/about); the editor's bare keys bind on
        // the PANEL rung, where they answer only while the Schematic
        // panel is active and never inside a focused field.
        (void)resolver_.install_defaults(commands_,
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
        panel(kKeyF, "view.fit");
        panel(ws::input::keys::kEscape, "app.escape");
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
            editor::push_transform_part(*editor_.doc, p->id, rot, p->mirror,
                                        name);
        }
    }

    void delete_selection() {
        if (editor_.doc == nullptr || editor_.schem() == nullptr) {
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
    }

    // --- the simulate job -------------------------------------------------------

    void submit_run() {
        if (run_job_.busy() || editor_.doc == nullptr) {
            return;
        }
        // The job owns a snapshot (the schematic and the symbol table
        // are value types; the run never sees the live document).
        ws::circsim::Schematic schem = editor_.doc->schem;
        ws::circsim::SymbolLibrary lib = editor_.library;
        run_progress_.reset();
        ws::jobs::JobCallbacks<ws::circsim::RunOutcome> callbacks =
            run_progress_.callbacks<ws::circsim::RunOutcome>();
        callbacks.on_complete =
            [this](ws::jobs::JobHandle<ws::circsim::RunOutcome> handle) {
                deliver_run(handle.take());
            };
        run_job_ = jobs_.submit<ws::circsim::RunOutcome>(
            [schem = std::move(schem),
             lib = std::move(lib)](ws::jobs::JobContext&)
                -> ws::core::Result<ws::circsim::RunOutcome> {
                return ws::circsim::run_schematic(schem, nullptr, {}, &lib);
            },
            ws::jobs::Priority::normal, {}, std::move(callbacks));
    }

    void deliver_run(ws::core::Result<ws::circsim::RunOutcome> result) {
        if (editor_.doc == nullptr || !result.has_value()) {
            if (!result.has_value()) {
                editor_.toast("simulation job failed: " +
                                  result.error().message,
                              gui::ToastSeverity::error);
            }
            return;
        }
        ws::circsim::RunOutcome& outcome = *result;
        // The problems carry both halves of a failure: the net diags
        // plus the mapped run error (run_schematic's own contract).
        editor_.problems.nets = std::move(outcome.nets);
        editor_.problems.nets_valid = true;
        editor_.problems.map = std::move(outcome.map);
        editor_.problems.map_valid = true;
        const std::size_t count = outcome.problems.size();
        editor_.refresh_problems(
            true, std::move(outcome.problems),
            outcome.run.has_value() ? "run" : "run (failed)");
        if (outcome.run.has_value()) {
            std::size_t rows = 0;
            for (const ws::spice::Table& t : outcome.run->results.tables) {
                rows += t.row_count();
            }
            char summary[160];
            std::snprintf(summary, sizeof summary,
                          "simulation ok: %zu table%s, %zu row%s, %zu "
                          "measurement%s",
                          outcome.run->results.tables.size(),
                          outcome.run->results.tables.size() == 1 ? ""
                                                                  : "s",
                          rows, rows == 1 ? "" : "s",
                          outcome.run->measurements.size(),
                          outcome.run->measurements.size() == 1 ? "" : "s");
            editor_.toast(summary, gui::ToastSeverity::info);
            editor_.status(count == 0 ? summary
                                      : editor_.problems.header);
        } else {
            editor_.toast("simulation failed: " +
                              outcome.run.error().message,
                          gui::ToastSeverity::error);
            editor_.status(editor_.problems.header);
        }
    }

    [[nodiscard]] std::vector<shell::AppShell::StatusJob> status_jobs() {
        std::vector<shell::AppShell::StatusJob> jobs;
        if (run_job_.busy()) {
            shell::AppShell::StatusJob row;
            row.label = "simulate";
            row.fraction = [this] { return run_progress_.fraction(); };
            row.message = [this] { return "running"; };
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
                    error_text_ = done.error().message;
                    shell_->toasts().push({error_text_.c_str(),
                                           gui::ToastSeverity::error,
                                           nullptr, 5.0f});
                }
            }
        }
    }

    // --- the members (order matters: the shell borrows them all) -------------------

    ws::settings::StdFileSystem fs_;

    ws::settings::Schema schema_;
    std::unique_ptr<ws::settings::SettingsStore> settings_;
    ws::app::CommandRegistry commands_;
    ws::app::ShortcutResolver resolver_;
    std::unique_ptr<shell::AppShell> shell_;

    GuiContext gui_;
    std::string last_title_;

    editor::Editor editor_;

    ws::jobs::JobSystem jobs_{2};
    std::unique_ptr<ws::platform::FileDialogs> dialogs_;
    gui::JobProgress run_progress_;
    ws::jobs::JobHandle<ws::circsim::RunOutcome> run_job_{};

    int watched_index_ = -1;
    std::size_t watched_count_ = 0;
    ws::app::UndoStack::ListenerId undo_listener_ = 0;

    float scale_ = 1.0f;
    ws::ui::r2d::Font font_;
    ws::ui::r2d::Font font_mono_;
    std::string about_text_;
    std::string error_text_;
};

}  // namespace

int main() {
#ifdef _WIN32
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
// Android is a later port (the touch interactions need their own
// pass); the app's CMake gate keeps it desktop-only, like
// shell_example.

