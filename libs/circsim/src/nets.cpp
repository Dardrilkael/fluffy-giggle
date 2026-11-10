#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ws/circsim/nets.hpp>

#include "util.hpp"

namespace ws::circsim {

namespace {

// lower rides util.hpp (the one ASCII-only, locale-independent
// definition); the diagnostic factory is make_diag (diag.hpp), every
// finding below filling Diag::target alongside its display `where`.

// --- union-find --------------------------------------------------------------

struct Dsu {
    std::vector<std::uint32_t> parent;

    explicit Dsu(std::size_t n) : parent(n) {
        for (std::size_t i = 0; i < n; ++i) {
            parent[i] = static_cast<std::uint32_t>(i);
        }
    }
    [[nodiscard]] std::uint32_t find(std::uint32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }
    void unite(std::uint32_t a, std::uint32_t b) {
        a = find(a);
        b = find(b);
        if (a != b) {
            parent[b] = a;
        }
    }
};

// --- geometry (exact: grid coordinates are integer-valued doubles) ------------

struct PosKey {
    double x, y;
    bool operator<(const PosKey& o) const {
        return x != o.x ? x < o.x : y < o.y;
    }
};

bool on_segment(Pos a, Pos b, Pos p) {
    const double cross =
        (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    if (cross != 0.0) {
        return false;
    }
    return p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x) &&
           p.y >= std::min(a.y, b.y) && p.y <= std::max(a.y, b.y);
}

bool on_polyline(const std::vector<Pos>& path, Pos p) {
    for (std::size_t i = 1; i < path.size(); ++i) {
        if (on_segment(path[i - 1], path[i], p)) {
            return true;
        }
    }
    return false;
}

bool same_pos(Pos a, Pos b) {
    return a.x == b.x && a.y == b.y;
}

bool is_vertex(const std::vector<Pos>& path, Pos p) {
    for (const Pos& v : path) {
        if (v.x == p.x && v.y == p.y) {
            return true;
        }
    }
    return false;
}

// --- entities ------------------------------------------------------------------

struct Entity {
    enum class Kind { Pin, Junction, Label };
    Kind kind = Kind::Pin;
    PinRef pin{};              // Pin
    std::uint32_t obj_id = 0;  // Junction / Label (and the Pin's part id
                               // rides pin.part)
    Pos pos{0.0, 0.0};
};

[[nodiscard]] std::uint64_t pin_key(PinRef r) {
    return (static_cast<std::uint64_t>(r.part) << 32) | r.pin;
}

// --- the segment candidate grid ----------------------------------------------
// The wire-vertex-on-wire and pin-on-wire coincidence rules are O(W^2)
// brute force below kGridThreshold wires (every test-sized schematic -
// zero behavior change), and a uniform grid over segment AABBs above
// it: each segment lands in every cell its bounding box overlaps (a
// pathological box covering more than kMaxCells cells rides the
// always-checked oversize list instead), and a point query touches one
// cell. Candidates come back sorted by (wire, segment) so the loops
// below visit exactly the pairs the brute force would, in the same
// order - identical unions, identical silent joins, just fewer
// on_polyline calls to get there.

struct SegRef {
    std::size_t wire;
    std::size_t seg;  // 1..path.size()-1 (the segment path[seg-1]..path[seg])
};

struct SegGrid {
    std::map<std::pair<std::int64_t, std::int64_t>, std::vector<SegRef>>
        cells;
    std::vector<SegRef> oversize;
    double cell = 16.0;

    static constexpr int kMaxCells = 64;  // per segment, each axis

