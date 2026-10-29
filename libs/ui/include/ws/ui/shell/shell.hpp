#pragma once

// AppShell (PLAN_UI_STAGE14_15 stage 15): the reusable skeleton that
// ties the registry, the dock/panel layer, the document layer and the
// platform services into one frame - the minimal in-repo version of
// Part A's shell, the seam clear so a future full Part A can replace
// it.
//
// Shape (the plan's stage-15 design notes, enforced here):
//   * AppShell is a PLAIN CLASS the host's App subclass owns and drives
//     from on_update(); it never touches ws::ui::App, so the whole
//     shell is headless-testable (the test drives draw() through the
//     r2d stub exactly like the gui suite).
//   * It never calls a make_* factory: the host injects the
//     FileDialogs / MessageBoxes interfaces (fake-friendly) and the
//     config's FileSystem seam.
//   * The shell contains no product code: a host registers commands,
//     panels, a document factory and a settings schema, and everything
//     else is provided.
//
// What one frame of draw() lays out, top to bottom:
//   menu bar (host menus + the automatic Window menu)
//   toolbar (the host's chosen command ids, when set)
//   dock space (tabbed leaves; splitters drag)
//   status-job rows (the host-registered running jobs, with cancel)
//   status bar (the shell's status text)
// and the overlays: the close-with-dirty 3-button modal, the autosave
// recovery prompt, the named-layout prompt, the command palette and
// the toast stratum.

#include <chrono>
#include <deque>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ws/app/command.hpp>
#include <ws/app/shortcut.hpp>
#include <ws/platform/dialogs.hpp>
#include <ws/platform/messagebox.hpp>
#include <ws/platform/recent.hpp>
#include <ws/settings/filesystem.hpp>
#include <ws/settings/schema.hpp>
#include <ws/settings/settings.hpp>
#include <ws/ui/gui/command_views.hpp>
#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/property_grid.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/ui/shell/document.hpp>
#include <ws/ui/shell/panels.hpp>

namespace ws::jobs {
class JobSystem;
}

namespace ws::ui::shell {

// What the shell is handed at construction. Nothing here is optional
// except the services: `fs` must outlive the shell (layout, session,
// recents, autosave and the user settings layer all ride it).
struct ShellConfig {
    std::string app_name = "app";       // the window title's tail
    std::string config_dir;             // where the json state lives
    ws::settings::FileSystem* fs = nullptr;
    ws::platform::FileDialogs* dialogs = nullptr;    // open/save pickers
    ws::platform::MessageBoxes* boxes = nullptr;     // OS asks (unused in v1
                                                     // - the gui modals own
                                                     // the prompts; kept for
                                                     // the plan's contract)
    ws::jobs::JobSystem* jobs = nullptr;             // cancel_all at shutdown
    // The autosave debounce, milliseconds (0 = snapshot on every draw;
    // the default is the plan's "a few seconds" cadence).
    std::int64_t autosave_interval_ms = 10000;
    // The ms clock seam (Autosave's own): injected for tests, wall
    // clock by default.
    std::function<std::int64_t()> clock_ms;
};

class AppShell {
public:
    // `commands`/`shortcuts`/`settings` are host-owned and must outlive
    // the shell; `settings` carries the host's schema (the settings
    // panel's property grid and the shell's theme key read it).
    AppShell(ShellConfig config, ws::app::CommandRegistry& commands,
             ws::app::ShortcutResolver& shortcuts,
             ws::settings::SettingsStore& settings);
    ~AppShell();

    AppShell(const AppShell&) = delete;
    AppShell& operator=(const AppShell&) = delete;

    // --- setup (before startup(); registration order is the menu order) ---

    [[nodiscard]] PanelRegistry& panels() noexcept { return panel_registry_; }
    // The document factory ("" path = a fresh untitled document).
    void set_document_factory(DocumentFactory factory);
    // The toolbar's commands, in order (empty = no toolbar row).
    void set_toolbar_commands(std::vector<std::string> ids);
    // The host's menu order (categories become menus). The shell
    // appends its own "Window" menu after these.
    void set_menu_specs(std::vector<gui::CommandMenuSpec> specs);

    // --- lifecycle ------------------------------------------------------------

    // Loads the user settings layer, the layout, the recents and the
    // session, and queues the autosave recovery prompts. Tolerant by
    // contract: a missing file is a first run, a corrupt one is noted
    // in the status line and answered with defaults - startup never
    // fails over state it can rebuild (the test pins this).
    [[nodiscard]] ws::core::Status startup();

