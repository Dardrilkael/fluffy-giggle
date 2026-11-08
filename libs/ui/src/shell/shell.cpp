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
#include <functional>
#include <utility>
#include <vector>

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

// --- the dock view's geometry constants -----------------------------------
//
// The tab drag's threshold: the pointer must carry a held tab this far
// before the drag arms, so a click with a jiggle stays the tab bar's
// click (the selection primitive) and never a re-dock.
constexpr float kTabDragPx = 6.0f;
// The drop-zone edge band: within this many pixels of a leaf's edge a
// drop splits to that side (dock_hit clamps the band to a quarter of
// the leaf's span per side, so a center zone always exists).
constexpr float kDockDropBandPx = 36.0f;
// The splitter double-click window, seconds of the frame clock.
constexpr float kSplitterDblClickS = 0.35f;
// The strip's zoom/restore button: a 12-px round hitbox, the close
// x's own size (the strip's two round affordances read as a pair).
constexpr float kZoomHit = 12.0f;
// The floating window's resize grip: a square at the bottom-right
// corner (three diagonal strokes, the classic grip mark). Big enough
// to hit at a glance, small enough to stay out of the scroll bars.
constexpr float kFloatGripPx = 14.0f;

// The drop preview's geometry, shared by the tab drag and the
// floating title-bar drag: the veil over where the drop would land -
// the whole target leaf for a retab, the half-leaf for a split (the
// accent as a tint, never a hole). A no-drop (leaf < 0) answers an
// empty rect, the caller's "nothing to paint".
[[nodiscard]] Rect drop_preview_rect(const DockTree::Layout& layout,
                                     const DockHit& over) {
    if (over.leaf < 0 ||
        static_cast<std::size_t>(over.leaf) >= layout.leaves.size()) {
        return Rect{};
    }
    Rect preview = layout.leaves[static_cast<std::size_t>(over.leaf)].rect;
    switch (over.area) {
        case DockPlacement::Area::left:
            preview.width *= 0.5f;
            break;
        case DockPlacement::Area::right:
            preview.x += preview.width * 0.5f;
            preview.width *= 0.5f;
            break;
        case DockPlacement::Area::top:
            preview.height *= 0.5f;
            break;
        case DockPlacement::Area::bottom:
            preview.y += preview.height * 0.5f;
            preview.height *= 0.5f;
            break;
        case DockPlacement::Area::center:
            break;  // the whole leaf: the tab joins it
    }
    return preview;
}

// `rect` re-seated to sit WHOLLY inside `bounds` (a window dragged to
// the edge keeps its title bar reachable; a layout restored under a
// smaller frame comes back on-screen). Width-first: a window wider
// than the frame pins to the frame's origin and overflows no further.
[[nodiscard]] Rect seat_in_bounds(Rect rect, const Rect& bounds) {
    rect.width = std::min(rect.width, bounds.width);
    rect.height = std::min(rect.height, bounds.height);
    rect.x = std::min(std::max(rect.x, bounds.x),
                      bounds.x + std::max(0.0f, bounds.width - rect.width));
    rect.y = std::min(std::max(rect.y, bounds.y),
                      bounds.y + std::max(0.0f, bounds.height - rect.height));
    return rect;
}

// `color` with its alpha channel replaced - the r2d packing is
// (r,g,b,a) one byte each, alpha in the low 8. The drop-preview veil:
// the theme's accent at a fraction of its ink, so the highlight reads
// as a tint over whatever it covers, not a hole.
[[nodiscard]] std::uint32_t with_alpha(std::uint32_t color,
                                       std::uint8_t alpha) {
    return (color & 0xffffff00u) | static_cast<std::uint32_t>(alpha);
}

// A square outline of four strokes, the geometry-drawn glyph spelling
// the strip's zoom/restore marks (the symbol table's own rule: a
// line-drawn mark reads in any font and none).
void square_outline(float cx, float cy, float arm, float weight,
                    std::uint32_t ink) {
    r2d::draw_line(cx - arm, cy - arm, cx + arm, cy - arm, weight, ink);
    r2d::draw_line(cx + arm, cy - arm, cx + arm, cy + arm, weight, ink);
    r2d::draw_line(cx + arm, cy + arm, cx - arm, cy + arm, weight, ink);
    r2d::draw_line(cx - arm, cy + arm, cx - arm, cy - arm, weight, ink);
}

// Pointer-in-array without the unspecified relational compare of
// unrelated pointers (std::less gives a total order).
template <class T>
[[nodiscard]] bool within(const std::vector<T>& items, const T* p) {
    return p != nullptr && !items.empty() &&
           !std::less<const T*>{}(p, items.data()) &&
           std::less<const T*>{}(p, items.data() + items.size());
}

