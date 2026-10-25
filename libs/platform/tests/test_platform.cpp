// ws::platform's suite: the lexical path helpers, the Locations seam
// (supplied roots win, the OS-truth smoke) and ensure_directory on
// real scratch dirs under the OS temp.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <ws/platform/errors.hpp>
#include <ws/platform/locations.hpp>
#include <ws/platform/paths.hpp>

namespace {

// A fresh scratch dir under the OS temp, wiped first so every run
// starts clean (the settings file-suite precedent for real-disk cases).
[[nodiscard]] std::filesystem::path scratch(const char* tag) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                ("ws_platform_" + std::string{tag});
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
    return dir;
}

[[nodiscard]] std::string code_of(const ws::core::Error& error) {
    return std::string{error.category} + "/" +
           std::to_string(error.code);  // stable "platform/2" spelling
}

[[nodiscard]] bool is_unavailable(const ws::core::Error& error) {
    return error.category == std::string_view{"platform"} &&
           error.code ==
               static_cast<std::int32_t>(ws::platform::PlatformError::unavailable);
}

}  // namespace

TEST_CASE("join_path folds, and an absolute right side wins") {
    REQUIRE(ws::platform::join_path("a", "b") == "a/b");
    REQUIRE(ws::platform::join_path("a/", "b") == "a/b");
    REQUIRE(ws::platform::join_path("a//", "b") == "a/b");
    REQUIRE(ws::platform::join_path("/", "b") == "/b");
    REQUIRE(ws::platform::join_path("C:/x", "y\\z") == "C:/x/y/z");
    REQUIRE(ws::platform::join_path("a", "/b") == "/b");
    REQUIRE(ws::platform::join_path("a", "C:/b") == "C:/b");
    REQUIRE(ws::platform::join_path("", "b") == "b");
    REQUIRE(ws::platform::join_path("a", "") == "a");
}

TEST_CASE("normalize_path canonicalizes separators, dots and roots") {
    REQUIRE(ws::platform::normalize_path("") == "");
    REQUIRE(ws::platform::normalize_path("/") == "/");
    REQUIRE(ws::platform::normalize_path("//") == "//");
    REQUIRE(ws::platform::normalize_path("a\\b\\c") == "a/b/c");
    REQUIRE(ws::platform::normalize_path("a//b///c") == "a/b/c");
    REQUIRE(ws::platform::normalize_path("a/./b") == "a/b");
    REQUIRE(ws::platform::normalize_path("a/b/") == "a/b");
    REQUIRE(ws::platform::normalize_path("a/b/../c") == "a/c");
    REQUIRE(ws::platform::normalize_path("/a/b/..") == "/a");
    REQUIRE(ws::platform::normalize_path("/../a") == "/a");  // never past the root
    REQUIRE(ws::platform::normalize_path("a/../..") == "..");  // relative climb stays
    REQUIRE(ws::platform::normalize_path("..") == "..");
    REQUIRE(ws::platform::normalize_path("C:\\a\\..\\b") == "C:/b");
    REQUIRE(ws::platform::normalize_path("C:/a/b/..") == "C:/a");
    REQUIRE(ws::platform::normalize_path("C:") == "C:");
    REQUIRE(ws::platform::normalize_path("C:x/./y") == "C:x/y");  // drive-relative stays
    REQUIRE(ws::platform::normalize_path("//srv/share/x//y") == "//srv/share/x/y");
}

TEST_CASE("is_absolute_path asks for a root or a drive root") {
    REQUIRE(ws::platform::is_absolute_path("/a"));
    REQUIRE(ws::platform::is_absolute_path("//a"));
    REQUIRE(ws::platform::is_absolute_path("\\\\a"));
    REQUIRE(ws::platform::is_absolute_path("C:/a"));
    REQUIRE_FALSE(ws::platform::is_absolute_path("C:a"));
    REQUIRE_FALSE(ws::platform::is_absolute_path("a/b"));
    REQUIRE_FALSE(ws::platform::is_absolute_path(""));
}

