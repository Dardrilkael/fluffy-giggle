// apps/shell_example - the stage-15 acceptance app: a text-and-plot
// "scratchpad" built ONLY from AppShell. The host's whole job:
//
//   * a document type (a .txt scratchpad whose edits ride the undo
//     stack, whose dirty flag rides the save point),
//   * two panels - an Editor (the document's lines + an append row +
//     undo/redo state) and a Plot (a bar chart a background job
//     computes, with live settings),
//   * a few commands of its own (undo/redo/clear/about) beside the
//     shell's File/Window set,
//   * a settings schema (theme, plot points/amplitude) the shell's
//     built-in settings panel renders.
//
// Everything else - the menu bar with its automatic Window menu, the
// toolbar, the dock with tabbed leaves and draggable splitters, the
// command palette, toasts, the close-with-dirty prompt, recent files,
// autosave/crash recovery, session restore, the status bar with its
// job rows, the ordered shutdown - is the shell's, and this file never
// touches a widget directly.
//
// Build (Windows):  .\tools\build.ps1 -Preset debug -Target shell_example
// (needs the whole-workspace preset: the app links ws::ui's window
// layer; the spice-only presets do not configure ui).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
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
#include <ws/settings/filesystem.hpp>
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

#ifndef __ANDROID__
#include <ws/platform/single_instance.hpp>
#else
#include <unistd.h>
#endif

namespace {

namespace gui = ws::ui::gui;
namespace shell = ws::ui::shell;
using gui::GuiContext;
using gui::LayoutCursor;

// The GLFW key codes the app's own chords ride (the workspace's plain
// int convention).
constexpr int kKeyZ = 90;
constexpr int kF1 = 290;
constexpr int kLeftSuper = 343;   // GLFW_KEY_LEFT_SUPER
constexpr int kRightSuper = 347;  // GLFW_KEY_RIGHT_SUPER

// The system UI face - the APP picks it, not the ui lib (r2d takes
// caller-resolved paths; the gallery/bounce rule). A missing face
// degrades to the fontless v1 label bars everywhere, gracefully -
// which is exactly what this example showed before it loaded one.
#ifdef __ANDROID__
constexpr const char* kDefaultFontPath = "/system/fonts/Roboto-Regular.ttf";
#else
constexpr const char* kDefaultFontPath = "C:/Windows/Fonts/segoeui.ttf";
#endif

// The CJK carrier (the gallery's second-face rule): every codepoint
// past the inline bake resolves through the stack before the fallback
// box, so a real header's Chinese comment lines render instead of
// drawing tofu. A missing file refuses gracefully - the boxes stay,
// nothing else changes.
#ifdef __ANDROID__
constexpr const char* kExtraFontPath =
    "/system/fonts/NotoSansCJK-Regular.ttc";
#else
constexpr const char* kExtraFontPath = "C:/Windows/Fonts/msyh.ttc";
#endif

// The editor's monospace face. A missing face refuses and the editor
// falls back to the UI font - readable, just not column-aligned.
#ifdef __ANDROID__
constexpr const char* kMonoFontPath = "/system/fonts/DroidSansMono.ttf";
#else
constexpr const char* kMonoFontPath = "C:/Windows/Fonts/consola.ttf";
#endif

// The app's store dir: the OS data location on desktop (the gallery's
// platform_store_dir, the same shape), the CWD on android.
std::string platform_store_dir() {
#ifndef __ANDROID__
    const ws::platform::Locations where{"ws shell example"};
    if (where.ensure(ws::platform::Location::data)) {
        const ws::core::Result<std::string> dir =
            where.get(ws::platform::Location::data);
        if (dir.has_value()) {
            return dir.value();
        }
    }
#endif
    return ".";
}

// --- the document ----------------------------------------------------------

// The scratchpad: plain text, one '\n'-joined string. Edits are
// whole-content commands over it (the gallery's DocEdit shape: the
// inverse captured at push, the merge hook coalescing a typing run).
class ScratchDocument final : public shell::Document {
public:
    explicit ScratchDocument(std::string path) : path_(std::move(path)) {}

