#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/spice/error.hpp>

// Netlist front end, stage 1 (DESIGN.md §8.1): records with provenance.
// read_deck turns deck text into the title + a flat list of records -
// comments stripped, '+' continuations merged, '.include' expanded (depth
// capped, cycles detected), '.end' terminating - with every token carrying
// {file, line, col} through include expansion. Pure function: no simulator
// types, no file I/O of its own (the CLI hands it text; includes resolve
// through an injected seam so tests run off a map).

namespace ws::spice {

// One lexical token with its provenance (§8.1).
struct Token {
    std::string text;  // as spelled; display spellings are kept everywhere
    SourceLoc loc;     // 1-based line, byte col, file as spelled on the card
};

// One logical record: a deck line with continuations merged.
struct Record {
    std::vector<Token> tokens;
    SourceLoc loc;  // start of the record's first physical line

    bool empty() const { return tokens.empty(); }
};

struct Deck {
    std::string title;          // line 1, ignored semantically
    std::vector<Record> records;
};

// Text-of-a-file seam for '.include': the CLI wires a disk reader, tests a
// map. Returns an error for a path it cannot produce.
using IncludeResolver =
    std::function<ws::core::Result<std::string>(const std::string& path)>;

struct ParseOptions {
    // Searched for .include targets AFTER the including file's own
    // directory, in order (the CLI's -I paths).
    std::vector<std::string> include_dirs;
    IncludeResolver include_resolver;  // required for decks that .include
    int max_include_depth = 16;        // §8.1: the cap
};

// Reads a deck: `deck_path` is the name used in diagnostics and as the
// include-resolution base, `deck_text` its content.
ws::core::Result<Deck> read_deck(std::string deck_path, std::string deck_text,
                                 const ParseOptions& opts);

// §8.2 SPICE number: optional sign, digits with optional '.', optional
// e[+-]digits, then the longest-match case-insensitive scale suffix
// (T G MEG K MIL M U/µ N P F - MEG and MIL before M), then ignorable
// trailing unit letters. The famous traps are pinned: '1m' = 1e-3,
// '1meg' = 1e6, '1F' = 1e-15 (femto, not farad), '1uF' = 1e-6. Errors:
// a suffix directly after an exponent ('1e3k'), a remainder that starts
// a number ('1.2.3'), no digits at all.
ws::core::Result<double> parse_number(std::string_view text);

// Same, attaching the token's location to any error.
ws::core::Result<double> parse_number(const Token& token);

// Case-insensitive matching key (§8.1): the lowercase form of a spelling.
std::string to_lower(std::string_view s);

}  // namespace ws::spice
