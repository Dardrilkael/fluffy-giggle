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
#include <ws/settings/json.hpp>
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

// The floor every floating window keeps, pixels: a torn-off panel
// smaller than this is a title bar with no body, so float_panel() and
// move_floating() clamp to it (the drag and the resize both pass
// through the host, so the floor is a data invariant, not a view
// nicety). The CEILING is the caller's dock frame - the shell clamps
// windows fully inside the space it hands draw_dock, the same
// "on-screen" rule the splitters live under.
inline constexpr float kFloatingMinW = 160.0f;
inline constexpr float kFloatingMinH = 96.0f;

// One floating window: a panel instance torn out of the dock tree,
// with its rect in DOCK-SPACE pixels (the same space resolve() and the
// drop zones speak). The vector's order is the z stack - index 0 the
// bottom window, the last entry the top - so raise = move to the end,
// and the view draws in order for free.
struct FloatingPanel {
    std::string panel;  // the instance id ("editor:1")
    Rect rect{};        // the whole window, title bar included
};

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

    // Re-docks an open instance at `placement` (a no-op when unknown):
    // the tree's move, then activation - the focus follows the tab to
    // its new home, and on_focus fires exactly when the ACTIVE PANEL
    // CHANGED (a re-dock that lands the panel where it already was is
    // the activate no-op). Opening and docking are one act; so are
    // re-docking and focusing. A FLOATING instance docks through the
    // same seam: the window leaves the floating layer, the tree's
    // add() lands the tab, and the focus follows - the drop over a
    // leaf and a host's programmatic re-dock share one spelling.
    void redock(const PanelRegistry& registry, std::string_view instance,
                DockPlacement placement);

    // Reorders one instance's tab: to `index` of leaf `leaf`, a
    // reorder inside its own strip or a retab into another (the tree's
    // move_tab, both clamped). The dropped tab fronts its strip, so
    // the instance activates too. A no-op when unknown.
    void retab(const PanelRegistry& registry, std::string_view instance,
               int leaf, int index);

    // --- the floating layer (the tear-off) ----------------------------------
    //
    // A panel instance lives in exactly one of two homes: a tab in the
    // dock tree, or a floating window. float_panel() is the tear-off
    // (the tab drag released outside every leaf); redock() is the way
    // back (the drop over a leaf). The tree answers only its own half
    // - dock().contains() is false for a floating panel - and these
    // seams answer the other.

    // The floating windows, back to front (the last entry draws on
    // top; raise_floating is the only reorder).
    [[nodiscard]] const std::vector<FloatingPanel>& floating()
        const noexcept {
        return floating_;
    }

    // True when `instance` is a floating window (open and torn off).
    [[nodiscard]] bool is_floating(std::string_view instance) const;

    // Tears `instance` off into a floating window at `rect` (dock-space
    // pixels; clamped to the minimum size). A floating instance again
    // just moves (the rect taken, the window raised). A no-op for an
    // unknown id. The instance stays active - the focus follows the
    // panel out of the tree, the way a torn-off tab keeps its focus.
    void float_panel(const PanelRegistry& registry, std::string_view instance,
                     const Rect& rect);

    // Moves/reshapes one floating window to `rect` (the drag and the
    // resize both land here, every frame they move; clamped to the
    // minimum size). A no-op for an unknown id or a docked one.
    void move_floating(std::string_view instance, const Rect& rect);

    // Lifts one floating window to the top of the stack (a no-op for
    // an unknown id or a docked one).
    void raise_floating(std::string_view instance);

    // Drops every floating window out of the layer WITHOUT docking it
    // anywhere: the panels stay open, homeless, and the caller re-docks
    // them (rebuild_default_layout's reset step - the tree is cleared
    // the same moment, and the defaults re-add every open instance).
    void clear_floating();

    // The active instance, nullptr when nothing is open.
    [[nodiscard]] const PanelInstance* active() const noexcept {
        return active_instance();
    }

    // Closes everything (shutdown phase 2 fires every on_close, dock
    // order). The active panel clears.
    void close_all(const PanelRegistry& registry);

    // --- the layout json (the tree + the floating layer) ---------------------
    //
    // One json value for the whole shell layout: the dock tree's own
    // shape (dock.hpp's contract, byte-for-byte) with ONE extra key
    // on the root object:
    //   "floating": [{"id": "plot", "x": 40, "y": 80,
    //                 "w": 320, "h": 240}, ...]
    // The array is the z stack, back to front. An old layout file
    // without the key parses to no floating windows; a reader that
    // does not know the key (an older build) still reads the tree -
    // the shape degrades the way every tolerant reader degrades.

    // The whole layout as the json above.
    [[nodiscard]] ws::settings::json::Value layout_to_json() const;

    // Parses it back. The tree half rides dock().from_json (same
    // tolerance, same accept filter); the floating half drops an
    // entry whose id the accept refuses, the host does not hold, or
    // the tree already docked (one instance, one home - the tree
    // wins, the way the tree's own dedupe keeps the first sighting),
    // and clamps each rect to the minimum size. Answers false only
    // for a non-object root (the tree's own rule); on false nothing
    // moved.
    bool layout_from_json(
        const ws::settings::json::Value& value,
        const std::function<bool(std::string_view panel)>& accept);

private:
    [[nodiscard]] PanelInstance* active_instance();
    [[nodiscard]] const PanelInstance* active_instance() const;

    DockTree dock_;
    std::vector<FloatingPanel> floating_;  // back to front: z stack
    std::vector<PanelInstance> instances_;  // open, docked or floating
    std::string active_;
    std::size_t multi_counter_ = 0;  // "type:N" ids, ever-increasing
};

}  // namespace ws::ui::shell
