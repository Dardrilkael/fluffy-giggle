#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <ws/app/command.hpp>
#include <ws/app/errors.hpp>
#include <ws/core/result.hpp>

namespace ws::app {

// The context ladder a binding lives on. Higher rungs are more
// specific and win: a text_editing binding beats a panel binding
// beats a global one for the same chord, but only while its rung is
// the ACTIVE one - the caller reports what holds focus now (a text
// field focused -> KeyContext::text_editing, a panel -> panel,
// otherwise global), and rungs ABOVE the active one never fire.
enum class KeyContext : std::uint8_t {
    global = 0,
    panel = 1,
    text_editing = 2,
};

// The platform's primary modifier. The workspace's desktop is
// Windows/GLFW today where control is primary; a mac bridge maps the
// command key's chords onto Mod::super_key instead - the mapping is
// the bridge's, the table here is the documented convention.
inline constexpr Mod kPrimaryModifier = Mod::control;

// One chord as display text, "Ctrl+Shift+S" and friends: modifiers
// in the Ctrl/Super/Alt/Shift order, printable keys by their uppercase
// letter, the GLFW-named keys by name, anything else "<key N>". The
// spelling the conflict messages build AND the one the ui views show
// as a menu hint, a palette row or a keymap cell - one text, so a
// rebind renames everywhere at once. Never a persistence format: the
// keymap file carries the numbers.
[[nodiscard]] std::string describe_chord(Shortcut chord);

// What a pressed chord resolved to. command is the id to dispatch
// ("" when nothing matched); consumed says the chord was the
// resolver's - a match, or an armed sequence's first step - so the
// caller must not also forward it (as text into a focused field);
// sequence_pending says a first step is armed and awaiting its second.
struct Resolution {
    std::string command;
    bool consumed = false;
    bool sequence_pending = false;
};

// The serialized shapes: one binding, one two-step sequence, and one
// context's whole table. install() takes these, table_of() answers
// them - the keymap file layer (keymap.hpp) builds them from json,
// the keymap editor's list view reads them back.
struct ShortcutBinding {
    Shortcut chord;
    std::string command;
};
struct ShortcutSequence {
    Shortcut first;
    Shortcut second;
    std::string command;
    std::int64_t timeout_ms = 0;
};
struct ShortcutTable {
    std::vector<ShortcutBinding> bindings;
    std::vector<ShortcutSequence> sequences;
};

// The live binding table: command ids to chords, per context, plus
// the two-step sequences and the user rebinds layered over the
// defaults. Owner-thread state, no locks (the command registry's
// rule): bind at setup or from the keymap editor, resolve on the key
// press edge.
class ShortcutResolver {
public:
    // The ms clock seam (the platform FileWatch shape): inject a
    // fake in tests to drive the sequence timeout deterministically;
    // the default is steady_clock ms.
    ShortcutResolver() = default;
    explicit ShortcutResolver(std::function<std::int64_t()> clock)
        : clock_(std::move(clock)) {}

    // Binds chord -> command in one context. A collision in the SAME
    // context answers conflict and stores nothing: with a plain
    // binding on that chord, or with a sequence whose FIRST step is
    // that chord - one chord in one context is either a plain binding
    // or a sequence prefix, never both. The same chord in another
    // context is the point of contexts and never conflicts.
    [[nodiscard]] ws::core::Status bind(Shortcut chord, std::string command,
                                        KeyContext context);

    // The two-step sequence: the first chord arms (consumed,
    // sequence_pending), the second completes it when pressed within
    // timeout_ms on the injected clock; the deadline is measured from
    // the first chord's press, re-armed by pressing the first chord
    // again. One sequence per first chord per context; a plain binding
    // on the first chord conflicts, and so does a second sequence on
    // it. The second chord never conflicts: while a sequence is
    // pending, its second outranks any plain binding on that chord.
    [[nodiscard]] ws::core::Status bind_sequence(Shortcut first, Shortcut second,
                                                 std::string command,
                                                 KeyContext context,
                                                 std::int64_t timeout_ms);

    // Frees a chord's plain binding in a context (a no-op when none;
    // a sequence prefix is not freed by this - unbind its command).
    [[nodiscard]] ws::core::Status unbind(Shortcut chord, KeyContext context);

    // Moves a command's plain binding in one context onto a new
    // chord (key 0 removes it). The target chord must be free of
    // OTHER commands' plain bindings and of every sequence prefix -
    // otherwise conflict, and the existing binding stands. Rebinding
    // onto the command's own current chord is the no-op it reads as.
    [[nodiscard]] ws::core::Status rebind(std::string_view command,
                                          KeyContext context, Shortcut chord);

