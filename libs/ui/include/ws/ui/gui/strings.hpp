#pragma once

// The gui's stock words, one seam for every language but English
// (G1.4). Before this header, each word was a hardcoded literal inside
// the widget that drew it - a library shipped in any language but
// English had no override point short of forking the widgets. Now the
// same words are one enum away: a host builds a StringTable (its own
// lookup over its own storage), points ChromeTheme::strings at it, and
// every stock word the library draws follows it.
//
// The seam stays deliberately dumb in v1 - the shape the call sites
// already imply:
//   * NO pluralization and NO interpolation machinery here. A word that
//     needs a number or a name stays the caller's snprintf, exactly as
//     shell.cpp always spelled its unsaved-changes body; the Str values
//     a smarter table will need (plural forms, format patterns) can be
//     reserved and added without breaking this one.
//   * The table is a BORROWED pointer on the theme, the font pointers'
//     own contract: the theme points, it owns nothing, and the table
//     (and every string it answers) must outlive every theme carrying
//     it. Null (the default) is the stock English table.
//   * Stock spelling is byte-identical to the pre-seam literals, so a
//     null table changes nothing on screen - the seam is invisible
//     until a host opts in.
//
// Order is ABI: kStockStrings is indexed by the enum's declaration
// order, so new values append AFTER the last entry and never renumber
// (a table built against today's enum must keep answering tomorrow's).

#include <ws/ui/gui/theme.hpp>

#include <cstddef>
#include <cstdint>

namespace ws::ui::gui {

// Every stock word the library itself draws. The names are roles, not
// spellings - Str::ok is "whatever the positive dialog button says",
// and the stock table says "OK".
enum class Str : std::uint16_t {
    // Dialog stock buttons (widgets_dialogs.cpp's sets).
    ok = 0,
    cancel,
    yes,
    no,
    retry,
    save,       // also the shell's unsaved-changes row
    discard,
    // The tab strip's close tooltip.
    close_tab,
    // The plot's empty-state and view-reset hints.
    no_data,
    all_signals_hidden,
    double_click_resets,
    // The command palette's empty-search footer.
    no_matching_command,
    // The keymap editor's recording row and empty-binding cells.
    recording_hint,
    none_label,
    // The job row's state words (a null handle's word included).
    no_job,
    job_queued,
    job_running,
    job_done,
    job_failed,
    job_cancelled,
    // The keymap editor's vanished-command status.
    command_gone,
};

// The stock English spellings, one per Str value in declaration
// order - byte-identical to the literals each widget drew before the
// seam, which is the null-table contract: a theme without a table
// renders exactly the v1 screen.
inline constexpr const char* kStockStrings[] = {
    "OK",
    "Cancel",
    "Yes",
    "No",
    "Retry",
    "Save",
    "Discard",
    "Close tab",
    "no data",
    "all signals hidden",
    "double-click resets",
    "no matching command",
    "recording - press a key (Escape cancels)",
    "<none>",
    "no job",
    "queued",
    "running",
    "done",
    "failed",
    "cancelled",
    "the command is gone",
};

// The stock lookup: the value out of the table above, or "" for a Str
// no table knows (the enum grew) - never a crash, never a null.
[[nodiscard]] constexpr const char* stock_lookup(Str id) {
    const std::size_t i = static_cast<std::size_t>(id);
    return i < sizeof(kStockStrings) / sizeof(kStockStrings[0])
               ? kStockStrings[i]
               : "";
}

// A localization's whole surface: one function from a Str to that
// language's spelling. The default member is the stock table, so an
// empty StringTable{} is English - a host that only overrides SOME
// words can delegate the rest to stock_lookup and keep one table.
struct StringTable {
    const char* (*lookup)(Str) = stock_lookup;
};

// The one call site spelling: the theme's table if it carries one,
// else stock. A table may answer nullptr for an id it does not know -
// that id falls back to the stock spelling (a half-built localization
// renders its gaps in English, never blanks a button).
[[nodiscard]] inline const char* theme_str(const ChromeTheme& theme,
                                           Str id) {
    if (theme.strings != nullptr && theme.strings->lookup != nullptr) {
        const char* spelling = theme.strings->lookup(id);
        if (spelling != nullptr) {
            return spelling;
        }
    }
    return stock_lookup(id);
}

}  // namespace ws::ui::gui
