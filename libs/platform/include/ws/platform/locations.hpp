#pragma once

#include <string>
#include <string_view>
#include <utility>

#include <ws/core/result.hpp>

namespace ws::platform {

// The five standard locations. config/data/cache are the app's own
// (private settings, private files, throwaway files); documents is the
// user's folder saved files default to; temp is the OS scratch space.
enum class Location {
    config,
    data,
    cache,
    documents,
    temp,
};

// Fully supplied roots, used exactly as given - the installer owns
// scoping and creation. Android's app TU builds these from the glue's
// activity paths (there is no headless way to compute them); tests
// build them from scratch dirs. An empty member answers
// `unavailable`, so a host can supply only the roots it has.
struct LocationRoots {
    std::string config;
    std::string data;
    std::string cache;
    std::string documents;
    std::string temp;
};

// One app's locations, caller-owned (the SettingsStore shape: no
// globals, one owner thread). Constructed with an app name it answers
// the OS truth for that app; constructed with roots it answers those
// instead.
class Locations {
  public:
    // The OS truth: config/data/cache under the OS's per-app areas
    // (suffixed with `app_name`), documents/temp user-wide.
    explicit Locations(std::string_view app_name);

    // Supplied roots, as given, winning over the OS truth.
    explicit Locations(LocationRoots roots);

    // The location's normalized path ('/'-canonical, no trailing
    // separator). Nothing is created - `ensure` does that.
    [[nodiscard]] ws::core::Result<std::string> get(Location which) const;

    // get() plus the directory existing afterwards (mkdir -p).
    ws::core::Status ensure(Location which) const;

    [[nodiscard]] const LocationRoots& roots() const { return roots_; }

  private:
    std::string app_;       // empty when roots were supplied
    LocationRoots roots_;   // a set member wins over the OS truth
};

// The raw OS truth behind the app-name constructor, public for hosts
// that want it without owning a Locations. Windows answers the
// KnownFolders (app-suffixed where the location is app-scoped);
// Android answers `unavailable` - the activity owns the package dirs,
// so the app TU installs a LocationRoots at startup instead.
[[nodiscard]] ws::core::Result<std::string> os_location(
    Location which, std::string_view app_name);

// mkdir -p over utf-8 narrow paths (the StdFileSystem char8_t
// routing): every missing parent is created; an existing directory is
// success. Any other blocker (a file where a parent should be) answers
// io_failed naming the path.
ws::core::Status ensure_directory(std::string_view path);

}  // namespace ws::platform
