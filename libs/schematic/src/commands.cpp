// ws::sch - the connect-on-drop candidates + the move command (phase 3).
//
// THE CONTRACTS:
//   * drop_candidates reads doc + overlay and answers PURE DATA - the
//     editor renders snap rings from it during the drag;
//   * only a DRAGGED pin (one the overlay moved) offers candidates: a
//     stationary pin never becomes a connection because something else
//     moved onto or past it;
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
#include <cmath>
#include <map>
#include <optional>
#include <utility>

#include "geom.hpp"

namespace ws::sch {

// --- drop candidates ----------------------------------------------------------

std::vector<DropCandidate> drop_candidates(const Doc& doc,
                                            const Overlay& overlay) {
    using detail::near;
    using detail::on_segment;

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

    // Only DRAGGED pins offer candidates (wires over wires never
    // auto-connect, and neither does a wire re-routed over a pin that did
    // not move). The overlay's node map is exactly the set of nodes that
    // landed somewhere new; std::map order keeps the answer deterministic.
    for (const auto& [pin_id, landed] : overlay.nodePos) {
        const Node* n = doc.find_node(pin_id);
        if (n == nullptr || !n->is_pin()) {
            continue;
        }
        const Vec2 at = landed;

        // PinOnPin: another PIN node at the same effective spot.
        for (const Node& other : doc.nodes) {
            if (!other.is_pin() || other.id == pin_id) {
                continue;
            }
            const std::optional<Vec2> op = pos_of(other.id);
            if (op && near(*op, at)) {
                DropCandidate c;
                c.kind = DropCandidate::Kind::PinOnPin;
                c.pin = pin_id;
                c.other = other.id;
                c.at = at;
                out.push_back(c);
            }
        }

        // PinOnWire: the pin's landing lies on another wire's effective
        // path INTERIOR (a wire whose own endpoint this pin is, is not a
        // candidate - the connection already exists by reference).
        for (const Wire& w : doc.wires) {
            if (w.a == pin_id || w.b == pin_id) {
                continue;
            }
            const std::vector<Vec2> path = path_of(w.id);
            if (path.size() < 2u) {
                continue;
            }
            // An endpoint's own position is not a drop onto the path (if
            // a PIN sits there it is the PinOnPin case above).
            if (near(path.front(), at) || near(path.back(), at)) {
                continue;
            }
            for (std::size_t i = 1; i < path.size(); ++i) {
                if (on_segment(path[i - 1], path[i], at)) {
                    DropCandidate c;
                    c.kind = DropCandidate::Kind::PinOnWire;
                    c.pin = pin_id;
                    c.wire = w.id;
                    c.at = at;
                    out.push_back(c);
                    break;
                }
            }
        }
    }

    // Dedup: two dragged pins landing on each other each report the pair;
    // collapse each unordered pin pair to one entry - the pin with the
    // smaller id stands as `pin`.
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

namespace {

// The overlay half of a commit / redo (node positions, wire shapes, end
// dirs). One function, so commit and redo cannot drift.
void apply_overlay(Doc& d, const Overlay& overlay) {
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
            (k.second ? w->a_dir : w->b_dir) = dir;
        }
    }
}

using Step = LandedStep;

// Lands one PinOnWire candidate. `pieces` are the wires currently
// carrying what used to be ONE wire: a split keeps the original id on its
// a-side half, so a later candidate on the same original wire may now sit
// on the new b-side half (or exactly on the junction the earlier one
// made) - both are found here instead of silently failing.
std::optional<Step> land_on_wire(Doc& d, std::vector<WireId>& pieces,
                                 const DropCandidate& c) {
    // 1. A junction already sits at the point (two pins dropped on the
    //    same spot of one wire): tie to it, no second split.
    for (const WireId id : pieces) {
        const Wire* w = d.find_wire(id);
        if (w == nullptr) {
            continue;
        }
        for (const NodeId end : {w->a, w->b}) {
            const Node* n = d.find_node(end);
            if (n != nullptr && n->kind == NodeKind::Junction &&
                detail::near(n->pos, c.at)) {
                Step s;
                s.kind = Step::Kind::Join;
                s.pin = c.pin;
                s.other = end;
                s.at = c.at;
                s.tie = d.add_wire(c.pin, end);
                return s;
            }
        }
    }
    // 2. Split whichever piece carries the point.
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        const WireId id = pieces[i];
        const auto split = d.split_wire(id, c.at);
        if (!split) {
            continue;
        }
        Step s;
        s.kind = Step::Kind::Split;
        s.wire = id;
        s.at = c.at;
        s.pin = c.pin;
        s.junction = split->junction;
        s.second = split->second;
        s.tie = d.add_wire(c.pin, split->junction);
        pieces.push_back(split->second);
        return s;
    }
    return std::nullopt;  // the point is on no piece: nothing to connect
}

