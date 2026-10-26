#include <chrono>
#include <cstddef>
#include <string>
#include <utility>

#include <ws/app/shortcut.hpp>

namespace ws::app {
namespace {

// The default clock: steady_clock ms since epoch (the platform
// FileWatch shape).
std::int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The GLFW key names describe_chord spells out; every other code
// falls back to "<key N>" (the numbering is a convention this lib
// documents, not a GLFW include).
const char* named_key(int key) {
    switch (key) {
        case 32: return "Space";
        case 256: return "Esc";
        case 257: return "Enter";
        case 258: return "Tab";
        case 259: return "Backspace";
        case 260: return "Insert";
        case 261: return "Delete";
        case 262: return "Right";
        case 263: return "Left";
        case 264: return "Down";
        case 265: return "Up";
        case 266: return "PageUp";
        case 267: return "PageDown";
        case 268: return "Home";
        case 269: return "End";
        default: return nullptr;
    }
}

}  // namespace

std::string describe_chord(Shortcut chord) {
    std::string text;
    if (has_mod(chord.mods, Mod::control)) {
        text += "Ctrl+";
    }
    if (has_mod(chord.mods, Mod::super_key)) {
        text += "Super+";
    }
    if (has_mod(chord.mods, Mod::alt)) {
        text += "Alt+";
    }
    if (has_mod(chord.mods, Mod::shift)) {
        text += "Shift+";
    }
    if (chord.key >= 32 && chord.key < 127) {
        // GLFW printable keys ride their ASCII values, letter keys the
        // UPPERCASE code, so spell them that way.
        char letter = static_cast<char>(chord.key);
        if (letter >= 'a' && letter <= 'z') {
            letter = static_cast<char>(letter - 'a' + 'A');
        }
        text += letter;
        return text;
    }
    if (const char* name = named_key(chord.key); name != nullptr) {
        text += name;
        return text;
    }
    text += "<key " + std::to_string(chord.key) + ">";
    return text;
}

bool ShortcutResolver::chord_modified(Shortcut chord) noexcept {
    // Shift alone is typing: only ctrl/alt/super make a chord the
    // resolver's while a text field holds focus.
    return has_mod(chord.mods, Mod::control) || has_mod(chord.mods, Mod::alt) ||
           has_mod(chord.mods, Mod::super_key);
}

ShortcutResolver::Table& ShortcutResolver::tables_for(std::vector<Table>& tables,
                                                      KeyContext context) {
    return tables[static_cast<std::size_t>(context)];
}

const ShortcutResolver::Table& ShortcutResolver::tables_for(
    const std::vector<Table>& tables, KeyContext context) {
    return tables[static_cast<std::size_t>(context)];
}

ws::core::Status ShortcutResolver::push_plain(Table& table, Shortcut chord,
                                              std::string command) {
    if (command.empty()) {
        return fail(AppError::invalid_argument, "a binding needs a command id");
    }
    if (chord.key == 0) {
        return fail(AppError::invalid_argument, "a binding needs a key");
    }
    for (const Plain& plain : table.plains) {
        if (plain.chord == chord) {
            return fail(AppError::conflict,
                        describe_chord(chord) + " is already bound to '" +
                            plain.command + "'");
        }
    }
    for (const Sequence& sequence : table.sequences) {
        if (sequence.first == chord) {
            return fail(AppError::conflict,
                        describe_chord(chord) + " opens the '" + sequence.command +
                            "' sequence");
        }
    }
    table.plains.push_back(Plain{chord, std::move(command)});
    return {};
}

ws::core::Status ShortcutResolver::push_sequence(Table& table, Shortcut first,
                                                 Shortcut second,
                                                 std::string command,
                                                 std::int64_t timeout_ms) {
    if (command.empty()) {
        return fail(AppError::invalid_argument, "a sequence needs a command id");
    }
    if (first.key == 0 || second.key == 0) {
        return fail(AppError::invalid_argument, "a sequence needs two keys");
    }
    if (timeout_ms <= 0) {
        return fail(AppError::invalid_argument, "a sequence needs a positive timeout");
    }
    for (const Plain& plain : table.plains) {
        if (plain.chord == first) {
            return fail(AppError::conflict,
                        describe_chord(first) + " is already bound to '" +
                            plain.command + "'");
        }
    }
    for (const Sequence& sequence : table.sequences) {
        if (sequence.first == first) {
            return fail(AppError::conflict,
                        describe_chord(first) + " already opens the '" +
                            sequence.command + "' sequence");
        }
    }
    table.sequences.push_back(
        Sequence{first, second, std::move(command), timeout_ms});
    return {};
}

ws::core::Status ShortcutResolver::bind(Shortcut chord, std::string command,
                                        KeyContext context) {
    return push_plain(tables_for(tables_, context), chord, std::move(command));
}

ws::core::Status ShortcutResolver::bind_sequence(Shortcut first, Shortcut second,
                                                 std::string command,
                                                 KeyContext context,
                                                 std::int64_t timeout_ms) {
    return push_sequence(tables_for(tables_, context), first, second,
                         std::move(command), timeout_ms);
}

ws::core::Status ShortcutResolver::unbind(Shortcut chord, KeyContext context) {
    if (chord.key == 0) {
        return fail(AppError::invalid_argument, "unbind needs a key");
    }
    Table& table = tables_for(tables_, context);
    for (std::size_t i = 0; i < table.plains.size(); ++i) {
        if (table.plains[i].chord == chord) {
            table.plains.erase(table.plains.begin() +
                               static_cast<std::ptrdiff_t>(i));
            return {};
        }
    }
    return {};  // freeing an empty slot is the no-op it reads as
}

ws::core::Status ShortcutResolver::rebind(std::string_view command,
                                          KeyContext context, Shortcut chord) {
    Table& table = tables_for(tables_, context);
    Plain* current = nullptr;
    for (Plain& plain : table.plains) {
        if (plain.command == command) {
            current = &plain;
            break;
        }
    }
    if (current == nullptr) {
        return fail(AppError::not_found,
                    "command '" + std::string{command} +
                        "' has no plain binding in that context to rebind");
    }
    if (current->chord == chord) {
        return {};  // onto its own chord: the no-op it reads as
    }
    if (chord.key == 0) {
        // Key 0 removes the binding - the keymap editor's "none" slot.
        const auto index =
            static_cast<std::size_t>(current - table.plains.data());
        table.plains.erase(table.plains.begin() + static_cast<std::ptrdiff_t>(index));
        return {};
    }
    for (const Plain& plain : table.plains) {
        if (&plain != current && plain.chord == chord) {
            return fail(AppError::conflict,
                        describe_chord(chord) + " is already bound to '" +
                            plain.command + "'");
        }
    }
    for (const Sequence& sequence : table.sequences) {
        if (sequence.first == chord) {
            return fail(AppError::conflict,
                        describe_chord(chord) + " opens the '" + sequence.command +
                            "' sequence");
        }
    }
    current->chord = chord;
    return {};
}

ws::core::Status ShortcutResolver::install_defaults(const CommandRegistry& registry,
                                                    KeyContext context) {
    // Built whole or not at all: a clash between two declared chords
    // seeds nothing at all.
    Table seed;
    for (const Command& command : registry.commands()) {
        if (command.shortcut.key == 0) {
            continue;  // no declared chord - nothing to seed
        }
        for (const Plain& plain : seed.plains) {
            if (plain.chord == command.shortcut) {
                return fail(AppError::conflict,
                            describe_chord(command.shortcut) +
                                " is declared by both '" + plain.command +
                                "' and '" + command.id + "'");
            }
        }
        seed.plains.push_back(Plain{command.shortcut, command.id});
    }
    tables_for(tables_, context) = seed;
    tables_for(defaults_, context) = std::move(seed);
    if (pending_.armed && pending_.context == context) {
        pending_.armed = false;  // the table it armed on is gone
    }
    return {};
}

void ShortcutResolver::reset_to_defaults() {
    tables_ = defaults_;  // the last installed seed, every context
    pending_.armed = false;
}

ws::core::Status ShortcutResolver::install(const ShortcutTable& global,
                                           const ShortcutTable& panel,
                                           const ShortcutTable& text_editing) {
    // Built whole or not at all: every context validates into a
    // scratch set before anything commits (the keymap load's
    // all-or-nothing edge).
    std::vector<Table> built{Table{}, Table{}, Table{}};
    const ShortcutTable* sources[3] = {&global, &panel, &text_editing};
    for (int rung = 0; rung < 3; ++rung) {
        Table& table = built[static_cast<std::size_t>(rung)];
        for (const ShortcutBinding& binding : sources[rung]->bindings) {
            const ws::core::Status pushed =
                push_plain(table, binding.chord, binding.command);
            if (!pushed.has_value()) {
                return pushed;
            }
        }
        for (const ShortcutSequence& sequence : sources[rung]->sequences) {
            const ws::core::Status pushed =
                push_sequence(table, sequence.first, sequence.second,
                              sequence.command, sequence.timeout_ms);
            if (!pushed.has_value()) {
                return pushed;
            }
        }
    }
    tables_ = std::move(built);
    pending_.armed = false;  // the tables it armed on are gone
    return {};
}

ShortcutTable ShortcutResolver::table_of(KeyContext context) const {
    const Table& table = tables_for(tables_, context);
    ShortcutTable out;
    out.bindings.reserve(table.plains.size());
    for (const Plain& plain : table.plains) {
        out.bindings.push_back(ShortcutBinding{plain.chord, plain.command});
    }
    out.sequences.reserve(table.sequences.size());
    for (const Sequence& sequence : table.sequences) {
        out.sequences.push_back(ShortcutSequence{sequence.first, sequence.second,
                                                 sequence.command,
                                                 sequence.timeout_ms});
    }
    return out;
}

Resolution ShortcutResolver::resolve(Shortcut pressed, KeyContext active) {
    const std::int64_t now = clock_ ? clock_() : steady_ms();
    Resolution result;

    // An armed first step outranks everything, on the rung it armed
    // on: its second chord completes within the deadline; any other
    // chord - or an expired deadline - drops it and resolves fresh.
    if (pending_.armed) {
        if (now <= pending_.deadline_ms) {
            const Table& table = tables_for(tables_, pending_.context);
            for (const Sequence& sequence : table.sequences) {
                if (sequence.first == pending_.first && sequence.second == pressed) {
                    pending_.armed = false;
                    result.command = sequence.command;
                    result.consumed = true;
                    return result;
                }
            }
        }
        pending_.armed = false;
    }

    // The ladder walk: from the active rung down to global, the first
    // rung holding the chord - as a plain binding or a sequence's
    // first step - wins.
    for (int rung = static_cast<int>(active); rung >= 0; --rung) {
        if (active == KeyContext::text_editing &&
            rung < static_cast<int>(active) && !chord_modified(pressed)) {
            // THE TEXT RULE: while a field holds focus, an unmodified
            // chord from a lower rung leaves the key to the field.
            return result;
        }
        const KeyContext context = static_cast<KeyContext>(rung);
        const Table& table = tables_for(tables_, context);
        for (const Plain& plain : table.plains) {
            if (plain.chord == pressed) {
                result.command = plain.command;
                result.consumed = true;
                return result;
            }
        }
        for (const Sequence& sequence : table.sequences) {
            if (sequence.first == pressed) {
                pending_.armed = true;
                pending_.context = context;
                pending_.first = pressed;
                pending_.deadline_ms = now + sequence.timeout_ms;
                result.consumed = true;
                result.sequence_pending = true;
                return result;
            }
        }
    }
    return result;
}

void ShortcutResolver::cancel_pending() noexcept {
    pending_ = Pending{};
}

const Shortcut* ShortcutResolver::binding_for(std::string_view command,
                                              KeyContext context) const {
    const Table& table = tables_for(tables_, context);
    for (const Plain& plain : table.plains) {
        if (plain.command == command) {
            return &plain.chord;
        }
    }
    return nullptr;
}

const std::string* ShortcutResolver::command_for(Shortcut chord,
                                                 KeyContext context) const {
    const Table& table = tables_for(tables_, context);
    for (const Plain& plain : table.plains) {
        if (plain.chord == chord) {
            return &plain.command;
        }
    }
    return nullptr;
}

}  // namespace ws::app
