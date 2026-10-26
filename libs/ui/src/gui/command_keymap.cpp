// The ws::app bridge's keymap editor (see command_views.hpp): the
// bindings panel over the resolver - the context picker, the search,
// the Command/Shortcut table, the record capture and the conflict
// warnings. One widget family, one file (src/gui/CLAUDE.md).

#include "widgets_detail.hpp"

#include <ws/ui/gui/command_views.hpp>
#include <ws/ui/gui/scoped.hpp>

#include <cstddef>

namespace ws::ui::gui {
namespace {

// The resolver ladder's rungs as the context picker spells them - the
// combo order IS the KeyContext numbering (static_cast either way).
const char* const kContextNames[] = {"Global", "Panel", "Text editing"};

// GLFW's super pair, unnamed in ws::input::keys (the gui gives the
// platform's left/right symmetry no meaning of its own) - spelled as
// plain ints here for the record capture, the ws::input convention.
constexpr int kLeftSuper = 343;   // GLFW_KEY_LEFT_SUPER
constexpr int kRightSuper = 347;  // GLFW_KEY_RIGHT_SUPER

bool is_modifier_key(int key) {
    namespace keys = ws::input::keys;
    return key == keys::kLeftShift || key == keys::kRightShift ||
           key == keys::kLeftControl || key == keys::kRightControl ||
           key == keys::kLeftAlt || key == keys::kRightAlt ||
           key == kLeftSuper || key == kRightSuper;
}

}  // namespace

void keymap_editor(GuiContext& gui, LayoutCursor& layout,
                   const ChromeTheme& theme, const char* label,
                   const ws::app::CommandRegistry& registry,
                   ws::app::ShortcutResolver& resolver,
                   KeymapEditor& state) {
    const ScopedId scope(gui.ids(), label);
    namespace keys = ws::input::keys;

    // The record capture runs FIRST: it owns this frame's key edges,
    // and the key it takes is consumed so nothing below (all locked
    // while a recording is live) could double-read it anyway.
    if (state.recording) {
        if (gui.input().key_pressed[static_cast<std::size_t>(
                keys::kEscape)]) {
            // Escape is the cancel, never a chord. Acted on here, so
            // the arbitration dies with it: the same press must not
            // also close an overlay above (a panel cannot claim -
            // claim_esc is modal-gated).
            gui.consume_escape();
            state.recording = false;
            state.status = "recording cancelled";
        } else {
            const auto down = [&gui](int key) {
                return gui.input().key_down[static_cast<std::size_t>(key)];
            };
            ws::app::Mod mods = ws::app::Mod::none;
            if (down(keys::kLeftShift) || down(keys::kRightShift)) {
                mods = mods | ws::app::Mod::shift;
            }
            if (down(keys::kLeftControl) || down(keys::kRightControl)) {
                mods = mods | ws::app::Mod::control;
            }
            if (down(keys::kLeftAlt) || down(keys::kRightAlt)) {
                mods = mods | ws::app::Mod::alt;
            }
            if (down(kLeftSuper) || down(kRightSuper)) {
                mods = mods | ws::app::Mod::super_key;
            }
            // The first non-modifier press edge is the chord's key -
            // Escape included in the skip so the cancel above owns it.
            int key = 0;
            for (int k = 1; k < ws::input::InputFrame::kMaxKeys; ++k) {
                if (!gui.input().key_pressed[static_cast<std::size_t>(k)]) {
                    continue;
                }
                if (is_modifier_key(k) || k == keys::kEscape) {
                    continue;
                }
                key = k;
                break;
            }
            if (key > 0) {
                gui.consume_key(key);
                const ws::app::Shortcut chord{key, mods};
                // The target was captured at the Record press, so a
                // registry change mid-recording cannot retarget it -
                // but it can remove the command; that answers, never
                // crashes.
                const ws::app::Command* cmd =
                    registry.find(state.recording_id);
                if (cmd == nullptr) {
                    state.status = "the command is gone";
                } else {
                    const auto ctx = static_cast<ws::app::KeyContext>(
                        state.context_index);
                    // rebind refuses a command with no binding in the
                    // context (the resolver's own contract) - the
                    // chordless command needs the bind path.
                    const bool had =
                        resolver.binding_for(cmd->id, ctx) != nullptr;
                    const ws::core::Status bound =
                        had ? resolver.rebind(cmd->id, ctx, chord)
                            : resolver.bind(chord, cmd->id, ctx);
                    if (bound.has_value()) {
                        state.status = "bound '" + cmd->label + "' to " +
                                       ws::app::describe_chord(chord);
                    } else {
                        // The resolver's own message - it names the
                        // chord and the command that owns it.
                        state.status = std::move(bound.error().message);
                    }
                }
                state.recording = false;
            }
        }
    }

    // The context picker and the search field, locked while a
    // recording owns the keys.
    (void)combo(gui, layout, theme, "context", kContextNames, 3,
                state.context_index, state.combo_open, -1.0f,
                state.recording);
    (void)text_input(gui, layout, theme, "search", state.search,
                     KeymapEditor::kSearchCap, -1.0f, state.recording);

    // The rows: every command whose label matches the search - an
    // empty search matches all (fuzzy_score's empty-query rule), a
    // non-empty one filters - registration order, the chord the
    // context's live table carries, or "<none>".
    const auto ctx = static_cast<ws::app::KeyContext>(state.context_index);
    state.chords.clear();
    state.cells.clear();
    state.row_commands.clear();
    for (std::size_t i = 0; i < registry.commands().size(); ++i) {
        const ws::app::Command& cmd = registry.commands()[i];
        if (state.search[0] != '\0' &&
            fuzzy_score(std::string_view{state.search}, cmd.label) == 0) {
            continue;
        }
        state.row_commands.push_back(static_cast<int>(i));
        state.cells.push_back(cmd.label.c_str());
        const ws::app::Shortcut* bound = resolver.binding_for(cmd.id, ctx);
        if (bound == nullptr) {
            state.cells.push_back("<none>");
        } else {
            state.chords.emplace_back(ws::app::describe_chord(*bound));
            state.cells.push_back(state.chords.back().c_str());
        }
    }
    const int rows = static_cast<int>(state.row_commands.size());
    if (state.selected >= rows) {
        state.selected = rows - 1;  // a narrowed search clamps the pick
    }

    const TableColumn columns[] = {
        TableColumn{"Command", 0.0f},
        TableColumn{"Shortcut", 110.0f},
    };
    {
        // Locked while recording: locked widgets claim nothing, so a
        // press during a recording neither picks nor retargets.
        const ScopedDisabled lock(gui, state.recording);
        (void)table(gui, layout, theme, "bindings", columns, 2,
                    state.cells.data(), rows, &state.selected);
    }

    // The selected row's summary, the Record button (dead with no
    // selection), and the status line - the last outcome or the
    // resolver's own conflict warning.
    if (state.selected >= 0 && state.selected < rows) {
        const ws::app::Command& cmd = registry.commands()[static_cast<std::size_t>(
            state.row_commands[static_cast<std::size_t>(state.selected)])];
        const ws::app::Shortcut* bound = resolver.binding_for(cmd.id, ctx);
        state.detail = cmd.label;
        state.detail += "  ";
        if (bound != nullptr) {
            state.detail += ws::app::describe_chord(*bound);
        } else {
            state.detail += "<none>";
        }
    } else {
        state.detail.clear();
    }
    text(layout, theme, state.detail.c_str());

    if (!state.recording) {
        char scratch[128];
        const LabelFit fit = fit_text("Record...", -1.0f, theme, scratch,
                                       sizeof scratch);
        if (button(gui, layout, theme, "Record...",
                   fit.advance + 2.0f * theme.padding, {},
                   state.selected < 0)) {
            const ws::app::Command& cmd =
                registry.commands()[static_cast<std::size_t>(
                    state.row_commands[static_cast<std::size_t>(
                        state.selected)])];
            state.recording = true;
            state.recording_id = cmd.id;  // captured HERE, not at capture
            state.status.clear();
        }
    } else {
        text(layout, theme, "recording - press a key (Escape cancels)");
    }
    text(layout, theme, state.status.c_str());
}

}  // namespace ws::ui::gui
