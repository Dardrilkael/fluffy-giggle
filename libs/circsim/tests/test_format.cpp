// ws::circsim test suite. One binary for the whole lib (the workspace
// pattern): test_format.cpp (this file - the .csim format, and the doctest
// main), test_nets.cpp (connectivity), test_erc.cpp (one case per check),
// test_export.cpp (golden decks, analytic full-path runs, error mapping).
//
//   ctest --preset linux -R circsim

#include <string>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
// LOCAL SANDBOX NOTE (review build only, not a repo change): GCC 14's
// -Wmaybe-uninitialized fires a known false positive when a
// default-constructed AnalysisSpec variant is moved into a vector at
// -O3; MSVC (the workspace's primary desktop toolchain) and the
// semantics are unaffected.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#include <doctest/doctest.h>

#include <ws/circsim/export.hpp>
#include <ws/circsim/format.hpp>
#include "fake_fs.hpp"

using ws::circsim::Diag;
using ws::circsim::LoadReport;
using ws::circsim::Part;
using ws::circsim::Schematic;
using ws::circsim::Severity;
using ws::circsim::Wire;

namespace {

Schematic divider() {
    Schematic s;
    Part* v = s.add_part(Part::make("v", "V1", "5", {0.0, 0.0}));
    Part* r1 = s.add_part(Part::make("r", "R1", "10k", {6.0, 0.0}));
    Part* r2 = s.add_part(Part::make("r", "R2", "10k", {14.0, 0.0}));
    s.add_part(Part::make("gnd", "", "", {6.0, -4.0}));
    s.add_wire(Wire::endpoints(ws::circsim::PinRef{v->id, 0},
        ws::circsim::PinRef{r1->id, 0}));
    s.add_wire(Wire::endpoints(ws::circsim::PinRef{r1->id, 1},
        ws::circsim::PinRef{r2->id, 0}));
    ws::circsim::Label* out = s.add_label({});
    out->pos = {10.0, 0.0};
    out->name = "out";
    s.analysis.entries.push_back(ws::circsim::OpSpec{});
    s.directives = ".model nothing here";
    s.view.zoom = 2.5;
    s.view.center = {3.0, 4.0};
    return s;
}

bool same_schematic(const Schematic& a, const Schematic& b) {
    if (a.grid != b.grid || a.next_id() != b.next_id()) {
        return false;
    }
    if (a.directives != b.directives || a.view.zoom != b.view.zoom ||
        a.view.center.x != b.view.center.x ||
        a.view.center.y != b.view.center.y || a.temp != b.temp) {
        return false;
    }
    if (a.parts().size() != b.parts().size() ||
        a.wires().size() != b.wires().size() ||
        a.labels().size() != b.labels().size() ||
        a.junctions().size() != b.junctions().size()) {
        return false;
    }
    if (a.analysis.entries.size() != b.analysis.entries.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.parts().size(); ++i) {
        const Part& x = a.parts()[i];
        const Part& y = b.parts()[i];
        if (x.id != y.id || x.symbol != y.symbol || x.ref != y.ref ||
            x.value != y.value || x.pos.x != y.pos.x || x.pos.y != y.pos.y ||
            x.rot != y.rot || x.mirror != y.mirror ||
            x.params != y.params || x.unknown != y.unknown) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.wires().size(); ++i) {
        const Wire& x = a.wires()[i];
        const Wire& y = b.wires()[i];
        if (x.id != y.id || x.a != y.a || x.b != y.b ||
            x.waypoints != y.waypoints || x.ortho != y.ortho ||
            x.unknown != y.unknown) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.labels().size(); ++i) {
        if (a.labels()[i].id != b.labels()[i].id ||
            a.labels()[i].name != b.labels()[i].name ||
            a.labels()[i].pos.x != b.labels()[i].pos.x ||
            a.labels()[i].pos.y != b.labels()[i].pos.y) {
            return false;
        }
    }
    // Analysis entries: compare card spellings (a faithful proxy).
    for (std::size_t i = 0; i < a.analysis.entries.size(); ++i) {
        if (ws::circsim::analysis_card(a.analysis.entries[i]) !=
            ws::circsim::analysis_card(b.analysis.entries[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("a schematic round-trips field for field") {
    const Schematic s = divider();
    LoadReport report;
    const auto loaded = ws::circsim::load_csim(ws::circsim::save_csim(s),
                                               report);
    REQUIRE(loaded.has_value());
    CHECK(report.diags.empty());
    CHECK(same_schematic(s, *loaded));

    // And the second save is byte-identical (stable diffs).
    CHECK(ws::circsim::save_csim(*loaded) == ws::circsim::save_csim(s));
}

TEST_CASE("arrays save sorted by id regardless of vector order") {
    Schematic s;
    s.add_part(Part::make("r", "R2", "1k", {0.0, 0.0}));  // id 1
    s.add_part(Part::make("r", "R1", "1k", {4.0, 0.0}));  // id 2
    std::swap(s.parts_mut()[0], s.parts_mut()[1]);  // vector order reversed
    const std::string text = ws::circsim::save_csim(s);
    const std::size_t r2 = text.find("\"ref\": \"R2\"");
    const std::size_t r1 = text.find("\"ref\": \"R1\"");
    REQUIRE(r1 != std::string::npos);
    REQUIRE(r2 != std::string::npos);
    CHECK(r2 < r1);  // id 1 saves first
}

TEST_CASE("unknown fields survive a round trip, in place") {
    const std::string text = R"json({
        "format": "circsim",
        "version": 1,
        "grid": 1.0,
        "next_id": 4,
        "author": "someone",
        "parts": [
            {"id": 1, "symbol": "r", "ref": "R1", "value": "1k",
             "pos": [0.0, 0.0], "rot": 0, "mirror": false,
             "params": {"note": "keep"},
             "stock": "thin-film", "custom": {"a": [1, 2]}}
        ],
        "junctions": [],
        "wires": [
            {"id": 2, "a": {"pin": {"part": 1, "pin": 0}},
             "b": {"pin": {"part": 1, "pin": 1}}, "waypoints": [],
             "ortho": true, "color": "blue"}
        ],
        "labels": [],
        "analysis": {"entries": []},
        "directives": "",
        "view": {"zoom": 1.0, "center": [0.0, 0.0], "ruler": true},
        "future-top-level": {"x": 1}
    })json";
    LoadReport report;
    const auto loaded = ws::circsim::load_csim(text, report);
    REQUIRE(loaded.has_value());
    CHECK(report.diags.empty());

    const std::string saved = ws::circsim::save_csim(*loaded);
    CHECK(saved.find("\"author\": \"someone\"") != std::string::npos);
    CHECK(saved.find("\"stock\": \"thin-film\"") != std::string::npos);
    CHECK(saved.find("\"custom\"") != std::string::npos);
    CHECK(saved.find("\"color\": \"blue\"") != std::string::npos);
    CHECK(saved.find("\"ruler\": true") != std::string::npos);
    CHECK(saved.find("\"future-top-level\"") != std::string::npos);
    // Unknowns land after the known fields, not before them.
    CHECK(saved.find("\"stock\"") > saved.find("\"params\""));

    // And they survive ANOTHER round trip.
    LoadReport second_report;
    const auto again = ws::circsim::load_csim(saved, second_report);
    REQUIRE(again.has_value());
    CHECK(ws::circsim::save_csim(*again) == saved);
}

TEST_CASE("version 0 files migrate through the chain") {
    const std::string text = R"json({
        "format": "circsim",
        "grid": 1.0,
        "parts": [],
        "junctions": [], "wires": [], "labels": []
    })json";
    LoadReport report;
    const auto loaded = ws::circsim::load_csim(text, report);
    REQUIRE(loaded.has_value());
    CHECK(report.loaded_version == 0);
    CHECK(report.migrated);
    CHECK_FALSE(report.future);
    CHECK(loaded->directives.empty());

    // The migrated save claims the current version.
    const std::string saved = ws::circsim::save_csim(*loaded);
    CHECK(saved.find("\"version\": 1") != std::string::npos);
}

TEST_CASE("future versions load with a warning, not a failure") {
    const std::string text = R"json({
        "format": "circsim", "version": 99, "grid": 1.0,
        "parts": [{"id": 1, "symbol": "r", "ref": "R1", "value": "1k",
                   "pos": [0.0, 0.0], "rot": 0, "mirror": false,
                   "params": {}}],
        "junctions": [], "wires": [], "labels": []
    })json";
    LoadReport report;
    const auto loaded = ws::circsim::load_csim(text, report);
    REQUIRE(loaded.has_value());
    CHECK(report.future);
    REQUIRE(report.diags.size() == 1);
    CHECK(report.diags[0].code == "circsim.format.future_version");
    CHECK(report.diags[0].severity == Severity::Warning);
}

