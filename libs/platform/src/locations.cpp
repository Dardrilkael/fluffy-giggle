#include <ws/platform/locations.hpp>

#include <string>
#include <system_error>
#include <utility>

#include <ws/platform/errors.hpp>
#include <ws/platform/paths.hpp>

namespace ws::platform {
namespace {

[[nodiscard]] const std::string& root_for(Location which,
                                          const LocationRoots& roots) {
    switch (which) {
        case Location::config: return roots.config;
        case Location::data: return roots.data;
        case Location::cache: return roots.cache;
        case Location::documents: return roots.documents;
        case Location::temp: return roots.temp;
    }
    return roots.temp;  // unreachable; keeps the reference well-formed
}

[[nodiscard]] const char* location_name(Location which) {
    switch (which) {
        case Location::config: return "config";
        case Location::data: return "data";
        case Location::cache: return "cache";
        case Location::documents: return "documents";
        case Location::temp: return "temp";
    }
    return "location";
}

}  // namespace

Locations::Locations(std::string_view app_name) : app_{app_name} {}

Locations::Locations(LocationRoots roots) : roots_{std::move(roots)} {}

ws::core::Result<std::string> Locations::get(Location which) const {
    const std::string& supplied = root_for(which, roots_);
    if (!supplied.empty()) {
        return normalize_path(supplied);
    }
    if (!app_.empty()) {
        return os_location(which, app_);
    }
    return fail(PlatformError::unavailable,
                std::string{"no supplied root and no app name for the os "
                            "truth, asked for "} +
                    location_name(which));
}

ws::core::Status Locations::ensure(Location which) const {
    WS_TRY_ASSIGN(const std::string path, get(which));
    return ensure_directory(path);
}

}  // namespace ws::platform
