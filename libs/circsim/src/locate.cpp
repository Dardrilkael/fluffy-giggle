#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include <ws/circsim/export.hpp>
#include <ws/circsim/locate.hpp>

#include "util.hpp"

// The Problems panel's jump seam (stage 3). The PREFERRED path: a
// producer that knew its object filled Diag::target at the source, and
// locate_diag answers it verbatim - no parsing, no grammar to keep
// true. The LEGACY path (below) parses the `where` spelling back into
// the object, for producers that have not migrated; its shapes, all in
// one place (keep this table true or the panel jumps nowhere):
//
//   erc.cpp        "R3" | "npn 7" | "R3 pin b" | "npn 7 pin c"
//                  "net N004" | "V1, L1, V2" (loops) | "" (ground)
//   nets.cpp       "wire 5" | "label out" | "net carries labels ..."
//   export.cpp     "part 12" | "part 12 pin b" | "net N003"
//                  "directives" | "analysis setup" | "simulation setup"
//                  (the temp card's origin; the Analysis block)
//
// Element names resolve through the export back-map FIRST (the mangled
// included - a part whose ref failed ERC still exports under a
// deterministic name), then against the schematic's own refs, then by
// the "symbol id"/"part id" spellings. Case-insensitive throughout:
// the refs and the net names both compare ci (the CiLess precedents).

