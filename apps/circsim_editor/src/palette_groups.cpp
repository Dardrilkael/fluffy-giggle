// apps/circsim_editor - the Palette's grouping + search (see the header).

#include "palette_groups.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>

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
        case 'T':  // the lossless line is a passive two-port
        case 'K':  // coupling and the transformer: magnetics passives
            return "Passive";
        case 'V':
        case 'I':
        case 'E':  // the controlled and behavioral sources are sources
        case 'F':
        case 'G':
        case 'H':
        case 'B':
            return "Sources";
        case 'D':
        case 'Q':
        case 'M':
        case 'J':
        case 'X':  // the op amp: the subckt instance is a semiconductor
            return "Semiconductors";
        case 'S':  // the switches get their own shelf
        case 'W':
            return "Switches";
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
bool palette_matches(std::string_view haystack, std::string_view needle) {
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

int palette_score(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return 0;
    }
    if (!palette_matches(haystack, needle)) {
        return -1;
    }
    const auto ieq = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    };
    // Tiers: 4 exact, 3 prefix, 2 whole needle contiguous from a word
    // start ("source" in "Voltage source"), 1 loose subsequence.
    int tier = 1;
    if (haystack.size() == needle.size() &&
        std::equal(needle.begin(), needle.end(), haystack.begin(), ieq)) {
        tier = 4;
    } else if (haystack.size() >= needle.size() &&
               std::equal(needle.begin(), needle.end(), haystack.begin(),
                          ieq)) {
        tier = 3;
    } else {
        for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
            const bool starts_word =
                i == 0 || !std::isalnum(static_cast<unsigned char>(
                                           haystack[i - 1]));
            if (starts_word &&
                std::equal(needle.begin(), needle.end(),
                           haystack.begin() + static_cast<std::ptrdiff_t>(i),
                           ieq)) {
                tier = 2;
                break;
            }
        }
    }
    // Shorter haystacks first inside a tier: "Resistor" beats
    // "Resistor (variable)" for "res".
    const int len = static_cast<int>(std::min<std::size_t>(haystack.size(), 99));
    return tier * 100 + (99 - len);
}

std::vector<PaletteGroup> palette_groups(const ws::circsim::SymbolLibrary& lib,
                                         std::string_view filter) {
    static constexpr std::array<const char*, 5> kOrder = {
        "Passive", "Sources", "Semiconductors", "Switches", "Power"};
    std::vector<PaletteGroup> groups;
    for (const char* name : kOrder) {
        groups.push_back(PaletteGroup{name, {}});
    }
    // Relevance per symbol (filtering only): the best of name and key,
    // or half the category's score (only if category matches) - a hit on
    // what the part IS outranks a hit on the group it sits in.
    std::unordered_map<const ws::circsim::SymbolDef*, int> score;
    for (const ws::circsim::SymbolDef& sym : lib.symbols()) {
        const std::string category = palette_category(sym);
        const int name_score = palette_score(sym.name, filter);
        const int key_score = palette_score(sym.key, filter);
        const int cat_raw = palette_score(category, filter);
        const int cat_score = cat_raw >= 0 ? cat_raw / 2 : -1;
        const int s = std::max({name_score, key_score, cat_score});
        if (s < 0) {
            continue;
        }
        score[&sym] = s;
        auto it = std::find_if(
            groups.begin(), groups.end(),
            [&](const PaletteGroup& g) { return g.name == category; });
        if (it == groups.end()) {
            groups.push_back(PaletteGroup{category, {}});
            it = groups.end() - 1;
        }
        it->items.push_back(&sym);
    }
    if (!filter.empty()) {
        // A search ranks: the best matches lead inside each group and
        // the group holding the best match leads the list, so the first
        // tile is the one Enter arms. Ties keep library / group order.
        const auto best = [&score](const PaletteGroup& g) {
            int top = -1;
            for (const ws::circsim::SymbolDef* s : g.items) {
                top = std::max(top, score[s]);
            }
            return top;
        };
        for (PaletteGroup& g : groups) {
            std::stable_sort(g.items.begin(), g.items.end(),
                             [&score](const ws::circsim::SymbolDef* a,
                                      const ws::circsim::SymbolDef* b) {
                                 return score[a] > score[b];
                             });
        }
        std::stable_sort(groups.begin(), groups.end(),
                         [&best](const PaletteGroup& a,
                                 const PaletteGroup& b) {
                             return best(a) > best(b);
                         });
    } else {
        // No filter: drop empty groups (the "Other" partition below
        // still runs so a custom "Other" group lands last).
        groups.erase(std::remove_if(groups.begin(), groups.end(),
                                    [](const PaletteGroup& g) {
                                        return g.items.empty();
                                    }),
                     groups.end());
    }
    // "Other" is the catch-all: always last, after any custom group.
    std::stable_partition(groups.begin(), groups.end(),
                          [](const PaletteGroup& g) {
                              return g.name != "Other";
                          });
    return groups;
}

}  // namespace editor
