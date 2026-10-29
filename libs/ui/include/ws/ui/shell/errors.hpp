#pragma once

#include <cstdint>

#include <ws/core/result.hpp>

namespace ws::ui::shell {

// The shell layer's error taxonomy (the workspace rule: each lib
// defines its own kinds and converts to ws::core::Error at its
// boundary). The codes ride ws::core::Error.code with category
// "ui.shell".
enum class ShellError : std::int32_t {
    invalid = 1,      // a setup bug: empty ids, missing callbacks
    io = 2,           // a config file that could not be read or written
    parse_failed = 3, // a config file that would not parse
    unavailable = 4,  // a service the config did not inject
    document = 5,     // a document load/save/close failure
};

[[nodiscard]] inline ws::core::Error shell_error(ShellError kind,
                                                 std::string message) {
    return ws::core::make_error(static_cast<std::int32_t>(kind), "ui.shell",
                                std::move(message));
}

}  // namespace ws::ui::shell