    [[nodiscard]] const std::string& path() const override { return path_; }
    void set_path(std::string path) override { path_ = std::move(path); }
    [[nodiscard]] std::string display_name() const override {
        if (path_.empty()) {
            return "untitled";
        }
        const std::size_t slash = path_.find_last_of("/\\");
        return slash == std::string::npos ? path_ : path_.substr(slash + 1);
    }
    [[nodiscard]] std::string doc_type() const override { return "scratch"; }

    [[nodiscard]] ws::core::Status load() override {
        std::ifstream in(path_, std::ios::binary);
        if (!in) {
            return ws::core::unexpected(ws::core::make_error(
                static_cast<std::int32_t>(shell::ShellError::document),
                "scratchpad", "cannot open '" + path_ + "'"));
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        content = buf.str();
        return {};
    }
    [[nodiscard]] ws::core::Status save() override {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        if (!out) {
            return ws::core::unexpected(ws::core::make_error(
                static_cast<std::int32_t>(shell::ShellError::document),
                "scratchpad", "cannot write '" + path_ + "'"));
        }
        out.write(content.data(),
                  static_cast<std::streamsize>(content.size()));
        return {};
    }

    [[nodiscard]] ws::app::UndoStack& undo() override { return undo_; }
    [[nodiscard]] std::uint64_t job_group() const override { return 0; }
    [[nodiscard]] std::string autosave_content() const override {
        return content;
    }
    void adopt_autosave_content(std::string_view bytes) override {
        content = bytes;
    }

    std::string content;
    ws::app::UndoStack undo_{100, 1 << 20};

private:
    std::string path_;
};

// One whole-content edit. apply() lands `next`, undo() restores the
// captured inverse; merge_with coalesces a same-kind run so one undo
// reverts the run (the typing rule the gallery's DocEdit set).
class ScratchEdit final : public ws::app::UndoCommand {
public:
    ScratchEdit(ScratchDocument* document, std::string was,
                std::string next, std::string kind)
        : document_(document),
          was_(std::move(was)),
          next_(std::move(next)),
          kind_(std::move(kind)) {}

    void apply() override { document_->content = next_; }
    void undo() override { document_->content = was_; }
    [[nodiscard]] std::string name() const override { return kind_; }
    [[nodiscard]] bool merge_with(const ws::app::UndoCommand& newer) override {
        const auto* edit = dynamic_cast<const ScratchEdit*>(&newer);
        if (edit == nullptr || kind_ != edit->kind_ || kind_ != "Typing") {
            return false;
        }
        next_ = edit->next_;  // the run's end; the run's start stands
        return true;
    }
    [[nodiscard]] std::size_t memory_estimate() const override {
        return sizeof(*this) + was_.size() + next_.size();
    }

private:
    ScratchDocument* document_;
    std::string was_;
    std::string next_;
    std::string kind_;
};

// --- the app ----------------------------------------------------------------

class ScratchApp final : public ws::ui::App {
public:
    explicit ScratchApp(ws::ui::AppConfig config)
        : ws::ui::App(std::move(config)) {}