    // The ordered shutdown (the test asserts the order): jobs
    // cancelled first, then the panels closed (every on_close), the
    // autosaves finished/snapshotted, the session, layout and recents
    // saved, and the user settings layer flushed. Idempotent.
    [[nodiscard]] ws::core::Status shutdown();
    // The phases as they landed ("cancel_jobs", "close_panels", ...),
    // the shutdown-ordering test's seam.
    [[nodiscard]] const std::vector<std::string>& shutdown_log() const {
        return shutdown_log_;
    }

    // --- the frame --------------------------------------------------------------

    // One frame: the whole chrome + the overlays. Call between
    // GuiContext::begin_frame/end_frame inside an r2d begin()/end()
    // block, like the gallery does. `frame` is the whole window.
    void draw(gui::GuiContext& gui, const gui::ChromeTheme& theme,
              const gui::Rect& frame);

    // The OS dropped files on the window (the host forwards its
    // FileDroppedEvent): each path opens as a document.
    void handle_dropped_files(const std::vector<std::string>& paths);

    // --- the services the host reads -----------------------------------------

    // The composed window title (ws::platform::window_title's shape:
    // "*document - app" when dirty). The host hands it to App::set_title.
    // (non-const like every reader: is_dirty() rides the undo stack,
    //  a non-const surface)
    [[nodiscard]] std::string window_title();

    // The key context the host's chord loop resolves under: the panel
    // rung while the active panel declared scoped_shortcuts, else
    // global. The resolver itself is never touched (ws::app's rule).
    [[nodiscard]] ws::app::KeyContext key_context() const;

    // The theme the settings say (the "shell.theme" key, when the
    // host's schema declares it: "dark" default, "light" the alt).
    // Read live per call - a settings change reacts next frame.
    [[nodiscard]] const gui::ChromeTheme& theme() const;

    [[nodiscard]] DocumentManager& documents() noexcept { return *documents_; }
    [[nodiscard]] PanelHost& panel_host() noexcept { return panel_host_; }
    [[nodiscard]] gui::Toasts& toasts() noexcept { return toasts_; }
    void set_status(std::string text);

    // --- status jobs: the status-bar progress rows --------------------------

    // One running job the status bar lists with its bar and a cancel
    // button. Plain getters the shell reads per frame - the host
    // rebuilds its list each frame (the provider shape), so a finished
    // job disappears by leaving the list.
    struct StatusJob {
        std::string label;
        std::function<float()> fraction;       // [0,1]
        std::function<std::string()> message;  // the state word
        std::function<void()> cancel;          // the button's act
    };
    void set_status_job_provider(
        std::function<std::vector<StatusJob>()> provider) {
        status_jobs_ = std::move(provider);
    }

    // --- prompts the host can drive directly (tests too) -------------------

    // Begins the close flow for the active document (the Ctrl+W
    // command's act): a clean document closes now, a dirty one raises
    // the 3-button modal.
    void request_close_active();
    // Saves the active document (Ctrl+S): through save() at its path,
    // or the save-as prompt when it is untitled.
    [[nodiscard]] ws::core::Status save_active();
    // Opens paths as documents (the open command's and the drop's act).
    [[nodiscard]] ws::core::Status open_paths(const std::vector<std::string>& paths);

    // The palette's open flag (the Ctrl+Shift+P command flips it).
    bool palette_open = false;

private:
    // --- the frame's pieces (shell.cpp) -------------------------------------
    void draw_menu_bar(gui::GuiContext& gui, gui::LayoutCursor& layout,
                       const gui::ChromeTheme& theme, const gui::Rect& bar);
    void draw_dock(gui::GuiContext& gui, gui::LayoutCursor& layout,
                   const gui::ChromeTheme& theme, const gui::Rect& bounds);
    void draw_status(gui::GuiContext& gui, gui::LayoutCursor& layout,
                     const gui::ChromeTheme& theme, const gui::Rect& frame,
                     float bottom_y);
    void draw_overlays(gui::GuiContext& gui, const gui::ChromeTheme& theme,
                       const gui::Rect& frame);
    void draw_close_prompt(gui::GuiContext& gui, const gui::ChromeTheme& theme,
                          const gui::Rect& frame);
    void draw_recovery_prompt(gui::GuiContext& gui,
                              const gui::ChromeTheme& theme,
                              const gui::Rect& frame);
    void draw_layout_prompt(gui::GuiContext& gui, const gui::ChromeTheme& theme,
                            const gui::Rect& frame);

