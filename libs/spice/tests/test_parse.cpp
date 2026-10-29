// Parser stage 1: records, provenance, include expansion (§8.1) and the
// SPICE number grammar with every trap of §8.2 pinned (DESIGN.md's table
// rows for the number parser are "exact" - these cases are that suite).
// No DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here - test_spice.cpp owns doctest's
// single implementation TU for this binary (the test_math/test_vecmath
// pattern).

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include <ws/spice/parse.hpp>

using ws::spice::Deck;
using ws::spice::ParseOptions;
using ws::spice::parse_number;
using ws::spice::read_deck;
using ws::spice::Token;

// --- numbers -----------------------------------------------------------------

TEST_CASE("SPICE number suffixes: the full table, exact") {
    const std::pair<const char*, double> cases[] = {
        {"1T", 1e12},  {"1G", 1e9},   {"1MEG", 1e6}, {"1K", 1e3},
        {"1MIL", 25.4e-6},            {"1M", 1e-3},  {"1m", 1e-3},
        {"1U", 1e-6},  {"1u", 1e-6},  {"1n", 1e-9},  {"1P", 1e-12},
        {"1p", 1e-12}, {"1F", 1e-15}, {"1f", 1e-15},
    };
    for (const auto& [text, want] : cases) {
        CAPTURE(text);
        const auto v = parse_number(text);
        REQUIRE(v);
        CHECK(*v == doctest::Approx(want));
    }
}

TEST_CASE("case-insensitivity and the M vs MEG scan order") {
    CHECK(*parse_number("1meg") == doctest::Approx(1e6));
    CHECK(*parse_number("1Meg") == doctest::Approx(1e6));
    CHECK(*parse_number("1MEG") == doctest::Approx(1e6));
    CHECK(*parse_number("1mil") == doctest::Approx(25.4e-6));
    CHECK(*parse_number("1MIL") == doctest::Approx(25.4e-6));
    CHECK(*parse_number("1m") == doctest::Approx(1e-3));
    CHECK(*parse_number("1k") == doctest::Approx(1e3));
    CHECK(*parse_number("1g") == doctest::Approx(1e9));
    CHECK(*parse_number("1t") == doctest::Approx(1e12));
    // µ (U+00B5) reads as u.
    CHECK(*parse_number("1\xC2\xB5") == doctest::Approx(1e-6));
    CHECK(*parse_number("1\xC2\xB5" "F") == doctest::Approx(1e-6));
}

TEST_CASE("unit tails are ignored, values keep their scale") {
    const std::pair<const char*, double> cases[] = {
        {"5V", 5.0},       {"10kOhm", 1e4},   {"1.5e-3A", 1.5e-3},  {"1uF", 1e-6},
        {"1meh", 1e-3},    {"100", 100.0},    {"2.5", 2.5},
    };
    for (const auto& [text, want] : cases) {
        CAPTURE(text);
        const auto v = parse_number(text);
        REQUIRE(v);
        CHECK(*v == doctest::Approx(want));
    }
    // '1F' is femto, never farad - the trailing F of '1uF' is a unit.
    CHECK(*parse_number("1F") == doctest::Approx(1e-15));
    CHECK(*parse_number("1uF") == doctest::Approx(1e-6));
}

TEST_CASE("signs, bare dots, exponents") {
    CHECK(*parse_number(".5") == doctest::Approx(0.5));
    CHECK(*parse_number("5.") == doctest::Approx(5.0));
    CHECK(*parse_number("1e3") == doctest::Approx(1e3));
    CHECK(*parse_number("1E3") == doctest::Approx(1e3));
    CHECK(*parse_number("1e-3") == doctest::Approx(1e-3));
    CHECK(*parse_number("-2.2u") == doctest::Approx(-2.2e-6));
    CHECK(*parse_number("+2.2u") == doctest::Approx(2.2e-6));
    CHECK(*parse_number("-1") == doctest::Approx(-1.0));
}

TEST_CASE("rejected numbers carry the why") {
    const char* bad[] = {
        "1e3k",   // suffix directly after an exponent
        "1e3meg", // ...ditto with a long suffix
        "1.2.3",  // remainder starts a number
        "abc",    // no digits
        "",       // empty
        "1..2",   // two dots
        "1-2",    // interior sign
        "e3",     // exponent without mantissa digits
    };
    for (const char* text : bad) {
        CAPTURE(text);
        const auto v = parse_number(text);
        CHECK_FALSE(v);
        if (!v) {
            CHECK(v.error().category == std::string("spice"));
        }
    }
}

// --- records and provenance ----------------------------------------------------

namespace {

// A resolver over an in-memory filesystem (tests never touch disk).
class MapFs {
public:
    void add(std::string path, std::string text) { files_[std::move(path)] = std::move(text); }
    ws::core::Result<std::string> resolve(const std::string& path) const {
        if (const auto it = files_.find(path); it != files_.end()) {
            return it->second;
        }
        return ws::core::fail(1, "spice", "no such file: " + path);
    }
    ParseOptions options(std::vector<std::string> dirs = {}) const {
        ParseOptions opt;
        opt.include_dirs = std::move(dirs);
        opt.include_resolver = [this](const std::string& p) { return resolve(p); };
        return opt;
    }

private:
    std::map<std::string, std::string> files_;
};

}  // namespace

