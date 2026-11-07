#pragma once

// apps/circsim_editor - the Palette's grouping + search, as pure data
// (no gui, no canvas) so the headless suite can pin it.
//
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

namespace editor {

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

// The groups for `filter` (empty = everything). The pointers borrow
// from `lib` - valid until the library changes.
[[nodiscard]] std::vector<PaletteGroup> palette_groups(
    const ws::circsim::SymbolLibrary& lib, std::string_view filter);

}  // namespace editor
