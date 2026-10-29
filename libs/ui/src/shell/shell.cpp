// AppShell's out-of-line half (shell.hpp's contract): the frame
// composition over the existing widget set, the startup/shutdown
// order, and the file-backed state (settings layer, layout, session,
// recents, named layouts) over the settings FileSystem + json seams.
//
// Purity: this TU joins ws_ui_gui - it draws through the r2d
// free-function seam and the widget set only; no Vulkan, GLFW or
// ImGui symbol may appear here (the stage-2 acceptance rule). The
// platform services arrive through the config's injected interfaces,
// never a make_* factory (the plan's stage-15 note).

#include <ws/ui/shell/shell.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include <ws/jobs/job_system.hpp>
#include <ws/platform/title.hpp>
#include <ws/ui/gui/property_grid.hpp>
#include <ws/ui/shell/errors.hpp>
#include <ws/ui/renderer2d.hpp>

namespace ws::ui::shell {

namespace {

using gui::ChromeTheme;
using gui::GuiContext;
using gui::LayoutCursor;
using gui::Rect;

// The GLFW key codes the shell's default chords ride (ws::app's
// numbering convention; plain ints by design).
constexpr int kKeyP = 80;
constexpr int kKeyN = 78;
constexpr int kKeyO = 79;
constexpr int kKeyS = 83;
constexpr int kKeyW = 87;

[[nodiscard]] bool point_in(const Rect& rect, float x, float y) {
    return x >= rect.x && x < rect.x + rect.width && y >= rect.y &&
           y < rect.y + rect.height;
}

}  // namespace

AppShell::AppShell(ShellConfig config, ws::app::CommandRegistry& commands,
                   ws::app::ShortcutResolver& shortcuts,
                   ws::settings::SettingsStore& settings)
    : config_(std::move(config)),
      commands_(commands),
      shortcuts_(shortcuts),
      settings_(settings) {
    documents_ = std::make_unique<DocumentManager>(
        [this](const std::string& path) -> ws::core::Result<
            std::unique_ptr<Document>> {
            if (!document_factory_) {
                return ws::core::unexpected(shell_error(
                    ShellError::unavailable, "no document factory set"));
            }
            return document_factory_(path);
        },
        config_.fs,
        config_.fs != nullptr && !config_.config_dir.empty()
            ? config_.config_dir + "/autosave"
            : std::string{});
    if (config_.fs != nullptr && !config_.config_dir.empty()) {
        recents_ = std::make_unique<ws::platform::RecentFiles>(
            *config_.fs, config_.config_dir + "/recent.json");
    }
    documents_->on_opened_path = [this](std::string_view type,
                                        std::string_view path) {
        if (recents_ != nullptr) {
            (void)recents_->touch(type, path);
        }
    };
}

AppShell::~AppShell() {
    // The host owes shutdown() a call, but a dropped shell must never
    // leave the crash markers claiming a session that is gone: the
    // panels' on_close callbacks and the file writes are skipped, the
    // markers are not.
    if (shutdown_log_.empty() && started_) {
        (void)shutdown();
    }
}

void AppShell::set_document_factory(DocumentFactory factory) {
    document_factory_ = std::move(factory);
}

void AppShell::set_toolbar_commands(std::vector<std::string> ids) {
    toolbar_ids_ = std::move(ids);
}

void AppShell::set_menu_specs(std::vector<gui::CommandMenuSpec> specs) {
    menu_specs_ = std::move(specs);
}

// --- paths -------------------------------------------------------------------

std::string AppShell::path_of(std::string_view name) const {
    return config_.config_dir.empty() ? std::string{name}
                                      : config_.config_dir + "/" +
                                            std::string{name};
}

// --- startup -----------------------------------------------------------------

ws::core::Status AppShell::startup() {
    started_ = true;
    register_commands();

    // The settings layer first: the theme key colors this very frame.
    (void)load_settings_layer();

    // Then the chrome: the layout names the panels that must be open
    // (a panel the layout knows but the registry does not is dropped
    // by the accept filter; a panel the registry knows but the layout
    // does not is the host's to open).
    if (!load_layout()) {
        rebuild_default_layout();
    }
    (void)load_recents();

    // The session: the open documents, best-effort; failures land in
    // the status line, never a failed startup (state that can be
    // rebuilt never fails the boot - the test pins this).
    std::vector<std::string> failures;
    std::vector<std::string> paths;
    int active = -1;
    if (config_.fs != nullptr && !config_.config_dir.empty()) {
        const ws::core::Result<std::string> text =
            config_.fs->read(path_of("session.json"));
        if (text) {
            const ws::core::Result<ws::settings::json::Value> parsed =
                ws::settings::json::parse(*text);
            if (parsed && parsed->is_object()) {
                if (const ws::settings::json::Value* docs =
                        parsed->as_object()->find("documents");
                    docs != nullptr && docs->is_array()) {
                    for (const ws::settings::json::Value& doc :
                         *docs->as_array()) {
                        if (const std::string* p = doc.as_text()) {
                            paths.push_back(*p);
                        }
                    }
                }
                if (const ws::settings::json::Value* act =
                        parsed->as_object()->find("active");
                    act != nullptr && act->is_integer()) {
                    active = static_cast<int>(act->get_integer().value_or(-1));
                }
            }
        }
    }
    if (!paths.empty()) {
        (void)documents_->restore_session(
            DocumentManager::Session{paths, active}, &failures);
        if (!failures.empty()) {
            status_ = std::to_string(failures.size()) +
                      " document(s) from the last session could not reopen";
        }
    }

    // The crash evidence: probe the autosave store for the session's
    // paths; every found marker becomes a recovery prompt (drawn by
    // the first frame).
    recoveries_ = documents_->probe_recovery(paths);
    recovery_at_ = 0;
    return {};
}

void AppShell::register_commands() {
    using ws::app::Command;
    using ws::app::Mod;

    const auto add_or_ignore = [this](Command command) {
        // A host that registered the same id keeps its own - the
        // shell never overwrites a host command.
        if (commands_.find(command.id) == nullptr) {
            (void)commands_.add(std::move(command));
        }
    };

    Command palette_cmd;
    palette_cmd.id = "shell.command_palette";
    palette_cmd.label = "Command Palette...";
    palette_cmd.category = "Window";
    palette_cmd.shortcut = {kKeyP, Mod::control | Mod::shift};
    palette_cmd.execute = [this](std::string_view) { palette_open = true; };
    add_or_ignore(std::move(palette_cmd));

    Command new_cmd;
    new_cmd.id = "shell.new_document";
    new_cmd.label = "New";
    new_cmd.category = "File";
    new_cmd.shortcut = {kKeyN, Mod::control};
    new_cmd.execute = [this](std::string_view) {
        if (documents_->new_document()) {
            set_status("new document");
        }
    };
    add_or_ignore(std::move(new_cmd));

    Command open_cmd;
    open_cmd.id = "shell.open_document";
    open_cmd.label = "Open...";
    open_cmd.category = "File";
    open_cmd.shortcut = {kKeyO, Mod::control};
    open_cmd.enabled = [this] {
        return config_.dialogs != nullptr;
    };
    open_cmd.execute = [this](std::string_view) {
        if (config_.dialogs == nullptr) {
            return;
        }
        ws::platform::FileDialogOptions options;
        options.title = "open document";
        const ws::core::Result<std::optional<std::string>> picked =
            config_.dialogs->open_file(options);
        if (picked && picked->has_value()) {
            (void)open_paths({**picked});
        }
    };
    add_or_ignore(std::move(open_cmd));

    Command save_cmd;
    save_cmd.id = "shell.save_document";
    save_cmd.label = "Save";
    save_cmd.category = "File";
    save_cmd.shortcut = {kKeyS, Mod::control};
    save_cmd.enabled = [this] { return documents_->active() != nullptr; };
    save_cmd.execute = [this](std::string_view) { (void)save_active(); };
    add_or_ignore(std::move(save_cmd));

    Command save_as_cmd;
    save_as_cmd.id = "shell.save_document_as";
    save_as_cmd.label = "Save As...";
    save_as_cmd.category = "File";
    save_as_cmd.enabled = [this] { return documents_->active() != nullptr; };
    save_as_cmd.execute = [this](std::string_view) {
        if (config_.dialogs == nullptr) {
            return;
        }
        ws::platform::FileDialogOptions options;
        options.title = "save document";
        const ws::core::Result<std::optional<std::string>> picked =
            config_.dialogs->save_file(options);
        if (picked && picked->has_value() && documents_->active() != nullptr) {
            Document* document = documents_->active();
            document->set_path(**picked);
            if (const ws::core::Status st = document->save(); st) {
                (void)document->undo().mark_saved();
                (void)documents_->finish_autosave(
                    static_cast<std::size_t>(
                        std::max(0, documents_->active_index())));
                set_status("saved " + document->display_name());
            } else {
                toast_text_ = "save failed: " + st.error().message;
                toasts_.push({toast_text_.c_str(), gui::ToastSeverity::error,
                              nullptr, 5.0f});
            }
        }
    };
    add_or_ignore(std::move(save_as_cmd));

    Command close_cmd;
    close_cmd.id = "shell.close_document";
    close_cmd.label = "Close";
    close_cmd.category = "File";
    close_cmd.shortcut = {kKeyW, Mod::control};
    close_cmd.enabled = [this] { return documents_->active() != nullptr; };
    close_cmd.execute = [this](std::string_view) { request_close_active(); };
    add_or_ignore(std::move(close_cmd));

    Command settings_cmd;
    settings_cmd.id = "shell.settings";
    settings_cmd.label = "Settings";
    settings_cmd.category = "Window";
    settings_cmd.execute = [this](std::string_view) {
        if (panel_host_.is_open("settings")) {
            panel_host_.close(panel_registry_, "settings");
        } else {
            (void)panel_host_.open(panel_registry_, "settings");
        }
    };
    add_or_ignore(std::move(settings_cmd));

    Command panels_cmd;
    panels_cmd.id = "shell.next_panel";
    panels_cmd.label = "Next Panel";
    panels_cmd.category = "Window";
    panels_cmd.execute = [this](std::string_view) {
        panel_host_.cycle(panel_registry_);
    };
    add_or_ignore(std::move(panels_cmd));

    Command reset_cmd;
    reset_cmd.id = "shell.reset_layout";
    reset_cmd.label = "Reset Layout";
    reset_cmd.category = "Window";
    reset_cmd.execute = [this](std::string_view) {
        rebuild_default_layout();
        set_status("layout reset");
    };
    add_or_ignore(std::move(reset_cmd));

    // The shell's built-in settings panel: the host's schema, one
    // property grid. A host that registered its own "settings" panel
    // keeps it (the add refusal below is swallowed on purpose).
    if (panel_registry_.find("settings") == nullptr) {
        PanelDesc settings_panel;
        settings_panel.id = "settings";
        settings_panel.title = "Settings";
        settings_panel.placement.area = DockPlacement::Area::right;
        settings_panel.draw =
            [this](GuiContext& g, LayoutCursor& l, const ChromeTheme& t,
                   const PanelInstance&) {
                (void)gui::property_grid(g, l, t, settings_.schema(),
                                        settings_, grid_state_);
            };
        (void)panel_registry_.add(std::move(settings_panel));
    }
}

// --- shutdown ----------------------------------------------------------------

ws::core::Status AppShell::shutdown() {
    if (!shutdown_log_.empty()) {
        return {};  // idempotent: the second call is the host's doubt
    }
    // 1. Jobs die first: a panel's on_close may read what a job was
    // feeding it, and a cancelled job never lands another completion.
    if (config_.jobs != nullptr) {
        config_.jobs->request_cancel_all();
    }
    shutdown_log_.push_back("cancel_jobs");

    // 2. Then the panels close (every on_close fires, dock order).
    panel_host_.close_all(panel_registry_);
    shutdown_log_.push_back("close_panels");

    // 3. The autosave pairs end with their documents: a clean document
    // finishes (marker cleared), a dirty one snapshots - a quit with
    // unsaved work is recoverable, exactly like a crash.
    const int was_active = documents_->active_index();
    for (std::size_t i = 0; i < documents_->count(); ++i) {
        Document* document = documents_->at(i);
        if (document == nullptr) {
            continue;
        }
        if (document->undo().is_dirty()) {
            (void)documents_->finish_autosave(i);  // clear a stale marker
            documents_->activate(i);
            (void)documents_->snapshot_active();
        } else {
            (void)documents_->finish_autosave(i);
        }
    }
    if (was_active >= 0) {
        documents_->activate(static_cast<std::size_t>(was_active));
    }
    shutdown_log_.push_back("autosaves");

    // 4-6. The state files.
    (void)save_session();
    shutdown_log_.push_back("save_session");
    (void)save_layout();
    shutdown_log_.push_back("save_layout");
    (void)save_recents();
    shutdown_log_.push_back("save_recents");

    // 7. The user settings layer.
    (void)save_settings_layer();
    shutdown_log_.push_back("flush_settings");
    return {};
}

// --- the file-backed state -----------------------------------------------------

ws::core::Status AppShell::load_settings_layer() {
    if (config_.fs == nullptr || config_.config_dir.empty()) {
        return {};
    }
    const ws::core::Result<std::string> text =
        config_.fs->read(path_of("settings.json"));
    if (!text) {
        return {};  // a missing file is the first run
    }
    const ws::core::Result<ws::settings::json::Value> parsed =
        ws::settings::json::parse(*text);
    if (!parsed) {
        status_ = "settings.json would not parse; defaults stand";
        return {};
    }
    return settings_.load_layer(ws::settings::Layer::user, *parsed,
                               ws::settings::Migrations{});
}

ws::core::Status AppShell::save_settings_layer() {
    if (config_.fs == nullptr || config_.config_dir.empty()) {
        return {};
    }
    const ws::core::Result<ws::settings::json::Value> doc =
        settings_.save_layer(ws::settings::Layer::user);
    if (!doc) {
        return ws::core::unexpected(doc.error());
    }
    return config_.fs->write(path_of("settings.json"),
                             ws::settings::json::write(*doc));
}

ws::core::Status AppShell::load_layout() {
    if (config_.fs == nullptr || config_.config_dir.empty()) {
        return ws::core::unexpected(
            shell_error(ShellError::unavailable, "no config dir"));
    }
    const ws::core::Result<std::string> text =
        config_.fs->read(path_of("layout.json"));
    if (!text) {
        return ws::core::unexpected(text.error());
    }
    const ws::core::Result<ws::settings::json::Value> parsed =
        ws::settings::json::parse(*text);
    if (!parsed || !panel_host_.dock().from_json(
                       *parsed, [this](std::string_view panel) {
                           return panel_host_.find(panel) != nullptr;
                       })) {
        status_ = "layout.json would not parse; the default layout stands";
        return ws::core::unexpected(
            shell_error(ShellError::parse_failed, "layout.json is corrupt"));
    }
    return {};
}

ws::core::Status AppShell::save_layout() {
    if (config_.fs == nullptr || config_.config_dir.empty()) {
        return {};
    }
    return config_.fs->write(
        path_of("layout.json"),
        ws::settings::json::write(panel_host_.dock().to_json()));
}

ws::core::Status AppShell::save_session() {
    if (config_.fs == nullptr || config_.config_dir.empty()) {
        return {};
    }
    const DocumentManager::Session session = documents_->session();
    ws::settings::json::Object root;
    ws::settings::json::Array docs;
    for (const std::string& path : session.paths) {
        docs.push_back(ws::settings::json::Value::text(path));
    }
    root.set("documents", ws::settings::json::Value::array(std::move(docs)));
    root.set("active", ws::settings::json::Value::integer(session.active));
    return config_.fs->write(
        path_of("session.json"),
        ws::settings::json::write(ws::settings::json::Value::object(
            std::move(root))));
}

ws::core::Status AppShell::load_recents() {
    if (recents_ == nullptr) {
        return {};
    }
    return recents_->load();  // a missing file is the first run
}

ws::core::Status AppShell::save_recents() {
    if (recents_ == nullptr) {
        return {};
    }
    return recents_->save();
}

void AppShell::rebuild_default_layout() {
    // Keep the open set, rebuild the tree: every open instance lands
    // at its descriptor's default placement, in open order.
    std::vector<std::string> ids = panel_host_.dock().panels();
    panel_host_.dock().clear();
    for (const std::string& id : ids) {
        const PanelInstance* instance = panel_host_.find(id);
        if (instance == nullptr) {
            continue;
        }
        const PanelDesc* desc = panel_registry_.find(instance->type);
        if (desc != nullptr) {
            panel_host_.dock().add(id, desc->placement);
        }
    }
}

std::vector<std::string> AppShell::layout_names() {
    std::vector<std::string> names;
    if (config_.fs == nullptr || config_.config_dir.empty()) {
        return names;
    }
    const ws::core::Result<std::string> text =
        config_.fs->read(path_of("layouts.json"));
    if (!text) {
        return names;
    }
    const ws::core::Result<ws::settings::json::Value> parsed =
        ws::settings::json::parse(*text);
    if (parsed && parsed->is_object()) {
        if (const ws::settings::json::Value* list =
                parsed->as_object()->find("names");
            list != nullptr && list->is_array()) {
            for (const ws::settings::json::Value& name : *list->as_array()) {
                if (const std::string* n = name.as_text()) {
                    names.push_back(*n);
                }
            }
        }
    }
    return names;
}

ws::core::Status AppShell::save_named_layout(const std::string& name) {
    if (config_.fs == nullptr || config_.config_dir.empty() ||
        name.empty()) {
        return ws::core::unexpected(
            shell_error(ShellError::invalid, "layout name is empty"));
    }
    // layouts/<name>.json + the index entry (no listdir on the seam,
    // the names ride layouts.json).
    const ws::core::Status st = config_.fs->write(
        config_.config_dir + "/layouts/" + name + ".json",
        ws::settings::json::write(panel_host_.dock().to_json()));
    if (!st) {
        return st;
    }
    std::vector<std::string> names = layout_names();
    if (std::find(names.begin(), names.end(), name) == names.end()) {
        names.push_back(name);
        ws::settings::json::Object root;
        ws::settings::json::Array list;
        for (const std::string& n : names) {
            list.push_back(ws::settings::json::Value::text(n));
        }
        root.set("names", ws::settings::json::Value::array(std::move(list)));
        (void)config_.fs->write(
            path_of("layouts.json"),
            ws::settings::json::write(
                ws::settings::json::Value::object(std::move(root))));
    }
    set_status("layout saved: " + name);
    return {};
}

ws::core::Status AppShell::load_named_layout(const std::string& name) {
    if (config_.fs == nullptr || config_.config_dir.empty()) {
        return ws::core::unexpected(
            shell_error(ShellError::unavailable, "no config dir"));
    }
    const ws::core::Result<std::string> text = config_.fs->read(
        config_.config_dir + "/layouts/" + name + ".json");
    if (!text) {
        return ws::core::unexpected(text.error());
    }
    const ws::core::Result<ws::settings::json::Value> parsed =
        ws::settings::json::parse(*text);
    if (!parsed || !panel_host_.dock().from_json(
                       *parsed, [this](std::string_view panel) {
                           return panel_host_.find(panel) != nullptr;
                       })) {
        return ws::core::unexpected(
            shell_error(ShellError::parse_failed,
                        "layout '" + name + "' is corrupt"));
    }
    set_status("layout restored: " + name);
    return {};
}

// --- the services ---------------------------------------------------------------

std::string AppShell::window_title() {
    Document* document = documents_->active();
    const bool dirty = document != nullptr && document->undo().is_dirty();
    const ws::core::Result<std::string> title = ws::platform::window_title(
        config_.app_name, document != nullptr ? document->display_name() : "",
        dirty);
    return title ? *title : config_.app_name;
}

ws::app::KeyContext AppShell::key_context() const {
    const PanelInstance* instance = panel_host_.active();
    if (instance == nullptr) {
        return ws::app::KeyContext::global;
    }
    const PanelDesc* desc = panel_registry_.find(instance->type);
    if (desc != nullptr && desc->scoped_shortcuts) {
        return ws::app::KeyContext::panel;
    }
    return ws::app::KeyContext::global;
}

const ChromeTheme& AppShell::theme() const {
    // Live read: the "shell.theme" key, when the host's schema declares
    // it ("light" picks the light chrome). A missing key or schema is
    // the dark default - the shell never requires its own keys.
    if (const ws::core::Result<ws::settings::SettingRead<std::string>> read =
            settings_.get<std::string>("shell.theme");
        read && read->value == "light") {
        return gui::kLightChrome;
    }
    return gui::kDefaultChrome;
}

void AppShell::set_status(std::string text) { status_ = std::move(text); }

void AppShell::request_close_active() {
    const int index = documents_->active_index();
    if (index < 0) {
        return;
    }
    const DocumentManager::ClosePlan plan =
        documents_->plan_close(static_cast<std::size_t>(index));
    if (!plan.valid) {
        return;
    }
    if (plan.prompt) {
        close_pending_ = index;  // the modal answers on a later frame
    } else {
        (void)documents_->finish_close(static_cast<std::size_t>(index), false);
        set_status("closed");
    }
}

ws::core::Status AppShell::save_active() {
    Document* document = documents_->active();
    if (document == nullptr) {
        return ws::core::unexpected(
            shell_error(ShellError::invalid, "no active document"));
    }
    if (document->path().empty()) {
        // Untitled: the save-as pick (the dialogs injection is the
        // whole story; without it the save refuses).
        if (config_.dialogs == nullptr) {
            return ws::core::unexpected(shell_error(
                ShellError::unavailable, "cannot save an untitled document"));
        }
        ws::platform::FileDialogOptions options;
        options.title = "save document";
        const ws::core::Result<std::optional<std::string>> picked =
            config_.dialogs->save_file(options);
        if (!picked || !picked->has_value()) {
            return {};
        }
        document->set_path(**picked);
    }
    if (const ws::core::Status st = document->save(); !st) {
        return st;
    }
    (void)document->undo().mark_saved();
    (void)documents_->finish_autosave(
        static_cast<std::size_t>(std::max(0, documents_->active_index())));
    set_status("saved " + document->display_name());
    return {};
}

ws::core::Status AppShell::open_paths(const std::vector<std::string>& paths) {
    for (const std::string& path : paths) {
        const ws::core::Result<int> index = documents_->open_path(path);
        if (!index) {
            toast_text_ = "open failed: " + index.error().message;
            toasts_.push({toast_text_.c_str(), gui::ToastSeverity::error,
                          nullptr, 5.0f});
            return ws::core::unexpected(index.error());
        }
        set_status("opened " + path);
    }
    return {};
}

void AppShell::handle_dropped_files(const std::vector<std::string>& paths) {
    (void)open_paths(paths);
}

void AppShell::autosave_tick() {
    if (config_.clock_ms == nullptr || config_.autosave_interval_ms <= 0) {
        return;
    }
    Document* document = documents_->active();
    if (document == nullptr || !document->undo().is_dirty()) {
        return;
    }
    const std::int64_t now = config_.clock_ms();
    if (now - last_autosave_ms_ >= config_.autosave_interval_ms) {
        last_autosave_ms_ = now;
        (void)documents_->snapshot_active();
    }
}

// --- the frame -----------------------------------------------------------------

void AppShell::draw(GuiContext& gui, const ChromeTheme& theme,
                    const Rect& frame) {
    autosave_tick();
    LayoutCursor layout;
    (void)layout.begin_panel(frame, 0.0f, theme.spacing);

    // The menu bar strip.
    const Rect bar{frame.x, frame.y, frame.width, theme.control_height};
    draw_menu_bar(gui, layout, theme, bar);

    // The toolbar strip (only when the host named commands for it).
    float top = frame.y + theme.control_height;
    if (!toolbar_ids_.empty()) {
        const Rect tool{frame.x, top, frame.width, theme.control_height};
        layout.begin_panel(tool, 4.0f, theme.spacing);
        const std::string_view fired = gui::command_toolbar(
            gui, layout, theme, "shell.toolbar", commands_, toolbar_ids_);
        if (!fired.empty()) {
            (void)commands_.execute(fired);
        }
        layout.end_panel();
        top += theme.control_height;
    }

    // The dock space: everything between the toolbar and the status
    // strip (the status bar anchors itself to the r2d frame's bottom
    // edge, so the room it takes is measured the honest way).
    const r2d::FrameSize screen = r2d::frame_size();
    const float bottom = screen.height > 0.0f
                             ? std::max(0.0f, screen.height -
                                                  theme.control_height)
                             : frame.y + frame.height;
    const Rect dock{frame.x, top, frame.width,
                    std::max(0.0f, bottom - top)};
    draw_dock(gui, layout, theme, dock);

    layout.end_panel();
    draw_status(gui, layout, theme, frame, bottom);
    draw_overlays(gui, theme, frame);
}

void AppShell::draw_menu_bar(GuiContext& gui, LayoutCursor& layout,
                             const ChromeTheme& theme, const Rect& bar) {
    // The host's menus first (categories become menus, in spec
    // order), the shell's Window menu after them.
    menu_builder_.build(commands_, &shortcuts_, key_context());

    window_items_.clear();
    window_actions_.clear();
    window_labels_.clear();
    recent_items_.clear();

    // The panel toggles: one check row per registered type.
    for (const PanelDesc& desc : panel_registry_.descriptors()) {
        window_labels_.emplace_back(desc.title);
        gui::MenuItem item;
        item.label = window_labels_.back().c_str();
        item.kind = gui::MenuItemKind::check;
        item.checked = panel_host_.is_open(desc.id);
        window_items_.push_back(item);
        window_actions_.push_back("panel:" + desc.id);
    }
    if (!window_items_.empty()) {
        gui::MenuItem sep;
        sep.kind = gui::MenuItemKind::separator;
        window_items_.push_back(sep);
        window_actions_.emplace_back("");
    }

    // The layout rows: reset, save-as, then every named layout.
    window_labels_.emplace_back("Reset Layout");
    gui::MenuItem reset;
    reset.label = window_labels_.back().c_str();
    window_items_.push_back(reset);
    window_actions_.emplace_back("layout:reset");

    window_labels_.emplace_back("Save Layout...");
    gui::MenuItem save_layout;
    save_layout.label = window_labels_.back().c_str();
    window_items_.push_back(save_layout);
    window_actions_.emplace_back("layout:save");

    for (const std::string& name : layout_names()) {
        window_labels_.emplace_back(name);
        gui::MenuItem row;
        row.label = window_labels_.back().c_str();
        window_items_.push_back(row);
        window_actions_.push_back("layout:load:" + name);
    }

    // The command palette row, with its live chord hint.
    if (const ws::app::Command* palette_cmd =
            commands_.find("shell.command_palette")) {
        window_hint_ = gui::command_hint(*palette_cmd, &shortcuts_,
                                         ws::app::KeyContext::global);
        window_labels_.emplace_back("Command Palette...");
        gui::MenuItem row;
        row.label = window_labels_.back().c_str();
        row.shortcut = window_hint_.c_str();
        window_items_.push_back(row);
        window_actions_.emplace_back("cmd:shell.command_palette");
    }

    // The recent files, as the Window menu's submenu (v1's documented
    // placement: the File menu is the host's own command categories).
    if (recents_ != nullptr) {
        const std::vector<ws::platform::RecentItem> recents =
            recents_->menu_items();
        if (!recents.empty()) {
            for (const ws::platform::RecentItem& item : recents) {
                gui::MenuItem row;
                row.label = item.label.c_str();
                recent_items_.push_back(row);
            }
            gui::MenuItem sep;
            sep.kind = gui::MenuItemKind::separator;
            window_items_.push_back(sep);
            window_actions_.emplace_back("");

            window_labels_.emplace_back("Recent Files");
            gui::MenuItem parent;
            parent.label = window_labels_.back().c_str();
            parent.submenu = recent_items_.data();
            parent.submenu_count = static_cast<int>(recent_items_.size());
            window_items_.push_back(parent);
            window_actions_.emplace_back("");
        }
    }

    // The bar: the builder's MenuDesc array with the Window menu
    // appended. Both borrow per-frame storage, so the composed vector
    // is per-frame too.
    std::vector<gui::MenuDesc> menus(
        menu_builder_.menus(),
        menu_builder_.menus() + menu_builder_.count());
    const int window_index = static_cast<int>(menus.size());
    gui::MenuDesc window;
    window.title = "&Window";
    window.items = window_items_.data();
    window.count = static_cast<int>(window_items_.size());
    menus.push_back(window);

    layout.begin_panel(bar, 0.0f, theme.spacing);
    const gui::MenuEvent event = gui::menu_bar(
        gui, layout, theme, "shell.menubar", menus.data(),
        static_cast<int>(menus.size()), menu_open_);
    layout.end_panel();

    if (event.item == nullptr) {
        return;
    }
    if (event.menu == window_index) {
        const std::ptrdiff_t index = event.item - window_items_.data();
        if (index >= 0 &&
            static_cast<std::size_t>(index) < window_actions_.size()) {
            run_window_action(
                window_actions_[static_cast<std::size_t>(index)]);
        }
        return;
    }
    // A host menu: the builder knows the command id.
    const std::string_view id = menu_builder_.command_of(event.item);
    if (!id.empty()) {
        (void)commands_.execute(id);
    }
}

void AppShell::run_window_action(const std::string& action) {
    if (action.rfind("panel:", 0) == 0) {
        const std::string type = action.substr(6);
        if (panel_host_.is_open(type)) {
            panel_host_.close(panel_registry_, type);
        } else {
            (void)panel_host_.open(panel_registry_, type);
        }
        return;
    }
    if (action == "layout:reset") {
        rebuild_default_layout();
        set_status("layout reset");
        return;
    }
    if (action == "layout:save") {
        layout_prompt_open_ = true;
        return;
    }
    if (action.rfind("layout:load:", 0) == 0) {
        (void)load_named_layout(action.substr(12));
        return;
    }
    if (action.rfind("recent:", 0) == 0) {
        (void)open_paths({action.substr(7)});
        return;
    }
    if (action.rfind("cmd:", 0) == 0) {
        (void)commands_.execute(action.substr(4));
    }
}

void AppShell::draw_dock(GuiContext& gui, LayoutCursor& layout,
                         const ChromeTheme& theme, const Rect& bounds) {
    if (bounds.width <= 0.0f || bounds.height <= 0.0f) {
        return;
    }
    r2d::draw_quad(bounds.x, bounds.y, bounds.width, bounds.height, theme.panel_bg);
    const DockTree::Layout resolved = panel_host_.dock().resolve(bounds);

    // The splitters first: their hit claims land before any leaf
    // widget's, so a boundary grab always wins over content under it.
    for (const DockTree::SplitterRect& splitter : resolved.splitters) {
        r2d::draw_quad(splitter.rect.x, splitter.rect.y, splitter.rect.width,
                       splitter.rect.height, theme.border);
        const gui::WidgetId id = gui.ids().id("shell.splitter");
        const gui::HitTest hit = gui.hit_test(id, splitter.rect);
        if (hit.pressed) {
            splitter_drag_.active = true;
            splitter_drag_.splitter = splitter;
        }
    }
    if (splitter_drag_.active) {
        const ws::input::InputFrame& in = gui.input();
        if (in.mouse_down[0] && in.mouse_present) {
            const float position =
                splitter_drag_.splitter.dir == DockDir::horizontal
                    ? in.mouse_x
                    : in.mouse_y;
            panel_host_.dock().apply_drag(splitter_drag_.splitter, position);
        } else {
            splitter_drag_.active = false;
        }
    }

    // Focus follows press: a press inside a leaf activates that leaf's
    // front tab (the tab strip's own selection does the same on click).
    const ws::input::InputFrame& in = gui.input();
    const bool pressed = in.mouse_present && in.mouse_pressed[0];
    if (pressed) {
        for (const DockTree::LeafRect& leaf : resolved.leaves) {
            if (point_in(leaf.rect, in.mouse_x, in.mouse_y) &&
                leaf.tabs != nullptr && !leaf.tabs->empty()) {
                const int index = std::clamp(
                    leaf.active, 0, static_cast<int>(leaf.tabs->size()) - 1);
                panel_host_.activate(panel_registry_,
                                     (*leaf.tabs)[static_cast<std::size_t>(index)]);
            }
        }
    }

    // The leaves: a tab strip, then the active tab's panel body in a
    // scroll scope.
    for (const DockTree::LeafRect& leaf : resolved.leaves) {
        const float strip_h = theme.control_height;
        const Rect strip{leaf.rect.x, leaf.rect.y, leaf.rect.width, strip_h};

        std::vector<const char*> labels;
        if (leaf.tabs != nullptr) {
            labels.reserve(leaf.tabs->size());
            for (const std::string& tab : *leaf.tabs) {
                const PanelInstance* instance = panel_host_.find(tab);
                labels.push_back(instance != nullptr ? instance->title.c_str()
                                                     : tab.c_str());
            }
        }
        int active = 0;
        if (!labels.empty()) {
            active = std::clamp(leaf.active, 0,
                                static_cast<int>(labels.size()) - 1);
            const float tab_w =
                std::max(40.0f, std::min(160.0f, leaf.rect.width /
                                                     static_cast<float>(
                                                         labels.size())));
            layout.begin_panel(strip, 0.0f, 0.0f);
            if (gui::tab_bar(gui, layout, theme, "shell.tabs", labels.data(),
                             static_cast<int>(labels.size()), active, tab_w)) {
                // The selection moved: the tree and the focus follow.
                panel_host_.activate(panel_registry_,
                                     (*leaf.tabs)[static_cast<std::size_t>(active)]);
            }
            // The tab strip's close button, right of the tabs.
            layout.same_line(0.0f);
            if (gui::button(gui, layout, theme, "shell.closetab", 22.0f)) {
                panel_host_.close(panel_registry_,
                                  (*leaf.tabs)[static_cast<std::size_t>(active)]);
                layout.end_panel();
                continue;  // the leaf's body is gone this frame
            }
            layout.end_panel();
        }

        const Rect content{leaf.rect.x, leaf.rect.y + strip_h, leaf.rect.width,
                           std::max(0.0f, leaf.rect.height - strip_h)};
        if (content.height <= 0.0f) {
            continue;
        }
        if (labels.empty()) {
            gui::panel_draw(content, theme);  // the empty-dock chrome
            continue;
        }
        const PanelInstance* instance =
            panel_host_.find((*leaf.tabs)[static_cast<std::size_t>(active)]);
        const PanelDesc* desc =
            instance == nullptr ? nullptr
                                : panel_registry_.find(instance->type);
        if (desc == nullptr || !desc->draw) {
            gui::panel_draw(content, theme);
            continue;
        }
        const std::string label = "shell.body." + instance->id;
        (void)gui::begin_scroll_panel(gui, layout, theme, label.c_str(),
                                      content);
        desc->draw(gui, layout, theme, *instance);
        gui::end_scroll_panel(gui, layout, theme);
    }
}

void AppShell::draw_status(GuiContext& gui, LayoutCursor& layout,
                           const ChromeTheme& theme, const Rect& frame,
                           float bottom_y) {
    // The bar draws itself, anchored to the r2d frame's bottom edge.
    const Rect bar = gui::status_bar(theme, status_.c_str());
    if (status_jobs_ == nullptr) {
        return;
    }
    // The running jobs stack up above the bar, first-listed on top.
    const std::vector<StatusJob> jobs = status_jobs_();
    const float row_h = 2.0f * theme.control_height + theme.spacing +
                        2.0f * theme.padding;
    float y = bar.height > 0.0f ? bar.y : bottom_y;
    for (std::size_t i = jobs.size(); i-- > 0;) {
        const StatusJob& job = jobs[i];
        y -= row_h;
        if (y < frame.y) {
            break;  // the frame ran out: the rest are simply not shown
        }
        const Rect row{frame.x, y, frame.width, row_h};
        layout.begin_panel(row, theme.padding, theme.spacing);
        char line[192];
        const std::string message = job.message ? job.message() : std::string{};
        std::snprintf(line, sizeof line, "%s: %s", job.label.c_str(),
                      message.c_str());
        gui::text(layout, theme, line, layout.remaining_width());
        const float cancel_w = 80.0f;
        const float bar_w =
            std::max(0.0f, layout.remaining_width() - cancel_w - theme.spacing);
        gui::progress(layout, theme,
                      job.fraction ? std::clamp(job.fraction(), 0.0f, 1.0f)
                                   : 0.0f,
                      bar_w);
        layout.same_line();
        if (gui::button(gui, layout, theme, "cancel", cancel_w)) {
            if (job.cancel) {
                job.cancel();
            }
        }
        layout.end_panel();
    }
}

void AppShell::draw_overlays(GuiContext& gui, const ChromeTheme& theme,
                             const Rect& frame) {
    draw_close_prompt(gui, theme, frame);
    draw_recovery_prompt(gui, theme, frame);
    draw_layout_prompt(gui, theme, frame);

    // The command palette.
    if (palette_open) {
        const gui::PaletteEvent event = gui::command_palette(
            gui, theme, "shell.palette", palette_open, commands_,
            &shortcuts_, key_context(), palette_);
        if (event.dispatch) {
            (void)commands_.execute(event.command, event.argument);
        }
        if (event.close) {
            palette_open = false;
        }
    }

    // The toast stratum, clear of the status bar.
    (void)gui::draw_toasts(gui, theme, toasts_, theme.control_height);
}

void AppShell::draw_close_prompt(GuiContext& gui, const ChromeTheme& theme,
                                 const Rect& frame) {
    if (close_pending_ < 0) {
        close_esc_ = false;  // the closed frames re-arm the Escape rule
        return;
    }
    if (gui.esc_won(&close_esc_)) {
        close_pending_ = -1;  // Escape answers Cancel
        return;
    }
    LayoutCursor mlayout;
    const float w = std::max(0.0f, std::min(360.0f, frame.width));
    const float h = std::max(0.0f, std::min(170.0f, frame.height));
    const Rect bounds{(frame.width - w) * 0.5f, (frame.height - h) * 0.5f, w, h};
    if (!gui::begin_modal(gui, mlayout, theme, "shell.close", bounds, true)) {
        return;
    }
    gui.claim_esc(&close_esc_);

    gui::text(mlayout, theme, "Save changes?");
    const Document* document =
        documents_->at(static_cast<std::size_t>(close_pending_));
    const std::string body =
        (document != nullptr ? document->display_name() : std::string{"the document"}) +
        " has unsaved changes.";
    gui::text(mlayout, theme, body.c_str());

    // The three-button row, centered like modal_dialog's own.
    const char* labels[3] = {"Save", "Discard", "Cancel"};
    float widest = 0.0f;
    char scratch[128];
    for (const char* label : labels) {
        const gui::LabelFit fit =
            gui::fit_text(label, -1.0f, theme, scratch, sizeof scratch);
        widest = std::max(widest, fit.advance);
    }
    const float button_width = widest + 2.0f * theme.padding;
    const float row_total = 3.0f * button_width + 2.0f * theme.spacing;
    const float lead =
        std::max(0.0f, mlayout.remaining_width() - row_total) * 0.5f;
    mlayout.next(lead, 0.0f);
    mlayout.same_line(0.0f);
    int answer = -1;
    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            mlayout.same_line();
        }
        if (gui::button(gui, mlayout, theme, labels[i], button_width)) {
            answer = i;
        }
    }
    gui::end_modal(gui, mlayout);

    if (answer < 0) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(close_pending_);
    close_pending_ = -1;  // cleared before acting: no re-entrancy
    if (answer == 2) {
        return;  // Cancel
    }
    if (answer == 0) {  // Save: a failed save keeps the document open
        if (const ws::core::Status st = save_active(); !st) {
            toast_text_ = "save failed: " + st.error().message;
            toasts_.push({toast_text_.c_str(), gui::ToastSeverity::error,
                          nullptr, 5.0f});
            return;
        }
    }
    (void)documents_->finish_close(index, false);
    set_status(answer == 0 ? "saved and closed" : "closed");
}

