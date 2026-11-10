#pragma once

#include <optional>
#include <string>
#include <string_view>

// The ONE number formatting/parsing seam for everything whose text a
// deck, a netlist or a .csim/.csym file will re-read (see
// docs/threading.md's sibling rule: netlist text is locale-free).
//
// std::snprintf("%g") and std::atof answer to LC_NUMERIC: under a
// German/French/Italian locale "0.1" formats as "0,1" and the round
// trip corrupts silently - a simulator may reject the deck or, worse,
// accept it with wrong values. Everything here is locale-independent
// by construction (std::to_chars / std::from_chars in the "C" locale,
// with a classic-locale fallback for toolchains without FP to_chars).

namespace ws::circsim {

// Case-insensitive less-than over ASCII, the comparator behind the
// export's element/node maps. Deliberately ASCII-only (std::tolower
// answers to LC_CTYPE; symbol keys, refs and net names are ASCII
// identifiers, and a locale that folds 'I' differently must not change
// net identity).
struct CiLess {
    [[nodiscard]] bool operator()(const std::string& a,
                                  const std::string& b) const;
};

// Locale-independent printf %.{digits}g: up to `digits` significant
// digits, trailing zeros stripped, the same f/e switch printf makes
// (std::to_chars(general, precision) - byte-identical to snprintf's
// "%.12g" on every value the decks spell, verified against the golden
// deck tests). digits is clamped to [1, 17]. Non-finite values print
// "nan"/"inf"/"-inf" exactly as before.
[[nodiscard]] std::string fmt_num(double v, int digits = 12);

// Locale-independent finite-number parse of the WHOLE text
// (std::from_chars, general grammar: "0.1", "1e+05", "-2.5"; the
// SPICE suffix grammar "10k"/"1meg" stays ws::spice::parse_number's).
// Nullopt on trailing junk, empty input or non-finite results - the
// std::atof replacement, but honest about what it consumed.
[[nodiscard]] std::optional<double> parse_plain_number(
    std::string_view text);

}  // namespace ws::circsim
