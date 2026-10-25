// ws::platform's suite: the lexical path helpers, the Locations seam
// (supplied roots win, the OS-truth smoke), ensure_directory on real
// scratch dirs under the OS temp, the debounced watch on a fake
// clock, the recent-files MRU over a FileSystem fake, the autosave
// crash-recovery state machine over a fake disk, the file dialogs'
// scripted fake + the refusal, and the message boxes' fake + the
// refusal (the clipboard has no case on purpose: a test that
// clobbers the user's clipboard would be a bad citizen - the
// gallery round-trips it interactively instead).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <ws/platform/autosave.hpp>
#include <ws/platform/dialogs.hpp>
#include <ws/platform/errors.hpp>
#include <ws/platform/locations.hpp>
#include <ws/platform/messagebox.hpp>
#include <ws/platform/paths.hpp>
#include <ws/platform/recent.hpp>
#include <ws/platform/watch.hpp>
#include <ws/settings/errors.hpp>
#include <ws/settings/json.hpp>

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

// ---- recent files (the MRU, its pins and its persistence) ----

// The FileSystem fake: a map of files, everything existing unless the
// case denies a path (the prune cases), and a write-failure countdown.
class FakeFs final : public ws::settings::FileSystem {
  public:
    std::map<std::string, std::string> files;
    std::set<std::string> missing;
    int fail_writes = 0;

    [[nodiscard]] ws::core::Result<std::string> read(
        std::string_view path) const override {
        const auto it = files.find(std::string{path});
        if (it == files.end()) {
            return ws::settings::fail(ws::settings::SettingsError::io_failed,
                                      "read: missing");
        }
        return it->second;
    }
    ws::core::Status write(std::string_view path,
                           std::string_view bytes) override {
        if (fail_writes > 0) {
            --fail_writes;
            return ws::settings::fail(ws::settings::SettingsError::io_failed,
                                      "write refused");
        }
        files[std::string{path}] = std::string{bytes};
        return {};
    }
    ws::core::Status rename(std::string_view from,
                            std::string_view to) override {
        const auto it = files.find(std::string{from});
        if (it == files.end()) {
            return ws::settings::fail(ws::settings::SettingsError::io_failed,
                                      "rename: missing");
        }
        files[std::string{to}] = it->second;
        files.erase(it);
        return {};
    }
    bool remove(std::string_view path) override {
        files.erase(std::string{path});
        return true;  // the seam's contract: gone afterwards - removed
                      // or never there - and the fake never gets stuck
    }
    [[nodiscard]] bool exists(std::string_view path) const override {
        return !missing.contains(std::string{path});
    }
};

TEST_CASE("the MRU orders, dedupes and caps") {
    FakeFs fs;
    ws::platform::RecentFiles recent{fs, "recent.json", 3};
    REQUIRE(recent.touch("C:/docs/a.txt"));
    REQUIRE(recent.touch("C:/docs/b.txt"));
    REQUIRE(recent.touch("C:/docs/c.txt"));
    auto items = recent.menu_items();
    REQUIRE(items.size() == 3);
    REQUIRE(items[0].path == "C:/docs/c.txt");
    REQUIRE(items[1].path == "C:/docs/b.txt");
    REQUIRE(items[2].path == "C:/docs/a.txt");

    // a re-touch promotes, never duplicates
    REQUIRE(recent.touch("C:/docs/a.txt"));
    items = recent.menu_items();
    REQUIRE(items.size() == 3);
    REQUIRE(items[0].path == "C:/docs/a.txt");

    // two spellings of one file are one entry (the dedupe normalizes)
    REQUIRE(recent.touch("C:\\docs\\b.txt"));
    items = recent.menu_items();
    REQUIRE(items.size() == 3);
    REQUIRE(items[0].path == "C:/docs/b.txt");

    // over the cap: the oldest unpinned tail entry falls off. The
    // dedupe promotion left c at the tail, so c goes - not a.
    REQUIRE(recent.touch("C:/docs/d.txt"));
    items = recent.menu_items();
    REQUIRE(items.size() == 3);
    REQUIRE(items[0].path == "C:/docs/d.txt");
    REQUIRE(items[1].path == "C:/docs/b.txt");
    REQUIRE(items[2].path == "C:/docs/a.txt");
}

