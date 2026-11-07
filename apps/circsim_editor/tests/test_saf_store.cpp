// The SAF port's pure half, headless (the suite the saf_store.hpp header
// promises): the sandbox-name policy, the map's parse/format round trip,
// and the write-through FileSystem wrapper's every verb - the data
// policy the JNI half (android_saf.cpp, android-only) rides. No Android
// type appears here; the push act is an injected recorder.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "saf_store.hpp"

#include <ws/settings/filesystem.hpp>

namespace {

// The in-memory FileSystem fake the shell suite's own shape rides: a
// path -> bytes map over the seam's five verbs.
class FakeFs final : public ws::settings::FileSystem {
  public:
    std::map<std::string, std::string> files;

    [[nodiscard]] ws::core::Result<std::string> read(
        std::string_view path) const override {
        const auto found = files.find(std::string{path});
        if (found == files.end()) {
            return ws::core::unexpected<ws::core::Error>(ws::core::make_error(
                1, "io", "missing '" + std::string{path} + "'"));
        }
        return found->second;
    }

    ws::core::Status write(std::string_view path,
                           std::string_view bytes) override {
        files[std::string{path}] = bytes;
        return {};
    }

    ws::core::Status rename(std::string_view from,
                            std::string_view to) override {
        const auto found = files.find(std::string{from});
        if (found == files.end()) {
            return ws::core::unexpected<ws::core::Error>(ws::core::make_error(
                1, "io", "missing '" + std::string{from} + "'"));
        }
        files[std::string{to}] = found->second;
        files.erase(found);
        return {};
    }

    bool remove(std::string_view path) override {
        return files.erase(std::string{path}) > 0;
    }

    [[nodiscard]] bool exists(std::string_view path) const override {
        return files.contains(std::string{path});
    }
};

// The push recorder: every attempt lands, and the scripted failures
// answer the way a refused ContentResolver hop would.
struct PushLog {
    std::vector<std::pair<std::string, std::string>> pushed;
    std::optional<std::string> fail_with;

    [[nodiscard]] editor::saf::SafPush hook() {
        return [this](const std::string& uri,
                      std::string_view bytes) -> ws::core::Status {
            if (fail_with.has_value()) {
                return ws::core::unexpected<ws::core::Error>(
                    ws::core::make_error(1, "io", *fail_with));
            }
            pushed.emplace_back(uri, bytes);
            return {};
        };
    }
};

}  // namespace

TEST_CASE("the sandbox name keeps the pick's shape, the URI's identity") {
    using editor::saf::sandbox_name_for;

    // The display name survives with its extension; the URI's hash
    // splices in before it.
    const std::string a =
        sandbox_name_for("my circuit.csim", "content://x/doc/1", "schematic");
    CHECK(a.starts_with("my circuit-"));
    CHECK(a.ends_with(".csim"));
    CHECK(a.size() == std::string{"my circuit-"}.size() + 8 + 5);

    // Two same-named documents from different folders stay two files.
    const std::string b =
        sandbox_name_for("my circuit.csim", "content://x/doc/2", "schematic");
    CHECK(a != b);

    // The SAME URI is one stable name across launches - the identity
    // session restore and recents ride.
    CHECK(sandbox_name_for("my circuit.csim", "content://x/doc/1",
                           "schematic") == a);

    // Path bytes fold away: a picked name cannot escape the sandbox.
    const std::string evil = sandbox_name_for(
        "../..\\escape/eaten.csim", "content://x/doc/3", "schematic");
    CHECK(evil.find('/') == std::string::npos);
    CHECK(evil.find('\\') == std::string::npos);
    CHECK(evil.starts_with("....escapeeaten-"));

    // A non-.csim pick keeps its extension - the document factory's
    // honest "not a .csim schematic" refusal needs to see it.
    CHECK(sandbox_name_for("notes.txt", "content://x/doc/4", "schematic")
              .ends_with(".txt"));

    // A bare name earns .csim; a name that sanitizes away falls back.
    CHECK(sandbox_name_for("plain", "content://x/doc/5", "schematic")
              .ends_with(".csim"));
    CHECK(sandbox_name_for("///", "content://x/doc/6", "fallback")
              .starts_with("fallback-"));

    // A long name caps before the splice.
    const std::string long_name(300, 'x');
    const std::string capped =
        sandbox_name_for(long_name, "content://x/doc/7", "schematic");
    CHECK(capped.size() <= 80 + 1 + 8 + 5);
}

