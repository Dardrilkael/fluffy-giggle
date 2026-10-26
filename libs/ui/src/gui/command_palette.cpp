// The ws::app bridge's palette (see command_views.hpp): the fuzzy
// scorer and the ranking - pure, so the scripted-states suite can
// pin the order - and the modal over them. One widget family, one
// file (src/gui/CLAUDE.md).

#include "widgets_detail.hpp"

#include <ws/ui/gui/command_views.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace ws::ui::gui {
namespace {

// ASCII-only lowercasing and the word-character test: the scorer
// never decodes UTF-8 (labels are host data; a non-ASCII byte just
// reads as itself, matching no query character).
char lower_ascii(unsigned char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
}

bool alnum_ascii(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}

// Fires the highlighted row - or swaps to the argument prompt when the
// command asks for one. The shared path of the search field's Enter
// and the table's pick (which has already written state.selection).
void fire_or_prompt(GuiContext& gui, WidgetId argument_id,
                    const ws::app::CommandRegistry& registry,
                    CommandPalette& state, int shown, PaletteEvent& event) {
    if (shown <= 0 || state.selection < 0 || state.selection >= shown) {
        return;
    }
    const ws::app::Command& cmd = registry.commands()[static_cast<std::size_t>(
        state.ranked[static_cast<std::size_t>(state.selection)])];
    if (cmd.needs_argument) {
        state.prompting = true;
        state.chosen = cmd.id;
        state.argument[0] = '\0';
        gui.set_focus(argument_id);
        return;
    }
    event.dispatch = true;
    event.command = cmd.id;
    event.argument = {};
    event.close = true;
    state.note_used(cmd.id);
}

}  // namespace

// --- the pure halves --------------------------------------------------------

int fuzzy_score(std::string_view query, std::string_view text) {
    if (query.empty()) {
        return 0;  // matches everything; the rank call sorts by recency
    }
    int score = 0;
    int prev = -1;   // the previous match's position (-1: none yet)
    std::size_t at = 0;  // where the scan resumes (matches come in order)
    for (const char qc : query) {
        std::size_t found = std::string_view::npos;
        for (std::size_t i = at; i < text.size(); ++i) {
            if (lower_ascii(static_cast<unsigned char>(text[i])) ==
                lower_ascii(static_cast<unsigned char>(qc))) {
                found = i;
                break;
            }
        }
        if (found == std::string_view::npos) {
            return 0;  // a missing character is no match, not a partial
        }
        score += 1;
        if (found == 0 ||
            !alnum_ascii(static_cast<unsigned char>(text[found - 1]))) {
            score += 2;  // a word start
        }
        if (prev >= 0 && static_cast<int>(found) == prev + 1) {
            score += 1;  // a run continues
        }
        prev = static_cast<int>(found);
        at = found + 1;
    }
    score -= static_cast<int>(text.size() / 4);  // the mild length penalty
    return score < 1 ? 1 : score;
}

std::vector<PaletteEntry> palette_rank(
    std::string_view query, const ws::app::CommandRegistry& registry,
    const std::vector<std::string>& recents) {
    const std::vector<ws::app::Command>& commands = registry.commands();
    std::vector<PaletteEntry> entries;
    entries.reserve(commands.size());
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const int score = fuzzy_score(query, commands[i].label);
        if (!query.empty() && score == 0) {
            continue;
        }
        entries.push_back(PaletteEntry{i, score});
    }
    const auto recency = [&recents](std::string_view id) {
        for (std::size_t r = 0; r < recents.size(); ++r) {
            if (recents[r] == id) {
                return r;
            }
        }
        return recents.size();  // never used: after every recent
    };
    // stable_sort keeps registration order under full ties; score and
    // recency decide before it ever matters.
    std::stable_sort(entries.begin(), entries.end(),
                     [&](const PaletteEntry& a, const PaletteEntry& b) {
                         if (a.score != b.score) {
                             return a.score > b.score;
                         }
                         return recency(commands[a.index].id) <
                                recency(commands[b.index].id);
                     });
    return entries;
}

// --- CommandPalette ---------------------------------------------------------

void CommandPalette::note_used(std::string_view id) {
    recents.erase(std::remove(recents.begin(), recents.end(), id),
                 recents.end());
    recents.insert(recents.begin(), std::string{id});
    if (static_cast<int>(recents.size()) > kRecentCap) {
        recents.resize(static_cast<std::size_t>(kRecentCap));
    }
}

void CommandPalette::reset_input() {
    search[0] = '\0';
    argument[0] = '\0';
    selection = 0;
    prompting = false;
    chosen.clear();
}

// --- the widget -------------------------------------------------------------