TEST_CASE("pins survive the cap, the prune and a re-touch") {
    FakeFs fs;
    ws::platform::RecentFiles recent{fs, "recent.json", 3};
    REQUIRE(recent.touch("one.txt"));
    REQUIRE(recent.touch("two.txt"));
    REQUIRE(recent.touch("three.txt"));
    REQUIRE(recent.pin("", "three.txt"));

    // three is pinned: the eviction picks the oldest UNPINNED (one)
    REQUIRE(recent.touch("four.txt"));
    auto items = recent.menu_items();
    REQUIRE(items.size() == 3);
    REQUIRE(items[0].path == "four.txt");
    REQUIRE(items[1].path == "three.txt");  // still here, still pinned
    REQUIRE(items[1].pinned);

    // a re-touch keeps the pin
    REQUIRE(recent.touch("three.txt"));
    REQUIRE(recent.menu_items()[0].pinned);

    // the missing file prunes unpinned entries; a pinned one stays
    fs.missing.insert("three.txt");
    fs.missing.insert("four.txt");
    fs.missing.insert("two.txt");
    REQUIRE(recent.pin("", "two.txt"));  // pin it before it vanishes
    items = recent.menu_items();
    REQUIRE(items.size() == 2);           // four pruned: missing, unpinned
    REQUIRE(items[0].path == "three.txt");  // pinned, missing, kept
    REQUIRE(items[1].path == "two.txt");  // pinned before vanishing: kept
    REQUIRE(items[1].pinned);

    // an all-pinned full list refuses a new touch, untouched
    REQUIRE(recent.unpin("", "three.txt"));
    REQUIRE(recent.touch("five.txt"));
    REQUIRE(recent.touch("six.txt"));
    REQUIRE(recent.touch("seven.txt"));
    // the cap evicted five and three on the way; pin what survived
    REQUIRE(recent.pin("", "seven.txt"));
    REQUIRE(recent.pin("", "six.txt"));
    const std::size_t before = recent.menu_items().size();
    REQUIRE_FALSE(recent.touch("eight.txt"));
    REQUIRE(recent.menu_items().size() == before);
}

TEST_CASE("menu labels disambiguate same-named siblings") {
    FakeFs fs;
    ws::platform::RecentFiles recent{fs, "recent.json", 10};
    REQUIRE(recent.touch("C:/a/report.txt"));
    REQUIRE(recent.touch("C:/b/report.txt"));
    REQUIRE(recent.touch("D:/c/other.txt"));
    const auto items = recent.menu_items();
    REQUIRE(items.size() == 3);
    REQUIRE(items[0].label == "other.txt");
    // the later-touched sibling keeps the bare name; the older one
    // disambiguates with its parent
    REQUIRE(items[1].label == "report.txt");
    REQUIRE(items[1].path == "C:/b/report.txt");
    REQUIRE(items[2].label == "report.txt (a)");
    REQUIRE(items[2].path == "C:/a/report.txt");
}

TEST_CASE("the json round trip carries the types and the pins") {
    FakeFs fs;
    {
        ws::platform::RecentFiles recent{fs, "recent.json", 10};
        REQUIRE(recent.touch("C:/docs/one.txt"));
        REQUIRE(recent.touch("scene", "C:/maps/valley.scene"));
        REQUIRE(recent.touch("C:/docs/two.txt"));
        REQUIRE(recent.pin("", "C:/docs/one.txt"));
        REQUIRE(recent.save());
    }
    // the saved shape: version 1, the lists object, MRU order kept
    const std::string saved = fs.files.at("recent.json");
    const auto parsed = ws::settings::json::parse(saved);
    REQUIRE(parsed.has_value());
    const auto* version = parsed->as_object()->find("version");
    REQUIRE(version != nullptr);
    const auto shape = version->get_integer();
    REQUIRE(shape.has_value());
    REQUIRE(*shape == 1);
    const auto* lists = parsed->as_object()->find("lists");
    REQUIRE(lists != nullptr);
    REQUIRE(lists->as_object()->find("") != nullptr);
    REQUIRE(lists->as_object()->find("scene") != nullptr);

    {
        ws::platform::RecentFiles back{fs, "recent.json", 10};
        REQUIRE(back.load());
        const auto whole = back.menu_items();
        REQUIRE(whole.size() == 2);
        REQUIRE(whole[0].path == "C:/docs/two.txt");
        REQUIRE_FALSE(whole[0].pinned);
        REQUIRE(whole[1].path == "C:/docs/one.txt");
        REQUIRE(whole[1].pinned);
        const auto scenes = back.menu_items("scene");
        REQUIRE(scenes.size() == 1);
        REQUIRE(scenes[0].path == "C:/maps/valley.scene");
    }
}

