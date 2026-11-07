#pragma once

// apps/circsim_editor - the side/bottom panels: the symbol Palette
// (placement arming + the wire/label tools, the fuzzy search), the
// Properties panel (a three-tab strip: the selection's fields over
// ParamSpecs, the Netlist view, and the Simulation setup - the
// analysis list and directives), the Problems list (ERC / run / load
// diags, click to select-and-zoom through locate.hpp) and the Netlist
// view's own bottom panel (the exported deck).

#include "editor.hpp"

namespace editor {

// The directives block's split/join pair (the Properties panel's
// serialization of the schematic's verbatim .model/.include text).
// SEPARATOR semantics, netlist-lines-style: `text` splits into the
// lines BETWEEN its newlines, so a trailing newline yields a trailing
// EMPTY line ("a\n" is {"a", ""}) and an empty text is one empty
// line; `join` glues the lines back with single newlines and no
// trailing one, so a final empty line keeps its newline ({"a", ""}
// is "a\n") - the round-trip that preserves every line the user
// cleared. (The old splice dropped the trailing empty line and the
// old join stripped the newline, so clearing the last row to empty
// silently deleted it on the next reload.)
[[nodiscard]] std::vector<std::string> split_directive_lines(
    const std::string& text);
[[nodiscard]] std::string join_directive_lines(
    const std::vector<std::string>& lines);

// The analysis list's "remove selected" act, on its own so it is
// headless-testable: removes the analysis `ed.analysis_index` names,
// nothing at all when nothing is selected (index < 0) or the index
// has gone stale beyond the list - the old clamp turned "no
// selection" (index -1) into SIZE_MAX and deleted the LAST entry.
// True when a removal actually landed.
bool remove_selected_analysis(Editor& ed);

// The Problems panel's severity filter: does a diag of `severity`
// pass the three toggles? The panel's counts never pass through this
// (they stay the full counts whatever is hidden) - only the row draw
// and the next/previous walk consult it.
[[nodiscard]] bool problems_filter_shows(ws::circsim::Severity severity,
                                         bool show_error, bool show_warning,
                                         bool show_note);

// The walk's enablement (the panel's Prev/Next pair and the F8
// commands' enabled predicates share it): a check has landed and the
// filter leaves at least one finding to land on.
[[nodiscard]] bool problems_any_visible(const Editor& ed);

// The next/previous walk's pure core: the index in [0, count) after
// `cursor` (forward) or before it (back) whose row passes `shows`,
// wrapping at the ends so the ends cycle; -1 when nothing passes or
// count is 0. `cursor` -1 (nothing under the cursor yet) starts at
// the first (forward) or last (back) passing index; a stale cursor
// past a shrunken list reads as none - the walk restarts at the far
// end, never a crash.
[[nodiscard]] int step_problem_index(
    int cursor, std::size_t count, bool forward,
    const std::function<bool(std::size_t)>& shows);

// The Problems panel's Prev/Next act (the buttons, and the F8 pair
// the app binds): walk ed.problems.diags through the severity filter,
// land the cursor on the finding and jump to it - locate_diag +
// jump_to, the row click's own path, so the keyboard and the click
// can never drift apart. True when a finding answered.
bool step_problem(Editor& ed, bool forward);

// The Netlist panel's Copy act's payload: the deck exactly as the
// panel shows it, one '\n' between the rows and none after the last
// (join_directive_lines' separator rule) - which is the exported
// text byte for byte, the round-trip refresh_netlist's own split
// rides, so the clipboard holds what the export wrote.
[[nodiscard]] std::string join_deck_lines(
    const std::vector<std::string>& lines);

void draw_palette_panel(Editor& ed, gui::GuiContext& gui,
                        gui::LayoutCursor& layout,
                        const gui::ChromeTheme& theme);
void draw_properties_panel(Editor& ed, gui::GuiContext& gui,
                           gui::LayoutCursor& layout,
                           const gui::ChromeTheme& theme);
// The Properties panel's Simulation tab, on its own seam: the
// schematic's analysis setup + directives editor (the tab body, but
// named so a headless test can drive it directly like the panels).
void draw_simulation_setup(Editor& ed, gui::GuiContext& gui,
                           gui::LayoutCursor& layout,
                           const gui::ChromeTheme& theme);
void draw_problems_panel(Editor& ed, gui::GuiContext& gui,
                         gui::LayoutCursor& layout,
                         const gui::ChromeTheme& theme);
void draw_netlist_panel(Editor& ed, gui::GuiContext& gui,
                        gui::LayoutCursor& layout,
                        const gui::ChromeTheme& theme);

}  // namespace editor
