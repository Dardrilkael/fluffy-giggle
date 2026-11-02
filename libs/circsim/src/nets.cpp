#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ws/circsim/nets.hpp>

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

Diag net_warn(std::string code, std::string message, std::string where) {
    Diag d;
    d.severity = Severity::Warning;
    d.code = std::move(code);
    d.message = std::move(message);
    d.where = std::move(where);
    return d;
}

[[nodiscard]] std::uint64_t pin_key(PinRef r) {
    return (static_cast<std::uint64_t>(r.part) << 32) | r.pin;
}

}  // namespace

NetBuild build_nets(const Schematic& s,
                    const SymbolDef* (*find)(std::string_view)) {
    NetBuild out;

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
        const SymbolDef* sym = find(p.symbol);
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
            out.diags.push_back(net_warn(
                "circsim.nets.dangling_wire",
                "wire " + std::to_string(w.id) +
                    " ends on a pin or junction that does not exist; it is"
                    " dropped from connectivity",
                "wire " + std::to_string(w.id)));
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
    for (std::size_t wi = 0; wi < paths.size(); ++wi) {
        for (const Pos& v : paths[wi]) {
            for (std::size_t wj = 0; wj < paths.size(); ++wj) {
                if (wj != wi && on_polyline(paths[wj], v)) {
                    dsu.unite(wire_ents[wi].first, wire_ents[wj].first);
                }
            }
        }
    }

    // --- pins/junctions lying on a wire path ------------------------------------
    for (const auto& [key, group] : by_pos) {
        const Pos p{key.x, key.y};
        for (std::uint32_t e : group) {
            for (std::size_t wi = 0; wi < paths.size(); ++wi) {
                if (on_polyline(paths[wi], p)) {
                    dsu.unite(e, wire_ents[wi].first);
                }
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
            out.diags.push_back(net_warn(
                "circsim.nets.label_at_crossing",
                "label '" + l.name + "' sits on a wire crossing; it names"
                " nothing (move it onto a wire endpoint or away from the"
                " crossing)",
                "label " + l.name));
        } else {
            out.diags.push_back(net_warn(
                "circsim.nets.label_unattached",
                "label '" + l.name +
                    "' is not on any wire, pin or junction",
                "label " + l.name));
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
            const SymbolDef* sym = find(p.symbol);
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
            const SymbolDef* sym = find(p->symbol);
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
                    out.diags.push_back(net_warn(
                        "circsim.nets.label_conflict",
                        "net carries labels '" + g.name + "' and '" +
                            g.label_objs[i]->name + "'; '" + g.name +
                            "' wins",
                        "label " + g.name));
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
