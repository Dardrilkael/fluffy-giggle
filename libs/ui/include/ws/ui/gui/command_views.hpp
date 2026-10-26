#pragma once

// The ws::app bridge (nextplan stage 13, phase 4): the gui-side views
// over the command registry, the shortcut resolver and - composed by
// the host, never named here - the undo stack. ws::app is pure std +
// ws::core + ws::settings (the jobs/settings bridge class of purity:
// headless, owner-thread), and this header translates its services
// into the gui's caller-owned-state contract, the job_progress.hpp
// pattern: the views hold no state the registry does not already own,
// and every widget takes the frame's data by reference and re-reads
// it per frame.
//
//   command_hint()      the chord text every view shows - the live
//                       binding when a resolver is given, else the
//                       declared default; "" when neither (a removed
//                       binding must not hint the chord it no longer
//                       answers)
//   CommandMenuBuilder the registry as MenuItem arrays: one top-level
//                       menu per category, rebuilt per frame
//   command_toolbar()  a button row for a caller-chosen id list
//   fuzzy_score() /    the palette's pure halves: the subsequence
//   palette_rank()     scorer and the ranking (score, then recency,
//                       then registration order)
//   command_palette()  the Ctrl+Shift-P overlay: fuzzy search over the
//                       labels, recent commands first, argument prompts
//   keymap_editor()    the bindings panel: context picker, search, the
//                       record capture, the resolver's own conflict
//                       warnings
//
// The views borrow the registry's strings for the frame (labels, ids)
// and the builder's hint texts live until its next build - both are
// app-lifetime objects read on the frame, never copied into the gui.
// A null resolver is legal everywhere: the hints fall back to the
// commands' declared chords (command_hint's rule).