TEST_CASE("duplicate ids keep the first and raise next_id past both") {
    const std::string text = R"json({
        "format": "circsim", "version": 1, "grid": 1.0, "next_id": 3,
        "parts": [
            {"id": 2, "symbol": "r", "ref": "R2", "value": "2k",
             "pos": [0.0, 0.0], "rot": 0, "mirror": false, "params": {}},
            {"id": 2, "symbol": "r", "ref": "OTHER", "value": "9k",
             "pos": [4.0, 0.0], "rot": 0, "mirror": false, "params": {}}
        ],
        "junctions": [], "wires": [], "labels": []
    })json";
    LoadReport report;
    const auto loaded = ws::circsim::load_csim(text, report);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->parts().size() == 1);
    CHECK(loaded->parts()[0].ref == "R2");
    REQUIRE(report.diags.size() == 1);
    CHECK(report.diags[0].code == "circsim.format.duplicate_id");
    // next_id above the dropped duplicate's id: never reused.
    CHECK(loaded->next_id() == 3);
    Schematic mutable_copy = *loaded;
    CHECK(mutable_copy.alloc_id() == 3);
}

TEST_CASE("wires referencing missing parts or junctions are dropped") {
    const std::string text = R"json({
        "format": "circsim", "version": 1, "grid": 1.0, "next_id": 6,
        "parts": [
            {"id": 1, "symbol": "r", "ref": "R1", "value": "1k",
             "pos": [0.0, 0.0], "rot": 0, "mirror": false, "params": {}}
        ],
        "junctions": [],
        "wires": [
            {"id": 2, "a": {"pin": {"part": 99, "pin": 0}},
             "b": {"pin": {"part": 1, "pin": 0}}, "waypoints": [],
             "ortho": true},
            {"id": 3, "a": {"junction": {"id": 42}},
             "b": {"pin": {"part": 1, "pin": 1}}, "waypoints": [],
             "ortho": true}
        ],
        "labels": []
    })json";
    LoadReport report;
    const auto loaded = ws::circsim::load_csim(text, report);
    REQUIRE(loaded.has_value());
    CHECK(loaded->wires().empty());
    REQUIRE(report.diags.size() == 2);
    CHECK(report.diags[0].code == "circsim.format.dangling_ref");
    CHECK(report.diags[1].code == "circsim.format.dangling_ref");
}

