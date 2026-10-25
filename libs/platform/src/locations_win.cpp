// The Windows half of os_location: the KnownFolders behind
// SHGetKnownFolderPath, GetTempPathW for temp. It only names
// directories - nothing here touches the disk (that is
// ensure_directory's job) and no Windows type crosses the header.

#include <string>
#include <string_view>

#include <ws/platform/errors.hpp>
#include <ws/platform/locations.hpp>
#include <ws/platform/paths.hpp>

// windows.h last: its min/max macros must not shadow the std headers.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>

namespace ws::platform {
namespace {

// wide -> utf-8 narrow: every OS string crosses this edge once.
[[nodiscard]] std::string to_utf8(const wchar_t* wide) {
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

[[nodiscard]] ws::core::Result<std::string> known_folder(
    REFKNOWNFOLDERID id, std::string_view what) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) ||
        raw == nullptr) {
        return fail(PlatformError::io_failed,
                    "SHGetKnownFolderPath failed for " + std::string{what});
    }
    std::string out = to_utf8(raw);
    CoTaskMemFree(raw);
    return out;
}

}  // namespace

ws::core::Result<std::string> os_location(Location which,
                                          std::string_view app_name) {
    if (app_name.empty()) {
        return fail(PlatformError::invalid_argument,
                    "os_location: the app name is empty");
    }
    switch (which) {
        case Location::config:
        case Location::data:
        case Location::cache: {
            // config roams with the user; data and cache are machine
            // local, cache nested under data.
            const bool roaming = which == Location::config;
            WS_TRY_ASSIGN(const std::string base,
                          known_folder(roaming ? FOLDERID_RoamingAppData
                                              : FOLDERID_LocalAppData,
                                      roaming ? "roaming app data"
                                              : "local app data"));
            std::string app_dir = join_path(base, app_name);
            if (which == Location::cache) {
                app_dir = join_path(app_dir, "cache");
            }
            return app_dir;
        }
        case Location::documents:
            return known_folder(FOLDERID_Documents, "documents");
        case Location::temp: {
            wchar_t buf[MAX_PATH + 1] = {};
            if (GetTempPathW(MAX_PATH + 1, buf) == 0) {
                return fail(PlatformError::io_failed,
                            "GetTempPathW failed");
            }
            // GetTempPath answers a trailing separator; the API's paths
            // never carry one.
            return normalize_path(to_utf8(buf));
        }
    }
    return fail(PlatformError::invalid_argument, "unknown location");
}

}  // namespace ws::platform