TEST_CASE("name_of and parent_of take the last segment") {
    REQUIRE(ws::platform::name_of("a/b/c.txt") == "c.txt");
    REQUIRE(ws::platform::name_of("a/b/") == "b");
    REQUIRE(ws::platform::name_of("C:/a") == "a");
    REQUIRE(ws::platform::name_of("/") == "");
    REQUIRE(ws::platform::name_of("") == "");
    REQUIRE(ws::platform::parent_of("a/b/c") == "a/b");
    REQUIRE(ws::platform::parent_of("a//b") == "a");
    REQUIRE(ws::platform::parent_of("/a") == "/");
    REQUIRE(ws::platform::parent_of("C:/a") == "C:/");
    REQUIRE(ws::platform::parent_of("//srv/x") == "//srv");
    REQUIRE(ws::platform::parent_of("a") == "");
    REQUIRE(ws::platform::parent_of("/") == "");
    REQUIRE(ws::platform::parent_of("") == "");
}

TEST_CASE("Locations answers supplied roots as given, and only those") {
    ws::platform::LocationRoots roots;
    roots.config = "C:\\scratch\\cfg//";   // normalized on the way out
    roots.temp = "/tmp/ws";
    ws::platform::Locations locs{std::move(roots)};
    REQUIRE(locs.get(ws::platform::Location::config).value() == "C:/scratch/cfg");
    REQUIRE(locs.get(ws::platform::Location::temp).value() == "/tmp/ws");
    REQUIRE(locs.roots().config == "C:\\scratch\\cfg//");  // stored verbatim

    const auto missing = locs.get(ws::platform::Location::documents);
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(is_unavailable(missing.error()));
}

TEST_CASE("Locations::ensure creates the whole chain, idempotently") {
    const std::filesystem::path base = scratch("ensure");
    const std::string deep =
        ws::platform::join_path(base.generic_string(), "a/b/c");
    ws::platform::LocationRoots roots;
    roots.config = deep;
    ws::platform::Locations locs{std::move(roots)};
    REQUIRE(locs.ensure(ws::platform::Location::config));
    REQUIRE(std::filesystem::is_directory(std::filesystem::path{deep}));
    REQUIRE(locs.ensure(ws::platform::Location::config));  // already there
    std::error_code ignored;
    std::filesystem::remove_all(base, ignored);
}

TEST_CASE("ensure_directory answers io_failed when a file blocks the chain") {
    const std::filesystem::path base = scratch("ensure_blocker");
    std::filesystem::create_directories(base);
    const std::string blocker = (base / "blocker").generic_string();
    {
        std::ofstream out{blocker, std::ios::binary | std::ios::trunc};
        out << "a file where a parent should be";
    }
    const ws::core::Status made =
        ws::platform::ensure_directory(ws::platform::join_path(blocker, "x"));
    REQUIRE_FALSE(made.has_value());
    REQUIRE(code_of(made.error()) == "platform/1");
    std::error_code ignored;
    std::filesystem::remove_all(base, ignored);
}

TEST_CASE("os_location answers the Windows truth for an app") {
    const auto cfg =
        ws::platform::os_location(ws::platform::Location::config, "ws-app");
    REQUIRE(cfg.has_value());
    REQUIRE(ws::platform::is_absolute_path(cfg.value()));
    REQUIRE(cfg.value().find("/ws-app") != std::string::npos);

    const auto data =
        ws::platform::os_location(ws::platform::Location::data, "ws-app");
    REQUIRE(data.has_value());
    const auto cache =
        ws::platform::os_location(ws::platform::Location::cache, "ws-app");
    REQUIRE(cache.has_value());
    REQUIRE(cache.value() == ws::platform::join_path(data.value(), "cache"));

    REQUIRE(ws::platform::is_absolute_path(
        ws::platform::os_location(ws::platform::Location::temp, "ws-app").value()));
    REQUIRE(ws::platform::is_absolute_path(
        ws::platform::os_location(ws::platform::Location::documents, "ws-app").value()));

    const auto refused = ws::platform::os_location(
        ws::platform::Location::config, std::string_view{""});
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(code_of(refused.error()) == "platform/6");

    // The app-name constructor rides the same truth.
    ws::platform::Locations locs{"ws-app"};
    REQUIRE(locs.get(ws::platform::Location::config).value() == cfg.value());
    REQUIRE(locs.get(ws::platform::Location::cache).value() == cache.value());
}