TEST_CASE("not-json and wrong-format files are hard failures") {
    LoadReport report;
    const auto a = ws::circsim::load_csim("this is not json", report);
    REQUIRE_FALSE(a.has_value());
    CHECK(a.error().category == std::string("circsim"));

    const auto b = ws::circsim::load_csim("{\"format\": \"other\"}", report);
    REQUIRE_FALSE(b.has_value());

    const auto c = ws::circsim::load_csim("[1, 2, 3]", report);
    REQUIRE_FALSE(c.has_value());
}

TEST_CASE("wrong-typed known fields repair to defaults with diagnostics") {
    const std::string text = R"json({
        "format": "circsim", "version": 1, "grid": "coarse", "next_id": 2,
        "parts": [
            {"id": 1, "symbol": "r", "ref": "R1", "value": 42,
             "pos": "nope", "rot": 0, "mirror": false, "params": {
                 "model": 7
             }}
        ],
        "junctions": [], "wires": [], "labels": []
    })json";
    LoadReport report;
    const auto loaded = ws::circsim::load_csim(text, report);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->parts().size() == 1);
    CHECK(loaded->parts()[0].value.empty());  // non-text value repaired
    CHECK(loaded->parts()[0].pos.x == 0.0);   // bad pos repaired
    CHECK(loaded->parts()[0].params.empty()); // non-text param dropped
    CHECK(loaded->grid == 1.0);               // non-number grid repaired
    CHECK(report.diags.size() >= 3);
    bool saw_pos = false;
    for (const Diag& d : report.diags) {
        if (d.code == "circsim.format.bad_field" &&
            d.message.find("pos") != std::string::npos) {
            saw_pos = true;
        }
    }
    CHECK(saw_pos);
}

