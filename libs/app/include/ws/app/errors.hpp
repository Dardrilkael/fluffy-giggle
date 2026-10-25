#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <ws/core/result.hpp>

namespace ws::app {

// ws::app's error codes (root CLAUDE.md, "Error handling"): every
// public failure returns a ws::core::Error with category "app" and one
// of these as its code. The enum stays ws::app's - core never knows
// these values. Codes are append-only: never renumber, only add.
// not_found/conflict/parse_failed/corrupt_file/io_failed exist for the
// arc's later phases (dispatching an unknown id now, then the shortcut
// resolver's registration-time conflict and the keymap file's
// parse/corrupt/io stories); they are named here so the table is
// complete the day the lib lands.
enum class AppError : std::int32_t {
    invalid_argument = 1,  // an empty id/label, a missing execute callback
    not_found = 2,        // dispatch or lookup of an unknown command id
    conflict = 3,         // a shortcut binding collides (reported at bind)
    parse_failed = 4,     // a persisted keymap did not parse
    corrupt_file = 5,     // a keymap entry is well-formed json, wrong shape
    io_failed = 6,        // a keymap read or write through the seam failed
};

// One-line description per code, from a static table. The string_view
// points into this header, so it stays valid forever - nothing builds
// messages per call.
[[nodiscard]] constexpr std::string_view describe(AppError code) noexcept {
    switch (code) {
        case AppError::invalid_argument: return "invalid argument";
        case AppError::not_found: return "no such command id";
        case AppError::conflict: return "shortcut binding conflict";
        case AppError::parse_failed: return "persisted document did not parse";
        case AppError::corrupt_file: return "persisted document unusable";
        case AppError::io_failed: return "filesystem operation failed";
    }
    return "unknown app error";
}

[[nodiscard]] inline ws::core::Error make_app_error(AppError code,
                                                    std::string message) {
    return ws::core::make_error(static_cast<std::int32_t>(code), "app",
                                std::move(message));
}

// unexpected<Error> that converts into any Result<T>:
//   return fail(AppError::not_found, "no command 'file.open'");
// RESULT-PATH ONLY: this allocates (ws::core::Error::message is a
// std::string by design). The name hides ws::core::fail inside
// ws::app on purpose: app errors go through the enum, never raw codes.
[[nodiscard]] inline ws::core::unexpected<ws::core::Error> fail(
    AppError code, std::string message = {}) {
    if (message.empty()) {
        message = std::string{describe(code)};
    }
    return ws::core::unexpected<ws::core::Error>(
        make_app_error(code, std::move(message)));
}

}  // namespace ws::app