// A layout name becomes a file name under <config>/layouts/: no
// separators, no drive colons, no dot-dot, no control characters.
[[nodiscard]] bool valid_layout_name(const std::string& name) {
    if (name.empty() || name == "." || name == ".." || name.size() > 128) {
        return false;
    }
    for (const char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' ||
            static_cast<unsigned char>(c) < 0x20) {
            return false;
        }
    }
    return name.find("..") == std::string::npos;
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
                return ws::core::unexpected<ws::core::Error>(shell_error(
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

void AppShell::set_toolbar_icons(std::vector<gui::ToolbarIcon> icons) {
    toolbar_icons_ = std::move(icons);
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
        // The pick rides a worker when one is installed (see
        // dispatch_dialog_pick): the completion opens what came back,
        // between frames where the shell's state is safe to touch.
        dispatch_dialog_pick(
            [this, options] { return config_.dialogs->open_file(options); },
            [this](const ws::core::Result<std::optional<std::string>>&
                       picked) {
                if (picked && picked->has_value()) {
                    (void)open_paths({**picked});
                }
            });
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
        Document* const document = documents_->active();
        if (document == nullptr) {
            return;
        }
        ws::platform::FileDialogOptions options;
        options.title = "save document";
        // The pick rides a worker when one is installed; the save lands
        // with it between frames. The document may close while the
        // picker was up - the answer lands only while it is still open.
        dispatch_dialog_pick(
            [this, options] { return config_.dialogs->save_file(options); },
            [this, document](const ws::core::Result<std::optional<std::string>>&
                                 picked) {
                if (find_document(document) < 0 || !picked ||
                    !picked->has_value()) {
                    return;
                }
                const ws::core::Status st =
                    apply_save_as(document, **picked);
                if (!st) {
                    toast_save_failure(st);
                }
            });
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

    // The tear-off and the way back, as commands: the same two acts
    // the gestures drive (the tab drag out, the title-bar drop in),
    // reachable from the palette when a pointer is not at hand. No
    // chords - the pointer owns the gesture, the palette owns the
    // menu.
    Command float_cmd;
    float_cmd.id = "shell.float_panel";
    float_cmd.label = "Float Active Panel";
    float_cmd.category = "Window";
    float_cmd.enabled = [this] {
        const PanelInstance* active = panel_host_.active();
        return active != nullptr && !panel_host_.is_floating(active->id);
    };
    float_cmd.execute = [this](std::string_view) {
        const PanelInstance* active = panel_host_.active();
        if (active == nullptr) {
            return;
        }
        // Half the dock, centered on it: a window big enough to work
        // in, small enough to see the dock it came out of. The last
        // frame's dock is the frame the user is looking at.
        Rect rect = last_dock_bounds_;
        rect.x += rect.width * 0.25f;
        rect.y += rect.height * 0.25f;
        rect.width *= 0.5f;
        rect.height *= 0.5f;
        panel_host_.float_panel(panel_registry_, active->id,
                                seat_in_bounds(rect, last_dock_bounds_));
        set_status("panel floated");
    };
    add_or_ignore(std::move(float_cmd));

    Command dock_cmd;
    dock_cmd.id = "shell.dock_panel";
    dock_cmd.label = "Dock Active Panel";
    dock_cmd.category = "Window";
    dock_cmd.enabled = [this] {
        const PanelInstance* active = panel_host_.active();
        return active != nullptr && panel_host_.is_floating(active->id);
    };
    dock_cmd.execute = [this](std::string_view) {
        const PanelInstance* active = panel_host_.active();
        if (active == nullptr) {
            return;
        }
        // Center into the last leaf: the tree's own "the new tab
        // joins what is there" (the same answer a center drop gives).
        panel_host_.redock(panel_registry_, active->id, DockPlacement{});
        set_status("panel docked");
    };
    add_or_ignore(std::move(dock_cmd));

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
        return ws::core::unexpected<ws::core::Error>(doc.error());
    }
    return config_.fs->write(path_of("settings.json"),
                             ws::settings::json::write(*doc));
}

ws::core::Status AppShell::load_layout() {
    if (config_.fs == nullptr || config_.config_dir.empty()) {
        return ws::core::unexpected<ws::core::Error>(
            shell_error(ShellError::unavailable, "no config dir"));
    }
    const ws::core::Result<std::string> text =
        config_.fs->read(path_of("layout.json"));
    if (!text) {
        return ws::core::unexpected<ws::core::Error>(text.error());
    }
    const ws::core::Result<ws::settings::json::Value> parsed =
        ws::settings::json::parse(*text);
    if (!parsed || !panel_host_.layout_from_json(
                           *parsed, [this](std::string_view panel) {
                               return panel_host_.find(panel) != nullptr;
                           })) {
        status_ = "layout.json would not parse; the default layout stands";
        return ws::core::unexpected<ws::core::Error>(
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
        ws::settings::json::write(panel_host_.layout_to_json()));
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
    // Keep the open set, rebuild the tree: every open instance -
    // docked OR floating (a reset must not eat a torn-off window's
    // panel) - lands at its descriptor's default placement, in open
    // order.
    std::vector<std::string> ids = panel_host_.dock().panels();
    for (const FloatingPanel& win : panel_host_.floating()) {
        ids.push_back(win.panel);
    }
    // Registration order, not the old tree's walk: the default
    // placements are written against it (the first panel fills the
    // empty root; later ones split or tab against what is there), so
    // a layout the user dragged around must not change what the reset
    // builds.
    const std::vector<PanelDesc>& order = panel_registry_.descriptors();
    const auto rank = [&](const std::string& id) {
        const PanelInstance* instance = panel_host_.find(id);
        for (std::size_t i = 0; instance != nullptr && i < order.size();
             ++i) {
            if (order[i].id == instance->type) {
                return i;
            }
        }
        return order.size();
    };
    std::stable_sort(ids.begin(), ids.end(),
                     [&](const std::string& a, const std::string& b) {
                         return rank(a) < rank(b);
                     });
    panel_host_.dock().clear();
    panel_host_.clear_floating();
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
        !valid_layout_name(name)) {
        return ws::core::unexpected<ws::core::Error>(shell_error(
            ShellError::invalid, "layout name is empty or not a valid file name"));
    }
    // layouts/<name>.json + the index entry (no listdir on the seam,
    // the names ride layouts.json).
    const ws::core::Status st = config_.fs->write(
        config_.config_dir + "/layouts/" + name + ".json",
        ws::settings::json::write(panel_host_.layout_to_json()));
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
        const ws::core::Status index_st = config_.fs->write(
            path_of("layouts.json"),
            ws::settings::json::write(
                ws::settings::json::Value::object(std::move(root))));
        if (!index_st) {
            return index_st;  // the file exists but the menu would never list it
        }
    }
    set_status("layout saved: " + name);
    return {};
}

