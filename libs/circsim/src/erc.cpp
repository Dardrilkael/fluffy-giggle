#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

#include <ws/circsim/erc.hpp>
#include <ws/spice/parse.hpp>

namespace ws::circsim {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return out;
}

Diag make(Severity sev, std::string code, std::string message,
          std::string where) {
    Diag d;
    d.severity = sev;
    d.code = std::move(code);
    d.message = std::move(message);
    d.where = std::move(where);
    return d;
}

bool is_identifier(std::string_view s) {
    if (s.empty()) {
        return false;
    }
    for (const char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// "R3 pin b" (or "part 12 pin b" for the ref-less).
std::string part_pin_where(const Schematic& s, PinRef pr,
                           const SymbolLibrary& lib) {
    const Part* p = s.find_part(pr.part);
    const SymbolDef* sym = p != nullptr ? lib.find(p->symbol) : nullptr;
    std::string pin_name =
        sym != nullptr && pr.pin < sym->pins.size()
            ? sym->pins[pr.pin].name
            : std::to_string(pr.pin);
    if (p == nullptr) {
        return "part " + std::to_string(pr.part) + " pin " + pin_name;
    }
    const std::string who =
        p->ref.empty() ? p->symbol + " " + std::to_string(p->id) : p->ref;
    return who + " pin " + pin_name;
}

// --- V/L loops over the net graph ------------------------------------------
// Constraint edges (V sources, inductors) over nets; a cycle - including a
// 2-cycle (parallel edges) or a self-loop (both pins on one net) - is a
// loop. A BFS over the edges seen so far recovers the cycle's members.
void check_constraint_loops(const Schematic& s, const NetList& nets,
                            const SymbolLibrary& lib,
                            std::vector<Diag>& out) {
    struct Edge {
        std::uint32_t net_a, net_b;  // net ids
        std::uint32_t part;
        std::string ref;
    };
    std::vector<Edge> edges;
    for (const Part& p : s.parts()) {
        const SymbolDef* sym = lib.find(p.symbol);
        if (sym == nullptr ||
            (sym->spice_letter != 'V' && sym->spice_letter != 'L')) {
            continue;
        }
        if (sym->pins.size() != 2) {
            continue;
        }
        const Net* a = nets.net_of_pin(PinRef{p.id, 0});
        const Net* b = nets.net_of_pin(PinRef{p.id, 1});
        if (a == nullptr || b == nullptr) {
            continue;
        }
        edges.push_back(
            {a->id, b->id, p.id, p.ref.empty() ? "?" : p.ref});
    }

    // Union-find over net ids; adjacency for cycle recovery.
    std::map<std::uint32_t, std::uint32_t> parent;
    const auto find = [&parent](std::uint32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    const auto ensure = [&parent, &find](std::uint32_t id) {
        if (parent.find(id) == parent.end()) {
            parent.emplace(id, id);
        }
        return find(id);
    };

    for (const Edge& e : edges) {
        ensure(e.net_a);
        ensure(e.net_b);
        const std::uint32_t ra = find(e.net_a);
        const std::uint32_t rb = find(e.net_b);
        if (ra != rb) {
            parent[rb] = ra;
            continue;
        }
        // Cycle: recover its members with a BFS over the earlier edges.
        std::vector<std::uint32_t> members{e.part};
        if (e.net_a != e.net_b) {
            std::map<std::uint32_t, std::uint32_t> prev;
            std::vector<std::uint32_t> queue{e.net_a};
            std::map<std::uint32_t, bool> seen{{e.net_a, true}};
            for (std::size_t qi = 0; qi < queue.size(); ++qi) {
                const std::uint32_t u = queue[qi];
                for (const Edge& f : edges) {
                    if (f.part == e.part) {
                        continue;
                    }
                    if (f.net_a != u && f.net_b != u) {
                        continue;
                    }
                    const std::uint32_t v = f.net_a == u ? f.net_b : f.net_a;
                    if (seen[v]) {
                        continue;
                    }
                    seen[v] = true;
                    prev[v] = f.part;
                    queue.push_back(v);
                }
            }
            for (std::uint32_t v = e.net_b; v != e.net_a;) {
                const auto it = prev.find(v);
                if (it == prev.end()) {
                    break;
                }
                members.push_back(it->second);
                for (const Edge& f : edges) {
                    if (f.part == it->second) {
                        v = f.net_a == v ? f.net_b : f.net_a;
                        break;
                    }
                }
            }
        }
        std::sort(members.begin(), members.end());
        members.erase(std::unique(members.begin(), members.end()),
                      members.end());
        std::string names;
        for (std::uint32_t pid : members) {
            const Part* p = s.find_part(pid);
            if (!names.empty()) {
                names += ", ";
            }
            names += p != nullptr && !p->ref.empty() ? p->ref
                                                     : "?" + std::to_string(pid);
        }
        out.push_back(make(
            Severity::Error, "circsim.erc.vsource_loop",
            "voltage-source/inductor loop through " + names, names));
    }
}

// --- I-source cutsets --------------------------------------------------------
// Remove every I source; any I source then bridging two components of the
// rest crosses an all-current-source cut.
void check_isource_cutsets(const Schematic& s, const NetList& nets,
                           const SymbolLibrary& lib,
                           std::vector<Diag>& out) {
    std::map<std::uint32_t, std::uint32_t> parent;
    const auto find = [&parent](std::uint32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    const auto ensure = [&parent, &find](std::uint32_t id) {
        if (parent.find(id) == parent.end()) {
            parent.emplace(id, id);
        }
        return find(id);
    };

    for (const Net& n : nets.nets) {
        ensure(n.id);
    }
    // Every non-I part unions all its pins' nets.
    for (const Part& p : s.parts()) {
        const SymbolDef* sym = lib.find(p.symbol);
        if (sym == nullptr || sym->spice_letter == 'I') {
            continue;
        }
        std::uint32_t first = 0;
        for (std::uint32_t i = 0; i < sym->pins.size(); ++i) {
            const Net* n = nets.net_of_pin(PinRef{p.id, i});
            if (n == nullptr) {
                continue;
            }
            ensure(n->id);
            if (first == 0) {
                first = n->id;
            } else {
                const std::uint32_t ra = find(first);
                const std::uint32_t rb = find(n->id);
                if (ra != rb) {
                    parent[rb] = ra;
                }
            }
        }
    }
    for (const Part& p : s.parts()) {
        const SymbolDef* sym = lib.find(p.symbol);
        if (sym == nullptr || sym->spice_letter != 'I' ||
            sym->pins.size() != 2) {
            continue;
        }
        const Net* a = nets.net_of_pin(PinRef{p.id, 0});
        const Net* b = nets.net_of_pin(PinRef{p.id, 1});
        if (a == nullptr || b == nullptr) {
            continue;
        }
        if (find(a->id) != find(b->id)) {
            out.push_back(make(
                Severity::Error, "circsim.erc.isource_cutset",
                "current source " + (p.ref.empty() ? "?" : p.ref) +
                    " bridges subcircuits with no DC path between them"
                    " (current-source cutset)",
                p.ref));
        }
    }
}

}  // namespace

std::vector<Diag> run_erc(const Schematic& s, const NetBuild& build,
                          const SymbolLibrary* symbols) {
    std::vector<Diag> out = build.diags;
    const NetList& nets = build.nets;
    const SymbolLibrary& lib =
        symbols != nullptr ? *symbols : builtin_library();

    // --- per-part checks -------------------------------------------------------
    std::map<std::string, std::vector<const Part*>> by_ref;
    for (const Part& p : s.parts()) {
        const std::string where =
            p.ref.empty() ? p.symbol + " " + std::to_string(p.id) : p.ref;
        const SymbolDef* sym = lib.find(p.symbol);
        if (sym == nullptr) {
            out.push_back(make(Severity::Error, "circsim.erc.unknown_symbol",
                               "unknown symbol '" + p.symbol + "'", where));
            continue;
        }
        if (sym->spice_letter == 0) {
            continue;  // power/ground: no ref, no value, no card
        }
        if (p.ref.empty()) {
            out.push_back(make(Severity::Error, "circsim.erc.missing_ref",
                               "part has no reference designator", where));
        } else {
            by_ref[lower(p.ref)].push_back(&p);
            if (!is_identifier(p.ref)) {
                out.push_back(make(
                    Severity::Error, "circsim.erc.bad_ref",
                    "reference designator '" + p.ref +
                        "' is not a valid SPICE name (letters, digits, _)",
                    where));
            } else if (static_cast<char>(std::toupper(
                           static_cast<unsigned char>(p.ref[0]))) !=
                       sym->spice_letter) {
                out.push_back(make(
                    Severity::Error, "circsim.erc.ref_prefix",
                    "reference '" + p.ref + "' must start with '" +
                        std::string(1, sym->spice_letter) +
                        "' to export as its symbol's element kind",
                    where));
            }
        }
        if (sym->needs_value && p.value.empty()) {
            out.push_back(make(Severity::Error, "circsim.erc.missing_value",
                               "part has no value", where));
        }
        if (sym->needs_model) {
            const auto it = p.params.find("model");
            if (it == p.params.end() || it->second.empty()) {
                out.push_back(make(
                    Severity::Error, "circsim.erc.missing_model",
                    "part names no .model (set the 'model' parameter)", where));
            }
        }
        // --- param typing (stage 2): the symbol's specs are the truth ---
        // A non-optional emit entry names a param the card cannot stand
        // without (the T-line's Z0=/TD=).
        for (const ParamEmit& pe : sym->emit) {
            if (pe.optional) {
                continue;
            }
            const auto it = p.params.find(pe.key);
            if (it == p.params.end() || it->second.empty()) {
                out.push_back(make(
                    Severity::Error, "circsim.erc.missing_param",
                    "parameter '" + pe.key + "' is required by symbol '" +
                        sym->key + "'",
                    where));
            }
        }
        for (const ParamSpec& ps : sym->params) {
            const auto it = p.params.find(ps.key);
            if (it == p.params.end() || it->second.empty()) {
                continue;  // absence is ERC's other checks' business
            }
            const std::string shown =
                ps.label.empty() ? ps.key : ps.label;
            if (ps.type == ParamSpec::Type::Number) {
                if (!ws::spice::parse_number(it->second)) {
                    out.push_back(make(
                        Severity::Error, "circsim.erc.bad_param",
                        "parameter '" + shown + "' is '" + it->second +
                            "', not a SPICE number (try 10k, 2.5, 1n)",
                        where));
                }
            } else if (ps.type == ParamSpec::Type::Choice) {
                const std::string value = lower(it->second);
                bool ok = false;
                for (const std::string& c : ps.choices) {
                    if (lower(c) == value) {
                        ok = true;
                        break;
                    }
                }
                if (!ok) {
                    std::string list;
                    for (const std::string& c : ps.choices) {
                        if (!list.empty()) {
                            list += ", ";
                        }
                        list += c;
                    }
                    out.push_back(make(
                        Severity::Warning, "circsim.erc.bad_param",
                        "parameter '" + shown + "' is '" + it->second +
                            "'; allowed: " + list,
                        where));
                }
            }
        }
        // A param the symbol neither specs nor emits can never reach
        // the deck - say so instead of dropping it silently.
        for (const auto& [key, value] : p.params) {
            if (value.empty()) {
                continue;
            }
            bool known = false;
            for (const ParamSpec& ps : sym->params) {
                if (ps.key == key) {
                    known = true;
                    break;
                }
            }
            for (const ParamEmit& pe : sym->emit) {
                if (pe.key == key) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                out.push_back(make(
                    Severity::Warning, "circsim.erc.unused_param",
                    "parameter '" + key +
                        "' is not used by symbol '" + sym->key +
                        "'; it never reaches the netlist",
                    where));
            }
        }
    }
    for (const auto& [ref, group] : by_ref) {
        if (group.size() > 1) {
            std::string names;
            for (const Part* p : group) {
                if (!names.empty()) {
                    names += ", ";
                }
                names += p->ref;
            }
            out.push_back(make(Severity::Error, "circsim.erc.duplicate_ref",
                               "reference designator '" + group.front()->ref +
                                   "' is used by " +
                                   std::to_string(group.size()) + " parts",
                               names));
        }
    }

    // --- net checks --------------------------------------------------------------
    for (const Net& n : nets.nets) {
        const std::string where = "net " + n.name;
        const bool has_attachment =
            !n.wires.empty() || !n.junctions.empty() || !n.labels.empty();
        if (n.pins.empty()) {
            if (!n.is_ground) {
                out.push_back(make(Severity::Warning,
                                   "circsim.erc.empty_net",
                                   "net has no pins (dangling wiring)", where));
            }
        } else if (n.pins.size() == 1 && !n.is_ground) {
            // Fixed-net pins (power/ground) are their own connection.
            const PinRef pr = n.pins.front();
            const Part* p = s.find_part(pr.part);
            const SymbolDef* sym =
                p != nullptr ? lib.find(p->symbol) : nullptr;
            const bool fixed = sym != nullptr && sym->fixed_net.has_value();
            if (!fixed && !has_attachment) {
                out.push_back(make(
                    Severity::Warning, "circsim.erc.floating_pin",
                    part_pin_where(s, pr, lib) + " is not connected to anything",
                    part_pin_where(s, pr, lib)));
            } else if (!fixed) {
                out.push_back(make(
                    Severity::Warning, "circsim.erc.one_pin_net",
                    "net " + n.name + " has only one connection",
                    where));
            }
        }
    }

    // Missing ground: nets exist but none is "0".
    if (!nets.nets.empty() && nets.ground() == nullptr) {
        out.push_back(make(Severity::Error, "circsim.erc.missing_ground",
                           "no ground: every schematic needs a ground (net 0)",
                           ""));
    }

    check_constraint_loops(s, nets, lib, out);
    check_isource_cutsets(s, nets, lib, out);
    return out;
}

std::vector<Diag> run_erc(const Schematic& s, const SymbolLibrary* symbols) {
    const NetBuild build = build_nets(s, symbols);
    return run_erc(s, build, symbols);
}

}  // namespace ws::circsim
