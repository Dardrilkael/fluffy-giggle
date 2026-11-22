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
                              const ws::circsim::SymbolLibrary& library,
                              const PressPose* press) {
    doc = Doc{};
    pin_node.clear();
    junction_node.clear();
    wire_ids.clear();
    node_junction.clear();
    wire_back.clear();
    schem = &s;
    lib = &library;

    // Pin nodes: one per pin of every known-symbol part. (`part_sym` indexes
    // the parts the wire pass needs to derive exit directions from, so
    // that pass never scans the schematic per wire end.)
    std::map<std::uint32_t,
             std::pair<const Part*, const ws::circsim::SymbolDef*>>
        part_sym;
    for (const Part& part : s.parts()) {
        const ws::circsim::SymbolDef* sym = library.find(part.symbol);
        if (sym == nullptr) {
            continue;  // the unknown-symbol diagnostic is ERC's
        }
        part_sym[part.id] = {&part, sym};
        // The press pose re-places this one part for the build (a copy
        // only when it is re-posed; the schematic itself is never
        // written).
        Part posed;
        const Part* placed = &part;
        if (press != nullptr) {
            if (const auto it = press->parts.find(part.id);
                it != press->parts.end()) {
                posed = part;
                posed.pos = it->second;
                placed = &posed;
            }
        }
        for (std::uint32_t i = 0; i < sym->pins.size(); ++i) {
            const NodeId n = doc.add_pin(
                part.id, i, v(ws::circsim::pin_pos(*sym, *placed, i)));
            pin_node[{part.id, i}] = n;
        }
    }
    // Where a junction stands in this build (the press pose or stored).
    const auto junction_pos = [&](const Junction& j) {
        if (press != nullptr) {
            if (const auto it = press->junctions.find(j.id);
                it != press->junctions.end()) {
                return it->second;
            }
        }
        return j.pos;
    };
    // Junction nodes.
    for (const Junction& j : s.junctions()) {
        const NodeId n = doc.add_junction(v(junction_pos(j)));
        junction_node[j.id] = n;
        node_junction[n] = j.id;
    }
    // Persisted wire id -> the sch pieces cut from it so far (filled on
    // its first tap; absent = the wire is still whole). Lets a tap find ITS piece without scanning
    // every wire in the document.
    std::map<std::uint32_t, std::vector<ws::sch::WireId>> pieces_of;
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
        // A pin end carries its pin's world exit direction (the router's
        // stored fact: "the wire leaves the pin east"). The .csim has no
        // such field, so it is re-derived from the placement here; a
        // junction end stays free. Without this every wire was routed
        // free-ended and could run back through the part it leaves.
        const auto dir_of = [&](const WireEndpoint& e) {
            if (const PinRef* pr = std::get_if<PinRef>(&e)) {
                const auto it = part_sym.find(pr->part);
                if (it != part_sym.end()) {
                    return pin_world_dir(*it->second.second, *it->second.first,
                                         pr->pin);
                }
            }
            return ws::sch::kDirFree;
        };
        const ws::sch::WireId id =
            doc.add_wire(node_of(w.a), node_of(w.b), w.waypoints,
                         dir_of(w.a), dir_of(w.b));
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
    std::map<NodeId, NodeId> node_remap;  // orphaned junction node -> its split
    for (const Junction& j : s.junctions()) {
        for (const std::uint32_t tapped : j.taps) {
            if (wire_ids.find(tapped) == wire_ids.end()) {
                continue;  // the tapped wire is gone (a stale tap)
            }
            const auto jit = junction_node.find(j.id);
            if (jit == junction_node.end()) {
                continue;
            }
            // A wire with several taps is already CUT into pieces by the
            // earlier ones, so the point may lie on any piece, not just
            // the one still carrying the original id: offer it to every
            // piece descended from this persisted wire until one takes
            // it (a tap on no piece is detached - nothing to split).
            std::optional<ws::sch::Doc::Split> split;
            std::vector<ws::sch::WireId>& cut = pieces_of[tapped];
            if (cut.empty()) {
                cut.push_back(wire_ids[tapped]);  // not cut yet: the whole wire
            }
            for (const ws::sch::WireId piece : cut) {
                split = doc.split_wire(piece, v(junction_pos(j)));
                if (split.has_value()) {
                    break;
                }
            }
            if (!split.has_value()) {
                continue;
            }
            // Re-point the junction: its node is the split's junction.
            // The ORIGINAL junction node (minted in the node pass) is
            // now an orphan; the wires that END on it re-bind to the
            // split junction (they are the junction's own wires - the
            // tap's branch).
            const NodeId old_node = jit->second;
            junction_node[j.id] = split->junction;
            node_junction.erase(old_node);
            node_junction[split->junction] = j.id;
            // The re-bind and the orphan removal are deferred to ONE pass
            // after every tap (a per-tap sweep of all wires and nodes made
            // the build quadratic in the tap count). A junction that taps
            // several wires remaps old -> split1 -> split2: resolved below.
            node_remap[old_node] = split->junction;
            // BOTH halves descend from the same persisted wire: the
            // first took the original sch id's SLOT (a fresh id in a
            // reused slot), the second is brand new. Map them both so
            // landing() can merge the pair back (and drops() can name
            // the persisted wire).
            wire_back[split->first] = tapped;
            wire_back[split->second] = tapped;
            cut.push_back(split->second);  // first == the piece itself
        }
    }
    if (!node_remap.empty()) {
        const auto resolve = [&](NodeId n) {
            for (auto it = node_remap.find(n); it != node_remap.end();
                 it = node_remap.find(n)) {
                n = it->second;
            }
            return n;
        };
        for (ws::sch::Wire& w : doc.wires) {
            w.a = resolve(w.a);
            w.b = resolve(w.b);
        }
        doc.nodes.erase(
            std::remove_if(doc.nodes.begin(), doc.nodes.end(),
                           [&](const ws::sch::Node& n) {
                               return node_remap.count(n.id) != 0u;
                           }),
            doc.nodes.end());
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
    // Wire shapes: the overlay's wire paths in circsim ids. A SPLIT
    // pair (two sch halves born of one persisted wire - the tap lift)
    // merges back: the persisted wire's fresh waypoints are first.mid +
    // [the split junction's position] + second.mid, each half reading
    // its overlay shape when the drag bent it and its stored shape when
    // it did not. Without the merge, the two halves would fight for one
    // entry and the split point (the T-connection the author drew)
    // would vanish from the persisted path.
    //
    // Group the sch wires by the persisted circsim wire they descend
    // from (wire_back), split pairs included.
    std::map<std::uint32_t, std::vector<ws::sch::WireId>> by_persisted;
    for (const auto& [sch_id, cs_id] : wire_back) {
        by_persisted[cs_id].push_back(sch_id);
    }
    for (const auto& [cs_id, sch_ids] : by_persisted) {
        if (sch_ids.size() == 1u) {
            // The plain case: the overlay's entry (or nothing - an
            // untouched wire keeps its stored shape).
            const auto it = ov.wirePath.find(sch_ids.front());
            if (it != ov.wirePath.end()) {
                out.wires[cs_id] = it->second;
            }
            continue;
        }
        // The split case: the persisted wire was cut into a CHAIN of
        // pieces (one cut per tap, any number of them). Every cut keeps
        // the a->b orientation, so piece k ends on the junction piece
        // k+1 starts on: the head is the piece whose `a` node no other
        // piece ends on, and each next piece is the one starting where
        // the last ended.
        std::vector<const ws::sch::Wire*> pieces;
        pieces.reserve(sch_ids.size());
        for (const ws::sch::WireId id : sch_ids) {
            if (const ws::sch::Wire* w = doc.find_wire(id)) {
                pieces.push_back(w);
            }
        }
        if (pieces.size() != sch_ids.size()) {
            continue;  // a piece is gone: skip honestly
        }
        std::vector<const ws::sch::Wire*> chain;
        const ws::sch::Wire* head = nullptr;
        for (const ws::sch::Wire* cand : pieces) {
            const bool fed = std::any_of(
                pieces.begin(), pieces.end(),
                [&](const ws::sch::Wire* o) { return o->b == cand->a; });
            if (!fed) {
                head = cand;
                break;
            }
        }
        for (const ws::sch::Wire* cur = head;
             cur != nullptr && chain.size() < pieces.size();) {
            chain.push_back(cur);
            const ws::sch::Wire* next = nullptr;
            for (const ws::sch::Wire* cand : pieces) {
                if (cand != cur && cand->a == cur->b) {
                    next = cand;
                    break;
                }
            }
            cur = next;
        }
        if (chain.size() != pieces.size()) {
            continue;  // not one clean a->b chain: skip honestly
        }
        const auto shape_of = [&](const ws::sch::Wire* w) {
            const auto it = ov.wirePath.find(w->id);
            return it != ov.wirePath.end() ? it->second : w->mid;
        };
        // A cut's junction stands between two pieces at its LANDING
        // position: when the drag moved the junction (a tap dragged
        // along), the overlay holds where it ends up; the press-time
        // doc's position would write the OLD spot into the path and
        // leave a diagonal leg with the dot off it.
        const auto landed_pos = [&](NodeId n) {
            const auto it = ov.nodePos.find(n);
            if (it != ov.nodePos.end()) {
                return it->second;
            }
            const ws::sch::Node* node = doc.find_node(n);
            return node != nullptr ? node->pos : Vec2{0.0, 0.0};
        };
        std::vector<ws::circsim::Pos> mids;
        for (std::size_t k = 0; k < chain.size(); ++k) {
            if (k > 0) {
                mids.push_back(p(landed_pos(chain[k]->a)));
            }
            for (const Vec2& m : shape_of(chain[k])) {
                mids.push_back(p(m));
            }
        }
        out.wires[cs_id] = std::move(mids);
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
