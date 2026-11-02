// The posix dev-box half of os_location: the XDG Base Directory truth
// over getenv, nothing more. The workspace's SHIPPED desktop is
// Windows (the ws::ui windowing precedent) - this half exists so the
// Linux dev box gets the same Locations answer (and so ws::platform,
// and everything that links it, builds and tests there at all: the
// headless suites' own docs promise they run on Linux). It only names
// directories - nothing here touches the disk (ensure_directory's
// job) and no posix type crosses the header.
//
// The truth, per the XDG spec's defaults:
//   config  $XDG_CONFIG_HOME           else ~/.config/<app>
//   data    $XDG_DATA_HOME             else ~/.local/share/<app>
//   cache   $XDG_CACHE_HOME            else ~/.cache/<app>
//   documents  (no XDG var; the spec leaves it to the portal) - $HOME
//   temp    $TMPDIR                    else /tmp
// An XDG var must be absolute (the spec's own rule); a relative one is
// ignored, the default answering instead. No $HOME and no supplied
// root is the one refusal - a posix user without a home has nowhere
// app-scoped to be.

#include <cstdlib>
#include <string>
#include <string_view>

#include <ws/platform/errors.hpp>
#include <ws/platform/locations.hpp>
#include <ws/platform/paths.hpp>

namespace ws::platform {
namespace {

// getenv behind a stable spelling (std::getenv is fine: this is
// setup-time, owner-thread state by the lib's rule).
[[nodiscard]] const char* env_value(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0' ? v : nullptr;
}

// The XDG var when it names an absolute path, else nullopt-shaped
// (null) - a relative or empty value reads as unset.
[[nodiscard]] const char* xdg_root(const char* name) {
    const char* v = env_value(name);
    if (v == nullptr || !is_absolute_path(v)) {
        return nullptr;
    }
    return v;
}

[[nodiscard]] ws::core::Result<std::string> home_based(
    std::string_view what) {
    const char* home = env_value("HOME");
    if (home == nullptr) {
        return fail(PlatformError::unavailable,
                    "os_location: no HOME for " + std::string{what});
    }
    return std::string{home};
}

}  // namespace

ws::core::Result<std::string> os_location(Location which,
                                          std::string_view app_name) {
    if (app_name.empty()) {
        return fail(PlatformError::invalid_argument,
                    "os_location: the app name is empty");
    }
    switch (which) {
        case Location::config: {
            if (const char* xdg = xdg_root("XDG_CONFIG_HOME")) {
                return join_path(xdg, app_name);
            }
            WS_TRY_ASSIGN(const std::string home, home_based("config"));
            return join_path(join_path(home, ".config"), app_name);
        }
        case Location::data: {
            if (const char* xdg = xdg_root("XDG_DATA_HOME")) {
                return join_path(xdg, app_name);
            }
            WS_TRY_ASSIGN(const std::string home, home_based("data"));
            return join_path(join_path(home, ".local/share"), app_name);
        }
        case Location::cache: {
            // XDG's cache root is already the cache - the app nests
            // directly under it, not under a data/cache pair the way
            // the Windows KnownFolders shape forces.
            if (const char* xdg = xdg_root("XDG_CACHE_HOME")) {
                return join_path(xdg, app_name);
            }
            WS_TRY_ASSIGN(const std::string home, home_based("cache"));
            return join_path(join_path(home, ".cache"), app_name);
        }
        case Location::documents:
            // XDG defines no documents var (user dirs are a portal
            // concern there); the home directory is the honest
            // saved-files default a dev box has.
            return home_based("documents");
        case Location::temp: {
            const char* tmp = env_value("TMPDIR");
            return std::string{tmp != nullptr && is_absolute_path(tmp) ? tmp
                                                                       : "/tmp"};
        }
    }
    return fail(PlatformError::invalid_argument, "unknown location");
}

}  // namespace ws::platform