namespace ws::circsim {
namespace {

// One token of a (possibly comma-list) where: the part's spelling plus
// the pin tail when present. "R3 pin b" -> {"R3", "pin b"}; the tail
// keeps its spaces so "pin b" parses below.
struct Token {
    std::string_view head;  // "R3" / "npn 7" / "part 12"
    std::string_view tail;  // " pin b" (leading space kept, "" = none)
};

// Splits a comma list into raw tokens, trimmed.
[[nodiscard]] std::vector<std::string_view> split_list(
    std::string_view where) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    while (at <= where.size()) {
        const std::size_t comma = where.find(',', at);
        const std::string_view raw =
            where.substr(at, comma == std::string_view::npos
                                 ? std::string_view::npos
                                 : comma - at);
        std::size_t begin = 0;
        std::size_t end = raw.size();
        while (begin < end && raw[begin] == ' ') {
            ++begin;
        }
        while (end > begin && raw[end - 1] == ' ') {
            --end;
        }
        if (end > begin) {
            out.push_back(raw.substr(begin, end - begin));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        at = comma + 1;
    }
    return out;
}

// "R3 pin b" -> head "R3", tail " pin b". The FIRST " pin " is the
// split (a pin NAME never contains " pin ", and a ref cannot either -
// ERC's is_identifier allows letters/digits/underscore only).
[[nodiscard]] Token split_pin(std::string_view token) {
    const std::size_t at = token.find(" pin ");
    if (at == std::string_view::npos) {
        return {token, {}};
    }
    return {token.substr(0, at), token.substr(at)};
}

// The trailing id of "npn 7" / "part 12": the digits after the last
// space, when the whole tail is digits.
[[nodiscard]] std::optional<std::uint32_t> trailing_id(
    std::string_view head) {
    const std::size_t space = head.rfind(' ');
    if (space == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view digits = head.substr(space + 1);
    if (digits.empty()) {
        return std::nullopt;
    }
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(
        std::strtoul(digits.data(), nullptr, 10));
}

// The "symbol id" spelling: a head with a space-digits tail whose
// front names a symbol key in the table (ci).
[[nodiscard]] const Part* part_by_symbol_id(std::string_view head,
                                            const Schematic& s,
                                            const SymbolLibrary& lib) {
    const std::optional<std::uint32_t> id = trailing_id(head);
    if (!id.has_value()) {
        return nullptr;
    }
    const std::string_view key = head.substr(0, head.rfind(' '));
    const SymbolDef* sym = lib.find(key);
    if (sym == nullptr) {
        return nullptr;  // a "part 12" or unknown spelling
    }
    return s.find_part(*id);
}

[[nodiscard]] const Part* part_by_ref(std::string_view ref,
                                      const Schematic& s) {
    const std::string wanted = lower(ref);
    for (const Part& p : s.parts()) {
        if (lower(p.ref) == wanted) {
            return &p;
        }
    }
    return nullptr;
}

// Resolves one part-spelling token to a part: the export back-map (the
// mangled included), then the schematic's own refs (ci), then "symbol
// id", then the raw "part id" (the export's dangling spelling - live
// ids answer, a gone part answers null and the row shows without a
// target).
[[nodiscard]] const Part* resolve_part(std::string_view head,
                                       const Schematic& s,
                                       const SpiceExport* map,
                                       const SymbolLibrary& lib) {
    if (map != nullptr) {
        const auto it = map->element_part.find(std::string{head});
        if (it != map->element_part.end()) {
            const Part* p = s.find_part(it->second);
            if (p != nullptr) {
                return p;
            }
        }
    }
    if (const Part* by_ref = part_by_ref(head, s)) {
        return by_ref;
    }
    if (const Part* by_sym = part_by_symbol_id(head, s, lib)) {
        return by_sym;
    }
    if (head.rfind("part ", 0) == 0) {
        const std::optional<std::uint32_t> id = trailing_id(head);
        if (id.has_value()) {
            return s.find_part(*id);
        }
    }
    return nullptr;
}

// " pin b" -> the pin index: by name (ci) through the symbol, falling
// back to a numeric index (" pin 2"). Answers npos when unresolvable.
[[nodiscard]] std::size_t resolve_pin(std::string_view tail,
                                      const SymbolDef& sym) {
    std::size_t begin = 0;
    while (begin < tail.size() && tail[begin] == ' ') {
        ++begin;
    }
    if (tail.compare(begin, 4, "pin ") != 0) {
        return DiagTarget::npos();
    }
    const std::string_view name = tail.substr(begin + 4);
    if (name.empty()) {
        return DiagTarget::npos();
    }
    const std::string wanted = lower(name);
    for (std::size_t i = 0; i < sym.pins.size(); ++i) {
        if (lower(sym.pins[i].name) == wanted) {
            return i;
        }
    }
    // A numeric spelling (the ref-less part fallback in erc.cpp).
    for (const char c : name) {
        if (c < '0' || c > '9') {
            return DiagTarget::npos();
        }
    }
    const unsigned long index =
        std::strtoul(std::string{name}.c_str(), nullptr, 10);
    return index < sym.pins.size() ? static_cast<std::size_t>(index)
                                   : DiagTarget::npos();
}

// One whole where-token (head + optional pin tail) to a target.
[[nodiscard]] DiagTarget locate_token(std::string_view token,
                                      const Schematic& s,
                                      const NetList* nets,
                                      const SpiceExport* map,
                                      const SymbolLibrary& lib) {
    DiagTarget out;
    if (token == "directives") {
        out.kind = DiagTarget::Kind::Directives;
        return out;
    }
    if (token == "analysis setup") {
        out.kind = DiagTarget::Kind::Analysis;
        return out;
    }
    // The temperature row's origin (the .options temp= card the
    // Simulation tab emits): the same no-geometry block the analysis
    // setup rows land on.
    if (token == "simulation setup") {
        out.kind = DiagTarget::Kind::Analysis;
        return out;
    }
    if (token.rfind("net ", 0) == 0) {
        const std::string_view name = token.substr(4);
        if (!name.empty()) {
            if (nets != nullptr) {
                if (const Net* n = nets->by_name(name)) {
                    out.kind = DiagTarget::Kind::Net;
                    out.net = n->id;
                    return out;
                }
            }
            if (map != nullptr) {
                const auto it = map->node_net.find(std::string{name});
                if (it != map->node_net.end()) {
                    out.kind = DiagTarget::Kind::Net;
                    out.net = it->second;
                    return out;
                }
            }
        }
        return out;  // unresolvable name: None
    }
    if (token.rfind("wire ", 0) == 0) {
        const std::optional<std::uint32_t> id = trailing_id(token);
        if (id.has_value() && s.find_wire(*id) != nullptr) {
            out.kind = DiagTarget::Kind::Wire;
            out.obj = *id;
        }
        return out;
    }
    if (token.rfind("label ", 0) == 0) {
        const std::string wanted = lower(token.substr(6));
        for (const Label& l : s.labels()) {
            if (lower(l.name) == wanted) {
                out.kind = DiagTarget::Kind::Label;
                out.obj = l.id;
                return out;
            }
        }
        return out;
    }

    const Token split = split_pin(token);
    const Part* p = resolve_part(split.head, s, map, lib);
    if (p == nullptr) {
        return out;
    }
    out.kind = DiagTarget::Kind::Part;
    out.part = p->id;
    if (!split.tail.empty()) {
        const SymbolDef* sym = lib.find(p->symbol);
        out.pin = sym != nullptr ? resolve_pin(split.tail, *sym)
                                 : DiagTarget::npos();
    }
    return out;
}

// A wire's polyline: the two endpoints resolved through their refs
// (a dangling ref contributes nothing - the loader already flagged it)
// plus the cosmetic waypoints between them.
template <typename Grow>
void grow_wire(const Wire& w, const Schematic& s,
               const SymbolLibrary& lib, Grow&& grow) {
    for (const WireEndpoint& end : {w.a, w.b}) {
        if (const PinRef* pin = std::get_if<PinRef>(&end)) {
            const Part* p = s.find_part(pin->part);
            const SymbolDef* sym =
                p != nullptr ? lib.find(p->symbol) : nullptr;
            if (p != nullptr && sym != nullptr &&
                pin->pin < sym->pins.size()) {
                const Pos at = pin_pos(*sym, *p, pin->pin);
                grow(at.x, at.y);
            }
        } else if (const JunctionRef* j = std::get_if<JunctionRef>(&end)) {
            const Junction* found = s.find_junction(j->junction);
            if (found != nullptr) {
                grow(found->pos.x, found->pos.y);
            }
        }
    }
    for (const Pos& p : w.waypoints) {
        grow(p.x, p.y);
    }
}

}  // namespace

DiagTarget locate_diag(const Diag& d, const Schematic& s,
                       const NetList* nets, const SpiceExport* map,
                       const SymbolLibrary* symbols) {
    // The structured path: a producer that knew its object. Answers
    // verbatim - no parse, no grammar.
    if (d.target.kind != DiagTarget::Kind::None) {
        return d.target;
    }
    const SymbolLibrary& lib =
        symbols != nullptr ? *symbols : builtin_library();
    if (d.where.empty()) {
        return {};  // file-level: nothing to select
    }
    for (const std::string_view token : split_list(d.where)) {
        const DiagTarget t =
            locate_token(token, s, nets, map, lib);
        if (t.kind != DiagTarget::Kind::None) {
            return t;
        }
    }
    return {};
}

std::optional<Bounds> focus_bounds(const DiagTarget& t,
                                   const Schematic& s,
                                   const NetList* supplied_nets,
                                   const SymbolLibrary* symbols) {
    const SymbolLibrary& lib =
        symbols != nullptr ? *symbols : builtin_library();
    Bounds b;
    bool any = false;
    const auto grow = [&](double x, double y) {
        if (!any) {
            b = {x, y, x, y};
            any = true;
            return;
        }
        b.min_x = std::min(b.min_x, x);
        b.min_y = std::min(b.min_y, y);
        b.max_x = std::max(b.max_x, x);
        b.max_y = std::max(b.max_y, y);
    };

    switch (t.kind) {
        case DiagTarget::Kind::Part: {
            const Part* p = s.find_part(t.part);
            const SymbolDef* sym =
                p != nullptr ? lib.find(p->symbol) : nullptr;
            if (p == nullptr) {
                break;
            }
            if (sym == nullptr) {
                grow(p->pos.x, p->pos.y);  // an unknown symbol: the
                break;                     // origin is all there is
            }
            const Bounds local = symbol_bounds(*sym);
            const Pos corners[4] = {
                transform_pos(*p, Pos{local.min_x, local.min_y}),
                transform_pos(*p, Pos{local.max_x, local.min_y}),
                transform_pos(*p, Pos{local.min_x, local.max_y}),
                transform_pos(*p, Pos{local.max_x, local.max_y}),
            };
            for (const Pos& c : corners) {
                grow(c.x, c.y);
            }
            break;
        }
        case DiagTarget::Kind::Net: {
            // The members' positions: through the caller's NetList when
            // it has one (the editor's ERC cache), else one built here
            // (a click-path cost, never a frame path).
            std::optional<NetBuild> built;
            const NetList* nets = supplied_nets;
            if (nets == nullptr) {
                built = build_nets(s, &lib);
                nets = &built->nets;
            }
            const Net* n = nullptr;
            for (const Net& candidate : nets->nets) {
                if (candidate.id == t.net) {
                    n = &candidate;
                    break;
                }
            }
            if (n == nullptr) {
                break;
            }
            for (const PinRef& pr : n->pins) {
                const Part* p = s.find_part(pr.part);
                const SymbolDef* sym =
                    p != nullptr ? lib.find(p->symbol) : nullptr;
                if (p != nullptr && sym != nullptr &&
                    pr.pin < sym->pins.size()) {
                    const Pos at = pin_pos(*sym, *p, pr.pin);
                    grow(at.x, at.y);
                }
            }
            for (const std::uint32_t id : n->junctions) {
                if (const Junction* j = s.find_junction(id)) {
                    grow(j->pos.x, j->pos.y);
                }
            }
            for (const std::uint32_t id : n->labels) {
                if (const Label* l = s.find_label(id)) {
                    grow(l->pos.x, l->pos.y);
                }
            }
            for (const std::uint32_t id : n->wires) {
                if (const Wire* w = s.find_wire(id)) {
                    grow_wire(*w, s, lib, grow);
                }
            }
            break;
        }
        case DiagTarget::Kind::Wire: {
            if (const Wire* w = s.find_wire(t.obj)) {
                grow_wire(*w, s, lib, grow);
            }
            break;
        }
        case DiagTarget::Kind::Label: {
            if (const Label* l = s.find_label(t.obj)) {
                // One unit of air so a fit is never a degenerate point.
                grow(l->pos.x - 1.0, l->pos.y - 1.0);
                grow(l->pos.x + 1.0, l->pos.y + 1.0);
            }
            break;
        }
        case DiagTarget::Kind::Directives:
        case DiagTarget::Kind::Analysis:
        case DiagTarget::Kind::None:
            break;
    }
    if (!any) {
        return std::nullopt;
    }
    return b;
}

}  // namespace ws::circsim
