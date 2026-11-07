// apps/circsim_editor - the Palette's grouping + search (see the header).

#include "palette_groups.hpp"

#include <algorithm>
#include <array>
#include <cctype>

#include <ws/settings/json.hpp>

namespace editor {

std::string palette_category(const ws::circsim::SymbolDef& sym) {
    if (const ws::settings::json::Value* v = sym.unknown.find("category")) {
        if (const std::string* text = v->as_text();
            text != nullptr && !text->empty()) {
            return *text;
        }
    }
    switch (sym.spice_letter) {
        case 'R':
        case 'C':
        case 'L':
            return "Passive";
        case 'V':
        case 'I':
            return "Sources";
        case 'D':
        case 'Q':
        case 'M':
        case 'J':
            return "Semiconductors";
        default:
            break;
    }
    if (sym.spice_letter == 0 || sym.fixed_net.has_value()) {
        return "Power";
    }
    return "Other";
}

// A case-insensitive (ASCII) fuzzy match, the acronym-then-subsequence
// shape: the FIRST needle character must land on a word start (the
// haystack's first character, or one after a non-alphanumeric), and
// the rest may skip letters in between - "res" and "rstr" both find
// "Resistor", "vsrc" finds "Voltage source", while "res" does NOT
// drag in "Current source" (its r sits mid-word) and "nmos" leaves
// "P-channel MOSFET" out. A superset of the old substring match, so
// every query that worked before keeps working; an empty needle
// matches.
[[nodiscard]] bool palette_matches(std::string_view haystack,
                                   std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    const auto lower = [](char c) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    };
    std::size_t at = 0;  // the next needle character to place
    for (std::size_t i = 0; i < haystack.size() && at < needle.size();
         ++i) {
        const bool starts_word =
            i == 0 || !std::isalnum(static_cast<unsigned char>(
                                       haystack[i - 1]));
        // The first character is the anchor: it only lands on a word
        // start, so a stray mid-word letter never opens a match. The
        // characters after it are free to skip (the fuzzy half).
        if (lower(haystack[i]) == lower(needle[at]) &&
            (at > 0 || starts_word)) {
            ++at;
        }
    }
    return at == needle.size();
}

std::vector<PaletteGroup> palette_groups(const ws::circsim::SymbolLibrary& lib,
                                         std::string_view filter) {
    static constexpr std::array<const char*, 4> kOrder = {
        "Passive", "Sources", "Semiconductors", "Power"};
    std::vector<PaletteGroup> groups;
    for (const char* name : kOrder) {
        groups.push_back(PaletteGroup{name, {}});
    }
    for (const ws::circsim::SymbolDef& sym : lib.symbols()) {
        const std::string category = palette_category(sym);
        if (!palette_matches(sym.name, filter) &&
            !palette_matches(sym.key, filter) &&
            !palette_matches(category, filter)) {
            continue;
        }
        auto it = std::find_if(
            groups.begin(), groups.end(),
            [&](const PaletteGroup& g) { return g.name == category; });
        if (it == groups.end()) {
            groups.push_back(PaletteGroup{category, {}});
            it = groups.end() - 1;
        }
        it->items.push_back(&sym);
    }
    groups.erase(std::remove_if(groups.begin(), groups.end(),
                                [](const PaletteGroup& g) {
                                    return g.items.empty();
                                }),
                 groups.end());
    // "Other" is the catch-all: always last, after any custom group.
    std::stable_partition(groups.begin(), groups.end(),
                          [](const PaletteGroup& g) {
                              return g.name != "Other";
                          });
    return groups;
}

}  // namespace editor