TEST_CASE("a corrupt file answers parse_failed and loads nothing") {
    FakeFs fs;
    fs.files["recent.json"] = "{ not json ]";
    ws::platform::RecentFiles recent{fs, "recent.json", 10};
    REQUIRE(recent.touch("keep.txt"));
    const auto loaded = recent.load();
    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(code_of(loaded.error()) == "platform/4");
    // the load is all-or-nothing: memory kept what it had
    REQUIRE(recent.menu_items().size() == 1);
    REQUIRE(recent.menu_items()[0].path == "keep.txt");

    // a newer shape is refused, never downgraded
    fs.files["recent.json"] = R"({"version": 2, "lists": {}})";
    REQUIRE_FALSE(recent.load());
    REQUIRE(code_of(recent.load().error()) == "platform/4");
}

TEST_CASE("a missing store is the first run") {
    FakeFs fs;
    fs.missing.insert("recent.json");  // the fake's "not there"
    ws::platform::RecentFiles recent{fs, "recent.json", 10};
    REQUIRE(recent.load());
    REQUIRE(recent.menu_items().empty());
}

TEST_CASE("a failed save leaves the previous file intact") {
    FakeFs fs;
    ws::platform::RecentFiles recent{fs, "recent.json", 10};
    REQUIRE(recent.touch("a.txt"));
    REQUIRE(recent.save());
    const std::string before = fs.files.at("recent.json");

    REQUIRE(recent.touch("b.txt"));
    fs.fail_writes = 1;
    REQUIRE_FALSE(recent.save());
    REQUIRE(fs.files.at("recent.json") == before);  // byte-identical

    fs.fail_writes = 0;
    REQUIRE(recent.save());
    ws::platform::RecentFiles back{fs, "recent.json", 10};
    REQUIRE(back.load());
    const auto items = back.menu_items();
    REQUIRE(items.size() == 2);
    REQUIRE(items[0].path == "b.txt");
    REQUIRE(items[1].path == "a.txt");
}

TEST_CASE("RecentFiles round trips through the real seam") {
    const std::filesystem::path base = scratch("recent");
    std::filesystem::create_directories(base);
    const std::string store = (base / "recent.json").generic_string();
    const std::string one = (base / "one.txt").generic_string();
    const std::string scene_file = (base / "valley.scene").generic_string();
    {
        std::ofstream out{one, std::ios::binary | std::ios::trunc};
        out << "one";
        std::ofstream scenes{scene_file, std::ios::binary | std::ios::trunc};
        scenes << "valley";
    }

    ws::settings::StdFileSystem fs;
    ws::platform::RecentFiles recent{fs, store, 5};
    REQUIRE(recent.load());  // the first run
    REQUIRE(recent.touch(one));
    REQUIRE(recent.touch("scene", scene_file));
    REQUIRE(recent.pin("", one));
    REQUIRE(recent.save());

    ws::platform::RecentFiles back{fs, store, 5};
    REQUIRE(back.load());
    const auto whole = back.menu_items();
    REQUIRE(whole.size() == 1);
    REQUIRE(whole[0].path == one);
    REQUIRE(whole[0].pinned);

    // the file gone from disk: the unpinned entry prunes lazily
    std::error_code ignored;
    std::filesystem::remove(scene_file, ignored);
    REQUIRE(back.menu_items("scene").empty());
    // the pinned missing file stays
    std::filesystem::remove(one, ignored);
    REQUIRE(back.menu_items().size() == 1);
    std::filesystem::remove_all(base, ignored);
}