    // --- state save/load (startup/shutdown) ---------------------------------
    [[nodiscard]] ws::core::Status load_settings_layer();
    [[nodiscard]] ws::core::Status save_settings_layer();
    [[nodiscard]] ws::core::Status load_layout();
    [[nodiscard]] ws::core::Status save_layout();
    [[nodiscard]] ws::core::Status save_session();
    [[nodiscard]] ws::core::Status load_recents();
    [[nodiscard]] ws::core::Status save_recents();
    void rebuild_default_layout();

    // The named-layout index (config_dir/layouts.json - the FileSystem
    // seam has no listdir, so the names ride a json index).
    [[nodiscard]] std::vector<std::string> layout_names();
    [[nodiscard]] ws::core::Status save_named_layout(const std::string& name);
    [[nodiscard]] ws::core::Status load_named_layout(const std::string& name);

    // The shell's own commands (registered at startup).
    void register_commands();
    void autosave_tick();
    // The Window menu's action strings, index-matched to window_items_
    // ("panel:type", "layout:reset", "layout:save", "layout:load:name",
    // "recent:path", "cmd:id").
    void run_window_action(const std::string& action);

    // Paths under config_dir.
    [[nodiscard]] std::string path_of(std::string_view name) const;

    ShellConfig config_;
    ws::app::CommandRegistry& commands_;
    ws::app::ShortcutResolver& shortcuts_;
    ws::settings::SettingsStore& settings_;

    PanelRegistry panel_registry_;
    PanelHost panel_host_;
    std::unique_ptr<DocumentManager> documents_;
    std::unique_ptr<ws::platform::RecentFiles> recents_;

    gui::Toasts toasts_;
    gui::CommandPalette palette_;
    gui::CommandMenuBuilder menu_builder_;
    std::vector<gui::CommandMenuSpec> menu_specs_;
    std::vector<std::string> toolbar_ids_;
    std::function<std::vector<StatusJob>()> status_jobs_;

    std::string status_;
    int menu_open_ = -1;  // the menu bar's open index (-1 none)

    // The close-with-dirty prompt: the document index awaiting the
    // answer (-1 = no prompt).
    int close_pending_ = -1;
    // The recovery prompt queue (document paths with crashed autosaves).
    std::vector<DocumentManager::RecoveryOffer> recoveries_;
    std::size_t recovery_at_ = 0;
    // The named-layout prompt.
    bool layout_prompt_open_ = false;
    char layout_name_[64] = {};

    // The autosave clock.
    std::int64_t last_autosave_ms_ = 0;
    bool started_ = false;
    std::vector<std::string> shutdown_log_;
    std::string toast_text_;  // the long-lived buffer toasts borrow

    // The host's document factory (set_document_factory; forwarded
    // into the manager's own factory lambda).
    DocumentFactory document_factory_;

    // The splitter drag: which band is held, kept whole so the drag
    // survives the per-frame re-resolve.
    struct SplitterDrag {
        bool active = false;
        DockTree::SplitterRect splitter;
    };
    SplitterDrag splitter_drag_;

    // The close prompt's Escape claim key (the modal rule: a stable
    // address per overlay).
    bool close_esc_ = false;
    // The recovery prompt's open flag (modal_dialog's bool& rule).
    bool recovery_open_ = false;

    // The settings panel's grid state (the built-in panel).
    gui::PropertyGridState grid_state_;

    // The status-job rows' persistent drag-free state: none needed -
    // progress() is chrome. The cancel buttons id under the shell's
    // own scope, rebuilt per frame.

    // The menu bar's Window-menu scratch (rebuilt per frame; labels
    // in a deque so the c_str()s the MenuItems borrow never move -
    // the CommandMenuBuilder's own hints_ precedent).
    std::vector<gui::MenuItem> window_items_;
    std::vector<std::string> window_actions_;
    std::deque<std::string> window_labels_;
    // The recent-files submenu (labels borrowed from the frame's own
    // menu_items() vector, alive for the call).
    std::vector<gui::MenuItem> recent_items_;
    std::string window_hint_;  // the palette row's chord text
};

}  // namespace ws::ui::shell
