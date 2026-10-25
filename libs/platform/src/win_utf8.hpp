// The Windows TUs' shared utf-8 <-> utf-16 edge, src-local (never
// under include/ - the no-leak rule; no Windows type crosses it).
// Every OS string crosses this edge once per TU; a malformed utf-8
// input maps to replacement characters, never a failure.

#pragma once

#include <string>
#include <string_view>

// windows.h last: its min/max macros must not shadow the std headers
// (the locations_win idiom, hoisted here once the win TUs hit three).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace ws::platform {

// A NUL-terminated wide string from the OS -> utf-8 narrow.
[[nodiscard]] inline std::string to_utf8(const wchar_t* wide) {
    if (wide == nullptr) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0,
                                          nullptr, nullptr);
    if (bytes <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), bytes, nullptr,
                        nullptr);
    return out;
}

// Utf-8 narrow -> wide; empty in, empty out.
[[nodiscard]] inline std::wstring from_utf8(std::string_view narrow) {
    if (narrow.empty()) {
        return {};
    }
    const int wide = MultiByteToWideChar(
        CP_UTF8, 0, narrow.data(), static_cast<int>(narrow.size()),
        nullptr, 0);
    if (wide <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, narrow.data(),
                        static_cast<int>(narrow.size()), out.data(), wide);
    return out;
}

}  // namespace ws::platform