#include <ws/app/command.hpp>
#include <ws/app/shortcut.hpp>
#include <ws/ui/gui/widgets.hpp>

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace ws::ui::gui {

// --- the shared hint text ----------------------------------------------------
// The chord text every view shows for one command: the LIVE binding in
// `context` when a resolver is given - "" when the command carries
// none there, because a binding the user removed must not fall back
// to hinting the declared chord it no longer answers - else, with no
// resolver, the command's declared default ("" when it declares
// none). The text is ws::app::describe_chord's spelling, so a rebind
// renames every hint at once.
[[nodiscard]] std::string command_hint(const ws::app::Command& command,
                                       const ws::app::ShortcutResolver* resolver,
                                       ws::app::KeyContext context);

// --- the registry as menus --------------------------------------------------
// One top-level menu: the title the bar draws (an '&' mnemonic
// allowed, the MenuItem rule) and the registry category its commands
// come from. The bar's order follows the spec order here.
struct CommandMenuSpec {
    const char* title = nullptr;
    std::string category;
};

// The registry as a menu bar's data: describe() once (which categories
// become menus, in which order), then build() every frame before
// menu_bar() - the same per-frame re-description discipline as a
// caller-owned MenuItem array, with the registry walk doing the
// describing. Commands land in registration order (a menu's section
// order); a command carrying a checked predicate lays as a check row
// whose box reads the predicate live per frame, one without lays as a
// plain action; disabled follows the enabled predicate the same way. A
// category with no commands drops its menu for the frame.
//
// v1 limits, deliberate: no submenus and no separators - the
// categories are the one level of grouping, registration order the
// walk - and no icons, because the gui layer has no icon seam yet
// (Command::icon is the id the future seam will map; it stays
// unread). The hint column is command_hint()'s text.
class CommandMenuBuilder {
public:
    // (Re)describes the bar: which categories become menus, in order.
    void describe(std::vector<CommandMenuSpec> specs);

    // Rebuilds every menu's items over the registry - once per frame,
    // before menu_bar. The labels and ids borrow the registry's
    // strings; the hint texts live inside this builder until the next
    // build (their storage never moves under the MenuDesc array - the
    // c_str()s stay put while the frame runs).
    void build(const ws::app::CommandRegistry& registry,
                const ws::app::ShortcutResolver* resolver,
                ws::app::KeyContext context);

    // The MenuDesc array menu_bar takes, with its count - empty (a
    // null array, count 0) lays a bare strip, the menu_bar's own
    // drop-safe rule.
    [[nodiscard]] const MenuDesc* menus() const noexcept {
        return menus_.empty() ? nullptr : menus_.data();
    }
    [[nodiscard]] int count() const noexcept {
        return static_cast<int>(menus_.size());
    }

    // Decodes a MenuEvent: the command id whose item fired - "" when
    // the item is not one of this frame's (a foreign menu's event, or
    // a null pointer). Read it the frame the event arrives; the
    // answer borrows this builder's id copy, which the next build
    // replaces.
    [[nodiscard]] std::string_view command_of(const MenuItem* item) const;

private:
    std::vector<CommandMenuSpec> specs_;
    std::vector<std::vector<MenuItem>> items_;  // one array per spec
    std::vector<std::vector<std::string>> ids_;  // the ids, kept apart so command_of is self-contained
    std::vector<MenuDesc> menus_;
    // The hint texts: a deque because a growing vector would MOVE the
    // strings (c_str()s are borrowed by the items above), and a deque
    // only ever allocates new blocks - existing elements never move.
    std::deque<std::string> hints_;
};

// A button row for the caller's chosen commands, in `ids` order: one
// button per id, the registry's label on it, laid fitted to the label
// and flowing on one line; an unknown id lays nothing (the builder's
// skip rule), and a command whose enabled predicate reads false lays
// dead through the button's own lock. Answers the fired command's id
// the frame a click or the focused button's Enter/Space lands - "" the
// rest of the time. The row pushes its own id scope (and an index per
// button, so equal labels stay apart), so two toolbars under two
// labels never fight. v1 has no tooltips: the chord a button's label
// does not carry is the menu hint's job.
// `label` ids the row; `ids` is the command-id list, the registry's
// own strings or copies - read only.
[[nodiscard]] std::string_view command_toolbar(
    GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
    const char* label, const ws::app::CommandRegistry& registry,
    const std::vector<std::string>& ids);

// --- the command palette ----------------------------------------------------
// The palette's pure scoring half: a case-insensitive subsequence
// match of `query` against `text` with a quality score - +1 per
// matched character, +2 more per match at a word start (the text's
// first character or one after a non-alphanumeric), +1 more per match
// continuing the previous match's position (a run), minus the text's
// length / 4 (a mild length penalty so tighter labels win), floored
// at 1 so every match outranks none. 0 answers "no match" - and so
// does the EMPTY query's: it matches everything and ranks by recency
// alone (palette_rank's case).
[[nodiscard]] int fuzzy_score(std::string_view query,
                              std::string_view text);

// One ranked command: the index into registry.commands() and the
// score fuzzy_score gave its label.
struct PaletteEntry {
    std::size_t index = 0;
    int score = 0;
};

// The ranked match list, best first: score descending, then recency
// (more recent first, `recents` being most-recent-first ids), then
// registration order (the stable walk) - so an empty query lists the
// recents and then the rest in registry order. Returns every match;
// the palette caps what it SHOWS (CommandPalette::kRows), the caller
// caps what it reads.
[[nodiscard]] std::vector<PaletteEntry> palette_rank(
    std::string_view query, const ws::app::CommandRegistry& registry,
    const std::vector<std::string>& recents);

// The palette's caller-owned state. The input halves reset on every
// fresh open (reset_input()); the recents survive - they are the
// palette's memory. The widget-only members are public too, so a
// scripted-states test can drive and assert them (the TextInputState
// discipline).
class CommandPalette {
public:
    static constexpr int kSearchCap = 64;    // the search buffer, NUL counted
    static constexpr int kArgumentCap = 64;  // the argument prompt's, same rule
    static constexpr int kRecentCap = 10;    // the recents list's length
    static constexpr int kRows = 8;         // rows the list shows (v1's cap)

    // The input halves the widget owns.
    char search[kSearchCap] = {};
    char argument[kArgumentCap] = {};
    int selection = 0;       // the highlighted row into this frame's ranked
    bool prompting = false;  // the argument phase, under the search row

    // Recency, most recent first, capped at kRecentCap. The widget
    // notes its own dispatches; the caller may note one dispatched
    // from elsewhere (a menu or toolbar fire) with the same call.
    std::vector<std::string> recents;
    void note_used(std::string_view id);
    void reset_input();

    // This frame's ranked display list - the registry indices of the
    // shown rows, capped at kRows. Rebuilt every open frame; readable
    // after (the scripted test's assertion seam).
    std::vector<int> ranked;

    // The widget's bookkeeping - stable-address state, never rebuilt.
    bool was_open = false;  // the fresh-open reset + the opening-chord Enter guard
    bool esc_guard = true;  // the Escape claim's identity (the address IS the claim)
    bool measured = false;  // the centered-geometry one-frame settle
    float extent = 0.0f;    // last frame's measured content height
    std::string chosen;     // the argument phase's target id

    // The table row scratch, rebuilt per frame; the chord texts live
    // here (a deque: existing elements never move, the c_str()s stay
    // borrowed by the cells).
    std::deque<std::string> hints;
    std::vector<const char*> cells;  // row-major: label, shortcut
    std::string footer;              // the highlighted row's label + chord line
};

// What one open frame of the palette reports. `dispatch` names a
// chosen command: `command` (the registry's own id bytes) and
// `argument` (the state's prompt buffer, "" when none was asked) are
// valid the frame the event arrives - dispatch through the registry
// right then. `close` asks the caller to drop their open flag
// (Escape, or a completed dispatch); the palette never clears the
// flag itself, the modal rule.
struct PaletteEvent {
    bool dispatch = false;
    bool close = false;
    std::string_view command;
    std::string_view argument;
};

// The command palette: a centered modal over the registry - a search
// field, the ranked rows (fuzzy_score over the labels; the live chord
// hint in the Shortcut column; kRows shown, v1's honest cap), the
// highlighted row's label + chord in a footer, and the argument
// prompt a needs_argument command swaps in under the search row.
//
// Keys: Up/Down move the highlight while the search field holds
// focus (the table's own arrows act only on a focused row - Tab into
// the list and they are the table's, the citizen path); Enter fires
// the highlighted row from the search field, or dispatches the prompt
// from the argument field; Escape closes through the overlay
// arbitration (one press, the claim keyed on the state's address).
// The frame that OPENED the palette answers no keys - the chord that
// opened it is still in that frame's edges (the modal_dialog rule).
//
// `open` by value, the modal rule - the caller clears their flag on
// event.close. The state must outlive the frames (the claim keys its
// address); `width` is the modal's width (clamped to the frame).
[[nodiscard]] PaletteEvent command_palette(
    GuiContext& gui, const ChromeTheme& theme, const char* label,
    bool open, const ws::app::CommandRegistry& registry,
    const ws::app::ShortcutResolver* resolver,
    ws::app::KeyContext context, CommandPalette& state,
    float width = 360.0f);

// --- the keymap editor ------------------------------------------------------
// The keymap editor's caller-owned state: the picker and search
// halves, the selected row, the record mode, and the per-frame table
// scratch. The record capture resolves its TARGET at the Record press
// (state.recording_id), so a registry change mid-recording can never
// retarget it.
struct KeymapEditor {
    static constexpr int kSearchCap = 48;  // the search buffer, NUL counted

    char search[kSearchCap] = {};
    int context_index = 0;    // the resolver ladder's rung (0 global,
                              // 1 panel, 2 text_editing - the combo order)
    int selected = -1;        // the highlighted row (-1 none)
    bool combo_open = false;  // the context picker's dropdown
    bool recording = false;   // the record mode: the next key edge (with
                              // its held modifiers) is the chord
    std::string recording_id;  // the recording target, captured at the press
    std::string status;        // the last outcome, laid every frame
    std::string detail;        // the selected row's summary line

    // The per-frame table scratch: the chord texts (a deque, existing
    // elements never move), the cells the table reads (row-major:
    // label, chord), and the shown-row to registry-index map.
    std::deque<std::string> chords;
    std::vector<const char*> cells;
    std::vector<int> row_commands;
};

// The bindings panel: the context picker (the resolver ladder's
// rungs), a search field, and a Command/Shortcut table over the WHOLE
// registry in registration order - every command, so a chordless one
// can be given a chord, the rows narrowed only by the search (an
// empty search shows all; a non-empty one filters by fuzzy_score).
// The selected row's summary and a Record button flow under the
// table, then the status line - the last outcome, or the resolver's
// own conflict warning verbatim (it names the chord and the command
// that owns it).
//
// Record mode: the next non-modifier key edge plus the held
// modifiers (GLFW numbering, ws::input's rule; super rides the same
// GLFW codes the platform layer would) becomes the chord, applied
// through rebind() when the target carries a binding in the context
// and through bind() when it carries none (rebind refuses the
// unbound - the two paths are the resolver's own contract). Every
// refusal lands its message in state.status and leaves the binding
// standing. Escape cancels a recording: the editor reads the raw
// edge, acts on it and consumes the arbitration (consume_escape -
// the same press must not also close an overlay above), because the
// claim table is modal-gated and a panel-level claim records
// nothing. While recording, the picker, the search and the table
// are locked, so the captured key types nowhere. The rows are the
// table's citizens: click, Up/Down and Enter pick.
//
// Lays rows in the CALLER'S flow - give it a panel, and a scroll
// panel when the registry is long. `label` ids the panel's widgets.
void keymap_editor(GuiContext& gui, LayoutCursor& layout,
                   const ChromeTheme& theme, const char* label,
                   const ws::app::CommandRegistry& registry,
                   ws::app::ShortcutResolver& resolver,
                   KeymapEditor& state);

}  // namespace ws::ui::gui