void AppShell::draw_recovery_prompt(GuiContext& gui,
                                    const ChromeTheme& theme,
                                    const Rect& frame) {
    (void)frame;  // modal_dialog centers itself on the r2d frame
    if (recovery_at_ >= recoveries_.size()) {
        recovery_open_ = false;
        return;
    }
    const DocumentManager::RecoveryOffer& offer =
        recoveries_[recovery_at_];
    recovery_open_ = true;
    const std::string body = offer.document +
                             " went down without a clean exit. Recover "
                             "the autosaved changes?";
    const gui::DialogEvent event = gui::modal_dialog(
        gui, theme, "shell.recovery", "recover autosave",
        gui::DialogButtons::yes_no, recovery_open_, 380.0f,
        [&body](GuiContext&, LayoutCursor& l, const ChromeTheme& t) {
            gui::text(l, t, body.c_str());
        });
    if (recovery_open_) {
        return;  // still asking
    }
    if (event.accepted) {
        const ws::core::Result<std::string> content =
            documents_->take_recovery(offer.document);
        if (content) {
            const ws::core::Result<int> index =
                documents_->open_path(offer.document);
            if (index) {
                if (Document* document =
                        documents_->at(static_cast<std::size_t>(*index))) {
                    document->adopt_autosave_content(*content);
                }
                set_status("recovered " + offer.document);
            }
        } else {
            toast_text_ = "recovery failed: " + content.error().message;
            toasts_.push({toast_text_.c_str(), gui::ToastSeverity::error,
                          nullptr, 5.0f});
        }
    } else {
        (void)documents_->discard_recovery(offer.document);
    }
    ++recovery_at_;  // the next offer, if any, asks on a later frame
}

void AppShell::draw_layout_prompt(GuiContext& gui,
                                  const ChromeTheme& theme,
                                  const Rect& frame) {
    (void)frame;  // modal_dialog centers itself on the r2d frame
    if (!layout_prompt_open_) {
        return;
    }
    const gui::DialogEvent event = gui::modal_dialog(
        gui, theme, "shell.layoutname", "save layout",
        gui::DialogButtons::ok_cancel, layout_prompt_open_, 320.0f,
        [this](GuiContext& g, LayoutCursor& l, const ChromeTheme& t) {
            gui::text_input(g, l, t, "name", layout_name_,
                            static_cast<int>(sizeof layout_name_));
        });
    if (layout_prompt_open_) {
        return;
    }
    if (event.accepted) {
        std::string name{layout_name_};
        const std::size_t end = name.find_last_not_of(" \t");
        name = end == std::string::npos ? "" : name.substr(0, end + 1);
        if (!name.empty()) {
            (void)save_named_layout(name);
        }
        layout_name_[0] = '\0';
    }
}

}  // namespace ws::ui::shell