    void insert(std::size_t wire, const std::vector<Pos>& path) {
        for (std::size_t seg = 1; seg < path.size(); ++seg) {
            const Pos& a = path[seg - 1];
            const Pos& b = path[seg];
            const double lo_x = std::min(a.x, b.x);
            const double hi_x = std::max(a.x, b.x);
            const double lo_y = std::min(a.y, b.y);
            const double hi_y = std::max(a.y, b.y);
            const auto cell_of = [&](double v) {
                return static_cast<std::int64_t>(std::floor(v / cell));
            };
            const std::int64_t cx0 = cell_of(lo_x);
            const std::int64_t cx1 = cell_of(hi_x);
            const std::int64_t cy0 = cell_of(lo_y);
            const std::int64_t cy1 = cell_of(hi_y);
            if ((cx1 - cx0 + 1) * (cy1 - cy0 + 1) > kMaxCells) {
                oversize.push_back({wire, seg});
                continue;
            }
            for (std::int64_t cx = cx0; cx <= cx1; ++cx) {
                for (std::int64_t cy = cy0; cy <= cy1; ++cy) {
                    cells[{cx, cy}].push_back({wire, seg});
                }
            }
        }
    }

    // Every segment whose AABB contains p (its cell, plus the oversize
    // list), sorted by (wire, seg).
    void query(Pos p, std::vector<SegRef>& out) const {
        out.clear();
        const auto cx = static_cast<std::int64_t>(std::floor(p.x / cell));
        const auto cy = static_cast<std::int64_t>(std::floor(p.y / cell));
        const auto it = cells.find({cx, cy});
        if (it != cells.end()) {
            out.insert(out.end(), it->second.begin(), it->second.end());
        }
        out.insert(out.end(), oversize.begin(), oversize.end());
        std::sort(out.begin(), out.end(), [](const SegRef& a,
                                             const SegRef& b) {
            return a.wire != b.wire ? a.wire < b.wire : a.seg < b.seg;
        });
    }
};

constexpr std::size_t kGridThreshold = 500;

}  // namespace

NetBuild build_nets(const Schematic& s, const SymbolLibrary* symbols) {
    NetBuild out;
    const SymbolLibrary& lib =
        symbols != nullptr ? *symbols : builtin_library();

    // --- entities: pins of known-symbol parts, junctions, labels -------------
    std::vector<Entity> ents;
    std::unordered_map<std::uint64_t, std::uint32_t> pin_ent;
    std::unordered_map<std::uint32_t, std::uint32_t> junction_ent;
    std::unordered_map<std::uint32_t, std::uint32_t> label_ent;
    std::map<PosKey, std::vector<std::uint32_t>> by_pos;  // pins + junctions

    const auto add_entity = [&](Entity e) {
        ents.push_back(e);
        return static_cast<std::uint32_t>(ents.size() - 1);
    };

    for (const Part& p : s.parts()) {
        const SymbolDef* sym = lib.find(p.symbol);
        if (sym == nullptr) {
            continue;  // the unknown-symbol diagnostic is ERC's to make
        }
        for (std::uint32_t i = 0; i < sym->pins.size(); ++i) {
            const std::uint32_t e = add_entity(
                {Entity::Kind::Pin, PinRef{p.id, i}, 0, pin_pos(*sym, p, i)});
            pin_ent[pin_key(PinRef{p.id, i})] = e;
            by_pos[{ents[e].pos.x, ents[e].pos.y}].push_back(e);
        }
    }
    for (const Junction& j : s.junctions()) {
        const std::uint32_t e =
            add_entity({Entity::Kind::Junction, PinRef{}, j.id, j.pos});
        junction_ent[j.id] = e;
        by_pos[{j.pos.x, j.pos.y}].push_back(e);
    }
    for (const Label& l : s.labels()) {
        label_ent[l.id] =
            add_entity({Entity::Kind::Label, PinRef{}, l.id, l.pos});
    }

    Dsu dsu(ents.size());

    // --- the silent-join recorder (stage 18) ----------------------------------
    // Positions where the coincidence rules below connect things with no
    // authored junction dot. An explicit junction anywhere suppresses the
    // record (it draws its own dot); one entry per position, a pin entry
    // replacing a wire entry at the same spot. The position index beside
    // the vector answers "already recorded here?" in one lookup (the
    // old linear rescan made a many-join build quadratic).
    std::set<PosKey> junction_at;
    for (const Junction& j : s.junctions()) {
        junction_at.insert({j.pos.x, j.pos.y});
    }
    std::map<PosKey, std::size_t> join_at;
    const auto record_join = [&](SilentJoin join) {
        const PosKey key{join.pos.x, join.pos.y};
        const auto it = join_at.find(key);
        if (it != join_at.end()) {
            // Upgrade a wire entry to the pin that names it; a second
            // wire at the same spot changes nothing (the first answer
            // stands).
            SilentJoin& seen = out.silent_joins[it->second];
            if (join.kind == SilentJoin::Kind::PinOnWire &&
                seen.kind == SilentJoin::Kind::WireOnWire) {
                seen = join;
            }
            return;
        }
        join_at.emplace(key, out.silent_joins.size());
        out.silent_joins.push_back(join);
    };

    // --- wires: validate, path, union endpoints --------------------------------
    std::vector<const Wire*> wires;
    std::vector<std::vector<Pos>> paths;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> wire_ents;

    const auto endpoint_ent = [&](const WireEndpoint& ep,
                                  bool& ok) -> std::uint32_t {
        if (const PinRef* pin = std::get_if<PinRef>(&ep)) {
            const auto it = pin_ent.find(pin_key(*pin));
            if (it == pin_ent.end()) {
                ok = false;
                return 0;
            }
            return it->second;
        }
        const auto it = junction_ent.find(std::get<JunctionRef>(ep).junction);
        if (it == junction_ent.end()) {
            ok = false;
            return 0;
        }
        return it->second;
    };

    for (const Wire& w : s.wires()) {
        bool ok_a = true;
        bool ok_b = true;
        const std::uint32_t ea = endpoint_ent(w.a, ok_a);
        const std::uint32_t eb = endpoint_ent(w.b, ok_b);
        if (!ok_a || !ok_b) {
            out.diags.push_back(make_diag(
                Severity::Warning,
                "circsim.nets.dangling_wire",
                "wire " + std::to_string(w.id) +
                    " ends on a pin or junction that does not exist; it is"
                    " dropped from connectivity",
                "wire " + std::to_string(w.id),
                DiagTarget{DiagTarget::Kind::Wire, 0, DiagTarget::npos(), 0,
                           w.id}));
            continue;
        }
        std::vector<Pos> path;
        path.reserve(w.waypoints.size() + 2);
        path.push_back(ents[ea].pos);
        for (const Pos& p : w.waypoints) {
            path.push_back(p);
        }
        path.push_back(ents[eb].pos);
        wires.push_back(&w);
        paths.push_back(std::move(path));
        wire_ents.emplace_back(ea, eb);
        dsu.unite(ea, eb);
    }

    // --- exact-position coincidence among pins/junctions -----------------------
    for (const auto& [key, group] : by_pos) {
        for (std::size_t i = 1; i < group.size(); ++i) {
            dsu.unite(group[0], group[i]);
        }
    }

    // --- wire vertices on other wires (the T-junction rule) --------------------
    // The candidate set comes either from the brute-force scan (small
    // schematics - exactly the historical behavior) or the segment
    // grid (large ones); both feed the same per-(vertex, wire) body.
    //
    // One wire's vertex v landing on wire wj.
    const auto vertex_on_wire = [&](std::size_t wi, const Pos& v,
                                    std::size_t wj) {
        dsu.unite(wire_ents[wi].first, wire_ents[wj].first);
        // v on wj's path INTERIOR (not its endpoints), with no
        // junction drawn there: a merge nothing on the canvas
        // says. If v is wj's endpoint the contact sits on an
        // entity both wires reference - a visible landing.
        if (!same_pos(v, paths[wj].front()) &&
            !same_pos(v, paths[wj].back()) &&
            junction_at.count({v.x, v.y}) == 0) {
            SilentJoin join;
            join.kind = SilentJoin::Kind::WireOnWire;
            join.pos = v;
            join.wire = wires[wi]->id;
            join.other = wires[wj]->id;
            record_join(join);
        }
    };
    // "p lies on wire wi's polyline", tested against one wire's
    // candidate segments (the grid's narrowed on_polyline).
    const auto on_wire_via_segments = [&](const std::vector<SegRef>& cands,
                                          std::size_t at, std::size_t end,
                                          const Pos& p) {
        for (std::size_t k = at; k < end; ++k) {
            const SegRef& c = cands[k];
            if (on_segment(paths[c.wire][c.seg - 1], paths[c.wire][c.seg],
                           p)) {
                return true;
            }
        }
        return false;
    };

    const bool use_grid = paths.size() >= kGridThreshold;
    SegGrid grid;
    if (use_grid) {
        for (std::size_t wi = 0; wi < paths.size(); ++wi) {
            grid.insert(wi, paths[wi]);
        }
    }
    std::vector<SegRef> cands;
    for (std::size_t wi = 0; wi < paths.size(); ++wi) {
        for (const Pos& v : paths[wi]) {
            if (use_grid) {
                grid.query(v, cands);
                for (std::size_t at = 0; at < cands.size();) {
                    std::size_t end = at;
                    while (end < cands.size() &&
                           cands[end].wire == cands[at].wire) {
                        ++end;
                    }
                    if (cands[at].wire != wi &&
                        on_wire_via_segments(cands, at, end, v)) {
                        vertex_on_wire(wi, v, cands[at].wire);
                    }
                    at = end;
                }
                continue;
            }
            for (std::size_t wj = 0; wj < paths.size(); ++wj) {
                if (wj == wi || !on_polyline(paths[wj], v)) {
                    continue;
                }
                vertex_on_wire(wi, v, wj);
            }
        }
    }

    // --- pins/junctions lying on a wire path ------------------------------------
    // One pin/junction entity e at p landing on wire wi.
    const auto entity_on_wire = [&](std::uint32_t e, const Pos& p,
                                    const PosKey& key, std::size_t wi) {
        dsu.unite(e, wire_ents[wi].first);
        // A PIN on the path's interior, no junction dot, is a
        // silent pickup (junction entities draw their own dot;
        // a pin at a path ENDPOINT is a wire landing on a named
        // entity - visible). One per position, pins over wires.
        if (ents[e].kind == Entity::Kind::Pin &&
            !same_pos(p, paths[wi].front()) &&
            !same_pos(p, paths[wi].back()) &&
            junction_at.count(key) == 0) {
            SilentJoin join;
            join.kind = SilentJoin::Kind::PinOnWire;
            join.pos = p;
            join.part = ents[e].pin.part;
            join.pin = ents[e].pin.pin;
            join.wire = wires[wi]->id;
            record_join(join);
        }
    };
    for (const auto& [key, group] : by_pos) {
        const Pos p{key.x, key.y};
        for (std::uint32_t e : group) {
            if (use_grid) {
                grid.query(p, cands);
                for (std::size_t at = 0; at < cands.size();) {
                    std::size_t end = at;
                    while (end < cands.size() &&
                           cands[end].wire == cands[at].wire) {
                        ++end;
                    }
                    if (on_wire_via_segments(cands, at, end, p)) {
                        entity_on_wire(e, p, key, cands[at].wire);
                    }
                    at = end;
                }
                continue;
            }
            for (std::size_t wi = 0; wi < paths.size(); ++wi) {
                if (!on_polyline(paths[wi], p)) {
                    continue;
                }
                entity_on_wire(e, p, key, wi);
            }
        }
    }

    // --- labels: attach, then merge same names ----------------------------------
    // An unattached label (empty space, or a wire crossing) joins NO net -
    // it is a dangling decoration, warned above and excluded from
    // grouping, same-name merging and ground forcing below.
    std::vector<std::uint32_t> attached_labels;
    const auto mark_attached = [&](std::uint32_t le) {
        attached_labels.push_back(le);
    };
    for (const Label& l : s.labels()) {
        const std::uint32_t le = label_ent[l.id];
        std::vector<std::uint32_t> anchored;  // entities/vertex-wires at pos
        std::vector<std::uint32_t> interior;  // wires carrying pos mid-segment
        const auto it = by_pos.find({l.pos.x, l.pos.y});
        if (it != by_pos.end()) {
            anchored.insert(anchored.end(), it->second.begin(),
                            it->second.end());
        }
        for (std::size_t wi = 0; wi < paths.size(); ++wi) {
            if (is_vertex(paths[wi], l.pos)) {
                anchored.push_back(wire_ents[wi].first);
            } else if (on_polyline(paths[wi], l.pos)) {
                interior.push_back(wire_ents[wi].first);
            }
        }
        if (!anchored.empty()) {
            for (std::uint32_t e : anchored) {
                dsu.unite(le, e);
            }
            mark_attached(le);
        } else if (interior.size() == 1) {
            dsu.unite(le, interior[0]);
            mark_attached(le);
        } else if (interior.size() >= 2) {
            out.diags.push_back(make_diag(
                Severity::Warning,
                "circsim.nets.label_at_crossing",
                "label '" + l.name + "' sits on a wire crossing; it names"
                " nothing (move it onto a wire endpoint or away from the"
                " crossing)",
                "label " + l.name,
                DiagTarget{DiagTarget::Kind::Label, 0, DiagTarget::npos(), 0,
                           l.id}));
        } else {
            out.diags.push_back(make_diag(
                Severity::Warning,
                "circsim.nets.label_unattached",
                "label '" + l.name +
                    "' is not on any wire, pin or junction",
                "label " + l.name,
                DiagTarget{DiagTarget::Kind::Label, 0, DiagTarget::npos(), 0,
                           l.id}));
        }
    }
    const auto is_attached = [&](std::uint32_t le) {
        return std::find(attached_labels.begin(), attached_labels.end(), le) !=
               attached_labels.end();
    };
    // Same-name ATTACHED labels merge (case-insensitive): one net per
    // spelling.
    {
        std::map<std::string, std::uint32_t> first_of_name;
        for (const Label& l : s.labels()) {
            if (!is_attached(label_ent[l.id])) {
                continue;
            }
            const std::string key = lower(l.name);
            const auto it = first_of_name.find(key);
            if (it == first_of_name.end()) {
                first_of_name.emplace(key, label_ent[l.id]);
            } else {
                dsu.unite(it->second, label_ent[l.id]);
            }
        }
    }

    // --- fixed-net pins (power/ground) -------------------------------------------
    // Same fixed name unions; every ground spelling ("0" pins, labels named
    // 0/gnd) unions into the one global ground.
    {
        std::map<std::string, std::uint32_t> first_fixed;
        std::uint32_t ground_ent = std::numeric_limits<std::uint32_t>::max();
        const auto unite_ground = [&](std::uint32_t e) {
            if (ground_ent == std::numeric_limits<std::uint32_t>::max()) {
                ground_ent = e;
            } else {
                dsu.unite(ground_ent, e);
            }
        };
        for (const Part& p : s.parts()) {
            const SymbolDef* sym = lib.find(p.symbol);
            if (sym == nullptr || !sym->fixed_net.has_value()) {
                continue;
            }
            const std::string key = lower(*sym->fixed_net);
            for (std::uint32_t i = 0; i < sym->pins.size(); ++i) {
                const std::uint32_t e = pin_ent.at(pin_key(PinRef{p.id, i}));
                if (key == "0") {
                    unite_ground(e);
                    continue;
                }
                const auto it = first_fixed.find(key);
                if (it == first_fixed.end()) {
                    first_fixed.emplace(key, e);
                } else {
                    dsu.unite(it->second, e);
                }
            }
        }
        for (const Label& l : s.labels()) {
            if (!is_attached(label_ent[l.id])) {
                continue;  // a dangling label forces nothing
            }
            const std::string key = lower(l.name);
            if (key == "0" || key == "gnd") {
                unite_ground(label_ent[l.id]);
            }
        }
    }

    // --- group entities into candidate nets --------------------------------------
    struct Group {
        std::vector<PinRef> pins;
        std::vector<std::uint32_t> wires;
        std::vector<std::uint32_t> junctions;
        std::vector<std::uint32_t> labels;  // entity ids (mapped back later)
        std::vector<const Label*> label_objs;
        bool ground = false;
        std::string name;         // resolved below
        std::string fixed_name;   // the winning power spelling ("" = none)
        std::uint32_t fixed_part = 0;
    };
    std::map<std::uint32_t, Group> groups;
    const auto group_of = [&](std::uint32_t root) -> Group& {
        return groups.try_emplace(root).first->second;
    };

    for (std::uint32_t e = 0; e < ents.size(); ++e) {
        if (ents[e].kind == Entity::Kind::Label && !is_attached(e)) {
            continue;  // unattached labels join no net
        }
        Group& g = group_of(dsu.find(e));
        switch (ents[e].kind) {
            case Entity::Kind::Pin:
                g.pins.push_back(ents[e].pin);
                break;
            case Entity::Kind::Junction:
                g.junctions.push_back(ents[e].obj_id);
                break;
            case Entity::Kind::Label: {
                const Label* l = s.find_label(ents[e].obj_id);
                g.labels.push_back(e);
                if (l != nullptr) {
                    g.label_objs.push_back(l);
                    const std::string key = lower(l->name);
                    if (key == "0" || key == "gnd") {
                        g.ground = true;
                    }
                }
                break;
            }
        }
    }
    for (std::size_t wi = 0; wi < wires.size(); ++wi) {
        group_of(dsu.find(wire_ents[wi].first)).wires.push_back(wires[wi]->id);
    }
    // Fixed names ride the pins' parts.
    for (auto& [root, g] : groups) {
        for (const PinRef& pr : g.pins) {
            const Part* p = s.find_part(pr.part);
            if (p == nullptr) {
                continue;
            }
            const SymbolDef* sym = lib.find(p->symbol);
            if (sym == nullptr || !sym->fixed_net.has_value()) {
                continue;
            }
            if (lower(*sym->fixed_net) == "0") {
                g.ground = true;
            } else if (g.fixed_part == 0 || p->id < g.fixed_part) {
                g.fixed_part = p->id;
                g.fixed_name = *sym->fixed_net;
            }
        }
    }

    // --- names ---------------------------------------------------------------------
    // ground always "0"; else the lowest-id label's name; else the lowest-id
    // power symbol's fixed name; else auto (below). Conflicts warn.
    for (auto& [root, g] : groups) {
        std::sort(g.label_objs.begin(), g.label_objs.end(),
                  [](const Label* a, const Label* b) { return a->id < b->id; });
    }
    std::map<std::string, std::uint32_t, std::less<>> used_names;  // lower -> count
    for (auto& [root, g] : groups) {
        if (g.ground) {
            g.name = "0";
            ++used_names["0"];
            continue;
        }
        if (!g.label_objs.empty()) {
            g.name = g.label_objs.front()->name;
            for (std::size_t i = 1; i < g.label_objs.size(); ++i) {
                if (lower(g.label_objs[i]->name) != lower(g.name)) {
                    out.diags.push_back(make_diag(
                        Severity::Warning,
                        "circsim.nets.label_conflict",
                        "net carries labels '" + g.name + "' and '" +
                            g.label_objs[i]->name + "'; '" + g.name +
                            "' wins",
                        "label " + g.name,
                        DiagTarget{DiagTarget::Kind::Label, 0,
                                   DiagTarget::npos(), 0,
                                   g.label_objs.front()->id}));
                }
            }
        } else if (!g.fixed_name.empty()) {
            g.name = g.fixed_name;
        }
        if (!g.name.empty()) {
            ++used_names[lower(g.name)];
        }
    }

    // Auto names: nets without one, ordered by their smallest member id
    // (ids come from the one schematic allocator, so they are unique across
    // parts/junctions/wires/labels).
    {
        std::vector<std::uint32_t> unnamed;
        for (auto& [root, g] : groups) {
            if (g.name.empty()) {
                unnamed.push_back(root);
            }
        }
        const auto anchor = [&](std::uint32_t root) {
            std::uint32_t best = std::numeric_limits<std::uint32_t>::max();
            const Group& g = groups.at(root);
            for (const PinRef& pr : g.pins) {
                best = std::min(best, pr.part);
            }
            for (std::uint32_t id : g.junctions) {
                best = std::min(best, id);
            }
            for (std::uint32_t id : g.wires) {
                best = std::min(best, id);
            }
            for (std::uint32_t id : g.labels) {
                const Label* l = s.find_label(
                    ents[id].obj_id);
                if (l != nullptr) {
                    best = std::min(best, l->id);
                }
            }
            return best;
        };
        std::sort(unnamed.begin(), unnamed.end(),
                  [&](std::uint32_t a, std::uint32_t b) {
                      const std::uint32_t aa = anchor(a);
                      const std::uint32_t ab = anchor(b);
                      return aa != ab ? aa < ab : a < b;
                  });
        std::uint32_t counter = 1;
        for (std::uint32_t root : unnamed) {
            std::string name;
            do {
                name = "N" + std::to_string(1000 + counter).substr(1);
                ++counter;
            } while (used_names.find(lower(name)) != used_names.end());
            groups.at(root).name = name;
            ++used_names[lower(name)];
        }
    }

    // --- assemble: ground first, then by name ---------------------------------------
    std::vector<std::uint32_t> order;
    order.reserve(groups.size());
    for (const auto& [root, g] : groups) {
        order.push_back(root);
    }
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        const Group& ga = groups.at(a);
        const Group& gb = groups.at(b);
        if (ga.ground != gb.ground) {
            return ga.ground;
        }
        const std::string la = lower(ga.name);
        const std::string lb = lower(gb.name);
        if (la != lb) {
            return la < lb;
        }
        return ga.name < gb.name;
    });
    for (std::uint32_t root : order) {
        Net n;
        n.id = static_cast<std::uint32_t>(out.nets.nets.size()) + 1;
        const Group& g = groups.at(root);
        n.name = g.name;
        n.is_ground = g.ground;
        n.pins = g.pins;
        n.wires = g.wires;
        n.junctions = g.junctions;
        for (std::uint32_t le : g.labels) {
            n.labels.push_back(ents[le].obj_id);
        }
        std::sort(n.pins.begin(), n.pins.end(), [](PinRef a, PinRef b) {
            return a.part != b.part ? a.part < b.part : a.pin < b.pin;
        });
        std::sort(n.wires.begin(), n.wires.end());
        std::sort(n.junctions.begin(), n.junctions.end());
        std::sort(n.labels.begin(), n.labels.end());
        out.nets.nets.push_back(std::move(n));
    }

    out.nets.index();
    return out;
}