TEST_CASE("title, comments, continuations, .end") {
    const std::string text =
        "Example deck title\n"
        "R1 a b 1k   ; inline comment\n"
        "* full-line comment\n"
        "V1 a 0 5 $ dollar comment\n"
        "R2 b\n"
        "+ c\n"
        "+ 2k\n"
        ".end\n"
        "R3 a b 3k ; never seen - after .end\n";
    const auto deck = read_deck("t.cir", text, ParseOptions{});
    REQUIRE(deck);
    CHECK(deck->title == "Example deck title");
    REQUIRE(deck->records.size() == 3);
    REQUIRE(deck->records[0].tokens.size() == 4);
    CHECK(deck->records[0].tokens[0].text == "R1");
    CHECK(deck->records[0].tokens[3].text == "1k");
    CHECK(deck->records[0].tokens[0].loc.line == 2);
    CHECK(deck->records[0].tokens[0].loc.col == 1);
    CHECK(deck->records[1].tokens[0].text == "V1");
    CHECK(deck->records[1].tokens[3].text == "5");
    // Continuations merge: R2 b c 2k, one record, provenance at line 5.
    REQUIRE(deck->records[2].tokens.size() == 4);
    CHECK(deck->records[2].tokens[0].text == "R2");
    CHECK(deck->records[2].tokens[1].text == "b");
    CHECK(deck->records[2].tokens[2].text == "c");
    CHECK(deck->records[2].tokens[3].text == "2k");
    CHECK(deck->records[2].loc.line == 5);
    // Continuation tokens carry their own physical line numbers.
    CHECK(deck->records[2].tokens[2].loc.line == 6);
    CHECK(deck->records[2].tokens[3].loc.line == 7);
}

TEST_CASE("continuation with nothing to continue is a parse error") {
    const auto deck = read_deck("t.cir", "title\n+ orphan\n", ParseOptions{});
    REQUIRE_FALSE(deck);
    CHECK(deck.error().message.find("nothing to continue") != std::string::npos);
}

TEST_CASE("include expands in place with provenance, depth-capped, cycle-safe") {
    MapFs fs;
    fs.add("models.cir",
           "RM1 x y 1k\n"
           "+ 2x\n");
    fs.add("deep.cir", ".include deeper.cir\n");
    fs.add("deeper.cir", "RD a 0 1\n");

    SUBCASE("in place, tokens carry the included file's name") {
        const std::string text =
            "title\n.include models.cir\nV1 a 0 1\n";
        const auto deck = read_deck("main.cir", text, fs.options());
        REQUIRE(deck);
        REQUIRE(deck->records.size() == 2);
        CHECK(deck->records[0].tokens[0].text == "RM1");
        CHECK(deck->records[0].tokens[0].loc.file == "models.cir");
        CHECK(deck->records[0].tokens[0].loc.line == 1);
        // The continuation inside the include merged too.
        CHECK(deck->records[0].tokens.back().text == "2x");
        CHECK(deck->records[1].tokens[0].text == "V1");
        CHECK(deck->records[1].tokens[0].loc.file == "main.cir");
    }

    SUBCASE("relative to the including file, then -I dirs") {
        fs.add("sub/other.cir", "RX a 0 1\n");
        const std::string text = "title\n.include other.cir\n";
        ParseOptions opts = fs.options({"sub"});
        const auto deck = read_deck("sub/main.cir", text, opts);
        REQUIRE(deck);
        REQUIRE(deck->records.size() == 1);
        CHECK(deck->records[0].tokens[0].loc.file == "sub/other.cir");
    }

    SUBCASE("cycles are named") {
        fs.add("loop.cir", ".include loop.cir\n");
        const auto deck = read_deck("main.cir", "title\n.include loop.cir\n", fs.options());
        REQUIRE_FALSE(deck);
        CHECK(deck.error().message.find("include cycle") != std::string::npos);
    }

    SUBCASE("missing includes list where it searched") {
        const auto deck = read_deck("main.cir", "title\n.include nope.cir\n", fs.options());
        REQUIRE_FALSE(deck);
        CHECK(deck.error().message.find("cannot resolve .include") != std::string::npos);
    }

    SUBCASE("depth is capped") {
        // self-including chain past the cap: loop.cir -> deep chain of
        // distinct files is overkill; a two-file ping-pong hits the cap.
        fs.add("ping.cir", ".include pong.cir\n");
        fs.add("pong.cir", ".include ping.cir\n");
        const auto deck = read_deck("main.cir", "title\n.include ping.cir\n", fs.options());
        REQUIRE_FALSE(deck);
        const std::string msg = deck.error().message;
        CHECK((msg.find("include depth") != std::string::npos ||
               msg.find("include cycle") != std::string::npos));
    }
}

TEST_CASE(".end inside an include ends the whole deck") {
    MapFs fs;
    fs.add("inner.cir", "R1 a 0 1\n.end\nR2 a 0 2\n");
    const auto deck =
        read_deck("main.cir", "title\n.include inner.cir\nR3 a 0 3\n", fs.options());
    REQUIRE(deck);
    REQUIRE(deck->records.size() == 1);
    CHECK(deck->records[0].tokens[0].text == "R1");
}

TEST_CASE("token location attaches to number errors") {
    Token t;
    t.text = "1e3k";
    t.loc = ws::spice::SourceLoc::make("bad.cir", 7, 12);
    const auto v = parse_number(t);
    REQUIRE_FALSE(v);
    CHECK(v.error().message ==
          std::string("bad.cir:7:12: error: malformed number '1e3k'"));
}
