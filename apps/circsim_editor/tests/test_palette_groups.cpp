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
          std::vector<std::string>{"d", "npn", "pnp", "nmos", "pmos"});
    CHECK(groups[3].name == "Power");
    CHECK(keys_of(groups[3]) == std::vector<std::string>{"gnd", "vcc"});
}

TEST_CASE("the filter matches name, key and group, ignoring case") {
    const ws::circsim::SymbolLibrary lib;
    auto by_name = editor::palette_groups(lib, "RESIST");
    REQUIRE(by_name.size() == 1);
    CHECK(keys_of(by_name[0]) == std::vector<std::string>{"r"});

    auto by_key = editor::palette_groups(lib, "nmos");
    REQUIRE(by_key.size() == 1);
    CHECK(keys_of(by_key[0]) == std::vector<std::string>{"nmos"});

    // The group name matches every member.
    auto by_group = editor::palette_groups(lib, "sources");
    REQUIRE(by_group.size() == 1);
    CHECK(by_group[0].items.size() == 2);

    CHECK(editor::palette_groups(lib, "zzz-no-such").empty());
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

TEST_CASE("palette_matches is an ASCII case-insensitive substring") {
    CHECK(editor::palette_matches("Voltage source", "SOUR"));
    CHECK(editor::palette_matches("anything", ""));
    CHECK_FALSE(editor::palette_matches("r", "resistor"));
}
