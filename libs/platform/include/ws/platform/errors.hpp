#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <ws/core/result.hpp>

namespace ws::platform {

// ws::platform's error codes (root CLAUDE.md, "Error handling"): every
// public failure returns a ws::core::Error with category "platform" and
// one of these as its code. The enum stays ws::platform's - core never
// knows these values. Codes are append-only: never renumber, only add.
// canceled/parse_failed/corrupt_file/invalid_argument exist for the
// arc's later phases (dialogs, the recent-files list, autosave
// recovery); they are named here so the table is complete the day the
// lib lands.
enum class PlatformError : std::int32_t {
    io_failed = 1,         // a location or directory operation failed
    unavailable = 2,       // this platform cannot answer (android os paths)
    canceled = 3,          // the user dismissed a dialog
    parse_failed = 4,      // a persisted document did not parse
    corrupt_file = 5,      // a recovery marker or backup was unusable
    invalid_argument = 6,  // an empty app name, a zero cap
};

// One-line description per code, from a static table. The string_view
// points into this header, so it stays valid forever - nothing builds
// messages per call.
[[nodiscard]] constexpr std::string_view describe(PlatformError code) noexcept {
    switch (code) {
        case PlatformError::io_failed: return "filesystem operation failed";
        case PlatformError::unavailable: return "unavailable on this platform";
        case PlatformError::canceled: return "canceled by the user";
        case PlatformError::parse_failed: return "persisted document did not parse";
        case PlatformError::corrupt_file: return "recovery data unusable";
        case PlatformError::invalid_argument: return "invalid argument";
    }
    return "unknown platform error";
}

[[nodiscard]] inline ws::core::Error make_platform_error(
    PlatformError code, std::string message) {
    return ws::core::make_error(static_cast<std::int32_t>(code), "platform",
                                std::move(message));
}

// unexpected<Error> that converts into any Result<T>:
//   return fail(PlatformError::unavailable, "no os truth without roots");
// RESULT-PATH ONLY: this allocates (ws::core::Error::message is a
// std::string by design). The name hides ws::core::fail inside
// ws::platform on purpose: platform errors go through the enum, never
// raw codes.
[[nodiscard]] inline ws::core::unexpected<ws::core::Error> fail(
    PlatformError code, std::string message = {}) {
    if (message.empty()) {
        message = std::string{describe(code)};
    }
    return ws::core::unexpected<ws::core::Error>(
        make_platform_error(code, std::move(message)));
}

}  // namespace ws::platform