// ---- autosave: the crash-recovery state machine ----

// The autosave suite's fake: an honest disk - only what was written
// exists - plus the write-failure countdown. (FakeFs models the MRU's
// world, where touched paths exist without being written.)
class DiskFs final : public ws::settings::FileSystem {
  public:
    std::map<std::string, std::string> files;
    int fail_writes = 0;

    [[nodiscard]] ws::core::Result<std::string> read(
        std::string_view path) const override {
        const auto it = files.find(std::string{path});
        if (it == files.end()) {
            return ws::settings::fail(ws::settings::SettingsError::io_failed,
                                      "read: missing");
        }
        return it->second;
    }
    ws::core::Status write(std::string_view path,
                           std::string_view bytes) override {
        if (fail_writes > 0) {
            --fail_writes;
            return ws::settings::fail(ws::settings::SettingsError::io_failed,
                                      "write refused");
        }
        files[std::string{path}] = std::string{bytes};
        return {};
    }
    ws::core::Status rename(std::string_view from,
                            std::string_view to) override {
        const auto it = files.find(std::string{from});
        if (it == files.end()) {
            return ws::settings::fail(ws::settings::SettingsError::io_failed,
                                      "rename: missing");
        }
        files[std::string{to}] = it->second;
        files.erase(it);
        return {};
    }
    bool remove(std::string_view path) override {
        files.erase(std::string{path});
        return true;  // gone afterwards - removed or never there
    }
    [[nodiscard]] bool exists(std::string_view path) const override {
        return files.contains(std::string{path});
    }
};

// The fake-disk cases never touch the real filesystem: snapshot's
// store-dir mkdir -p is injectable (DirEnsurer, the WatchSource
// precedent for the half the FileSystem seam leaves out). Only the
// snapshot-calling constructions need it - probe/recover/discard
// never make a directory.
const ws::platform::Autosave::DirEnsurer no_ensure =
    [](std::string_view) { return ws::core::Status{}; };

TEST_CASE("the autosave clean cycle: snapshot, finish, nothing claimed") {
    DiskFs fs;
    ws::platform::Autosave autosave{fs, "C:/docs/report.txt", "C:/store",
                                    [] { return std::int64_t{1000}; },
                                    no_ensure};
    const auto fresh = autosave.probe();
    REQUIRE(fresh.has_value());
    REQUIRE_FALSE(fresh->has_value());

    REQUIRE(autosave.snapshot("draft v1"));

    // the marker on disk IS the crash evidence: a fresh instance - the
    // next session - sees a session that never finished
    ws::platform::Autosave next{fs, "C:/docs/report.txt", "C:/store",
                                 [] { return std::int64_t{9000}; }};
    const auto found = next.probe();
    REQUIRE(found.has_value());
    REQUIRE(found->has_value());
    const ws::platform::Recovery& recovery = **found;
    REQUIRE(recovery.document == "C:/docs/report.txt");
    REQUIRE(recovery.autosave == autosave.autosave_path());
    REQUIRE(recovery.saved_ms == 1000);

    // the clean exit clears both, and probe agrees
    REQUIRE(autosave.finish());
    REQUIRE(fs.files.empty());
    const auto after = next.probe();
    REQUIRE(after.has_value());
    REQUIRE_FALSE(after->has_value());
}

TEST_CASE("the crash + recover cycle hands the content back") {
    DiskFs fs;
    {
        ws::platform::Autosave crashed{fs, "C:/docs/report.txt", "C:/store",
                                       [] { return std::int64_t{1000}; },
                                       no_ensure};
        REQUIRE(crashed.snapshot("the draft"));
        // no finish() - the session dies with the files in place
    }
    ws::platform::Autosave next{fs, "C:/docs/report.txt", "C:/store",
                                [] { return std::int64_t{9000}; }};
    REQUIRE(next.probe()->has_value());

    const auto bytes = next.recover();
    REQUIRE(bytes.has_value());
    REQUIRE(*bytes == "the draft");
    REQUIRE(fs.files.empty());               // the pair cleared
    REQUIRE_FALSE(next.probe()->has_value());  // and nothing claimed since

    // recover again: nothing left
    const auto again = next.recover();
    REQUIRE_FALSE(again.has_value());
    REQUIRE(code_of(again.error()) == "platform/6");
}

