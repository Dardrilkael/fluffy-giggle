#include <ws/platform/locations.hpp>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include <ws/platform/errors.hpp>
#include <ws/platform/paths.hpp>

namespace ws::platform {
namespace {

// utf-8 narrow -> fs::path. Routed through char8_t so non-ascii
// paths survive Windows too (the StdFileSystem spelling - one routing
// rule across the workspace's file seams).
[[nodiscard]] std::filesystem::path to_fs_path(std::string_view path) {
    return std::filesystem::path{std::u8string{path.begin(), path.end()}};
}

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

ws::core::Status ensure_directory(std::string_view path) {
    if (path.empty()) {
        return fail(PlatformError::invalid_argument,
                    "ensure_directory: the path is empty");
    }
    std::error_code ec;
    std::filesystem::create_directories(to_fs_path(path), ec);
    if (ec) {
        return fail(PlatformError::io_failed,
                    "mkdir '" + std::string{path} + "': " + ec.message());
    }
    return {};
}

}  // namespace ws::platform
