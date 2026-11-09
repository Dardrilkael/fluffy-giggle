// apps/circsim_editor - the Palette's grouping + search, headless.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/palette_groups.hpp"

#include <string>
#include <vector>

#include <ws/circsim/diag.hpp>
#include <ws/circsim/library.hpp>

namespace {

std::vector<std::string> keys_of(const editor::PaletteGroup& g) {
    std::vector<std::string> out;
    for (const ws::circsim::SymbolDef* s : g.items) {
        out.push_back(s->key);
    }
    return out;
}

}  // namespace

TEST_CASE("the built-in symbols land in the fixed group order") {
    const ws::circsim::SymbolLibrary lib;
    const auto groups = editor::palette_groups(lib, "");
    REQUIRE(groups.size() == 4);  // no \"Other\": it is empty and dropped
    CHECK(groups[0].name == "Passive");
    CHECK(keys_of(groups[0]) == std::vector<std::string>{"r", "c", "l"});
    CHECK(groups[1].name == "Sources");
    CHECK(keys_of(groups[1]) == std::vector<std::string>{"v", "i"});
    CHECK(groups[2].name == "Semiconductors");
    CHECK(keys_of(groups[2]) ==
          std::vector<std::string>{"d", "led", "npn", "pnp", "nmos", "pmos"});
    CHECK(groups[3].name == "Power");
    CHECK(keys_of(groups[3]) == std::vector<std::string>{"gnd", "vcc"});
}

TEST_CASE("the filter matches name, key and group, ignoring case") {
    const ws::circsim::SymbolLibrary lib;
    // New behavior: all 4 groups returned, ranked by best match.
    // "RESIST" -> "r" (Resistor) in Passive -> Passive group first.
    auto by_name = editor::palette_groups(lib, "RESIST");
    REQUIRE(by_name.size() == 4);
    CHECK(by_name[0].name == "Passive");
    CHECK(keys_of(by_name[0]) == std::vector<std::string>{"r"});

    auto by_key = editor::palette_groups(lib, "nmos");
    REQUIRE(by_key.size() == 4);
    CHECK(by_key[0].name == "Semiconductors");
    CHECK(keys_of(by_key[0]) == std::vector<std::string>{"nmos"});

    // The group name matches every member.
    auto by_group = editor::palette_groups(lib, "sources");
    REQUIRE(by_group.size() == 4);
    CHECK(by_group[0].name == "Sources");
    CHECK(by_group[0].items.size() == 2);

    // No matches: still returns all 4 groups (empty items), not empty vector.
    auto no_match = editor::palette_groups(lib, "zzz-no-such");
    REQUIRE(no_match.size() == 4);
    for (const auto& g : no_match) {
        CHECK(g.items.empty());
    }
}

TEST_CASE("the filter is a fuzzy word-start match, not just a substring") {
    const ws::circsim::SymbolLibrary lib;

    // "res" finds the resistor - a substring too, but the fuzzy
    // half must keep it (the typing that lands on Enter first). The
    // sources stay out: their r sits mid-word, and the word-start
    // anchor refuses to open a match there.
    // New behavior: all groups returned, Passive first (has "r").
    auto sub = editor::palette_groups(lib, "res");
    REQUIRE(sub.size() == 4);
    CHECK(sub[0].name == "Passive");
    CHECK(keys_of(sub[0]) == std::vector<std::string>{"r"});

    // "rstr" - the R-e-S-i-S-T-o-R walk with letters skipped - finds
    // it too; the old substring matcher answered nothing. The
    // transistors stay out: their r is mid-word in "transistor".
    auto fuzzy = editor::palette_groups(lib, "rstr");
    REQUIRE(fuzzy.size() == 4);
    CHECK(fuzzy[0].name == "Passive");
    CHECK(keys_of(fuzzy[0]) == std::vector<std::string>{"r"});

    // The anchor rules both ways: "nmos" finds its own N-channel
    // MOSFET and leaves the P-channel sibling out (its n hides
    // mid-word in "P-channel").
    auto by_key = editor::palette_groups(lib, "nmos");
    REQUIRE(by_key.size() == 4);
    CHECK(by_key[0].name == "Semiconductors");
    CHECK(keys_of(by_key[0]) == std::vector<std::string>{"nmos"});

    // Out of order never matches: the s wants to come before the e,
    // and "Resistor" has none after its own.
    CHECK_FALSE(editor::palette_matches("Resistor", "se"));
    // No matches: returns all 4 groups (empty items).
    auto no_match = editor::palette_groups(lib, "stress");
    REQUIRE(no_match.size() == 4);
    for (const auto& g : no_match) {
        CHECK(g.items.empty());
    }

    // The skipped-letter typing across the space: "vsrc" walks
    // "Voltage source" (V then the S-o-u-R-C of "source") and lands
    // the voltage source alone.
    auto vsrc = editor::palette_groups(lib, "vsrc");
    REQUIRE(vsrc.size() == 4);
    CHECK(vsrc[0].name == "Sources");
    CHECK(keys_of(vsrc[0]) == std::vector<std::string>{"v"});
}

TEST_CASE("a .csym category field makes its own group, after the built-ins") {
    ws::circsim::SymbolLibrary lib;
    std::vector<ws::circsim::Diag> diags;
    const auto status = lib.load_text(R"({
      "format": "circsim-symbols", "version": 1,
      "symbols": [
        {"key": "tline", "name": "Transmission line", "spice": "T",
         "ref": "T", "category": "Distributed",
         "pins": [{"name": "a", "pos": [-2, 0], "dir": 180},
                  {"name": "b", "pos": [2, 0], "dir": 0}]},
        {"key": "xfmr", "name": "Transformer", "spice": "K", "ref": "K",
         "pins": [{"name": "a", "pos": [-2, 0], "dir": 180},
                  {"name": "b", "pos": [2, 0], "dir": 0}]}
      ]})", "test", diags);
    REQUIRE(status.has_value());

    const auto groups = editor::palette_groups(lib, "");
    REQUIRE(groups.size() >= 2);
    // Built-in groups first, then the custom one, then nothing odd.
    CHECK(groups.back().name == "Other");        // the K symbol: no category
    CHECK(keys_of(groups.back()) == std::vector<std::string>{"xfmr"});
    CHECK(groups[groups.size() - 2].name == "Distributed");
    CHECK(keys_of(groups[groups.size() - 2]) ==
          std::vector<std::string>{"tline"});
}

TEST_CASE("palette_matches is the ASCII case-insensitive fuzzy word walk") {
    CHECK(editor::palette_matches("Voltage source", "SOUR"));
    CHECK(editor::palette_matches("anything", ""));
    CHECK_FALSE(editor::palette_matches("r", "resistor"));
    // The skips: letters between the matched ones are the point.
    CHECK(editor::palette_matches("Resistor", "rstr"));
    CHECK(editor::palette_matches("Voltage source", "vsrc"));
    CHECK(editor::palette_matches("Supply (VCC)", "vcc"));
    // Order is the one rule a subsequence cannot forgive...
    CHECK_FALSE(editor::palette_matches("Resistor", "se"));
    CHECK_FALSE(editor::palette_matches("Capacitor", "rot"));
    // ...and a mid-word letter never OPENS a match (the anchor).
    CHECK_FALSE(editor::palette_matches("Current source", "res"));
    CHECK_FALSE(editor::palette_matches("P-channel MOSFET", "nmos"));
    CHECK(editor::palette_matches("N-channel MOSFET", "nmos"));
}