    // Seeds the defaults from a registry: every command's declared
    // chord binds in the given context (global is the common case;
    // a command without a declared chord seeds nothing). The seed is
    // built whole or not at all: two commands declaring the same
    // chord answer conflict and nothing is seeded. Re-running
    // REPLACES that context's seed and its live table together -
    // the seed is declarative data, not merged state, so rebinds
    // made on the old table are lost (reset_to_defaults() restores
    // the last seed without re-reading the registry).
    [[nodiscard]] ws::core::Status install_defaults(const CommandRegistry& registry,
                                                    KeyContext context = KeyContext::global);

    // Restores every context's table to the last installed defaults
    // and drops any pending sequence step.
    void reset_to_defaults();

    // Replaces every context's live table in one all-or-nothing step
    // (the keymap load's edge): every entry runs the bind-time rules
    // - an empty command, a keyless chord, a non-positive timeout, a
    // duplicate chord, a chord that is both a plain binding and a
    // prefix, a second sequence on one prefix - and the first
    // refusal leaves the resolver exactly as it was. The defaults
    // seed is untouched (reset_to_defaults() still restores the last
    // install_defaults), and an armed sequence step is dropped with
    // the tables it armed on.
    [[nodiscard]] ws::core::Status install(const ShortcutTable& global,
                                           const ShortcutTable& panel,
                                           const ShortcutTable& text_editing);

    // One context's live table as a copy - the keymap serializer's
    // and the editor's list view's read.
    [[nodiscard]] ShortcutTable table_of(KeyContext context) const;

    // Resolves one pressed chord (a key PRESS edge - the caller
    // tracks edges, the resolver never sees a held repeat). Walks
    // the ladder from the active rung down to global: the first rung
    // holding the chord wins. THE TEXT RULE: while text_editing is
    // active, a winning binding from a LOWER rung fires only when
    // its chord carries a modifier beyond shift (shift alone is
    // typing) - an unmodified global chord leaves the key to the
    // field, consumed=false. An armed first step outranks everything
    // and holds the rung it armed on: its second chord completes
    // there (consumed) even if focus moved before it landed, any
    // other chord cancels it and resolves fresh, and an expired
    // deadline (the injected clock) cancels it before the chord
    // resolves.
    [[nodiscard]] Resolution resolve(Shortcut pressed, KeyContext active);

    // A sequence's first step is armed (the keymap editor's "awaiting
    // second chord" hint); cancel_pending() drops it.
    [[nodiscard]] bool sequence_pending() const noexcept { return pending_.armed; }
    void cancel_pending() noexcept;

    // The lookups the menu/toolbar builders use for their shortcut
    // hints: nullptr when the command carries no plain binding in
    // that context.
    [[nodiscard]] const Shortcut* binding_for(std::string_view command,
                                              KeyContext context) const;

    // The reverse lookup: nullptr when the chord is free in that
    // context (plain bindings only - a sequence prefix is not "a
    // command's shortcut" for display).
    [[nodiscard]] const std::string* command_for(Shortcut chord,
                                                  KeyContext context) const;

private:
    struct Plain {
        Shortcut chord;
        std::string command;
    };
    struct Sequence {
        Shortcut first;
        Shortcut second;
        std::string command;
        std::int64_t timeout_ms;
    };
    struct Table {
        std::vector<Plain> plains;
        std::vector<Sequence> sequences;
    };

    struct Pending {
        bool armed = false;
        KeyContext context = KeyContext::global;
        Shortcut first;
        std::int64_t deadline_ms = 0;
    };

    [[nodiscard]] static Table& tables_for(std::vector<Table>& tables,
                                           KeyContext context);
    [[nodiscard]] static const Table& tables_for(const std::vector<Table>& tables,
                                                 KeyContext context);
    [[nodiscard]] static bool chord_modified(Shortcut chord) noexcept;
    // The bind-time rules, shared by bind()/bind_sequence() and
    // install() (the load path): validate + append into one context's
    // table, nothing stored on refusal.
    [[nodiscard]] static ws::core::Status push_plain(Table& table, Shortcut chord,
                                                     std::string command);
    [[nodiscard]] static ws::core::Status push_sequence(Table& table, Shortcut first,
                                                        Shortcut second,
                                                        std::string command,
                                                        std::int64_t timeout_ms);

    std::function<std::int64_t()> clock_;
    std::vector<Table> tables_{
        Table{}, Table{}, Table{}};  // indexed by KeyContext
    std::vector<Table> defaults_{
        Table{}, Table{}, Table{}};  // the last install_defaults seed
    Pending pending_;
};

}  // namespace ws::app