TEST_CASE("the crash + discard cycle drops the content") {
    DiskFs fs;
    ws::platform::Autosave autosave{fs, "doc.txt", "store",
                                    [] { return std::int64_t{100}; },
                                    no_ensure};
    REQUIRE(autosave.snapshot("draft"));
    ws::platform::Autosave next{fs, "doc.txt", "store", {}};
    REQUIRE(next.probe()->has_value());
    REQUIRE(next.discard());
    REQUIRE(fs.files.empty());
    REQUIRE_FALSE(next.probe()->has_value());
}

TEST_CASE("a failed snapshot never promises a recovery") {
    DiskFs fs;
    ws::platform::Autosave autosave{fs, "doc.txt", "store",
                                    [] { return std::int64_t{100}; },
                                    no_ensure};
    REQUIRE(autosave.snapshot("v1"));

    fs.fail_writes = 1;  // the content write is refused
    REQUIRE_FALSE(autosave.snapshot("v2"));
    // the pair on disk is exactly the v1 pair
    REQUIRE(fs.files.at(autosave.autosave_path()) == "v1");
    const auto found = autosave.probe();
    REQUIRE(found.has_value());
    REQUIRE(found->has_value());
    REQUIRE((**found).saved_ms == 100);  // the marker still claims v1

    fs.fail_writes = 0;
    REQUIRE(autosave.snapshot("v2"));  // the next snapshot lands
    REQUIRE(fs.files.at(autosave.autosave_path()) == "v2");
    REQUIRE(autosave.finish());
}

TEST_CASE("recover requires content; an orphan is recoverable") {
    DiskFs fs;
    ws::platform::Autosave autosave{fs, "doc.txt", "store", {}};
    const auto nothing = autosave.recover();
    REQUIRE_FALSE(nothing.has_value());
    REQUIRE(code_of(nothing.error()) == "platform/6");

    // an orphan: content without a marker - the crash window between
    // the two writes - is recoverable by construction
    fs.files[autosave.autosave_path()] = "orphaned";
    const auto bytes = autosave.recover();
    REQUIRE(bytes.has_value());
    REQUIRE(*bytes == "orphaned");
    REQUIRE(fs.files.empty());
}

TEST_CASE("a corrupt marker answers corrupt_file; discard still cleans") {
    DiskFs fs;
    ws::platform::Autosave autosave{fs, "doc.txt", "store", {}, no_ensure};
    REQUIRE(autosave.snapshot("v1"));

    fs.files[autosave.marker_path()] = "{ not json ]";
    const auto found = autosave.probe();
    REQUIRE_FALSE(found.has_value());
    REQUIRE(code_of(found.error()) == "platform/5");
    REQUIRE(autosave.discard());  // cleanup never needs the marker parsed
    REQUIRE(fs.files.empty());

    // a future shape is refused, never downgraded
    fs.files[autosave.marker_path()] = R"({"version": 2})";
    REQUIRE(code_of(autosave.probe().error()) == "platform/5");
    // a marker missing its fields is refused
    fs.files[autosave.marker_path()] =
        R"({"version": 1, "document": "x"})";
    REQUIRE(code_of(autosave.probe().error()) == "platform/5");
    REQUIRE(autosave.discard());
}

