#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <ws/app/errors.hpp>
#include <ws/core/result.hpp>

namespace ws::app {

// A chord as data: the key code plus the modifier mask. Key codes
// ride as plain ints in GLFW numbering (the workspace convention,
// ws::input's rule) and the modifier bits mirror GLFW_MOD_* (shift 1,
// control 2, alt 4, super 8), so ws::ui's bridge passes input state
// straight through with no mapping table. This is the DECLARED
// default a command ships with; live bindings - defaults plus user
// rebinds, with conflict detection - are the shortcut resolver's
// table, a later phase of this arc.
enum class Mod : std::uint32_t {
    none = 0,
    shift = 1,
    control = 2,
    alt = 4,
    super_key = 8,
};

[[nodiscard]] constexpr Mod operator|(Mod a, Mod b) noexcept {
    return static_cast<Mod>(static_cast<std::uint32_t>(a) |
                            static_cast<std::uint32_t>(b));
}

[[nodiscard]] constexpr Mod operator&(Mod a, Mod b) noexcept {
    return static_cast<Mod>(static_cast<std::uint32_t>(a) &
                            static_cast<std::uint32_t>(b));
}

// Whether `value` carries `test` (Mod::none tests true only for none).
[[nodiscard]] constexpr bool has_mod(Mod value, Mod test) noexcept {
    if (test == Mod::none) {
        return value == Mod::none;
    }
    return (static_cast<std::uint32_t>(value) &
            static_cast<std::uint32_t>(test)) != 0;
}

struct Shortcut {
    int key = 0;    // GLFW numbering; 0 = no shortcut declared
    Mod mods = Mod::none;
};

// One registrable action. id is the stable key: menus and keymap
// persistence name commands by it, so it must survive refactors -
// renaming one orphans user bindings. label is the UI text, category
// groups the menu builders and the palette. enabled/checked are read
// per frame by the views (an empty predicate = always enabled / never
// checked - a command without one never pays a call). execute takes
// the argument payload ("" = none); needs_argument tells the palette
// to prompt for one before dispatching.
struct Command {
    std::string id;
    std::string label;
    std::string category;
    int icon = 0;      // the ui layer maps it to a face; 0 = none
    Shortcut shortcut;  // the declared default chord
    bool needs_argument = false;
    std::function<bool()> enabled;
    std::function<bool()> checked;
    std::function<void(std::string_view)> execute;
};

// The command table an app shell composes. Owner-thread state, no
// locks (the jobs/settings/platform rule): registration happens at
// setup, queries and dispatch on the frame - nothing here is callable
// from render or audio callbacks.
class CommandRegistry {
public:
    CommandRegistry() = default;

    // Adds a command. Refuses an empty id, an empty label, an empty
    // execute callback and an id already registered - all
    // invalid_argument, because each is a registration bug rather
    // than a runtime failure. The command is moved in; the registry
    // owns every callback.
    [[nodiscard]] ws::core::Status add(Command command);

    // Dispatch by id. An unknown id answers not_found. enabled() is
    // deliberately NOT consulted: the views gate their chrome on it,
    // but a direct dispatch is the caller's decision - programmatic
    // invokes are how buttons and palette picks run.
    [[nodiscard]] ws::core::Status execute(std::string_view id,
                                           std::string_view argument = {}) const;

    // Lookup for the menu/toolbar/palette builders; nullptr when
    // unknown, so a builder skips ids it cannot resolve.
    [[nodiscard]] const Command* find(std::string_view id) const;

    // The per-frame view helpers over a resolved command: empty
    // predicates read as always-enabled / never-checked.
    [[nodiscard]] static bool is_enabled(const Command& command);
    [[nodiscard]] static bool is_checked(const Command& command);

    // Every command in registration order - the builders' stable
    // walk (a menu's section order follows it).
    [[nodiscard]] const std::vector<Command>& commands() const noexcept {
        return commands_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return commands_.size();
    }

private:
    std::vector<Command> commands_;
};

}  // namespace ws::app
