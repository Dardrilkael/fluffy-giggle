#pragma once

// PropertyGrid: a schema-driven settings/property grid. The host
// declares a ws::settings::Schema (the keys, types, constraints,
// categories), and this widget renders one row per setting - a
// checkbox for booleans, a typed numeric field (input_int/input_float)
// for numerics - ranges from the schema, unbounded without one - a
// combo for enums, a text input for free text - reading and writing
// through the SettingsStore. The seam is clear so a future full Part A
// can replace it.
//
// The grid groups settings by their `category` field (one collapsing
// header per category, in first-declared order). Each row shows the
// setting's key as a label and the appropriate widget. The grid is
// stateless itself - the caller owns a PropertyGridState member that
// holds the per-category open-state, the per-text-setting input
// buffers and the per-enum-setting dropdown flags, so the headers, the
// edits and the open lists persist across frames the way every other
// immediate-mode widget's state does.

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
    // Per-text-setting "the field holds the focus" - the editing flag
    // that gates the buffer sync: while it stands, the field owns the
    // spelling (no store writes, no re-syncs) and the edit ends at the
    // first unfocused frame with one commit. Keyed like the buffers.
    std::unordered_map<std::string, bool> text_editing;
    // Per-enum-setting "the row's dropdown hangs open" - the combo
    // contract's caller-owned flag, keyed by the setting's key like
    // the buffers. A frame-local spelling would lay the list for
    // exactly the frame of the click that opened it: the next frame
    // re-arms false, the list vanishes, and the pick - a press on an
    // item, one frame or ten later - falls through to whatever sat
    // beneath. The popup's own rules (a pick, the outside press,
    // Escape) close it through the same reference.
    std::unordered_map<std::string, bool> combo_open;
};

// Renders the property grid for `schema` bound to `settings`. Draws
// one collapsing header per category, then one widget row per setting.
// Returns the number of settings whose value changed this frame (for
// the host to react - apply a theme, rebake a font, etc.).
//
// The grid uses the existing widget set (checkbox, input_int,
// input_float, combo, text_input) and the standard layout cursor. The
// row width fills the current panel's content width. The text rows
// own their edit like the numeric ones: a keystroke edits the buffer
// only, and the store sees the text once, at the blur. The enum rows
// keep the combo's own state rule: the dropdown's open flag lives in
// the state, so the list the click opened still hangs when the pick
// lands, a frame or ten later.
std::size_t property_grid(GuiContext& gui, LayoutCursor& layout,
                          const ChromeTheme& theme,
                          const settings::Schema& schema,
                          settings::SettingsStore& settings,
                          PropertyGridState& state);

}  // namespace ws::ui::gui
