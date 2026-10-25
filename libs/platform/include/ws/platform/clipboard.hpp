// The session clipboard, text-first, behind Result-only free
// functions. Every read answers nullopt when the clipboard holds
// no such content - an empty clipboard is a normal answer, never
// an error; the failure channel stays for the OS refusing (a
// clipboard held by another window, a failed global alloc).
//
// No interface here, unlike the dialogs: there is no fake to
// script - a clipboard round trip is the real thing, and the
// gallery demos it interactively (a test that clobbers the
// user's clipboard would be a bad citizen; the suite has no
// clipboard case on purpose).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <ws/core/result.hpp>

namespace ws::platform {

// Utf-8 narrow in and out. The OS channel is NUL-terminated, so
// embedded NULs stop at the first - the payload channel below is
// the binary-safe one.
[[nodiscard]] ws::core::Status set_clipboard_text(std::string_view text);
[[nodiscard]] ws::core::Result<std::optional<std::string>> clipboard_text();

// Arbitrary bytes under a caller-chosen type name. The type
// doubles as the OS format name, so two ws apps sharing a payload
// type interoperate with anything else that registers the same
// name - and never collide with the system formats.
[[nodiscard]] ws::core::Status set_clipboard_payload(
    std::string_view type, std::string_view bytes);
[[nodiscard]] ws::core::Result<std::optional<std::string>>
clipboard_payload(std::string_view type);

// An image as plain pixels: 8-bit RGBA, top-down rows, tightly
// packed. The DIB shuffle - BGRA, row order, four-byte alignment -
// is this lib's business, never the caller's.
struct ClipboardImage {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::string bytes;  // width * height * 4, RGBA top-down
};

[[nodiscard]] ws::core::Status set_clipboard_image(
    const ClipboardImage& image);
[[nodiscard]] ws::core::Result<std::optional<ClipboardImage>>
clipboard_image();

}  // namespace ws::platform
