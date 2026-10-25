// ws::platform's suite: the lexical path helpers, the Locations seam
// (supplied roots win, the OS-truth smoke) and ensure_directory on
// real scratch dirs under the OS temp.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
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
#include <ws/platform/watch.hpp>

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

// The watch's test seam: presence and stamp driven by hand, no disk.
class FakeWatchSource final : public ws::platform::WatchSource {
  public:
    bool present = false;
    std::int64_t mtime_ms = 0;

    [[nodiscard]] ws::core::Result<std::int64_t> modified_ms(
        std::string_view) const override {
        if (!present) {
            return ws::platform::fail(ws::platform::PlatformError::io_failed);
        }
        return mtime_ms;
    }
    [[nodiscard]] bool exists(std::string_view) const override {
        return present;
    }
};

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

TEST_CASE("the watch fires a settled change once, after quiet") {
    FakeWatchSource source;
    source.present = true;
    source.mtime_ms = 1000;
    std::int64_t now = 0;
    ws::platform::FileWatch watch{source, "tracked.json",
                                  [&now] { return now; }, 400};
    REQUIRE(watch.poll() == ws::platform::FileEvent::none);

    now = 100;
    source.mtime_ms = 2000;
    REQUIRE(watch.poll() == ws::platform::FileEvent::none);  // just observed
    now = 300;
    REQUIRE(watch.poll() == ws::platform::FileEvent::none);  // 200ms, not 400
    now = 501;
    REQUIRE(watch.poll() == ws::platform::FileEvent::modified);  // 401ms quiet
    now = 600;
    REQUIRE(watch.poll() == ws::platform::FileEvent::none);  // once only
}

TEST_CASE("a still-moving writer slides the deadline, never stacks") {
    FakeWatchSource source;
    source.present = true;
    source.mtime_ms = 1000;
    std::int64_t now = 0;
    ws::platform::FileWatch watch{source, "tracked.json",
                                  [&now] { return now; }, 400};

    now = 10;
    source.mtime_ms = 2000;
    REQUIRE(watch.poll() == ws::platform::FileEvent::none);
    now = 110;
    source.mtime_ms = 3000;  // still writing: the deadline slides to 110
    REQUIRE(watch.poll() == ws::platform::FileEvent::none);
    now = 310;
    source.mtime_ms = 4000;  // and again, to 310
    REQUIRE(watch.poll() == ws::platform::FileEvent::none);
    now = 500;
    REQUIRE(watch.poll() == ws::platform::FileEvent::none);  // 190ms since
    now = 711;
    REQUIRE(watch.poll() == ws::platform::FileEvent::modified);  // 401ms since
    now = 800;
    REQUIRE(watch.poll() == ws::platform::FileEvent::none);
}

TEST_CASE("a removal fires; a fast reappear merges into modified") {
    FakeWatchSource source;
    source.present = true;
    source.mtime_ms = 1000;
    std::int64_t now = 0;
    ws::platform::FileWatch watch{source, "tracked.json",
                                  [&now] { return now; }, 400};

    now = 100;
    source.present = false;  // gone at 100
    REQUIRE(watch.poll() == ws::platform::FileEvent::none);
    now = 300;
    REQUIRE(watch.poll() == ws::platform::FileEvent::none);
    now = 501;
    REQUIRE(watch.poll() == ws::platform::FileEvent::removed);  // stayed gone

    now = 600;
    source.present = true;  // back with new content
    source.mtime_ms = 5000;
    REQUIRE(watch.poll() == ws::platform::FileEvent::none);  // observed at 600
    now = 1001;
    REQUIRE(watch.poll() == ws::platform::FileEvent::modified);

    // The interim case: gone at 100, back at 200 - one modified, and
    // the removal never fires.
    FakeWatchSource quick;
    quick.present = false;  // missing at construction
    now = 0;
    ws::platform::FileWatch merges{quick, "quick.json",
                                   [&now] { return now; }, 400};
    quick.present = true;
    quick.mtime_ms = 900;
    now = 100;
    REQUIRE(merges.poll() == ws::platform::FileEvent::none);  // observed at 100
    now = 501;
    REQUIRE(merges.poll() == ws::platform::FileEvent::modified);
}

TEST_CASE("arm re-baselines and swallows the pending event") {
    FakeWatchSource source;
    source.present = true;
    source.mtime_ms = 1000;
    std::int64_t now = 0;
    ws::platform::FileWatch watch{source, "tracked.json",
                                  [&now] { return now; }, 400};
    now = 100;
    source.mtime_ms = 2000;
    REQUIRE(watch.poll() == ws::platform::FileEvent::none);  // pending

    watch.arm();  // the load's own write: baseline moves to 2000
    now = 1000;
    REQUIRE(watch.poll() == ws::platform::FileEvent::none);  // swallowed
    source.mtime_ms = 3000;  // a real later change still lands
    REQUIRE(watch.poll() == ws::platform::FileEvent::none);
    now = 1401;
    REQUIRE(watch.poll() == ws::platform::FileEvent::modified);
}

TEST_CASE("a missing file's first appearance fires modified") {
    FakeWatchSource source;
    source.present = false;  // not there at construction
    std::int64_t now = 0;
    ws::platform::FileWatch watch{source, "created.json",
                                  [&now] { return now; }, 400};
    REQUIRE(watch.poll() == ws::platform::FileEvent::none);

    now = 10;
    source.present = true;
    source.mtime_ms = 700;
    REQUIRE(watch.poll() == ws::platform::FileEvent::none);  // observed at 10
    now = 411;
    REQUIRE(watch.poll() == ws::platform::FileEvent::modified);
}

TEST_CASE("StdWatchSource watches a real file's stamps") {
    const std::filesystem::path base = scratch("watch");
    std::filesystem::create_directories(base);
    const std::string file = (base / "tracked.txt").generic_string();
    {
        std::ofstream out{file, std::ios::binary | std::ios::trunc};
        out << "v1";
    }

    ws::platform::StdWatchSource source;
    // quiet 0: any settled change is due the same poll, so no sleeps -
    // the stamps are set explicitly, not raced against the clock.
    ws::platform::FileWatch watch{source, file,
                                  ws::platform::FileWatch::Clock{}, 0};
    REQUIRE(watch.poll() == ws::platform::FileEvent::none);

    const auto old_time =
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(2);
    const auto new_time = old_time + std::chrono::hours(1);
    std::filesystem::last_write_time(std::filesystem::path{file}, old_time);
    watch.arm();  // baseline: old_time
    std::filesystem::last_write_time(std::filesystem::path{file}, new_time);
    REQUIRE(watch.poll() == ws::platform::FileEvent::modified);
    REQUIRE(watch.poll() == ws::platform::FileEvent::none);

    std::error_code ignored;
    std::filesystem::remove(file, ignored);
    REQUIRE(watch.poll() == ws::platform::FileEvent::removed);
    std::filesystem::remove_all(base, ignored);
}