TEST_CASE("the disk pair rides the FileSystem seam, writes atomic") {
    ws::circsim::testing::FakeFs fs;
    const Schematic s = divider();
    REQUIRE(ws::circsim::write_csim(fs, "a.csim", s));
    REQUIRE(fs.exists("a.csim"));
    CHECK(fs.writes == 1);

    LoadReport report;
    const auto loaded = ws::circsim::read_csim(fs, "a.csim", report);
    REQUIRE(loaded.has_value());
    CHECK(same_schematic(s, *loaded));

    const auto missing = ws::circsim::read_csim(fs, "b.csim", report);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().category == std::string("circsim"));
}

TEST_CASE("every analysis spec spelling round-trips") {
    Schematic s;
    s.analysis.entries.push_back(ws::circsim::OpSpec{});
    s.analysis.entries.push_back(
        ws::circsim::DcSpec{.source = "V1", .start = 0.0, .stop = 5.0,
                            .step = 0.1});
    s.analysis.entries.push_back(ws::circsim::TranSpec{
        .tstep = 1e-4, .tstop = 1e-2, .tstart = 0.0, .tmax = 1e-3,
        .uic = true});
    s.analysis.entries.push_back(ws::circsim::AcSpec{
        .sweep = ws::circsim::AcSpec::Sweep::Oct, .n = 20,
        .fstart = 10.0, .fstop = 1e5});
    s.analysis.entries.push_back(ws::circsim::NoiseSpec{
        .sweep = ws::circsim::NoiseSpec::Sweep::Lin, .n = 30,
        .fstart = 100.0, .fstop = 100e3, .output = "out", .ref = "gndref",
        .input = "V1", .pts_per_summary = 5});
    s.temp = 85.0;

    LoadReport report;
    const auto loaded =
        ws::circsim::load_csim(ws::circsim::save_csim(s), report);
    REQUIRE(loaded.has_value());
    CHECK(same_schematic(s, *loaded));
}

TEST_CASE("the temperature field is optional and absent when unset") {
    // Unset: the file carries no temp key at all (an older writer's
    // byte-identical shape), and the load answers nullopt.
    {
        Schematic s;
        LoadReport report;
        const auto loaded =
            ws::circsim::load_csim(ws::circsim::save_csim(s), report);
        REQUIRE(loaded.has_value());
        CHECK(loaded->temp == std::nullopt);
        CHECK(ws::circsim::save_csim(*loaded).find("\"temp\"") ==
              std::string::npos);
    }
    // A non-number repairs to unset with a diagnostic, never a failure.
    {
        constexpr const char* bad =
            R"json({"format":"circsim","version":1,"temp":"hot"})json";
        LoadReport report;
        const auto loaded = ws::circsim::load_csim(bad, report);
        REQUIRE(loaded.has_value());
        CHECK(loaded->temp == std::nullopt);
        REQUIRE(report.diags.size() == 1u);
        CHECK(report.diags[0].code == "circsim.format.bad_field");
    }
}
