// The window-title composer - pure, every platform, no OS half.

#include <string>
#include <string_view>

#include <ws/platform/errors.hpp>
#include <ws/platform/title.hpp>

namespace ws::platform {

ws::core::Result<std::string> window_title(std::string_view app,
                                           std::string_view document,
                                           bool dirty) {
    if (app.empty()) {
        return fail(PlatformError::invalid_argument,
                    "window title: the app name is empty");
    }
    std::string title;
    if (dirty) {
        title += "*";
    }
    if (!document.empty()) {
        title += document;
        title += " - ";
    }
    title += app;
    return title;
}

}  // namespace ws::platform
