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

bool palette_matches(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    const auto same = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    };
    return std::search(haystack.begin(), haystack.end(), needle.begin(),
                       needle.end(), same) != haystack.end();
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
