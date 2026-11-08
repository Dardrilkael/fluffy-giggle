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

// --- the source waveform editor's pure half ----------------------------------
// A source part's `wave` param is the verbatim SPICE tail its deck
// line carries after the DC value and the AC magnitude - "SIN(0 1 1k)",
// "PULSE(0 5 0 1u 1u 1m 2m)". The Properties panel edits it as a
// format picker plus one number field per parameter instead of that
// text; this pair is the translation, pure and headless so the
// round-trip is testable without a gui.
struct SourceWave {
    enum class Kind : std::uint8_t {
        None,   // empty text: the DC value is the whole story
        Sin,    // SIN(VO VA FREQ TD THETA PHASE) - 6 numbers
        Pulse,  // PULSE(V1 V2 TD TR TF PW PER) - 7 numbers
        Exp,    // EXP(V1 V2 TD1 TAU1 TD2 TAU2) - 6 numbers
        Pwl,    // PWL(t1 v1 t2 v2 ...) - free-form corners
        Raw,    // anything else: kept verbatim, edited as text
    };
    Kind kind = Kind::None;
    // The per-kind numbers in ngspice order (Sin reads [0..5], Pulse
    // [0..6], Exp [0..5]); a short hand-typed form pads with 0s.
    double p[7] = {0, 0, 0, 0, 0, 0, 0};
    // Pwl/Raw: the text itself, kept byte for byte (nothing the field
    // editor could faithfully round-trip).
    std::string raw;
};

// Parses a `wave` text. Tolerant by design: an empty or blank text is
// None; a recognized head ("SIN(", "PULSE(", "EXP(", "PWL(" - case
// insensitive) with parseable SPICE numbers inside (commas or spaces
// between) fills the slots it has and zeros the rest; anything else -
// a bad number, more numbers than the kind takes, no parenthesis at
// all - answers Raw so the panel falls back to the plain text row and
// the model never silently rewrites what it could not read.
[[nodiscard]] SourceWave parse_source_wave(const std::string& text);

// Composes the canonical text: every number the kind takes, %.12g,
// single spaces, uppercase head - "SIN(0 1 1000 0 0 0)". None
// composes the empty text (the DC-only form); Pwl/Raw answer `raw`
// unchanged. parse(compose(w)) == w for every kind, so an edit cycle
// never drifts.
[[nodiscard]] std::string compose_source_wave(const SourceWave& w);

// --- the source waveform editor's preview half -------------------------------
// The picture of the form the fields are shaping: the waveform sampled
// over its own natural window, pure and headless so the shape is
// testable without a gui (the panel only maps it to pixels). The
// values are ngspice's own closed forms - the deck's truth, not an
// approximation of it - with the one license that a zero TAU reads as
// the instant step its limit is (ngspice's exp(-(t-td)/0) evaluates
// there too, but a preview should not divide by zero to say it).
struct WavePreview {
    // False when there is no honest picture: a Raw text, a PWL whose
    // corners do not read as number pairs, a corner before t=0 (the
    // window starts at 0 and would clip mid-shape), or a DC level that
    // is not a number. The panel draws nothing rather than a guess.
    bool drawable = false;
    // Every sample finite and equal: one line, one label (a DC level,
    // a zero-amplitude sine, a held PWL). The drawer labels the value
    // on the line instead of padding min/max labels around it.
    bool flat = false;
    // The window's span; ts runs [0, t_end]. Chosen per kind from the
    // form's own numbers - three cycles of a sine, two periods of a
    // pulse (its single-shot tail when PER is 0), EXP's settling span
    // (5 tau past each delay), the corners' own extent for PWL, an
    // arbitrary 1 for a flat level.
    double t_end = 0.0;
    // Ascending times and the waveform's value at each. Curves (SIN,
    // EXP) land on a uniform grid plus both sides of their delay jumps;
    // piecewise-linear forms (PULSE, PWL) land on their own corners and
    // cycle breaks, each sampled just-before and at the break so a
    // zero-width edge draws as the vertical it is. Non-finite values
    // are allowed through (a negative tau explodes): the drawer breaks
    // the line there instead of poisoning the tessellator.
    std::vector<double> ts;
    std::vector<double> vs;
};

// Samples the waveform. `dc_value` is the source's DC value row, the
// level a None (DC-only) form draws as; non-finite leaves None
// undrawable (a blank or non-number value row has no level to show).
// Any non-finite parameter leaves the whole form undrawable - parse
// gates the fields, but an imported deck can still carry "1e999".
[[nodiscard]] WavePreview sample_source_wave(const SourceWave& w,
                                             double dc_value);

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