ws::core::Status AppShell::load_named_layout(const std::string& name) {
    if (config_.fs == nullptr || config_.config_dir.empty()) {
        return ws::core::unexpected<ws::core::Error>(
            shell_error(ShellError::unavailable, "no config dir"));
    }
    if (!valid_layout_name(name)) {
        return ws::core::unexpected<ws::core::Error>(
            shell_error(ShellError::invalid, "invalid layout name"));
    }
    const ws::core::Result<std::string> text = config_.fs->read(
        config_.config_dir + "/layouts/" + name + ".json");
    if (!text) {
        return ws::core::unexpected<ws::core::Error>(text.error());
    }
    const ws::core::Result<ws::settings::json::Value> parsed =
        ws::settings::json::parse(*text);
    if (!parsed || !panel_host_.layout_from_json(
                           *parsed, [this](std::string_view panel) {
                               return panel_host_.find(panel) != nullptr;
                           })) {
        return ws::core::unexpected<ws::core::Error>(
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
        return ws::core::unexpected<ws::core::Error>(
            shell_error(ShellError::invalid, "no active document"));
    }
    if (document->path().empty()) {
        // Untitled: the save-as pick (the dialogs injection is the
        // whole story; without it the save refuses).
        if (config_.dialogs == nullptr) {
            return ws::core::unexpected<ws::core::Error>(shell_error(
                ShellError::unavailable, "cannot save an untitled document"));
        }
        ws::platform::FileDialogOptions options;
        options.title = "save document";
        if (config_.jobs != nullptr) {
            // The pick rides a worker and the save lands with it
            // between frames (dispatch_dialog_pick's rule); the
            // command's answer is "accepted", and a landed failure
            // toasts on its own.
            dispatch_dialog_pick(
                [this, options] { return config_.dialogs->save_file(options); },
                [this, document](const ws::core::Result<
                                     std::optional<std::string>>& picked) {
                    if (find_document(document) < 0 || !picked ||
                        !picked->has_value()) {
                        return;
                    }
                    // A save that landed meanwhile (a second Ctrl+S
                    // through the picker) wins; the stale pick answers
                    // nothing.
                    if (!document->path().empty()) {
                        return;
                    }
                    const ws::core::Status st =
                        apply_save_as(document, **picked);
                    if (!st) {
                        toast_save_failure(st);
                    }
                });
            return {};
        }
        const ws::core::Result<std::optional<std::string>> picked =
            config_.dialogs->save_file(options);
        if (!picked || !picked->has_value()) {
            return {};
        }
        return apply_save_as(document, **picked);
    }
    return apply_save_as(document, document->path());
}

// --- the pickers' dispatch (the header's own contract) ------------------------

void AppShell::dispatch_dialog_pick(DialogPick pick, DialogLanded landed) {
    if (config_.jobs == nullptr) {
        // No pool: the historic inline shape - the headless hosts and
        // every existing test.
        landed(pick());
        return;
    }
    // The dialogs header's rule, honoured: the blocking call runs on a
    // worker (on android the surface lifecycle marches on while the
    // SAF picker is up - the render thread must never be the waiter),
    // and the answer lands on the main thread between frames, where
    // the shell's state is safe to touch. The handle drops on the
    // floor: the completion owns the follow-up, and a completion
    // still queued at teardown is dropped by the job system (never
    // run) - a picker open at shutdown closes nothing.
    // The completion rides the callbacks struct the way every host
    // spells it (the jobs suite's own shape).
    ws::jobs::JobCallbacks<std::optional<std::string>> callbacks;
    callbacks.on_complete = [landed =
                                 std::move(landed)](
                                ws::jobs::JobHandle<std::optional<std::string>>
                                    handle) {
        landed(handle.take());
    };
    (void)config_.jobs->submit<std::optional<std::string>>(
        [pick = std::move(pick)](
            ws::jobs::JobContext&) -> ws::core::Result<std::optional<std::string>> {
            return pick();  // blocks the worker until the user decides
        },
        ws::jobs::Priority::normal, {}, std::move(callbacks));
}

ws::core::Status AppShell::apply_save_as(Document* document,
                                         const std::string& path) {
    const std::string previous_path = document->path();
    document->set_path(path);
    const ws::core::Status st = document->save();
    if (!st) {
        // the save failed: keep the old identity
        document->set_path(previous_path);
        return st;
    }
    (void)document->undo().mark_saved();
    (void)documents_->finish_autosave(static_cast<std::size_t>(
        std::max(0, find_document(document))));
    set_status("saved " + document->display_name());
    return {};
}

int AppShell::find_document(const Document* document) const {
    if (document == nullptr) {
        return -1;
    }
    for (std::size_t i = 0; i < documents_->count(); ++i) {
        if (documents_->at(i) == document) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void AppShell::toast_save_failure(const ws::core::Status& st) {
    toast_text_ = "save failed: " + st.error().message;
    toasts_.push({toast_text_.c_str(), gui::ToastSeverity::error, nullptr,
                  5.0f});
}

ws::core::Status AppShell::open_paths(const std::vector<std::string>& paths) {
    std::optional<ws::core::Error> first_error;
    std::size_t failed = 0;
    for (const std::string& path : paths) {
        const ws::core::Result<int> index = documents_->open_path(path);
        if (!index) {
            if (!first_error) {
                first_error = index.error();
            }
            ++failed;
            continue;  // the rest of the drop still opens
        }
        set_status("opened " + path);
    }
    if (first_error) {
        toast_text_ = "open failed: " + first_error->message;
        if (failed > 1) {
            toast_text_ += " (+" + std::to_string(failed - 1) + " more)";
        }
        toasts_.push({toast_text_.c_str(), gui::ToastSeverity::error, nullptr,
                      5.0f});
        return ws::core::unexpected<ws::core::Error>(*first_error);
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

    // The menu bar strip: the bar's own face first - the chrome is a
    // depth layer away from the panels, so the frame reads as frame
    // (nothing painted it before; the strip rode the r2d clear color,
    // one flat void with the dock below it).
    const Rect bar{frame.x, frame.y, frame.width, theme.control_height};
    r2d::draw_quad(bar.x, bar.y, bar.width, bar.height, theme.bar_bg);
    draw_menu_bar(gui, layout, theme, bar);
    r2d::draw_quad(bar.x, bar.y + bar.height - 1.0f, bar.width, 1.0f,
                   theme.border);

    // The toolbar strip (only when the host named commands for it).
    // The icon-and-tooltip spelling: every button carries its hint
    // (the live chord through the shell's own resolver at the frame's
    // key context), and a command the host mapped in set_toolbar_icons
    // wears its glyph instead of the text.
    float top = frame.y + theme.control_height;
    if (!toolbar_ids_.empty()) {
        const Rect tool{frame.x, top, frame.width, theme.control_height};
        r2d::draw_quad(tool.x, tool.y, tool.width, tool.height, theme.bar_bg);
        layout.begin_panel(tool, 3.0f, theme.spacing);
        const std::string_view fired = gui::command_toolbar(
            gui, layout, theme, "shell.toolbar", commands_, toolbar_ids_,
            toolbar_icons_.empty() ? nullptr : toolbar_icons_.data(),
            toolbar_icons_.size(), &shortcuts_, key_context());
        if (!fired.empty()) {
            (void)commands_.execute(fired);
        }
        layout.end_panel();
        r2d::draw_quad(tool.x, tool.y + tool.height - 1.0f, tool.width,
                       1.0f, theme.border);
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
        if (within(recent_items_, event.item)) {
            return;  // submenu rows live in recent_items_: not an index into window_items_
        }
        if (!within(window_items_, event.item)) {
            return;
        }
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
    last_dock_bounds_ = bounds;  // the float command's geometry source
    r2d::draw_quad(bounds.x, bounds.y, bounds.width, bounds.height, theme.panel_bg);
    const DockTree::Layout resolved = panel_host_.dock().resolve(bounds);
    // While a pane is zoomed, resolve() answers the PROJECTION: one
    // leaf (index 0) spanning the dock. A drop aimed at it must land
    // in the zoomed panel's REAL leaf or the tab would dock into a
    // pane the screen is not showing - every drop path (the tab drag,
    // the title-bar drag) re-maps leaf 0 through this one rule.
    DockTree& dock = panel_host_.dock();
    const std::string zoomed{dock.maximized()};
    const auto drop_leaf = [&dock, &zoomed](int leaf) {
        if (!zoomed.empty() && leaf == 0) {
            return dock.leaf_of(zoomed);
        }
        return leaf;
    };
    // The one input view for the whole dock view, and the splitter
    // clock's tick: the frame's dt is the honest clock a double-click
    // needs (a hand-built test frame reports 0 - every press then
    // lands "instantly", and the window still needs TWO of them).
    const ws::input::InputFrame& in = gui.input();
    splitter_clock_ += in.dt_seconds;

    // The splitters first: their hit claims land before any leaf
    // widget's, so a boundary grab always wins over content under it.
    // The fill is the bar face - a visible recessed gutter between
    // the panels, not a 1-px color change nobody can find.
    for (const DockTree::SplitterRect& splitter : resolved.splitters) {
        r2d::draw_quad(splitter.rect.x, splitter.rect.y, splitter.rect.width,
                       splitter.rect.height, theme.bar_bg);
        // The splitter's identity: its split node's path plus the pair
        // it divides - unique per band, stable while the tree holds its
        // shape. One flat "shell.splitter" for every band made them one
        // widget in the census's eyes (the duplicate-id warning's own
        // case): with two or more splitters a press on either lit both
        // and each drag overwrote the last claim, so the boundary you
        // grabbed was not the one that moved.
        for (const int leg : splitter.path) {
            gui.ids().push(leg);
        }
        gui.ids().push(static_cast<int>(splitter.first_child));
        const gui::WidgetId id = gui.ids().id("shell.splitter");
        const gui::HitTest hit = gui.hit_test(id, splitter.rect);
        for (std::size_t popped = 0;
             popped < splitter.path.size() + 1; ++popped) {
            gui.ids().pop();
        }
        if (hit.pressed) {
            // A FRESH press on this band (the claim rides the whole
            // hold; the edge is the frame's own) may be the second
            // half of a double-click: two presses on one band inside
            // the window even the split - the splitter's reset, the
            // gesture every dock ships. The same hold must not then
            // also drag the boundary it just evened under the pointer.
            if (in.mouse_pressed[0]) {
                const bool same_band =
                    splitter_last_press_.path == splitter.path &&
                    splitter_last_press_.first_child ==
                        splitter.first_child;
                if (same_band &&
                    splitter_clock_ - splitter_last_press_at_ <
                        kSplitterDblClickS) {
                    panel_host_.dock().distribute(splitter);
                    splitter_last_press_at_ = -1.0e30f;  // consumed: a
                    // third press is a fresh first, not a triple
                    splitter_drag_.consumed = true;
                } else {
                    splitter_last_press_at_ = splitter_clock_;
                    splitter_last_press_ = splitter;
                    splitter_drag_.consumed = false;
                }
            }
            splitter_drag_.active = true;
            splitter_drag_.splitter = splitter;
        }
    }
    if (splitter_drag_.active) {
        if (in.mouse_down[0] && in.mouse_present) {
            if (!splitter_drag_.consumed) {
                const float position =
                    splitter_drag_.splitter.dir == DockDir::horizontal
                        ? in.mouse_x
                        : in.mouse_y;
                panel_host_.dock().apply_drag(splitter_drag_.splitter,
                                              position);
            }
        } else {
            splitter_drag_.active = false;
        }
    }

    // Focus follows press: a press inside a leaf activates that leaf's
    // front tab (the tab strip's own selection does the same on click).
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
    std::string closing_panel;  // closed after the walk: see below
    for (std::size_t leaf_idx = 0; leaf_idx < resolved.leaves.size(); ++leaf_idx) {
        const DockTree::LeafRect& leaf = resolved.leaves[leaf_idx];
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

        // The strip's own face + a 1-px rule at its bottom edge, BEFORE
        // the tabs: the selected tab's panel-bg face paints over the
        // rule under itself, so it merges with its body while the
        // unselected tabs keep the rule as their separator (the
        // editor-tab geometry). Painted for an empty leaf too - the
        // band is chrome, tabs or not, so an empty dock still reads as
        // a docked frame rather than a hole in the face.
        r2d::draw_quad(strip.x, strip.y, strip.width, strip.height,
                       theme.bar_bg);
        r2d::draw_quad(strip.x, strip.y + strip.height - 1.0f,
                       strip.width, 1.0f, theme.border);

        const Rect content{leaf.rect.x, leaf.rect.y + strip_h, leaf.rect.width,
                           std::max(0.0f, leaf.rect.height - strip_h)};
        if (content.height <= 0.0f) {
            continue;
        }
        if (labels.empty()) {
            gui::panel_draw(content, theme);  // the empty-dock chrome
            draw_empty_dock(gui, theme, content);
            continue;
        }

        // The leaf's own id scope. Every leaf's strip used to spell the
        // same "shell.tabs" ids, and two strips in one frame SHARE a
        // widget (the census's duplicate-id warning, fired for real
        // here): a press on either tab lit both, and the earlier
        // leaf's release ate the later leaf's clicks whole - in the
        // stock split, plot's tab and close were dead buttons beside a
        // live editor's. The resolve index scopes the strips apart
        // (stable while the tree holds its shape; nothing worth keeping
        // keys under a strip - the bodies id by panel instance below).
        const gui::ScopedId leaf_ids(gui.ids(), leaf.leaf);
        int active = std::clamp(leaf.active, 0,
                                static_cast<int>(labels.size()) - 1);
        // The tab pitch leaves the strip's right end its own room: the
        // zoom button below is a strip-level affordance, and a full
        // row of tabs must not cover it (or its x) with the last tab.
        const float tab_w =
            std::max(40.0f,
                     std::min(160.0f,
                              (leaf.rect.width -
                               (2.0f * theme.padding + kZoomHit)) /
                                  static_cast<float>(labels.size())));
        layout.begin_panel(strip, 0.0f, 0.0f);
        // The closable strip: each tab carries its own round x INSIDE
        // the tab (a small circular hitbox - the strip-level square
        // button right of the tabs read as nothing at all, and it
        // closed only the FRONT tab). tab_bar's out-params name the tab
        // whose x fired and the tab whose press is still held; the
        // strip stays the selection primitive, the drag rides its
        // claim.
        int closed_tab = -1;
        int held_tab = -1;
        if (gui::tab_bar(gui, layout, theme, "shell.tabs", labels.data(),
                         static_cast<int>(labels.size()), active, tab_w,
                         false, &closed_tab, &held_tab)) {
            // The selection moved: the tree and the focus follow.
            panel_host_.activate(panel_registry_,
                                 (*leaf.tabs)[static_cast<std::size_t>(active)]);
        }
        layout.end_panel();
        if (held_tab >= 0 && !tab_drag_.armed) {
            // The drag's arm: which panel's tab is under a held press,
            // and where the hold started. The threshold test below
            // decides drag-vs-click; the strip's own click keeps the
            // selection when the pointer never carries the tab off.
            // The leaf's own bounds ride along: a tear-off sizes its
            // window after the pane the tab came out of.
            tab_drag_.armed = true;
            tab_drag_.dragging = false;
            tab_drag_.panel = (*leaf.tabs)[static_cast<std::size_t>(held_tab)];
            tab_drag_.press_x = in.mouse_x;
            tab_drag_.press_y = in.mouse_y;
            tab_drag_.origin_rect = leaf.rect;
        }

        // The strip's right end: the focused-pane zoom. A round ghost
        // button the close x's own size, with a GEOMETRY mark - the
        // lone square for "zoom this pane to the whole dock", the
        // doubled square while zoomed for "put the layout back". The
        // hitbox sits in the room the tab pitch reserved, so it never
        // covers a tab or its x.
        const Rect zoom_rect{strip.x + strip.width - theme.padding -
                                 kZoomHit,
                             strip.y + (strip_h - kZoomHit) * 0.5f,
                             kZoomHit, kZoomHit};
        const gui::WidgetId zoom_id = gui.ids().id("shell.zoom");
        gui::HitShape zoom_shape;
        zoom_shape.kind = gui::HitShape::Kind::circle;
        const gui::ButtonResult zoom =
            gui::button_behavior(gui, zoom_id, zoom_rect, false, zoom_shape,
                                 gui::ButtonStyle::ghost);
        std::uint32_t zoom_face = theme.control_bg;
        if (zoom.state == gui::WidgetState::Pressed) {
            zoom_face = theme.control_pressed;
        } else if (zoom.state == gui::WidgetState::Hovered) {
            zoom_face = theme.control_hovered;
        }
        r2d::draw_rounded_quad(zoom_rect.x, zoom_rect.y, kZoomHit, kZoomHit,
                               kZoomHit * 0.5f, zoom_face);
        const bool is_zoomed = !panel_host_.dock().maximized().empty();
        const bool zoom_live = zoom.state == gui::WidgetState::Hovered ||
                               zoom.state == gui::WidgetState::Pressed;
        const std::uint32_t zoom_ink =
            zoom_live ? theme.text : theme.text_muted;
        if (is_zoomed) {
            square_outline(zoom_rect.center_x() - 1.6f,
                           zoom_rect.center_y() - 1.6f, 3.4f, 1.4f, zoom_ink);
            square_outline(zoom_rect.center_x() + 1.6f,
                           zoom_rect.center_y() + 1.6f, 3.4f, 1.4f, zoom_ink);
        } else {
            square_outline(zoom_rect.center_x(), zoom_rect.center_y(), 4.2f,
                           1.4f, zoom_ink);
        }
        gui::tooltip_text(gui, layout, theme, "shell.zoom", zoom_rect,
                          is_zoomed ? "Restore layout" : "Maximize panel");
        if (zoom.activated()) {
            DockTree& dock_ref = panel_host_.dock();
            if (is_zoomed) {
                dock_ref.restore();
            } else {
                dock_ref.maximize(
                    (*leaf.tabs)[static_cast<std::size_t>(active)]);
            }
        }
        if (closed_tab >= 0) {
            // Not closed here: closing prunes the tree and moves nodes, and
            // the later leaves' LeafRect::tabs still point into it.
            closing_panel =
                (*leaf.tabs)[static_cast<std::size_t>(closed_tab)];
            continue;  // the leaf's body is gone this frame
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
    if (!closing_panel.empty()) {
        panel_host_.close(panel_registry_, closing_panel);
    }

    // The tab drag's frame: the threshold, the preview, the drop. The
    // strips above armed it off their held claim; the tree answers
    // where a drop lands (dock_hit over THIS frame's resolve - the
    // tree has not moved since the drag began, so the answer is the
    // one the release sees).
    if (tab_drag_.armed) {
        if (!(in.mouse_present && in.mouse_down[0])) {
            if (tab_drag_.dragging) {
                const DockHit drop =
                    dock_hit(resolved, in.mouse_x, in.mouse_y,
                             kDockDropBandPx);
                if (drop.leaf >= 0) {
                    DockPlacement placement;
                    placement.area = drop.area;
                    placement.leaf = drop_leaf(drop.leaf);
                    panel_host_.redock(panel_registry_, tab_drag_.panel,
                                       placement);
                } else {
                    // Released outside every leaf: the tear-off. The
                    // panel becomes a floating window centered on the
                    // release, sized after the pane it came out of -
                    // the ImGui gesture, the half-way point between
                    // "cancel" (a dragged tab snapping home) and
                    // "guess" (a panel dropped into a leaf nobody
                    // pointed at).
                    Rect tear = tab_drag_.origin_rect;
                    const float max_w = std::max(kFloatingMinW, bounds.width);
                    const float max_h = std::max(kFloatingMinH, bounds.height);
                    tear.width = std::clamp(tear.width, kFloatingMinW, max_w);
                    tear.height = std::clamp(tear.height, kFloatingMinH, max_h);
                    tear.x = in.mouse_x - tear.width * 0.5f;
                    tear.y = in.mouse_y - tear.height * 0.5f;
                    panel_host_.float_panel(panel_registry_,
                                            tab_drag_.panel,
                                            seat_in_bounds(tear, bounds));
                }
            }
            tab_drag_ = TabDrag{};  // the hold ended: dropped or torn off
        } else {
            const float dx = in.mouse_x - tab_drag_.press_x;
            const float dy = in.mouse_y - tab_drag_.press_y;
            if (!tab_drag_.dragging &&
                dx * dx + dy * dy >= kTabDragPx * kTabDragPx) {
                tab_drag_.dragging = true;
            }
            if (tab_drag_.dragging) {
                // The drop preview: the shared veil over where the
                // panel would land (the whole leaf for a retab, the
                // half-leaf for a split).
                const DockHit over = dock_hit(resolved, in.mouse_x,
                                              in.mouse_y, kDockDropBandPx);
                const Rect preview = drop_preview_rect(resolved, over);
                if (preview.width > 0.0f && preview.height > 0.0f) {
                    r2d::draw_quad(preview.x, preview.y, preview.width,
                                   preview.height,
                                   with_alpha(theme.control_on, 56));
                }
            }
        }
    }

    // The floating layer, over the dock and its veil: the torn-off
    // windows, their drags and their drops.
    draw_floating(gui, layout, theme, bounds, resolved);
}

void AppShell::draw_floating(GuiContext& gui, LayoutCursor& layout,
                             const ChromeTheme& theme, const Rect& bounds,
                             const DockTree::Layout& resolved) {
    const ws::input::InputFrame& in = gui.input();
    const std::vector<FloatingPanel>& windows = panel_host_.floating();
    if (windows.empty() && !float_drag_.active) {
        return;  // nothing torn off, nothing dragging
    }

    // The window the frame actually shows: the stored rect re-seated
    // whole inside the dock (a restored layout may sit outside a
    // smaller frame; a drag never stores an off-frame rect because it
    // moves through this clamp).
    const auto seat = [&bounds](const FloatingPanel& win) {
        return seat_in_bounds(win.rect, bounds);
    };

    // The press's raise + focus: the TOPMOST window under the pointer
    // (the z walk, back to front - a press on an overlap must pick
    // exactly one window, the one the eye sees on top).
    if (in.mouse_present && in.mouse_pressed[0]) {
        for (std::size_t i = windows.size(); i-- > 0;) {
            if (point_in(seat(windows[i]), in.mouse_x, in.mouse_y)) {
                panel_host_.raise_floating(windows[i].panel);
                panel_host_.activate(panel_registry_, windows[i].panel);
                break;
            }
        }
    }

    // A live move-drag's veil, under the windows and over the dock:
    // the same tint the tab drag paints, the same zones - one drop
    // grammar, two gestures into it. Only once the pointer CARRIED the
    // window: a held click paints nothing.
    if (float_drag_.active && float_drag_.dragging &&
        !float_drag_.resizing) {
        const DockHit over = dock_hit(resolved, in.mouse_x, in.mouse_y,
                                      kDockDropBandPx);
        const Rect preview = drop_preview_rect(resolved, over);
        if (preview.width > 0.0f && preview.height > 0.0f) {
            r2d::draw_quad(preview.x, preview.y, preview.width,
                           preview.height,
                           with_alpha(theme.control_on, 56));
        }
    }

    // The windows, back to front: the later entries claim later, so
    // the top window's widgets win the taps over whatever it covers -
    // the draw order IS the z order.
    std::vector<std::string> closing;  // closed after the walk
    for (const FloatingPanel& win : windows) {
        // The id first, stable in hand: raise_floating/move_floating
        // reorder the very vector this loop walks, and a moved element
        // must not pull the id out from under the paint that follows.
        const std::string panel = win.panel;
        const Rect rect = seat(win);
        const PanelInstance* instance = panel_host_.find(panel);
        const PanelDesc* desc =
            instance == nullptr ? nullptr
                                : panel_registry_.find(instance->type);

        const gui::ScopedId win_ids(gui.ids(), panel.c_str());
        const Rect bar{rect.x, rect.y, rect.width, theme.control_height};
        const Rect content{rect.x, rect.y + bar.height, rect.width,
                           std::max(0.0f, rect.height - bar.height)};
        const Rect close_rect{bar.x + bar.width - theme.padding - kZoomHit,
                              bar.y + (bar.height - kZoomHit) * 0.5f,
                              kZoomHit, kZoomHit};
        const Rect grip{rect.x + rect.width - kFloatGripPx,
                        rect.y + rect.height - kFloatGripPx, kFloatGripPx,
                        kFloatGripPx};

        // The window's face, then its 1-px frame, then the title bar
        // over both - a floating panel reads as a raised card, not a
        // hole in the dock.
        r2d::draw_quad(rect.x, rect.y, rect.width, rect.height,
                       theme.panel_bg);
        r2d::draw_quad(rect.x, rect.y, rect.width, 1.0f, theme.border);
        r2d::draw_quad(rect.x, rect.y + rect.height - 1.0f, rect.width,
                       1.0f, theme.border);
        r2d::draw_quad(rect.x, rect.y, 1.0f, rect.height, theme.border);
        r2d::draw_quad(rect.x + rect.width - 1.0f, rect.y, 1.0f,
                       rect.height, theme.border);
        r2d::draw_quad(bar.x, bar.y, bar.width, bar.height, theme.bar_bg);
        r2d::draw_quad(bar.x, bar.y + bar.height - 1.0f, bar.width, 1.0f,
                       theme.border);

        // The title bar's drag: a press on the bar (the close button's
        // pixels excepted - its claim registers below and owns them)
        // arms the move. The bar's own hit_test answers the capture;
        // the PRESS EDGE is the arm, so a click that never carries the
        // window is just the raise + focus the press already did.
        const gui::WidgetId bar_id = gui.ids().id("shell.float.bar");
        const gui::HitTest bar_hit = gui.hit_test(bar_id, bar);
        if (bar_hit.pressed && in.mouse_pressed[0] && !float_drag_.active &&
            !tab_drag_.armed && !point_in(close_rect, in.mouse_x, in.mouse_y)) {
            float_drag_.active = true;
            float_drag_.resizing = false;
            float_drag_.panel = panel;
            float_drag_.grab_x = in.mouse_x;
            float_drag_.grab_y = in.mouse_y;
            float_drag_.base_x = rect.x;
            float_drag_.base_y = rect.y;
            float_drag_.start = rect;
        }

        // The title: the panel's label, ellipsized into the room the
        // close button leaves.
        {
            const float text_w = std::max(
                0.0f, bar.width - 2.0f * theme.padding - kZoomHit);
            LayoutCursor title;
            const Rect text_rect{bar.x + theme.padding, bar.y, text_w,
                                 bar.height};
            title.begin_panel(text_rect, 0.0f, 0.0f);
            gui::text(title, theme,
                      instance != nullptr ? instance->title.c_str()
                                          : panel.c_str(),
                      title.remaining_width());
            title.end_panel();
        }

        // The close button: the tab strip's own round x, the same
        // size and the same ghost face (one close affordance, docked
        // or torn off).
        gui::HitShape close_shape;
        close_shape.kind = gui::HitShape::Kind::circle;
        const gui::ButtonResult close =
            gui::button_behavior(gui, gui.ids().id("shell.float.close"),
                                 close_rect, false, close_shape,
                                 gui::ButtonStyle::ghost);
        std::uint32_t close_face = theme.control_bg;
        if (close.state == gui::WidgetState::Pressed) {
            close_face = theme.control_pressed;
        } else if (close.state == gui::WidgetState::Hovered) {
            close_face = theme.control_hovered;
        }
        r2d::draw_rounded_quad(close_rect.x, close_rect.y, kZoomHit,
                               kZoomHit, kZoomHit * 0.5f, close_face);
        const bool close_live =
            close.state == gui::WidgetState::Hovered ||
            close.state == gui::WidgetState::Pressed;
        const std::uint32_t close_ink =
            close_live ? theme.text : theme.text_muted;
        {
            const float cx = close_rect.center_x();
            const float cy = close_rect.center_y();
            const float a = 2.6f;
            r2d::draw_line(cx - a, cy - a, cx + a, cy + a, 1.4f, close_ink);
            r2d::draw_line(cx - a, cy + a, cx + a, cy - a, 1.4f, close_ink);
        }
        gui::tooltip_text(gui, layout, theme, "shell.float.close",
                          close_rect, "Close panel");
        if (close.activated()) {
            closing.push_back(panel);  // closed after the walk, like a tab
        }

        // The resize grip: the bottom-right corner, three diagonal
        // strokes. A press there arms the resize; the drag frame below
        // grows the rect by the pointer's delta.
        const gui::WidgetId grip_id = gui.ids().id("shell.float.grip");
        const gui::HitTest grip_hit = gui.hit_test(grip_id, grip);
        if (grip_hit.pressed && in.mouse_pressed[0] && !float_drag_.active) {
            float_drag_.active = true;
            float_drag_.resizing = true;
            float_drag_.panel = panel;
            float_drag_.grab_x = in.mouse_x;
            float_drag_.grab_y = in.mouse_y;
            float_drag_.base_x = rect.x;
            float_drag_.base_y = rect.y;
            float_drag_.start = rect;
        }
        {
            const bool grip_live =
                grip_hit.hovered || grip_hit.pressed;
            const std::uint32_t grip_ink =
                grip_live ? theme.text : theme.text_muted;
            const float s = kFloatGripPx;
            r2d::draw_line(rect.x + rect.width - 4.0f, rect.y + rect.height - s + 4.0f,
                           rect.x + rect.width - s + 4.0f,
                           rect.y + rect.height - 4.0f, 1.2f, grip_ink);
            r2d::draw_line(rect.x + rect.width - 4.0f,
                           rect.y + rect.height - s + 8.0f,
                           rect.x + rect.width - s + 8.0f,
                           rect.y + rect.height - 4.0f, 1.2f, grip_ink);
            r2d::draw_line(rect.x + rect.width - 8.0f,
                           rect.y + rect.height - s + 4.0f,
                           rect.x + rect.width - s + 4.0f,
                           rect.y + rect.height - 8.0f, 1.2f, grip_ink);
        }
        gui::tooltip_text(gui, layout, theme, "shell.float.grip", grip,
                          "Resize");

        // The body: the panel's own draw in a scroll scope, the same
        // shape a leaf's body takes.
        if (content.height > 0.0f) {
            gui::panel_draw(content, theme);
            if (desc != nullptr && desc->draw && instance != nullptr) {
                const std::string label = "shell.float.body." + panel;
                (void)gui::begin_scroll_panel(gui, layout, theme,
                                              label.c_str(), content);
                desc->draw(gui, layout, theme, *instance);
                gui::end_scroll_panel(gui, layout, theme);
            }
        }
    }
    for (const std::string& panel : closing) {
        if (float_drag_.active && float_drag_.panel == panel) {
            float_drag_ = FloatDrag{};  // the dragged window left
        }
        panel_host_.close(panel_registry_, panel);
    }

    // The drag frame: the press armed it, the threshold starts it (a
    // click never moves the window - the raise + focus the press did
    // is the whole click), and the release decides the home - over a
    // leaf, back into the tree; anywhere else, the floating rect the
    // move already left behind.
    if (!float_drag_.active) {
        return;
    }
    if (in.mouse_present && in.mouse_down[0]) {
        const float dx = in.mouse_x - float_drag_.grab_x;
        const float dy = in.mouse_y - float_drag_.grab_y;
        if (!float_drag_.dragging &&
            dx * dx + dy * dy >= kTabDragPx * kTabDragPx) {
            float_drag_.dragging = true;
        }
        if (!float_drag_.dragging) {
            return;
        }
        if (float_drag_.resizing) {
            Rect grown = float_drag_.start;
            grown.width += dx;
            grown.height += dy;
            panel_host_.move_floating(float_drag_.panel,
                                      seat_in_bounds(grown, bounds));
        } else {
            Rect moved = float_drag_.start;
            moved.x = float_drag_.base_x + dx;
            moved.y = float_drag_.base_y + dy;
            panel_host_.move_floating(float_drag_.panel,
                                      seat_in_bounds(moved, bounds));
        }
        return;
    }
    if (float_drag_.dragging && !float_drag_.resizing) {
        DockTree& dock = panel_host_.dock();
        const std::string zoomed{dock.maximized()};
        const DockHit drop = dock_hit(resolved, in.mouse_x, in.mouse_y,
                                      kDockDropBandPx);
        if (drop.leaf >= 0) {
            DockPlacement placement;
            placement.area = drop.area;
            // The zoomed projection answers leaf 0; the real leaf is
            // the zoomed panel's own (the tab drag's own rule).
            placement.leaf = !zoomed.empty() && drop.leaf == 0
                                 ? dock.leaf_of(zoomed)
                                 : drop.leaf;
            panel_host_.redock(panel_registry_, float_drag_.panel,
                               placement);
        }
        // A release over no leaf (or over another floating window -
        // the zones are the dock's) leaves the window where the move
        // put it: floating is a home, not a limbo.
    }
    float_drag_ = FloatDrag{};
}

void AppShell::draw_empty_dock(GuiContext& gui, const ChromeTheme& theme,
                               const Rect& content) {
    // The way back. The Window menu's check rows answer "reopen a
    // panel" from the menu bar; this answers it where the eye already
    // is - one button per registered panel type, a click reopening it
    // at its default placement. Without it, closing the last panel
    // leaves a bare face whose only exit is a menu you may never open.
    const std::vector<PanelDesc>& descs = panel_registry_.descriptors();
    if (descs.empty() || content.width <= 0.0f || content.height <= 0.0f) {
        return;
    }
    const float row = theme.control_height;
    const float block = row + theme.spacing +
                        static_cast<float>(descs.size()) * (row + theme.spacing);
    const float top =
        content.y + std::max(theme.padding, (content.height - block) * 0.5f);
    const float width =
        std::min(220.0f, std::max(80.0f, content.width - 2.0f * theme.padding));
    const Rect host{content.x + (content.width - width) * 0.5f, top, width,
                    std::max(0.0f, content.y + content.height - top)};
    if (host.height < 2.0f * row) {
        return;  // not even a header and one row: the chrome alone
    }
    LayoutCursor reopen;
    reopen.begin_panel(host, theme.padding, theme.spacing);
    gui::text(reopen, theme, "No panels open");
    {
        // The loop-children discipline: a row index under one scope, so
        // two descriptors sharing a title still id apart (the registry
        // guarantees distinct ids, not distinct titles).
        const gui::ScopedId reopen_ids(gui.ids(), "shell.reopen");
        int index = 0;
        for (const PanelDesc& desc : descs) {
            const gui::ScopedId row_ids(gui.ids(), index++);
            if (gui::button(gui, reopen, theme, desc.title.c_str(), -1.0f)) {
                (void)panel_host_.open(panel_registry_, desc.id);
            }
        }
    }
    reopen.end_panel();
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
        documents_->activate(index);  // save_active() saves the active one
        Document* const doc = documents_->at(index);
        if (doc != nullptr && doc->path().empty() &&
            config_.dialogs != nullptr && config_.jobs != nullptr) {
            // An untitled save through the worker (the pickers' rule):
            // the close lands WITH the save, in the completion. A
            // dismissed picker keeps the document open - the prompt
            // stays answered, the user re-asks.
            ws::platform::FileDialogOptions options;
            options.title = "save document";
            dispatch_dialog_pick(
                [this, options] { return config_.dialogs->save_file(options); },
                [this, doc](const ws::core::Result<
                                     std::optional<std::string>>& picked) {
                    const int at = find_document(doc);
                    if (at < 0 || !picked || !picked->has_value()) {
                        return;
                    }
                    if (apply_save_as(doc, **picked)) {
                        (void)documents_->finish_close(
                            static_cast<std::size_t>(at), false);
                        set_status("saved and closed");
                    }
                    // a failed save toasts; the document stays open
                });
            return;  // the modal is answered; the completion closes
        }
        if (const ws::core::Status st = save_active(); !st) {
            toast_save_failure(st);
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
            } else {
                toast_text_ = "recovery failed: " + index.error().message;
                toasts_.push({toast_text_.c_str(), gui::ToastSeverity::error,
                              nullptr, 5.0f});
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