    // The ordered exit: the shell's shutdown (jobs cancelled, panels
    // closed, state files written) runs while every member is still
    // live, then the final drain lands the last completions (the
    // gallery's rule - the marshal queue must not die holding them).
    ~ScratchApp() override {
        if (shell_ != nullptr) {
            (void)shell_->shutdown();
        }
        jobs_.drain();
    }

protected:
    void on_setup() override {
        install_clipboard(gui_);
        install_job_system(jobs_);

        // The one look asset (bounce's rule): the system face, baked
        // once at the ui scale, with the CJK carrier stacked under it
        // (the gallery's growing-font rule) so foreign text renders.
        // The shell's stock themes are constexpr and can never carry
        // the pointer, so the host installs it on the theme it hands
        // draw() each frame - without it every label is the fontless
        // v1 bar placeholder.
        scale_ = ui_scale();
        font_ = ws::ui::r2d::load_font(kDefaultFontPath, 16.0f * scale_);
        if (!font_.empty()) {
            // The primary face re-stacks for its own past-ASCII glyphs
            // (Latin-1 supplements), then the Han carrier under it.
            (void)ws::ui::r2d::font_add_face(font_, kDefaultFontPath, 0);
            (void)ws::ui::r2d::font_add_face(font_, kExtraFontPath, 1);
        }
        // The editor's mono face, same stack shape: consola for the
        // code, msyh for the comments consola cannot carry.
        font_mono_ = ws::ui::r2d::load_font(kMonoFontPath, 15.0f * scale_);
        if (!font_mono_.empty()) {
            (void)ws::ui::r2d::font_add_face(font_mono_, kMonoFontPath, 0);
            (void)ws::ui::r2d::font_add_face(font_mono_, kExtraFontPath, 1);
        }

        // The settings schema: the shell reads shell.theme itself; the
        // plot keys are the plot panel's own, rendered by the shell's
        // built-in settings panel (the property grid).
        declare_settings();

        // The shell: config + the host-owned registry/resolver/store.
        shell::ShellConfig config;
        config.app_name = "scratchpad";
        config.config_dir =
            ws::platform::join_path(store_dir_, "shell-example");
        config.fs = &fs_;
        dialogs_ = ws::platform::make_file_dialogs();
        config.dialogs = dialogs_.get();
        config.jobs = &jobs_;
        shell_ = std::make_unique<shell::AppShell>(config, commands_,
                                                   resolver_, *settings_);

        // The document factory: every open path is a scratchpad.
        shell_->set_document_factory(
            [](const std::string& path)
                -> ws::core::Result<std::unique_ptr<shell::Document>> {
                return std::make_unique<ScratchDocument>(path);
            });

        register_panels();
        register_commands();
        shell_->set_menu_specs({
            {"&File", "File"},
            {"&Edit", "Edit"},
            {"&View", "Window"},
            {"&Help", "Help"},
        });
        shell_->set_toolbar_commands({
            "shell.new_document",
            "shell.open_document",
            "shell.save_document",
            "app.undo",
            "app.redo",
        });
        shell_->set_status_job_provider([this] { return status_jobs(); });

        (void)shell_->startup();

        // First run: open the default panels (the layout/session files
        // were empty; a returning run restores what it saved).
        if (shell_->panel_host().instances().empty()) {
            (void)shell_->panel_host().open(shell_->panels(), "editor");
            (void)shell_->panel_host().open(shell_->panels(), "plot");
        }
        if (shell_->documents().count() == 0) {
            (void)shell_->documents().new_document();
        }
        submit_plot_job();
    }

    // The files the OS dropped: straight into the shell (the drop
    // IS an open, the shell's own rule).
    void on_event(ws::input::Event& event) override {
        if (event.type() == ws::input::EventType::FileDropped) {
            const auto& dropped =
                static_cast<const ws::input::FileDroppedEvent&>(event);
            shell_->handle_dropped_files(dropped.paths);
        }
    }

