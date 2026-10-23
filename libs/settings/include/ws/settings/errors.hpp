#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <ws/core/result.hpp>

namespace ws::settings {

// ws::settings's error codes (root CLAUDE.md, "Error handling"): every
// public failure returns a ws::core::Error with category "settings" and
// one of these as its code. The enum stays ws::settings's - core never
// knows these values. Codes are append-only: never renumber, only add.
// parse_failed/migrate_failed/io_failed/corrupt_file exist for the arc's
// later phases (the json parser, the migration chain, the FileSystem
// seam); they are named here so the table is complete the day the lib
// lands.
enum class SettingsError : std::int32_t {
    invalid_argument = 1,    // a layer that takes no explicit values (defaults)
    unknown_key = 2,         // key not declared in the schema
    type_mismatch = 3,       // value or read type differs from the schema's
    constraint_rejected = 4, // a reject-policy constraint said no
    bad_schema = 5,          // a malformed declaration (duplicate, wrong default, ...)
    parse_failed = 6,        // json text did not parse
    migrate_failed = 7,      // a missing migration step, or a step that failed
    io_failed = 8,           // a FileSystem operation failed
    corrupt_file = 9,        // a loaded file was unusable (.bak kept, defaults served)
};

// One-line description per code, from a static table. The string_view
// points into this header, so it stays valid forever - nothing builds
// messages per call.
[[nodiscard]] constexpr std::string_view describe(SettingsError code) noexcept {
    switch (code) {
        case SettingsError::invalid_argument: return "invalid argument";
        case SettingsError::unknown_key: return "unknown setting key";
        case SettingsError::type_mismatch: return "type mismatch";
        case SettingsError::constraint_rejected: return "value rejected by constraint";
        case SettingsError::bad_schema: return "malformed schema declaration";
        case SettingsError::parse_failed: return "json parse failed";
        case SettingsError::migrate_failed: return "migration failed";
        case SettingsError::io_failed: return "filesystem operation failed";
        case SettingsError::corrupt_file: return "settings file corrupt";
    }
    return "unknown settings error";
}

[[nodiscard]] inline ws::core::Error make_settings_error(
    SettingsError code, std::string message) {
    return ws::core::make_error(static_cast<std::int32_t>(code), "settings",
                                std::move(message));
}

// unexpected<Error> that converts into any Result<T>:
//   return fail(SettingsError::unknown_key, "no such setting: editor.x");
// RESULT-PATH ONLY: this allocates (ws::core::Error::message is a
// std::string by design). The name hides ws::core::fail inside
// ws::settings on purpose: settings errors go through the enum, never
// raw codes.
[[nodiscard]] inline ws::core::unexpected<ws::core::Error> fail(
    SettingsError code, std::string message = {}) {
    if (message.empty()) {
        message = std::string{describe(code)};
    }
    return ws::core::unexpected<ws::core::Error>(
        make_settings_error(code, std::move(message)));
}

}  // namespace ws::settings