PaletteEvent command_palette(GuiContext& gui, const ChromeTheme& theme,
                             const char* label, bool open,
                             const ws::app::CommandRegistry& registry,
                             const ws::app::ShortcutResolver* resolver,
                             ws::app::KeyContext context,
                             CommandPalette& state, float width) {
    PaletteEvent event;
    if (!open) {
        // The closed frames re-arm the Enter guard (and the fresh-open
        // reset with it); the recents are the palette's memory and
        // survive every close.
        state.was_open = false;
        return event;
    }
    const bool freshly_opened = !state.was_open;
    if (freshly_opened) {
        state.reset_input();
    }
    state.was_open = true;

    // Last frame's arbitration: the claim this state owns won, so the
    // palette asks to close. The dialog idiom - settled here, at the
    // entry, before any chrome.
    if (gui.esc_won(&state.esc_guard)) {
        state.was_open = false;
        event.close = true;
        return event;
    }

    // The centered window: the dialog geometry, with the one-frame
    // settle until the measured extent takes over.
    const auto frame = r2d::frame_size();
    width = std::max(0.0f, std::min(width, frame.width));
    const float settle = 3.0f * theme.control_height + 2.0f * theme.spacing;
    const float content_h = state.measured ? state.extent : settle;
    const float height =
        std::max(0.0f, std::min(content_h + 2.0f * theme.padding,
                                 frame.height));
    const Rect bounds{(frame.width - width) * 0.5f,
                      (frame.height - height) * 0.5f, width, height};
    LayoutCursor layout;
    if (!begin_modal(gui, layout, theme, label, bounds, open)) {
        return event;
    }
    // The claim, recorded while the veil is up: the next Escape press
    // resolves to this state's address.
    gui.claim_esc(&state.esc_guard);

    // The search row; a fresh open focuses it so typing starts there.
    const WidgetId search_id = gui.ids().id("search");
    (void)text_input(gui, layout, theme, "search", state.search,
                     CommandPalette::kSearchCap, -1.0f, state.prompting);
    if (freshly_opened) {
        gui.set_focus(search_id);
    }

    // The ranked display list - AFTER the search field, so the
    // frame's own typing narrows the frame's own list (one frame, one
    // reaction; computing it before the field would show the
    // keystroke a frame late): the search text over the registry,
    // recents first on a tie, registration order under a full tie,
    // capped at the shown-rows limit.
    state.ranked.clear();
    for (const PaletteEntry& entry :
         palette_rank(std::string_view{state.search}, registry,
                      state.recents)) {
        state.ranked.push_back(static_cast<int>(entry.index));
        if (static_cast<int>(state.ranked.size()) >= CommandPalette::kRows) {
            break;
        }
    }
    const int shown = static_cast<int>(state.ranked.size());
    if (state.selection >= shown) {
        state.selection = shown - 1;  // a narrowed search clamps the highlight
    }
    if (state.selection < 0) {
        state.selection = 0;
    }

    // The argument row, only in the prompt phase, under the search.
    const WidgetId argument_id = gui.ids().id("argument");
    if (state.prompting) {
        (void)text_input(gui, layout, theme, "argument", state.argument,
                         CommandPalette::kArgumentCap);
    }

    // The keys. Enter: the prompt dispatches from the argument field,
    // the search fires the highlighted row - and a focused table row
    // keeps its own Enter (the citizen path below, through the pick).
    // Up/Down walk the highlight from the search field only (the
    // table's arrows act on focused rows, not on the field's).
    // The opening frame answers nothing: the chord that opened the
    // palette is still in its edges.
    const bool enter_edge = gui.input().key_pressed[static_cast<std::size_t>(
        ws::input::keys::kEnter)];
    if (!freshly_opened && enter_edge) {
        if (state.prompting && gui.is_focused(argument_id)) {
            gui.consume_key(ws::input::keys::kEnter);
            event.dispatch = true;
            event.command = state.chosen;
            event.argument = std::string_view{state.argument};
            event.close = true;
            state.note_used(state.chosen);
        } else if (!state.prompting && gui.is_focused(search_id)) {
            gui.consume_key(ws::input::keys::kEnter);
            fire_or_prompt(gui, argument_id, registry, state, shown, event);
        }
    }
    if (!freshly_opened && !state.prompting && gui.is_focused(search_id)) {
        if (gui.input().key_pressed[static_cast<std::size_t>(
                ws::input::keys::kUp)]) {
            gui.consume_key(ws::input::keys::kUp);
            if (state.selection > 0) {
                --state.selection;
            }
        }
        if (gui.input().key_pressed[static_cast<std::size_t>(
                ws::input::keys::kDown)]) {
            gui.consume_key(ws::input::keys::kDown);
            if (state.selection < shown - 1) {
                ++state.selection;
            }
        }
    }

    // The matches table: label + live chord per row, the highlight on
    // state.selection (a pick writes it back, then fires).
    state.hints.clear();
    state.cells.clear();
    state.cells.reserve(static_cast<std::size_t>(shown) * 2);
    for (int r = 0; r < shown; ++r) {
        const ws::app::Command& cmd = registry.commands()[static_cast<std::size_t>(
            state.ranked[static_cast<std::size_t>(r)])];
        state.cells.push_back(cmd.label.c_str());
        std::string hint = command_hint(cmd, resolver, context);
        if (hint.empty()) {
            state.cells.push_back("");  // no binding, no declared chord: blank
        } else {
            state.hints.emplace_back(std::move(hint));
            state.cells.push_back(state.hints.back().c_str());
        }
    }
    const TableColumn columns[] = {
        TableColumn{"Command", 0.0f},
        TableColumn{"Shortcut", 110.0f},
    };
    if (table(gui, layout, theme, "matches", columns, 2, state.cells.data(),
              shown, &state.selection)) {
        fire_or_prompt(gui, argument_id, registry, state, shown, event);
    }

    // The footer: what Enter would fire - the highlighted row's label
    // and chord, or the empty-search note. Rebuilt into the state's
    // own string (no per-frame allocation once it steadies).
    if (shown <= 0) {
        state.footer = "no matching command";
    } else {
        const ws::app::Command& cmd =
            registry.commands()[static_cast<std::size_t>(
                state.ranked[static_cast<std::size_t>(state.selection)])];
        state.footer = cmd.label;
        std::string hint = command_hint(cmd, resolver, context);
        if (!hint.empty()) {
            state.footer += "  ";
            state.footer += hint;
        }
    }
    text(layout, theme, state.footer.c_str());

    // The measured extent feeds the next frame's settle.
    state.extent = layout.flow_extent();
    state.measured = true;
    end_modal(gui, layout);
    return event;
}

}  // namespace ws::ui::gui