    void on_update() override {
        namespace r2d = ws::ui::r2d;

        // The lazy CJK bake: a small batch per frame, both fonts, so
        // the first foreign-text frame never hitches (the gallery's
        // amortized cadence).
        (void)r2d::font_bake_pending(font_, 8u);
        (void)r2d::font_bake_pending(font_mono_, 8u);

        // The chord loop, before the frame (the gallery's rule): the
        // shell says the rung (the editor panel declared scoped
        // shortcuts, so its bindings win while it is active).
        const ws::input::InputFrame& frame_in = ws::input::frame_input();
        resolve_chords(frame_in);

        gui_.begin_frame(frame_in);
        r2d::begin();
        const r2d::FrameSize frame = r2d::frame_size();
        gui_.begin_layer(gui::Layer::base);
        // The shell's live theme (dark or light, from settings) plus
        // what the stock constants cannot carry: the density scale
        // and the font pointer. The per-frame copy is the contract's
        // shape (theme.hpp: the theme does NOT own the font; scaled()
        // carries the pointer as-is, the app loads the bigger bake).
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

        SettingDesc theme{
            "shell.theme",
            SettingType::text,
            SettingValue::text("dark"),
            {},
            "the chrome: dark or light",
            "shell"};
        (void)schema_.declare(std::move(theme));

        SettingDesc points{
            "plot.points",
            SettingType::integer,
            SettingValue::integer(64),
            {ws::settings::RangeConstraint{
                16.0, 2048.0, ws::settings::ConstraintAction::clamp}},
            "samples the plot job computes",
            "plot"};
        (void)schema_.declare(std::move(points));

        SettingDesc amplitude{
            "plot.amplitude",
            SettingType::real,
            SettingValue::real(1.0),
            {ws::settings::RangeConstraint{
                0.1, 4.0, ws::settings::ConstraintAction::clamp}},
            "the damped sine's peak",
            "plot"};
        (void)schema_.declare(std::move(amplitude));

        settings_ = std::make_unique<ws::settings::SettingsStore>(schema_);
    }

    void register_panels() {
        // The editor: the document's lines, an append row, the undo
        // state. scoped_shortcuts is set so the panel rung is live
        // while it is focused (the shell reports it; this app binds
        // nothing at that rung, but the seam is the point).
        shell::PanelDesc editor;
        editor.id = "editor";
        editor.title = "Editor";
        editor.placement.area = shell::DockPlacement::Area::left;
        editor.scoped_shortcuts = true;
        editor.draw = [this](GuiContext& g, LayoutCursor& l,
                             const gui::ChromeTheme& t,
                             const shell::PanelInstance&) {
            draw_editor(g, l, t);
        };
        (void)shell_->panels().add(std::move(editor));

        // The plot: the job's samples as bars, a regenerate row.
        shell::PanelDesc plot;
        plot.id = "plot";
        plot.title = "Plot";
        plot.placement.area = shell::DockPlacement::Area::right;
        plot.draw = [this](GuiContext& g, LayoutCursor& l,
                           const gui::ChromeTheme& t,
                           const shell::PanelInstance&) {
            draw_plot(g, l, t);
        };
        (void)shell_->panels().add(std::move(plot));
    }

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
            shell::Document* document = shell_->documents().active();
            return document != nullptr && document->undo().can_undo();
        };
        undo_cmd.execute = [this](std::string_view) {
            if (shell::Document* document = shell_->documents().active()) {
                (void)document->undo().undo();
            }
        };
        add(std::move(undo_cmd));

        Command redo_cmd;
        redo_cmd.id = "app.redo";
        redo_cmd.label = "Redo";
        redo_cmd.category = "Edit";
        redo_cmd.shortcut = {kKeyZ, Mod::control | Mod::shift};
        redo_cmd.enabled = [this] {
            shell::Document* document = shell_->documents().active();
            return document != nullptr && document->undo().can_redo();
        };
        redo_cmd.execute = [this](std::string_view) {
            if (shell::Document* document = shell_->documents().active()) {
                (void)document->undo().redo();
            }
        };
        add(std::move(redo_cmd));

        Command clear_cmd;
        clear_cmd.id = "app.clear";
        clear_cmd.label = "Clear Document";
        clear_cmd.category = "Edit";
        clear_cmd.enabled = [this] {
            return shell_->documents().active() != nullptr;
        };
        clear_cmd.execute = [this](std::string_view) { edit("", "Clear"); };
        add(std::move(clear_cmd));

        Command about_cmd;
        about_cmd.id = "app.about";
        about_cmd.label = "About Scratchpad";
        about_cmd.category = "Help";
        about_cmd.shortcut = {kF1, Mod::none};
        about_cmd.execute = [this](std::string_view) {
            about_text_ = "scratchpad - the ws::ui::shell example app";
            shell_->toasts().push({about_text_.c_str(),
                                   gui::ToastSeverity::info, nullptr, 4.0f});
        };
        add(std::move(about_cmd));

