// The window title, composed: app, document, dirty bit. The
// convention is Notepad's - the document leads, and a star
// prefixes the whole title when there is unsaved work:
//
//   clean, no document  -> "App"
//   dirty, no document  -> "*App"
//   clean, document     -> "Document - App"
//   dirty, document     -> "*Document - App"
//
// Pure composition - the windowing layer (ui, which owns the
// window) hands the result to the OS. An empty app name answers
// invalid_argument: a title with no app is a caller bug, not a
// platform answer.

#pragma once

#include <string>
#include <string_view>

#include <ws/core/result.hpp>

namespace ws::platform {

[[nodiscard]] ws::core::Result<std::string> window_title(
    std::string_view app, std::string_view document, bool dirty);

}  // namespace ws::platform
