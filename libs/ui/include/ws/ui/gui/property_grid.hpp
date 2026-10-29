#pragma once

// PropertyGrid: a schema-driven settings/property grid. The host
// declares a ws::settings::Schema (the keys, types, constraints,
// categories), and this widget renders one row per setting - a
// checkbox for booleans, a slider for ranged numerics, a combo for
// enums, a text input for free text - reading and writing through the
// SettingsStore. The grid is the Stage 15 minimal in-repo version of
// Part A's property grid; the seam is clear so a future full Part A
// can replace it.
//
// The grid groups settings by their `category` field (one collapsing
// header per category, in first-declared order). Each row shows the
// setting's key as a label and the appropriate widget. The grid is
// stateless itself - the caller owns a PropertyGridState member that
// holds the per-category open-state and the per-text-setting input
// buffers, so the headers and the editing persist across frames the
// way every other immediate-mode widget's state does.

#include <array>
#include <cstddef>
#include <string>
#include <unordered_map>

#include <ws/settings/schema.hpp>
#include <ws/settings/settings.hpp>
#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>

namespace ws::ui::gui {

// The persistent state the caller owns (one per grid instance). The
// grid reads/writes through it each frame.
struct PropertyGridState {
    // Per-category collapsing-header open-state, keyed by category name.
    std::unordered_map<std::string, bool> category_open;
    // Per-text-setting input buffer (256 bytes, enough for any
    // reasonable text setting). Keyed by the setting's key.
    std::unordered_map<std::string, std::array<char, 256>> text_bufs;
};

// Renders the property grid for `schema` bound to `settings`. Draws
// one collapsing header per category, then one widget row per setting.
// Returns the number of settings whose value changed this frame (for
// the host to react - apply a theme, rebake a font, etc.).
//
// The grid uses the existing widget set (checkbox, slider_float,
// slider_int, combo, text_input) and the standard layout cursor. The
// row width fills the current panel's content width.
std::size_t property_grid(GuiContext& gui, LayoutCursor& layout,
                          const ChromeTheme& theme,
                          const settings::Schema& schema,
                          settings::SettingsStore& settings,
                          PropertyGridState& state);

}  // namespace ws::ui::gui