TEST_CASE("the derived paths collide never and read as the document") {
    DiskFs fs;
    ws::platform::Autosave a{fs, "C:/docs/report.txt", "C:/store", {}};
    ws::platform::Autosave b{fs, "D:/other/report.txt", "C:/store", {}};
    REQUIRE(a.document() == "C:/docs/report.txt");  // normalized
    REQUIRE(a.autosave_path() != b.autosave_path());
    REQUIRE(a.marker_path() != b.marker_path());
    // the doc's name leads (extension included), the store dir
    // prefixes, the hash separates
    REQUIRE(a.autosave_path().starts_with("C:/store/report.txt-"));
    REQUIRE(a.autosave_path().ends_with(".autosave"));
    REQUIRE(a.marker_path().ends_with(".marker"));
    // two spellings of one document derive one path family
    ws::platform::Autosave c{fs, "C:\\docs\\report.txt", "C:/store", {}};
    REQUIRE(c.autosave_path() == a.autosave_path());
    REQUIRE(c.marker_path() == a.marker_path());
}

TEST_CASE("Autosave round trips through the real seam") {
    const std::filesystem::path base = scratch("autosave");
    const std::string doc = (base / "report.txt").generic_string();
    // the store dir does not exist yet - snapshot's mkdir -p makes it
    const std::string store = (base / "cfg" / "autosave").generic_string();
    {
        ws::settings::StdFileSystem fs;
        ws::platform::Autosave session{fs, doc, store,
                                       [] { return std::int64_t{1000}; }};
        REQUIRE_FALSE(session.probe()->has_value());
        REQUIRE(session.snapshot("real draft"));
        REQUIRE(fs.exists(session.autosave_path()));
        REQUIRE(fs.exists(session.marker_path()));
    }
    {
        ws::settings::StdFileSystem fs;
        ws::platform::Autosave next{fs, doc, store, {}};
        const auto found = next.probe();
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        REQUIRE((**found).saved_ms == 1000);
        const auto bytes = next.recover();
        REQUIRE(bytes.has_value());
        REQUIRE(*bytes == "real draft");
        REQUIRE_FALSE(next.probe()->has_value());
    }
    std::error_code ignored;
    std::filesystem::remove_all(base, ignored);
}

// ---- file dialogs: the scripted fake + the refusal ----

TEST_CASE("the fake dialogs play the scripted answers") {
    ws::platform::FakeFileDialogs scripted;
    ws::platform::FileDialogs& dialogs = scripted;

    ws::platform::FileDialogOptions options;
    options.title = "pick";
    options.default_path = "C:/docs";
    options.filters.push_back({"Text", "*.txt"});
    options.filters.push_back({"All", "*.*"});

    // nothing staged: every kind answers a dismissed dialog
    const auto no_open = dialogs.open_file(options);
    REQUIRE(no_open.has_value());
    REQUIRE_FALSE(no_open->has_value());
    const auto no_files = dialogs.open_files(options);
    REQUIRE(no_files.has_value());
    REQUIRE(no_files->empty());
    const auto no_save = dialogs.save_file(options);
    REQUIRE(no_save.has_value());
    REQUIRE_FALSE(no_save->has_value());
    const auto no_folder = dialogs.select_folder(options);
    REQUIRE(no_folder.has_value());
    REQUIRE_FALSE(no_folder->has_value());

    // scripted answers pop in order; a nullopt entry is a dismissal
    scripted.open_queue = {"C:/docs/a.txt", std::nullopt};
    REQUIRE(dialogs.open_file(options)->value_or("") == "C:/docs/a.txt");
    const auto dismissed = dialogs.open_file(options);
    REQUIRE(dismissed.has_value());
    REQUIRE_FALSE(dismissed->has_value());
    // and the queue stays empty: dismissed from here on
    REQUIRE_FALSE(dialogs.open_file(options)->has_value());

    scripted.files_queue = {{"C:/docs/a.txt", "C:/docs/b.txt"}};
    const auto picked = dialogs.open_files(options);
    REQUIRE(picked.has_value());
    REQUIRE(picked->size() == 2);
    REQUIRE((*picked)[1] == "C:/docs/b.txt");

    scripted.save_queue = {"C:/docs/report.txt"};
    REQUIRE(dialogs.save_file(options)->value_or("") ==
            "C:/docs/report.txt");
    scripted.folder_queue = {"C:/docs"};
    REQUIRE(dialogs.select_folder(options)->value_or("") == "C:/docs");

    // every call recorded its options verbatim
    REQUIRE(scripted.open_calls.size() == 4);
    REQUIRE(scripted.files_calls.size() == 2);
    REQUIRE(scripted.save_calls.size() == 2);
    REQUIRE(scripted.folder_calls.size() == 2);
    const ws::platform::FileDialogOptions& recorded = scripted.open_calls[0];
    REQUIRE(recorded.title == "pick");
    REQUIRE(recorded.default_path == "C:/docs");
    REQUIRE(recorded.filters.size() == 2);
    REQUIRE(recorded.filters[0].pattern == "*.txt");
    REQUIRE(recorded.filters[1].pattern == "*.*");
}