// --- NetList lookups -------------------------------------------------------------

void NetList::index() {
    pin_net_.clear();
    wire_net_.clear();
    junction_net_.clear();
    name_net_.clear();
    for (const Net& n : nets) {
        for (const PinRef& p : n.pins) {
            pin_net_[pin_key(p)] = n.id;
        }
        for (std::uint32_t w : n.wires) {
            wire_net_[w] = n.id;
        }
        for (std::uint32_t j : n.junctions) {
            junction_net_[j] = n.id;
        }
        name_net_[lower(n.name)] = n.id;
    }
}

const Net* NetList::by_id(std::uint32_t id) const {
    for (const Net& n : nets) {
        if (n.id == id) {
            return &n;
        }
    }
    return nullptr;
}

const Net* NetList::net_of_pin(PinRef pin) const {
    const auto it = pin_net_.find(pin_key(pin));
    return it == pin_net_.end() ? nullptr : by_id(it->second);
}

const Net* NetList::net_of_wire(std::uint32_t wire_id) const {
    const auto it = wire_net_.find(wire_id);
    return it == wire_net_.end() ? nullptr : by_id(it->second);
}

const Net* NetList::net_of_junction(std::uint32_t junction_id) const {
    const auto it = junction_net_.find(junction_id);
    return it == junction_net_.end() ? nullptr : by_id(it->second);
}

const Net* NetList::by_name(std::string_view name) const {
    const auto it = name_net_.find(lower(name));
    return it == name_net_.end() ? nullptr : by_id(it->second);
}

const Net* NetList::ground() const {
    return by_name("0");
}

}  // namespace ws::circsim