        // The plot's regenerate, beside the shell's set.
        Command replot_cmd;
        replot_cmd.id = "app.replot";
        replot_cmd.label = "Regenerate Plot";
        replot_cmd.category = "Window";
        replot_cmd.enabled = [this] { return !plot_pending(); };
        replot_cmd.execute = [this](std::string_view) { submit_plot_job(); };
        add(std::move(replot_cmd));
    }

    // The one edit funnel: push IS the edit (the undo rule).
    void edit(std::string next, const std::string& kind) {
        ScratchDocument* document = active_scratch();
        if (document == nullptr) {
            return;
        }
        (void)document->undo().push(std::make_unique<ScratchEdit>(
            document, document->content, std::move(next), kind));
    }

    [[nodiscard]] ScratchDocument* active_scratch() {
        shell::Document* document = shell_->documents().active();
        return dynamic_cast<ScratchDocument*>(document);
    }

    // --- the panels ----------------------------------------------------------

    void draw_editor(GuiContext& gui, LayoutCursor& layout,
                     const gui::ChromeTheme& theme) {
        ScratchDocument* document = active_scratch();
        if (document == nullptr) {
            gui::text(layout, theme, "no document");
            return;
        }

        // The lazy-bake request: mark the document's codepoints once
        // per content change (idempotent, the documented cadence), so
        // the CJK stack fills in as the file draws.
        if (requested_content_ != document->content) {
            if (!font_mono_.empty()) {
                (void)ws::ui::r2d::font_request_text(
                    font_mono_, document->content.c_str());
            }
            requested_content_ = document->content;
        }

        // The editor rows: a dim right-aligned number in a gutter,
        // then the content in the mono face with comment lines a step
        // quieter - an editor, not a status line. The fontless theme
        // keeps the old plain rows (the v1 bars carry it).
        const ws::ui::r2d::Font* const mono =
            font_mono_.empty() ? nullptr : &font_mono_;
        const ws::ui::r2d::Font* const text_font =
            mono != nullptr ? mono : theme.font;
        std::size_t line_at = 0;
        if (text_font != nullptr && theme.font != nullptr) {
            const ws::ui::r2d::Font& num_font = *theme.font;
            const float line_h = text_font->line_height();

            // The gutter: wide enough for the last line's number plus
            // air on both sides (one pass to count the lines).
            std::size_t lines = 1;
            for (const char c : document->content) {
                lines += (c == '\n') ? 1u : 0u;
            }
            char widest[16];
            std::snprintf(widest, sizeof widest, "%zu", lines);
            const float gutter = num_font.measure_text(widest) + 16.0f;

            const std::uint32_t num_color = gui::dim(theme.text, 0.42f);
            const std::uint32_t code_color = theme.text;
            const std::uint32_t comment_color = gui::dim(theme.text, 0.60f);

            std::size_t search = 0;
            while (search < document->content.size()) {
                const std::size_t newline =
                    document->content.find('\n', search);
                const std::size_t len =
                    (newline == std::string::npos ? document->content.size()
                                                  : newline) -
                    search;
                char text_buf[512];
                const std::size_t keep =
                    std::min(len, sizeof text_buf - 1);
                std::memcpy(text_buf, document->content.data() + search,
                            keep);
                text_buf[keep] = '\0';

                const gui::Rect row =
                    layout.next(layout.remaining_width(), line_h);
                char num[16];
                std::snprintf(num, sizeof num, "%zu", ++line_at);
                (void)ws::ui::r2d::draw_text(
                    row.x + gutter - 8.0f - num_font.measure_text(num),
                    row.y, num, num_font, num_color);

                // A comment line: the first non-blank chars are // or #.
                std::size_t first = 0;
                while (text_buf[first] == ' ' || text_buf[first] == '\t') {
                    ++first;
                }
                const bool comment =
                    text_buf[first] == '#' ||
                    (text_buf[first] == '/' && text_buf[first + 1] == '/');
                (void)ws::ui::r2d::draw_text(
                    row.x + gutter, row.y, text_buf, *text_font,
                    comment ? comment_color : code_color);

                if (newline == std::string::npos) {
                    break;
                }
                search = newline + 1;
            }
        } else {
            // The fontless rows: the number rides the text, v1 style.
            std::size_t search = 0;
            while (search < document->content.size()) {
                const std::size_t newline =
                    document->content.find('\n', search);
                const std::string line = document->content.substr(
                    search, newline - search);
                char row[160];
                std::snprintf(row, sizeof row, "%2zu  %s", ++line_at,
                              line.c_str());
                gui::text(layout, theme, row, layout.remaining_width());
                if (newline == std::string::npos) {
                    break;
                }
                search = newline + 1;
            }
        }
        if (line_at == 0) {
            gui::text(layout, theme, "(empty - type a line below)");
        }

        gui::separator(layout, theme);

        // The append row: a field + an Add button on one line.
        const float button_w = 80.0f;
        const float field_w = std::max(
            0.0f, layout.remaining_width() - button_w - theme.spacing);
        if (gui::text_input(gui, layout, theme, "append", input_buf_,
                            static_cast<int>(sizeof input_buf_), field_w)) {
            // (the edit lands on the Enter key or the button, below -
            //  the field only reports its own editing)
        }
        layout.same_line();
        if (gui::button(gui, layout, theme, "add", button_w)) {
            commit_input();
        }
        if (gui::button(gui, layout, theme, "remove last line")) {
            const std::size_t last = document->content.rfind('\n');
            edit(last == std::string::npos
                     ? ""
                     : document->content.substr(0, last),
                 "Remove Line");
        }

        // The undo state, live.
        char state[128];
        std::snprintf(state, sizeof state, "undo: %zu   redo: %zu   %s",
                      document->undo().undo_count(),
                      document->undo().redo_count(),
                      document->undo().is_dirty() ? "unsaved" : "clean");
        gui::text(layout, theme, state);
    }