// Replays a landed step with the ids it originally minted (redo).
void replay_step(Doc& d, const Step& s) {
    Wire tie;
    tie.id = s.tie;
    tie.a = s.pin;
    switch (s.kind) {
        case Step::Kind::Split: {
            (void)d.split_wire(s.wire, s.at, SplitIds{s.junction, s.second});
            tie.b = s.junction;
            break;
        }
        case Step::Kind::Join:
        case Step::Kind::PinPin:
            tie.b = s.other;
            break;
    }
    d.restore_wire(tie);
}

}  // namespace

bool commit_move(Doc& d, UndoStack& undo, const Overlay& overlay,
                 const std::vector<DropCandidate>& candidates) {
    const bool any = !overlay.empty() || !candidates.empty();
    if (!any) {
        return false;  // a no-op gesture is no undo step
    }

    UndoStack::Entry e;
    e.name = "Move";

    // Capture the before-state of everything the command may touch.
    const auto remember_wire = [&](WireId id) {
        const Wire* w = d.find_wire(id);
        if (w != nullptr &&
            std::none_of(e.wire_before.begin(), e.wire_before.end(),
                         [&](const auto& p) { return p.first == id; })) {
            e.wire_before.emplace_back(id, *w);
        }
    };
    for (const auto& [id, pos] : overlay.nodePos) {
        if (const Node* n = d.find_node(id)) {
            e.node_before.emplace_back(id, n->pos);
        }
    }
    for (const auto& [id, mid] : overlay.wirePath) {
        remember_wire(id);
    }
    for (const auto& [k, dir] : overlay.endDir) {
        remember_wire(k.first);
    }
    for (const DropCandidate& c : candidates) {
        if (c.kind == DropCandidate::Kind::PinOnWire) {
            remember_wire(c.wire);
        }
    }

    // APPLY: the overlay, then exactly the candidates the drag showed.
    apply_overlay(d, overlay);

    std::map<WireId, std::vector<WireId>> pieces;  // original -> its pieces
    for (const DropCandidate& c : candidates) {
        if (c.kind == DropCandidate::Kind::PinOnWire) {
            std::vector<WireId>& fam = pieces[c.wire];
            if (fam.empty()) {
                fam.push_back(c.wire);
            }
            if (const auto s = land_on_wire(d, fam, c)) {
                e.steps.push_back(*s);
            }
        } else {
            Step s;
            s.kind = Step::Kind::PinPin;
            s.pin = c.pin;
            s.other = c.other;
            s.at = c.at;
            s.tie = d.add_wire(c.pin, c.other);
            e.steps.push_back(s);
        }
    }

    // Push (dropping any redo tail). The entry carries the overlay and
    // the landed steps for REDO.
    e.overlay = overlay;
    undo.list_.resize(undo.at_);
    undo.list_.push_back(std::move(e));
    undo.at_ = undo.list_.size();
    return true;
}

bool UndoStack::undo(Doc& d) {
    if (!undoable()) {
        return false;
    }
    Entry& e = list_[at_ - 1];
    // Erase exactly what the steps minted (newest first).
    for (auto it = e.steps.rbegin(); it != e.steps.rend(); ++it) {
        (void)d.erase_wire(it->tie);
        if (it->kind == Step::Kind::Split) {
            (void)d.erase_wire(it->second);
            (void)d.erase_node(it->junction);
        }
    }
    // Restore every touched wire in place. A split keeps the original
    // wire's id, so the wire is still there and the restore overwrites it
    // (the push_back is only a defensive fallback).
    for (const auto& [id, w] : e.wire_before) {
        if (Wire* live = d.find_wire(id)) {
            *live = w;
        } else {
            (void)d.restore_wire(w);
        }
    }
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
    const Entry& e = list_[at_];
    apply_overlay(d, e.overlay);
    for (const Step& s : e.steps) {
        replay_step(d, s);
    }
    ++at_;
    return true;
}

}  // namespace ws::sch
