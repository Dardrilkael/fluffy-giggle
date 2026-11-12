#pragma once

// ws::circsim - the symbol palette's grouping + search, as pure data
// over the symbol library (no gui, no canvas). Lifted from
// apps/circsim_editor (stage 11 - the lib extraction): the panel
// remains the app's; the grouping POLICY - the categories, the fuzzy
// filter, the ranking - is library domain (any frontend building a
// parts browser over a .csym library rides here).
// A symbol's group is, in order:
//   1. the "category" text field a .csym symbol carries (it rides in
//      SymbolDef::unknown, so a user library can name its own groups);
//   2. else derived from the SPICE letter - R/C/L Passive, V/I Sources,
//      D/Q/M/J Semiconductors, no letter or a fixed net Power;
//   3. else "Other".
// The built-in groups come first in that fixed order, then any custom
// ones in first-seen order, then "Other" last; empty groups are
// dropped. The filter is the fuzzy word-start match over the name,
// the key and the group (see palette_matches).

#include <string>
#include <string_view>
#include <vector>

#include <ws/circsim/library.hpp>
#include <ws/circsim/symbols.hpp>

namespace ws::circsim {

struct PaletteGroup {
    std::string name;
    std::vector<const ws::circsim::SymbolDef*> items;  // library order
};

[[nodiscard]] std::string palette_category(const ws::circsim::SymbolDef& sym);

// Case-insensitive (ASCII) fuzzy match, the acronym-then-subsequence
// shape: the first needle character must land on a word start (the
// first character, or one after a non-alphanumeric), the rest may
// skip letters in between - "res" and "rstr" both find "Resistor",
// "vsrc" finds "Voltage source", while an out-of-order "se" does
// not. A superset of the old substring match - every query that
// worked before still works. An empty needle matches.
[[nodiscard]] bool palette_matches(std::string_view haystack,
                                   std::string_view needle);

// How well `needle` matches `haystack`: -1 for no match (exactly when
// palette_matches says no), else higher is better - exact, then prefix,
// then a whole-needle hit at a word start, then a loose subsequence;
// shorter haystacks rank first inside a tier. An empty needle scores 0.
[[nodiscard]] int palette_score(std::string_view haystack,
                                std::string_view needle);

// The groups for `filter` (empty = everything). With a filter the
// result is RANKED: best matches first inside each group, and the group
// holding the best match first (a custom "Other" still goes last), so
// the first tile is what Enter arms. Without one: library order. The pointers borrow
// from `lib` - valid until the library changes.
[[nodiscard]] std::vector<PaletteGroup> palette_groups(
    const ws::circsim::SymbolLibrary& lib, std::string_view filter);

}  // namespace ws::circsim