    void commit_input() {
        ScratchDocument* document = active_scratch();
        if (document == nullptr || input_buf_[0] == '\0') {
            return;
        }
        std::string next = document->content;
        if (!next.empty()) {
            next += '\n';
        }
        next += input_buf_;
        input_buf_[0] = '\0';
        edit(std::move(next), "Typing");
    }

    void draw_plot(GuiContext& gui, LayoutCursor& layout,
                   const gui::ChromeTheme& theme) {
        // The header: the action, then what the chart shows.
        if (gui::button(gui, layout, theme, "regenerate")) {
            submit_plot_job();
        }
        char header[128];
        std::snprintf(header, sizeof header, "damped sine - %zu samples",
                      plot_data_.size());
        gui::text(layout, theme, plot_pending() ? "computing..." : header);
        gui::separator(layout, theme);

        // The chart: one rounded bar per sample against a mid
        // baseline, guides at the peak and the zero, the peak value
        // labeled - enough axis that the decay reads as data, not as
        // decoration. The panel draws through the r2d seam directly
        // (the same screen-space rule every widget rides).
        const gui::Rect body = layout.panel_content();
        if (body.height <= 4.0f * theme.padding || plot_data_.empty()) {
            return;
        }

        const double amplitude =
            settings_->get<double>("plot.amplitude")
                .transform([](const ws::settings::SettingRead<double>& r) {
                    return r.value;
                })
                .value_or(1.0);

        const std::size_t n = plot_data_.size();
        const float chart_h = body.height - 2.0f * theme.padding;
        const gui::Rect chart{body.x, body.y + theme.padding, body.width,
                              chart_h};
        const float baseline = chart.y + chart_h * 0.72f;
        const float peak_h = chart_h * 0.62f;  // where +amplitude sits
        const float bar_w = chart.width / static_cast<float>(n);
        const float bar_r = std::min(2.0f, std::max(0.0f, bar_w * 0.25f));

        // The guides: +A at the top, the zero line at the baseline -
        // the two lines the decay is measured against.
        ws::ui::r2d::draw_quad(chart.x, chart.y, chart.width, 1.0f,
                               theme.border);
        ws::ui::r2d::draw_quad(chart.x, baseline, chart.width, 1.0f,
                               gui::dim(theme.text, 0.35f));

        // The bars: positive samples in the accent, negative ones a
        // step quieter - the two halves of the sine read apart at a
        // glance.
        for (std::size_t i = 0; i < n; ++i) {
            const float sample = plot_data_[i];
            const float scaled = static_cast<float>(
                std::abs(sample) / std::max(0.25, amplitude));
            const float h = std::max(1.0f, scaled * peak_h);
            const float y = sample >= 0.0f ? baseline - h : baseline;
            ws::ui::r2d::draw_rounded_quad(
                chart.x + static_cast<float>(i) * bar_w, y,
                std::max(1.0f, bar_w - 1.0f), h, bar_r,
                sample >= 0.0f ? theme.control_on
                                : gui::dim(theme.control_on, 0.55f));
        }

        // The axis labels, only when a font is installed (the
        // fontless bars carry the chart alone): the peak value at the
        // top guide, 0 at the baseline, the sample count bottom-right.
        if (theme.font != nullptr) {
            char label[32];
            std::snprintf(label, sizeof label, "+%.1f", amplitude);
            (void)ws::ui::r2d::draw_text(chart.x, chart.y + 2.0f, label,
                                         *theme.font,
                                         gui::dim(theme.text, 0.55f));
            (void)ws::ui::r2d::draw_text(chart.x, baseline + 2.0f, "0",
                                         *theme.font,
                                         gui::dim(theme.text, 0.55f));
            std::snprintf(label, sizeof label, "%zu samples", n);
            const float label_w = theme.font->measure_text(label);
            (void)ws::ui::r2d::draw_text(
                chart.x + chart.width - label_w,
                chart.y + chart.height - theme.font->line_height(), label,
                *theme.font, gui::dim(theme.text, 0.55f));
        }
    }

