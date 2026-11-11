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
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

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

    // The schema walk, cached: the category names in first-declared
    // order and each category's descriptor pointers, read once per
    // schema binding instead of the per-frame vectors the Schema
    // introspection returns by value (categories() copies every
    // name, in_category() allocates a list per category - the one
    // allocating path an otherwise allocation-free walk had left).
    //
    // The cache is lifetime-sound by construction, on two halves:
    // the ROW pointers are re-bucketed from the live descriptors
    // EVERY frame - they always point into THIS frame's binding, never
    // into storage an earlier binding has freed - while the cached
    // half is the part that is values, not storage: the category
    // names. The validity of reusing that half is keyed, beyond the
    // binding's ADDRESS (a re-bind to another schema) and descriptor
    // COUNT (a growth - which may also move the descriptors' own
    // storage), on a STAMP: an FNV over every descriptor's identity
    // as the grid consumes it - key, type, category, and the
    // constraints' shapes - re-computed and compared every frame.
    // The address and the count alone cannot name the object: two
    // DIFFERENT schemas can share an address (a per-frame copy, a
    // temporary in the caller's stack slot) and a count, and a cache
    // keyed on the pair alone would serve the first one's rows
    // through pointers into storage that binding has already freed.
    // The one content a stamp still cannot see is an enum's VALUE
    // edit behind an identical key, type, category and entry count -
    // reachable only by alternating two hand-built twin schemas
    // through one stack slot, never through Schema's own append-only
    // declare(); the write path reads the live descriptor's own
    // allowed list, so nothing stale is ever written. The enum rows'
    // choice lists ride the same rebuild.
    const void* walk_from = nullptr;
    std::size_t walk_size = 0;
    std::uint64_t walk_stamp = 0;
    std::vector<std::string> walk_categories;
    std::vector<std::vector<const settings::SettingDesc*>> walk_rows;

    // Per-enum-setting choice lists (the spelled labels and the
    // c_str()s the combo reads off them), cleared with the walk on
    // every schema change and rebuilt on a row's first use - built
    // once per binding, not once per frame.
    struct EnumChoices {
        std::vector<std::string> labels;
        std::vector<const char*> ptrs;  // labels[i].c_str(), parallel
    };
    std::unordered_map<std::string, EnumChoices> enum_choices;

    // The state follows the schema it last rendered: each frame the
    // maps are pruned to the schema's live keys and categories, so a
    // state re-bound to another schema - or a schema re-declared at
    // runtime - starts clean. A key that left mid-edit returns with
    // no buffer, no editing flag, and no open dropdown: nothing it
    // left behind can commit on its arrival.
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
