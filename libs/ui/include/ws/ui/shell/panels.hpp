#pragma once

// The panel framework (PLAN_UI_STAGE14_15 stage 15): the registry of
// panel TYPES plus the host that tracks open panel INSTANCES over the
// dock tree. The minimal in-repo version of Part A's panel layer - the
// seam is clear so a future full Part A can replace it.
//
// Split of labor: the registry is the static half (what panels exist),
// the host is the live half (which are open, which is focused) and it
// OWNS the DockTree, because opening and docking are one act. The
// AppShell composes both; a host app registers panel descriptors and
// never touches the tree by hand.
//
// Panel-scoped shortcuts (the plan's KeyContext::panel rung): a
// descriptor with `scoped_shortcuts` set raises the shell's key context
// to the panel rung while an instance of it is the active panel - the
// resolver itself stays untouched (ws::app's contract), the shell just
// reports the rung and gates panel-scoped commands' enabled predicates
// on the active panel's type.

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/shell/dock.hpp>

namespace ws::ui::shell {

// The gui types the callbacks speak, imported under the shell's own
// names (the same readability import dock.hpp's Rect rides).
using GuiContext = ws::ui::gui::GuiContext;
using LayoutCursor = ws::ui::gui::LayoutCursor;
using ChromeTheme = ws::ui::gui::ChromeTheme;

// One OPEN panel: the instance's stable id (unique across the
// process), the descriptor's type id and the title the tabs draw.
// Multi-instance panels get "type:2", "type:3", ...; single-instance
// ones are just their type id.
struct PanelInstance {
    std::string id;     // "editor:1" - the dock tree's tab key
    std::string type;   // "editor" - the registry key
    std::string title;  // "Editor" - the tab strip's label
};

// One registrable panel type. The draw callback receives the frame's
// gui/layout/theme (the caller-owned-state contract every widget
// rides) plus the instance it is drawing for - a multi-instance panel
// branches on instance.id when its state must differ per instance.
struct PanelDesc {
    std::string id;     // the registry key; must be non-empty and unique
    std::string title;  // the tab/menu label
    int icon = 0;       // the ui layer's future icon seam; unread in v1
    DockPlacement placement{};  // where a fresh instance docks
    bool multi_instance = false;  // "editor:2" allowed?
    bool scoped_shortcuts = false;  // KeyContext::panel while active
    // Optional per-panel settings scope: the prefix ("editor.")
    // property-grid rows for this panel's settings live under. Empty =
    // the panel reads whole-app settings only.
    std::string settings_scope;
    std::function<void(GuiContext&, LayoutCursor&, const ChromeTheme&,
                       const PanelInstance&)>
        draw;
    std::function<void(const PanelInstance&)> on_focus;  // became active
    std::function<void(const PanelInstance&)> on_close;  // about to close
};

// The static half: what panel types exist. Owner-thread, setup-time
// registration; the queries run per frame.
class PanelRegistry {
public:
    PanelRegistry() = default;

    // Adds a descriptor. Refuses an empty id, an empty title, a missing
    // draw callback and a duplicate id - all invalid_argument, each a
    // registration bug rather than a runtime failure.
    [[nodiscard]] ws::core::Status add(PanelDesc desc);

    [[nodiscard]] const PanelDesc* find(std::string_view type) const;
    [[nodiscard]] bool contains(std::string_view type) const;
    [[nodiscard]] std::size_t size() const noexcept { return descs_.size(); }

    // Registration order - the Window menu's stable walk.
    [[nodiscard]] const std::vector<PanelDesc>& descriptors() const noexcept {
        return descs_;
    }

private:
    std::vector<PanelDesc> descs_;
};

// The live half: which instances are open over the dock tree, and
// which one is active. Owner-thread state, no locks (the workspace
// rule); on_focus/on_close callbacks run on the caller's thread.
class PanelHost {
public:
    PanelHost() = default;

    // The tree this host docks into (the host owns it - opening and
    // docking are one act).
    [[nodiscard]] DockTree& dock() noexcept { return dock_; }
    [[nodiscard]] const DockTree& dock() const noexcept { return dock_; }

    // Opens one instance of `type`. Refuses an unknown type and a
    // single-instance type that is already open (invalid_argument).
    // Returns the instance id docked at the descriptor's placement.
    [[nodiscard]] ws::core::Result<std::string> open(const PanelRegistry& registry,
                                                     std::string_view type);

    // Closes one instance (a no-op returning false when unknown);
    // fires the descriptor's on_close, then undocks.
    bool close(const PanelRegistry& registry, std::string_view instance);

    // Closes every instance of a type (the Window menu's toggle).
    void close_type(const PanelRegistry& registry, std::string_view type);

    // True when at least one instance of `type` is open.
    [[nodiscard]] bool is_open(std::string_view type) const;

    // The open instances in dock order.
    [[nodiscard]] const std::vector<PanelInstance>& instances() const noexcept {
        return instances_;
    }
    [[nodiscard]] const PanelInstance* find(std::string_view instance) const;

    // Makes `instance` the active panel: its leaf's front tab and the
    // focus. Fires on_focus only when the ACTIVE PANEL CHANGED (a
    // no-op for an unknown id; activating the already-active panel is
    // the same no-op, no duplicate callback).
    void activate(const PanelRegistry& registry, std::string_view instance);

    // Moves activation to the next open instance in dock order,
    // wrapping (the panel-cycling command). A no-op with nothing open.
    void cycle(const PanelRegistry& registry);

    // The active instance, nullptr when nothing is open.
    [[nodiscard]] const PanelInstance* active() const noexcept {
        return active_instance();
    }

    // Closes everything (shutdown phase 2 fires every on_close, dock
    // order). The active panel clears.
    void close_all(const PanelRegistry& registry);

private:
    [[nodiscard]] PanelInstance* active_instance();
    [[nodiscard]] const PanelInstance* active_instance() const;

    DockTree dock_;
    std::vector<PanelInstance> instances_;
    std::string active_;
    std::size_t multi_counter_ = 0;  // "type:N" ids, ever-increasing
};

}  // namespace ws::ui::shell