    // --- the background job ---------------------------------------------------

    void submit_plot_job() {
        if (plot_job_.busy()) {
            return;  // one flight at a time
        }
        const std::int64_t points =
            settings_->get<std::int64_t>("plot.points")
                .transform([](const ws::settings::SettingRead<std::int64_t>& r) {
                    return r.value;
                })
                .value_or(64);
        const double amplitude =
            settings_->get<double>("plot.amplitude")
                .transform([](const ws::settings::SettingRead<double>& r) {
                    return r.value;
                })
                .value_or(1.0);
        plot_progress_.reset();
        ws::jobs::JobCallbacks<std::vector<float>> callbacks =
            plot_progress_.callbacks<std::vector<float>>();
        callbacks.on_complete =
            [this](ws::jobs::JobHandle<std::vector<float>> handle) {
                ws::core::Result<std::vector<float>> result = handle.take();
                if (result.has_value()) {
                    plot_data_ = std::move(*result);
                }
            };
        plot_job_ = jobs_.submit<std::vector<float>>(
            [points, amplitude](ws::jobs::JobContext& context)
                -> ws::core::Result<std::vector<float>> {
                std::vector<float> samples(static_cast<std::size_t>(
                    std::max<std::int64_t>(16, points)));
                for (std::size_t i = 0; i < samples.size(); ++i) {
                    const double t =
                        static_cast<double>(i) /
                        static_cast<double>(samples.size() - 1);
                    samples[i] = static_cast<float>(
                        amplitude * std::exp(-3.0 * t) *
                        std::sin(8.0 * t * 3.141592653589793));
                    if (i % (samples.size() / 16 + 1) == 0) {
                        context.report_progress(
                            static_cast<float>(i) /
                                static_cast<float>(samples.size()),
                            "sampling");
                    }
                    if (context.cancelled()) {
                        break;  // the drain lands whatever landed
                    }
                }
                return samples;
            },
            ws::jobs::Priority::normal, {}, std::move(callbacks));
    }

    [[nodiscard]] bool plot_pending() const { return plot_job_.busy(); }

