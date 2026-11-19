// ws::sch - the connect-on-drop candidates + the move command (phase 3).
//
// THE CONTRACTS:
//   * drop_candidates reads doc + overlay and answers PURE DATA - the
//     editor renders snap rings from it during the drag;
//   * commit_move creates ONLY the candidates the drag showed (the
//     editor passes the list it rendered);
//   * a PinOnWire candidate lands as split_wire (the T-connection IS
//     the split - there are no interior attachments anywhere);
//   * a PinOnPin candidate lands as a wire joining the two pins (the
//     author's gesture made explicit by the drop);
//   * undo restores nodes, wires and nets EXACTLY (the command captures
//     the full before-state of everything it touches).

#include <ws/schematic/commands.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <utility>

namespace ws::sch {

// --- drop candidates ----------------------------------------------------------

std::vector<DropCandidate> drop_candidates(const Doc& doc,
                                            const Overlay& overlay) {
    std::vector<DropCandidate> out;

    // The effective position of every node: the overlay's landing for
    // moved ones, the stored one otherwise.
    const auto pos_of = [&](NodeId n) -> std::optional<Vec2> {
        const Node* node = doc.find_node(n);
        if (node == nullptr) {
            return std::nullopt;
        }
        const auto it = overlay.nodePos.find(n);
        return it != overlay.nodePos.end() ? it->second : node->pos;
    };

    // The effective path of every wire (overlay shapes for bent ones).
    const auto path_of = [&](WireId id) -> std::vector<Vec2> {
        const Wire* w = doc.find_wire(id);
        if (w == nullptr) {
            return {};
        }
        std::vector<Vec2> path;
        const auto pa = pos_of(w->a);
        if (!pa) {
            return {};
        }
        path.push_back(*pa);
        const auto it = overlay.wirePath.find(id);
        if (it != overlay.wirePath.end()) {
            path.insert(path.end(), it->second.begin(), it->second.end());
        } else {
            path.insert(path.end(), w->mid.begin(), w->mid.end());
        }
        const auto pb = pos_of(w->b);
        if (!pb) {
            return {};
        }
        path.push_back(*pb);
        return path;
    };

    constexpr double kEps = 1e-9;
    const auto on_segment = [](const Vec2& a, const Vec2& b, const Vec2& p) {
        const double cross =
            (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (std::fabs(cross) > kEps) {
            return false;
        }
        return p.x >= std::min(a.x, b.x) - kEps &&
               p.x <= std::max(a.x, b.x) + kEps &&
               p.y >= std::min(a.y, b.y) - kEps && p.y <= std::max(a.y, b.y) + kEps;
    };

    // Only PIN nodes offer candidates (wires over wires never
    // auto-connect; a junction's binding is the split gesture).
    for (const Node& n : doc.nodes) {
        if (!n.is_pin()) {
            continue;
        }
        const std::optional<Vec2> at = pos_of(n.id);
        if (!at) {
            continue;
        }
        // PinOnPin: another PIN node at the same effective spot.
        for (const Node& other : doc.nodes) {
            if (!other.is_pin() || other.id == n.id) {
                continue;
            }
            const std::optional<Vec2> op = pos_of(other.id);
            if (!op) {
                continue;
            }
            if (std::fabs(op->x - at->x) <= kEps &&
                std::fabs(op->y - at->y) <= kEps) {
                DropCandidate c;
                c.kind = DropCandidate::Kind::PinOnPin;
                c.pin = n.id;
                c.other = other.id;
                c.at = *at;
                out.push_back(c);
            }
        }
        // PinOnWire: the pin's landing lies on another wire's effective
        // path INTERIOR (a wire whose own endpoint this pin is, is not a
        // candidate - the connection already exists by reference).
        for (const Wire& w : doc.wires) {
            const bool own =
                w.a == n.id || w.b == n.id;
            if (own) {
                continue;
            }
            const std::vector<Vec2> path = path_of(w.id);
            if (path.size() < 2u) {
                continue;
            }
            // An endpoint's own position is not a drop onto the path.
            if ((std::fabs(path.front().x - at->x) <= kEps &&
                 std::fabs(path.front().y - at->y) <= kEps) ||
                (std::fabs(path.back().x - at->x) <= kEps &&
                 std::fabs(path.back().y - at->y) <= kEps)) {
                // The pin lands ON the wire's endpoint node: the endpoint
                // is a pin or junction there; if it is a PIN this is the
                // PinOnPin case above, else nothing to do here.
                continue;
            }
            for (std::size_t i = 1; i < path.size(); ++i) {
                if (on_segment(path[i - 1], path[i], *at)) {
                    DropCandidate c;
                    c.kind = DropCandidate::Kind::PinOnWire;
                    c.pin = n.id;
                    c.wire = w.id;
                    c.at = *at;
                    out.push_back(c);
                    break;
                }
            }
        }
    }

    // Dedup: the pin-on-pin pair reports from BOTH orders (each pin
    // sees the other); collapse each unordered pin pair to one entry -
    // the pin with the smaller id stands as `pin`.
    std::vector<DropCandidate> dedup;
    for (DropCandidate c : out) {
        if (c.kind == DropCandidate::Kind::PinOnPin && c.pin > c.other) {
            std::swap(c.pin, c.other);
        }
        const bool dup = std::any_of(
            dedup.begin(), dedup.end(), [&](const DropCandidate& k) {
                return k.pin == c.pin &&
                       ((c.kind == DropCandidate::Kind::PinOnPin &&
                         k.kind == DropCandidate::Kind::PinOnPin &&
                         k.other == c.other) ||
                        (c.kind == DropCandidate::Kind::PinOnWire &&
                         k.kind == DropCandidate::Kind::PinOnWire &&
                         k.wire == c.wire));
            });
        if (!dup) {
            dedup.push_back(c);
        }
    }
    return dedup;
}

// --- the command ----------------------------------------------------------------

bool commit_move(Doc& d, UndoStack& undo, const Overlay& overlay,
                 const std::vector<DropCandidate>& candidates) {
    const bool any = !overlay.empty() || !candidates.empty();
    if (!any) {
        return false;  // a no-op gesture is no undo step
    }

    UndoStack::Entry e;
    e.name = "Move";

    // Capture the before-state of everything the command may touch.
    for (const auto& [id, pos] : overlay.nodePos) {
        const Node* n = d.find_node(id);
        if (n != nullptr) {
            e.node_before.emplace_back(id, n->pos);
        }
    }
    for (const auto& [id, mid] : overlay.wirePath) {
        const Wire* w = d.find_wire(id);
        if (w != nullptr) {
            e.wire_before.emplace_back(id, *w);
        }
    }
    for (const auto& [k, dir] : overlay.endDir) {
        const Wire* w = d.find_wire(k.first);
        if (w != nullptr &&
            std::none_of(e.wire_before.begin(), e.wire_before.end(),
                         [&](const auto& p) { return p.first == k.first; })) {
            e.wire_before.emplace_back(k.first, *w);
        }
    }
    for (const DropCandidate& c : candidates) {
        if (c.kind == DropCandidate::Kind::PinOnWire) {
            const Wire* w = d.find_wire(c.wire);
            if (w != nullptr &&
                std::none_of(e.wire_before.begin(), e.wire_before.end(),
                             [&](const auto& p) { return p.first == c.wire; })) {
                e.wire_before.emplace_back(c.wire, *w);
            }
        }
    }

    // APPLY: the overlay.
    for (const auto& [id, pos] : overlay.nodePos) {
        if (Node* n = d.find_node(id)) {
            n->pos = pos;
        }
    }
    for (const auto& [id, mid] : overlay.wirePath) {
        if (Wire* w = d.find_wire(id)) {
            w->mid = mid;
        }
    }
    for (const auto& [k, dir] : overlay.endDir) {
        if (Wire* w = d.find_wire(k.first)) {
            if (k.second) {
                w->a_dir = dir;
            } else {
                w->b_dir = dir;
            }
        }
    }

    // APPLY: the candidates - exactly what the drag showed. Every minted
    // id is recorded (minted_wires/minted_nodes) so undo erases EXACTLY
    // these and redo re-mints THE SAME ids (an id once retired stays
    // retired - the doc's own rule).
    for (const DropCandidate& c : candidates) {
        if (c.kind == DropCandidate::Kind::PinOnWire) {
            // The split IS the connection - plus the dropped pin's tie
            // to the split junction (the pin lands ON the point; the
            // junction is a fresh node there, and the pin's membership
            // in the wire's net rides the tie wire - the explicit
            // gesture the drop made).
            const auto s = d.split_wire(c.wire, c.at);
            if (s.has_value()) {
                e.minted_wires.push_back(s->first);
                e.minted_wires.push_back(s->second);
                e.minted_nodes.push_back(s->junction);
                const auto tie = d.add_wire(c.pin, s->junction);
                e.minted_wires.push_back(tie);
            }
        } else {
            // PinOnPin: a wire joining the two pins (the explicit
            // gesture the drop made).
            const auto id = d.add_wire(c.pin, c.other);
            e.minted_wires.push_back(id);
        }
    }

    // Push (dropping any redo tail). The entry carries the overlay and
    // the landed candidates for REDO.
    e.overlay = overlay;
    e.candidates = candidates;
    undo.list_.resize(undo.at_);
    undo.list_.push_back(std::move(e));
    undo.at_ = undo.list_.size();
    return true;
}

namespace {

// Rebuilds a committed split with its EXACT minted ids (a redo after an
// undo restores the identical document: same junction node, same half
// wires, same tie wire). Mirrors Doc::split_wire's shape logic without
// minting fresh ids.
void replay_split(Doc& d, WireId wire, Vec2 pos, WireId first_id,
                  WireId second_id, NodeId junction_id, WireId tie_id,
                  NodeId pin) {
    const std::vector<Vec2> path = d.wire_path(wire);
    if (path.size() < 2u) {
        return;
    }
    constexpr double kEps2 = 1e-9;
    const auto on_segment2 = [](const Vec2& a, const Vec2& b, const Vec2& p) {
        const double cross =
            (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (std::fabs(cross) > kEps2) {
            return false;
        }
        return p.x >= std::min(a.x, b.x) - kEps2 &&
               p.x <= std::max(a.x, b.x) + kEps2 &&
               p.y >= std::min(a.y, b.y) - kEps2 && p.y <= std::max(a.y, b.y) + kEps2;
    };
    std::size_t seg = path.size() - 1;
    for (std::size_t k = 1; k < path.size(); ++k) {
        if (on_segment2(path[k - 1], path[k], pos)) {
            seg = k - 1;
            break;
        }
    }
    Node j;
    j.id = junction_id;
    j.kind = NodeKind::Junction;
    j.pos = pos;
    Wire* w = d.find_wire(wire);
    if (w == nullptr) {
        return;
    }
    Wire first = *w;
    first.id = first_id;
    first.b = junction_id;
    first.b_dir = w->b_dir;
    first.split_of = w->id;
    first.mid.clear();
    Wire second;
    second.id = second_id;
    second.a = junction_id;
    second.b = w->b;
    second.a_dir = w->a_dir;
    second.b_dir = w->b_dir;
    second.split_of = w->id;
    second.mid.clear();
    for (std::size_t k = 1; k + 1 < path.size(); ++k) {
        if (std::fabs(path[k].x - pos.x) <= kEps2 &&
            std::fabs(path[k].y - pos.y) <= kEps2) {
            continue;
        }
        if (k <= seg) {
            first.mid.push_back(path[k]);
        } else {
            second.mid.push_back(path[k]);
        }
    }
    Wire tie;
    tie.id = tie_id;
    tie.a = pin;
    tie.b = junction_id;
    d.nodes.push_back(j);
    *w = first;
    d.wires.push_back(second);
    d.wires.push_back(tie);
    d.raise_node_counter(junction_id);
    d.raise_wire_counter(first_id);
    d.raise_wire_counter(second_id);
    d.raise_wire_counter(tie_id);
}

}  // namespace

bool UndoStack::undo(Doc& d) {
    if (!undoable()) {
        return false;
    }
    Entry& e = list_[at_ - 1];
    // The minted entities go first (they name nodes/wires the restore
    // below would otherwise fight).
    for (const WireId id : e.minted_wires) {
        (void)d.erase_wire(id);
    }
    for (const NodeId id : e.minted_nodes) {
        d.nodes.erase(std::remove_if(d.nodes.begin(), d.nodes.end(),
                                      [&](const Node& n) {
                                          return n.id == id;
                                      }),
                       d.nodes.end());
    }
    // The touched wires' full before-state. A wire whose slot a SPLIT
    // overwrote (or the minted erase removed) is RE-INSERTED whole -
    // the exact restore puts back the same id with the same shape.
    for (const auto& [id, w] : e.wire_before) {
        if (Wire* live = d.find_wire(id)) {
            *live = w;
            live->id = id;  // the slot keeps its id
        } else {
            Wire restored = w;
            restored.id = id;
            d.wires.push_back(restored);
            d.raise_wire_counter(id);
        }
    }
    // The touched nodes' positions.
    for (const auto& [id, pos] : e.node_before) {
        if (Node* n = d.find_node(id)) {
            n->pos = pos;
        }
    }
    --at_;
    return true;
}

bool UndoStack::redo(Doc& d) {
    if (!redoable()) {
        return false;
    }
    Entry& e = list_[at_];
    for (const auto& [id, pos] : e.overlay.nodePos) {
        if (Node* n = d.find_node(id)) {
            n->pos = pos;
        }
    }
    for (const auto& [id, mid] : e.overlay.wirePath) {
        if (Wire* w = d.find_wire(id)) {
            w->mid = mid;
        }
    }
    for (const auto& [k, dir] : e.overlay.endDir) {
        if (Wire* w = d.find_wire(k.first)) {
            if (k.second) {
                w->a_dir = dir;
            } else {
                w->b_dir = dir;
            }
        }
    }
    // The minted ids replay in the SAME order commit minted them, so a
    // redo after an undo restores the identical document (the exact
    // ids, not just the shape).
    std::size_t mi = 0;  // minted_wires cursor
    std::size_t ni = 0;  // minted_nodes cursor
    for (const DropCandidate& c : e.candidates) {
        if (c.kind == DropCandidate::Kind::PinOnWire) {
            if (mi + 2u < e.minted_wires.size() + 1u &&
                ni < e.minted_nodes.size()) {
                const WireId first = e.minted_wires[mi];
                const WireId second = e.minted_wires[mi + 1u];
                const WireId tie = e.minted_wires[mi + 2u];
                const NodeId junction = e.minted_nodes[ni];
                mi += 3u;
                ++ni;
                replay_split(d, c.wire, c.at, first, second, junction, tie,
                              c.pin);
            }
        } else {
            if (mi < e.minted_wires.size()) {
                const WireId id = e.minted_wires[mi];
                ++mi;
                Wire w;
                w.id = id;
                w.a = c.pin;
                w.b = c.other;
                d.wires.push_back(w);
                d.raise_wire_counter(id);
            }
        }
    }
    ++at_;
    return true;
}

}  // namespace ws::sch