TEST_CASE("the map parses what it formats, and drops what it cannot") {
    editor::saf::SafMap map;
    map.add("/data/schematics/a-1111.csim", "content://x/a");
    map.add("/data/schematics/b-2222.csim", "content://x/b");

    // The round trip.
    editor::saf::SafMap reloaded;
    reloaded.load(map.format());
    REQUIRE(reloaded.entries().size() == 2);
    CHECK(reloaded.entries()[0].path == "/data/schematics/a-1111.csim");
    CHECK(reloaded.entries()[0].uri == "content://x/a");
    CHECK(reloaded.entries()[1].uri == "content://x/b");

    // Lookups.
    REQUIRE(map.find("/data/schematics/a-1111.csim") != nullptr);
    CHECK(*map.find("/data/schematics/a-1111.csim") == "content://x/a");
    CHECK(map.find("/data/schematics/missing") == nullptr);

    // A re-picked path re-points: the newest entry wins.
    map.add("/data/schematics/a-1111.csim", "content://x/new");
    CHECK(map.entries().size() == 2);
    CHECK(*map.find("/data/schematics/a-1111.csim") == "content://x/new");

    // A hand-damaged map degrades line by line, never kills.
    editor::saf::SafMap damaged;
    damaged.load("no-tab-line\n\n/data/x\tcontent://ok\n/content-only\t\n");
    REQUIRE(damaged.entries().size() == 1);
    CHECK(damaged.entries()[0].path == "/data/x");
    CHECK(damaged.entries()[0].uri == "content://ok");

    // remove() drops the pair.
    map.remove("/data/schematics/b-2222.csim");
    CHECK(map.find("/data/schematics/b-2222.csim") == nullptr);
}

TEST_CASE("the write-through seam mirrors mapped writes, records refused "
          "pushes") {
    FakeFs base;
    editor::saf::SafMap map;
    PushLog push;
    editor::saf::WriteThroughFs fs{base, map, push.hook(), "/state/map.txt"};

    // An ordinary write: no URI behind it, no push.
    CHECK(fs.write("/plain.txt", "bytes"));
    CHECK(base.files["/plain.txt"] == "bytes");
    CHECK(push.pushed.empty());

    // A mapped write: the local file AND the mirror both land.
    map.add("/sandbox/a-1111.csim", "content://x/a");
    CHECK(fs.write("/sandbox/a-1111.csim", "one"));
    CHECK(base.files["/sandbox/a-1111.csim"] == "one");
    REQUIRE(push.pushed.size() == 1);
    CHECK(push.pushed[0].first == "content://x/a");
    CHECK(push.pushed[0].second == "one");

    // A refused push degrades to "saved locally": the write answers ok
    // and the error waits for the reader.
    push.fail_with = "provider refused";
    CHECK(fs.write("/sandbox/a-1111.csim", "two"));
    CHECK(base.files["/sandbox/a-1111.csim"] == "two");  // the save stands
    CHECK(push.pushed.size() == 1);                      // the push did not
    const std::optional<std::string> error = fs.take_push_error();
    REQUIRE(error.has_value());
    CHECK(*error == "provider refused");
    CHECK_FALSE(fs.take_push_error().has_value());  // drained

    // A failed local write never reaches the push.
    push.fail_with.reset();
    map.add("/sandbox/dead.csim", "content://x/dead");
    struct RefusingFs final : ws::settings::FileSystem {
        [[nodiscard]] ws::core::Result<std::string> read(
            std::string_view) const override {
            return "";
        }
        ws::core::Status write(std::string_view,
                               std::string_view) override {
            return ws::core::unexpected<ws::core::Error>(
                ws::core::make_error(1, "io", "disk full"));
        }
        ws::core::Status rename(std::string_view, std::string_view) override {
            return {};
        }
        bool remove(std::string_view) override { return true; }
        [[nodiscard]] bool exists(std::string_view) const override {
            return false;
        }
    } refusing;
    editor::saf::SafMap dead_map;
    editor::saf::WriteThroughFs dead{refusing, dead_map, push.hook(), "/m"};
    dead_map.add("/sandbox/dead.csim", "content://x/dead");
    CHECK_FALSE(dead.write("/sandbox/dead.csim", "x"));
    CHECK(push.pushed.size() == 1);  // unchanged

    // map() persists through the seam: the map file is a plain write.
    fs.map("/sandbox/b-2222.csim", "content://x/b");
    CHECK(base.files["/state/map.txt"].find(
              "/sandbox/b-2222.csim\tcontent://x/b") != std::string::npos);

    // remove() unmaps (the pair leaves the persisted map too).
    CHECK(fs.remove("/sandbox/a-1111.csim"));
    CHECK(base.files["/state/map.txt"].find("/sandbox/a-1111.csim") ==
          std::string::npos);

    // read/exists delegate; rename of a mapped file drops the mapping
    // (the honest "local fork" answer).
    CHECK(fs.write("/sandbox/b-2222.csim", "three"));
    CHECK(push.pushed.size() == 2);
    CHECK(fs.exists("/sandbox/b-2222.csim"));
    CHECK(fs.read("/sandbox/b-2222.csim").value_or("") == "three");
    CHECK(fs.rename("/sandbox/b-2222.csim", "/sandbox/renamed.csim"));
    CHECK(fs.write("/sandbox/renamed.csim", "four"));  // no push: unmapped
    CHECK(push.pushed.size() == 2);
}