    // The status-bar rows the shell draws (the provider's answer,
    // rebuilt per frame: a finished job leaves the list by itself).
    [[nodiscard]] std::vector<shell::AppShell::StatusJob> status_jobs() {
        std::vector<shell::AppShell::StatusJob> jobs;
        if (plot_pending()) {
            shell::AppShell::StatusJob row;
            row.label = "plot";
            row.fraction = [this] { return plot_progress_.fraction(); };
            row.message = [this] {
                const std::string& message = plot_progress_.message();
                message_ = message.empty() ? "running" : message;
                return message_;
            };
            row.cancel = [this] { plot_job_.request_cancel(); };
            jobs.push_back(std::move(row));
        }
        return jobs;
    }

    // --- the chord loop (the gallery's shape, the shell's rung) ---------------

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
        for (int key = 0; key < ws::input::InputFrame::kMaxKeys; ++key) {
            if (!frame.key_pressed[key]) {
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
            const ws::app::Resolution hit =
                resolver_.resolve({key, mods}, shell_->key_context());
            if (hit.consumed && !hit.command.empty()) {
                const ws::core::Status done =
                    commands_.execute(hit.command);
                if (!done.has_value()) {
                    error_text_ = done.error().message;
                    shell_->toasts().push({error_text_.c_str(),
                                           gui::ToastSeverity::error,
                                           nullptr, 5.0f});
                }
            }
        }
    }

    // --- the members (order matters: the shell borrows them all) ------------

    ws::settings::StdFileSystem fs_;
    std::string store_dir_ = platform_store_dir();

    ws::settings::Schema schema_;
    std::unique_ptr<ws::settings::SettingsStore> settings_;
    ws::app::CommandRegistry commands_;
    ws::app::ShortcutResolver resolver_;
    std::unique_ptr<shell::AppShell> shell_;

    GuiContext gui_;
    std::string last_title_;

    // The look the shell cannot own: the baked face plus the ui scale
    // it was baked for (an empty font keeps the fontless v1 bars, by
    // design - the boxes this example shipped with were that
    // degradation, not a renderer fault). The mono face is the
    // editor's own; requested_content_ is the lazy-bake request guard
    // (mark once per content change, the documented cadence).
    float scale_ = 1.0f;
    ws::ui::r2d::Font font_;
    ws::ui::r2d::Font font_mono_;
    std::string requested_content_;

    // The plot job's state.
    ws::jobs::JobSystem jobs_{2};
    std::unique_ptr<ws::platform::FileDialogs> dialogs_;
    gui::JobProgress plot_progress_;
    ws::jobs::JobHandle<std::vector<float>> plot_job_{};
    std::vector<float> plot_data_;
    std::string message_;  // the status row's borrowed bytes

    // The editor's input row + the toast texts (borrowed bytes).
    char input_buf_[128] = {};
    std::string about_text_;
    std::string error_text_;
};

}  // namespace

#ifndef __ANDROID__
int main() {
    // The single-instance grab: a second launch complains and exits.
    const ws::platform::SingleInstance guard{"ws shell example"};
    if (!guard.is_primary()) {
        const std::unique_ptr<ws::platform::MessageBoxes> boxes =
            ws::platform::make_message_boxes();
        ws::platform::MessageBoxRequest request;
        request.kind = ws::platform::MessageBoxKind::warning;
        request.buttons = ws::platform::MessageBoxButtons::ok;
        request.title = "scratchpad";
        request.message =
            "another scratchpad is already running - this copy exits";
        (void)boxes->ask(request);
        return 1;
    }
    ws::ui::AppConfig config;
    config.title = "scratchpad";
    config.width = 1200;
    config.height = 760;
    ScratchApp app(std::move(config));
    return app.run();
}
#else
extern "C" void android_main(android_app* app) {
    chdir(app->activity->internalDataPath);
    ws::ui::AppConfig config;
    config.app_handle = app;
    ScratchApp scratch(std::move(config));
    (void)scratch.run();
}
#endif
