// apps/circsim_editor - the schematic adapter implementation (phase 4a).
// See schematic_adapter.hpp for the design. Everything here is a straight
// translation: circsim ids <-> sch node/wire ids, positions <-> nodes,
// and the ONE routing answer (the lib's overlay) -> circsim writes.

#include "schematic_adapter.hpp"

#include <algorithm>

namespace editor {

using ws::circsim::Junction;
using ws::circsim::JunctionRef;
using ws::circsim::Part;
using ws::circsim::PinRef;
using ws::circsim::Wire;
using ws::circsim::WireEndpoint;
using ws::sch::Doc;
using ws::sch::Vec2;

namespace {

constexpr double kEps = 1e-9;

Vec2 v(ws::circsim::Pos p) { return {p.x, p.y}; }
ws::circsim::Pos p(Vec2 q) { return {q.x, q.y}; }

// The world exit direction a part's pin faces (the same math the lib's
// adapters need, spelled over circsim's public transform_pos).
int pin_world_dir(const ws::circsim::SymbolDef& sym, const Part& part,
                  std::uint32_t pin) {
    if (pin >= sym.pins.size()) {
        return ws::sch::kDirFree;
    }
    const int local = ((sym.pins[pin].dir % 360) + 360) % 360;
    ws::circsim::Pos dir{0.0, 0.0};
    switch (local) {
        case 0: dir = {1.0, 0.0}; break;
        case 90: dir = {0.0, 1.0}; break;
        case 180: dir = {-1.0, 0.0}; break;
        case 270: dir = {0.0, -1.0}; break;
        default: return ws::sch::kDirFree;
    }
    Part probe{};
    probe.rot = part.rot;
    probe.mirror = part.mirror;
    const ws::circsim::Pos w = ws::circsim::transform_pos(probe, dir);
    if (w.x > 0.5) return 0;
    if (w.y > 0.5) return 90;
    if (w.x < -0.5) return 180;
    return 270;
}

}  // namespace

void SchematicAdapter::build(ws::circsim::Schematic& s,
                              const ws::circsim::SymbolLibrary& library) {
    doc = Doc{};
    pin_node.clear();
    junction_node.clear();
    wire_ids.clear();
    node_junction.clear();
    wire_back.clear();
    schem = &s;
    lib = &library;

    // Pin nodes: one per pin of every known-symbol part.
    for (const Part& part : s.parts()) {
        const ws::circsim::SymbolDef* sym = library.find(part.symbol);
        if (sym == nullptr) {
            continue;  // the unknown-symbol diagnostic is ERC's
        }
        for (std::uint32_t i = 0; i < sym->pins.size(); ++i) {
            const NodeId n = doc.add_pin(
                part.id, i, v(ws::circsim::pin_pos(*sym, part, i)));
            pin_node[{part.id, i}] = n;
        }
    }
    // Junction nodes.
    for (const Junction& j : s.junctions()) {
        const NodeId n = doc.add_junction(v(j.pos));
        junction_node[j.id] = n;
        node_junction[n] = j.id;
    }
    // Wires: endpoints through the back-maps.
    for (const Wire& w : s.wires()) {
        const auto node_of = [&](const WireEndpoint& e) {
            if (const PinRef* pin = std::get_if<PinRef>(&e)) {
                const auto it = pin_node.find({pin->part, pin->pin});
                return it == pin_node.end() ? 0u : it->second;
            }
            const JunctionRef* j = std::get_if<JunctionRef>(&e);
            const auto it = j != nullptr
                                ? junction_node.find(j->junction)
                                : junction_node.end();
            return it == junction_node.end() ? 0u : it->second;
        };
        const ws::sch::WireId id =
            doc.add_wire(node_of(w.a), node_of(w.b), w.waypoints);
        wire_ids[w.id] = id;
        wire_back[id] = w.id;
    }
    // The persisted TAPS: every junction that taps a wire becomes a
    // SPLIT of that wire at the junction's position (the T-connection
    // IS the split - no interior attachments anywhere in the new
    // core). A detached tap (the junction no longer on the wire's
    // path) splits nothing: the old binding is honored by POSITION in
    // the persisted model, and the new model's honest answer is "not
    // connected" - the adapter keeps the wire whole and the tap drops
    // out of the view (the drawing shows a dot riding a wire it no
    // longer touches).
    for (const Junction& j : s.junctions()) {
        for (const std::uint32_t tapped : j.taps) {
            const auto wit = wire_ids.find(tapped);
            if (wit == wire_ids.end()) {
                continue;  // the tapped wire is gone (a stale tap)
            }
            const auto jit = junction_node.find(j.id);
            if (jit == junction_node.end()) {
                continue;
            }
            const std::optional<ws::sch::Doc::Split> s =
                doc.split_wire(wit->second, v(j.pos));
            if (!s.has_value()) {
                continue;
            }
            // Re-point the junction: its node is the split's junction.
            // The ORIGINAL junction node (minted in the node pass) is
            // now an orphan; the wires that END on it re-bind to the
            // split junction (they are the junction's own wires - the
            // tap's branch).
            const NodeId old_node = jit->second;
            junction_node[j.id] = s->junction;
            node_junction.erase(old_node);
            node_junction[s->junction] = j.id;
            for (ws::sch::Wire& w : doc.wires) {
                if (w.a == old_node) {
                    w.a = s->junction;
                }
                if (w.b == old_node) {
                    w.b = s->junction;
                }
            }
            // Drop the orphaned original from the doc.
            doc.nodes.erase(
                std::remove_if(doc.nodes.begin(), doc.nodes.end(),
                               [&](const ws::sch::Node& n) {
                                   return n.id == old_node;
                               }),
                doc.nodes.end());
            (void)wit;
        }
    }
}

// --- sessions -----------------------------------------------------------------

ws::sch::MoveSession SchematicAdapter::part_session(
    std::uint32_t part_id, ws::circsim::Pos to) const {
    ws::sch::MoveSession s;
    if (schem == nullptr || lib == nullptr) {
        return s;
    }
    const Part* part = schem->find_part(part_id);
    const ws::circsim::SymbolDef* sym =
        part != nullptr ? lib->find(part->symbol) : nullptr;
    if (part == nullptr || sym == nullptr) {
        return s;
    }
    s.parts.push_back({part_id, v(to)});
    // The part's pins land at their resolved positions under the NEW
    // placement; the stored dirs ride unchanged (the pin's face turned
    // with the part - the wire's stub follows).
    Part probe = *part;
    probe.pos = to;
    for (std::uint32_t i = 0; i < sym->pins.size(); ++i) {
        const auto it = pin_node.find({part_id, i});
        if (it == pin_node.end()) {
            continue;
        }
        s.land[it->second] =
            v(ws::circsim::pin_pos(*sym, probe, i));
    }
    return s;
}

ws::sch::MoveSession SchematicAdapter::rotate_session(
    std::uint32_t part_id, int rot, bool mirror) const {
    ws::sch::MoveSession s;
    if (schem == nullptr || lib == nullptr) {
        return s;
    }
    const Part* part = schem->find_part(part_id);
    const ws::circsim::SymbolDef* sym =
        part != nullptr ? lib->find(part->symbol) : nullptr;
    if (part == nullptr || sym == nullptr) {
        return s;
    }
    Part probe = *part;
    probe.rot = rot;
    probe.mirror = mirror;
    for (std::uint32_t i = 0; i < sym->pins.size(); ++i) {
        const auto it = pin_node.find({part_id, i});
        if (it == pin_node.end()) {
            continue;
        }
        // A rotation moves pin positions AND faces: both land.
        s.land[it->second] = v(ws::circsim::pin_pos(*sym, probe, i));
        s.land_dir[it->second] = pin_world_dir(*sym, probe, i);
    }
    return s;
}

ws::sch::MoveSession SchematicAdapter::junction_session(
    std::uint32_t junction_id, ws::circsim::Pos to) const {
    ws::sch::MoveSession s;
    const auto it = junction_node.find(junction_id);
    if (it == junction_node.end()) {
        return s;
    }
    s.nodes.push_back(it->second);
    s.land[it->second] = v(to);
    return s;
}

ws::sch::MoveSession SchematicAdapter::group_session(
    const std::vector<MemberMove>& members) const {
    ws::sch::MoveSession s;
    for (const MemberMove& m : members) {
        const auto to = m.to;
        const std::uint32_t sel_id = m.id;
        if (m.kind == 1u) {
            const Part* part = schem != nullptr ? schem->find_part(sel_id)
                                                : nullptr;
            const ws::circsim::SymbolDef* sym =
                part != nullptr && lib != nullptr
                    ? lib->find(part->symbol)
                    : nullptr;
            if (part != nullptr && sym != nullptr) {
                s.parts.push_back({sel_id, v(to)});
                Part probe = *part;
                probe.pos = to;
                for (std::uint32_t i = 0; i < sym->pins.size(); ++i) {
                    const auto it = pin_node.find({sel_id, i});
                    if (it != pin_node.end()) {
                        s.land[it->second] =
                            v(ws::circsim::pin_pos(*sym, probe, i));
                    }
                }
            }
        } else if (m.kind == 3u) {
            const auto it = junction_node.find(sel_id);
            if (it != junction_node.end()) {
                s.nodes.push_back(it->second);
                s.land[it->second] = v(to);
            }
        }
        // Wires and labels carry no attachment (a wire's shape is the
        // router's; a label's binding is authored).
    }
    return s;
}

// --- the landing translation -----------------------------------------------------

SchematicAdapter::Landing SchematicAdapter::landing(
    const Overlay& ov) const {
    Landing out;
    // Node positions -> junctions (parts' positions are the drag's own
    // `to`, passed by the canvas; pin landings ride the wire shapes).
    for (const auto& [node, pos] : ov.nodePos) {
        const ws::sch::Node* n = doc.find_node(node);
        if (n == nullptr) {
            continue;
        }
        const auto jit = node_junction.find(node);
        if (jit != node_junction.end()) {
            out.junctions[jit->second] = p(pos);
        }
    }
    // Wire shapes: the overlay's wire paths in circsim ids.
    for (const auto& [wire, mid] : ov.wirePath) {
        const auto bit = wire_back.find(wire);
        if (bit == wire_back.end()) {
            continue;
        }
        out.wires[bit->second] = mid;
    }
    return out;
}

std::vector<SchematicAdapter::Drop> SchematicAdapter::drops(
    const Overlay& ov) const {
    std::vector<Drop> out;
    if (schem == nullptr) {
        return out;
    }
    const auto pin_pair = [&](NodeId n)
        -> std::optional<std::pair<std::uint32_t, std::uint32_t>> {
        const ws::sch::Node* node = doc.find_node(n);
        if (node == nullptr || !node->is_pin()) {
            return std::nullopt;
        }
        return std::pair<std::uint32_t, std::uint32_t>{node->part, node->pin};
    };
    for (const ws::sch::DropCandidate& c :
         ws::sch::drop_candidates(doc, ov)) {
        Drop d;
        d.pin = pin_pair(c.pin).value_or(std::pair<std::uint32_t,
                                                   std::uint32_t>{});
        d.at = p(c.at);
        if (c.kind == ws::sch::DropCandidate::Kind::PinOnWire) {
            const auto wit = wire_back.find(c.wire);
            if (wit == wire_back.end()) {
                continue;
            }
            d.kind = Drop::Kind::PinOnWire;
            d.wire = wit->second;
        } else {
            d.kind = Drop::Kind::PinOnPin;
            d.other =
                pin_pair(c.other).value_or(std::pair<std::uint32_t,
                                                     std::uint32_t>{});
        }
        out.push_back(d);
    }
    return out;
}

}  // namespace editor