TEST_CASE("the unavailable dialog set refuses every kind") {
    ws::platform::UnavailableFileDialogs dialogs;

    const auto opened = dialogs.open_file({});
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(code_of(opened.error()) == "platform/2");
    REQUIRE(code_of(dialogs.open_files({}).error()) == "platform/2");
    REQUIRE(code_of(dialogs.save_file({}).error()) == "platform/2");
    REQUIRE(code_of(dialogs.select_folder({}).error()) == "platform/2");
}

TEST_CASE("make_file_dialogs hands back the native set") {
    // Constructed and destroyed only: a real call raises a modal
    // dialog and would hang the suite - the native set is exercised
    // interactively by the gallery (phase 7).
    const std::unique_ptr<ws::platform::FileDialogs> dialogs =
        ws::platform::make_file_dialogs();
    REQUIRE(dialogs != nullptr);
}

// ---- message boxes: the scripted fake + the refusal ----

TEST_CASE("the fake message boxes play the scripted answers") {
    ws::platform::FakeMessageBoxes scripted;
    ws::platform::MessageBoxes& boxes = scripted;

    ws::platform::MessageBoxRequest request;
    request.kind = ws::platform::MessageBoxKind::warning;
    request.buttons = ws::platform::MessageBoxButtons::ok_cancel;
    request.title = "keep going?";
    request.message = "the document is unsaved";

    // an empty queue answers the request's first choice - the
    // click-through - never a choice the buttons did not offer
    const auto click_through = boxes.ask(request);
    REQUIRE(click_through.has_value());
    REQUIRE(*click_through == ws::platform::MessageBoxChoice::ok);
    request.buttons = ws::platform::MessageBoxButtons::yes_no;
    const auto yes_through = boxes.ask(request);
    REQUIRE(yes_through.has_value());
    REQUIRE(*yes_through == ws::platform::MessageBoxChoice::yes);

    // scripted answers pop in order
    scripted.queue = {ws::platform::MessageBoxChoice::no,
                      ws::platform::MessageBoxChoice::cancel};
    const auto refused = boxes.ask(request);
    REQUIRE(refused.has_value());
    REQUIRE(*refused == ws::platform::MessageBoxChoice::no);
    const auto backed_out = boxes.ask(request);
    REQUIRE(backed_out.has_value());
    REQUIRE(*backed_out == ws::platform::MessageBoxChoice::cancel);

    // every ask recorded its request verbatim
    REQUIRE(scripted.calls.size() == 4);
    const ws::platform::MessageBoxRequest& recorded = scripted.calls[0];
    REQUIRE(recorded.kind == ws::platform::MessageBoxKind::warning);
    REQUIRE(recorded.buttons ==
            ws::platform::MessageBoxButtons::ok_cancel);
    REQUIRE(recorded.title == "keep going?");
    REQUIRE(recorded.message == "the document is unsaved");
    REQUIRE(scripted.calls[2].buttons ==
            ws::platform::MessageBoxButtons::yes_no);
}

TEST_CASE("the unavailable message boxes refuse") {
    ws::platform::UnavailableMessageBoxes boxes;
    const auto refused = boxes.ask({});
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(code_of(refused.error()) == "platform/2");
}

TEST_CASE("make_message_boxes hands back the native set") {
    // Constructed and destroyed only: a real ask raises a modal box
    // and would hang the suite - the native set is exercised
    // interactively by the gallery (phase 7).
    const std::unique_ptr<ws::platform::MessageBoxes> boxes =
        ws::platform::make_message_boxes();
    REQUIRE(boxes != nullptr);
}
