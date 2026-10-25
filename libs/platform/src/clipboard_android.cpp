// The Android refusal: the clipboard lives behind the activity's
// ClipboardManager, reached only through the JNI the app TU owns
// - the locations/dialogs precedent. A bridge lands with the
// first android consumer.

#include <optional>
#include <string>
#include <string_view>

#include <ws/platform/clipboard.hpp>
#include <ws/platform/errors.hpp>

namespace ws::platform {

ws::core::Status set_clipboard_text(std::string_view /*text*/) {
    return fail(PlatformError::unavailable,
                "clipboard: unavailable on this platform");
}

ws::core::Result<std::optional<std::string>> clipboard_text() {
    return fail(PlatformError::unavailable,
                "clipboard: unavailable on this platform");
}

ws::core::Status set_clipboard_payload(std::string_view /*type*/,
                                       std::string_view /*bytes*/) {
    return fail(PlatformError::unavailable,
                "clipboard: unavailable on this platform");
}

ws::core::Result<std::optional<std::string>> clipboard_payload(
    std::string_view /*type*/) {
    return fail(PlatformError::unavailable,
                "clipboard: unavailable on this platform");
}

ws::core::Status set_clipboard_image(const ClipboardImage& /*image*/) {
    return fail(PlatformError::unavailable,
                "clipboard: unavailable on this platform");
}

ws::core::Result<std::optional<ClipboardImage>> clipboard_image() {
    return fail(PlatformError::unavailable,
                "clipboard: unavailable on this platform");
}

}  // namespace ws::platform
