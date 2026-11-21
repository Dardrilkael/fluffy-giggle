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
#include <cassert>
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
    if (overlay.nodePos.empty()) {
        return out;  // nothing dragged: nothing to offer
    }

    // A node's effective position: the overlay's landing, else stored.
    const auto eff = [&](const Node& n) {
        const auto it = overlay.nodePos.find(n.id);
        return it != overlay.nodePos.end() ? it->second : n.pos;
    };

    // Every wire's effective path (overlay shapes for bent ones) + its
    // bounding box, built LAZILY and AT MOST ONCE per call: this runs on
    // every mouse-move of a drag, so it must not rebuild a wire's path
    // per dragged pin.
    struct EffPath {
        bool built = false;
        std::vector<Vec2> path;
        Vec2 lo{0.0, 0.0};
        Vec2 hi{0.0, 0.0};
    };
    std::vector<EffPath> cache(doc.wires.size());
    const auto eff_path = [&](std::size_t wi) -> const EffPath& {
        EffPath& e = cache[wi];
        if (e.built) {
            return e;
        }
        e.built = true;
        const Wire& w = doc.wires[wi];
        const Node* na = doc.find_node(w.a);
        const Node* nb = doc.find_node(w.b);
        if (na == nullptr || nb == nullptr) {
            return e;  // dangling: empty path
        }
        const auto wp = overlay.wirePath.find(w.id);
        const std::vector<Vec2>& mid =
            wp != overlay.wirePath.end() ? wp->second : w.mid;
        e.path.reserve(mid.size() + 2u);
        e.path.push_back(eff(*na));
        e.path.insert(e.path.end(), mid.begin(), mid.end());
        e.path.push_back(eff(*nb));
        e.lo = e.hi = e.path.front();
        for (const Vec2& p : e.path) {
            e.lo = {std::min(e.lo.x, p.x), std::min(e.lo.y, p.y)};
            e.hi = {std::max(e.hi.x, p.x), std::max(e.hi.y, p.y)};
        }
        return e;
    };

    // Only DRAGGED pins offer candidates (wires over wires never
    // auto-connect, and neither does a wire re-routed over a pin that did
    // not move). The overlay's node map is exactly the set of nodes that
    // landed somewhere new; std::map order keeps the answer deterministic.
    for (const auto& [pin_id, at] : overlay.nodePos) {
        const Node* n = doc.find_node(pin_id);
        if (n == nullptr || !n->is_pin()) {
            continue;
        }

        // PinOnPin: another PIN node at the same effective spot. `pin` is
        // always the dragged one. When BOTH pins were dragged onto each
        // other the pair is reported once, by the smaller id.
        for (const Node& other : doc.nodes) {
            if (!other.is_pin() || other.id == pin_id) {
                continue;
            }
            if (other.id < pin_id && overlay.nodePos.count(other.id) != 0) {
                continue;  // the smaller dragged pin reports this pair
            }
            if (near(eff(other), at)) {
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
        for (std::size_t wi = 0; wi < doc.wires.size(); ++wi) {
            const Wire& w = doc.wires[wi];
            if (w.a == pin_id || w.b == pin_id) {
                continue;
            }
            const EffPath& e = eff_path(wi);
            if (e.path.size() < 2u) {
                continue;
            }
            // Cheap reject: outside the path's bounding box.
            if (at.x < e.lo.x - detail::kEps || at.x > e.hi.x + detail::kEps ||
                at.y < e.lo.y - detail::kEps || at.y > e.hi.y + detail::kEps) {
                continue;
            }
            // An endpoint's own position is not a drop onto the path (if
            // a PIN sits there it is the PinOnPin case above).
            if (near(e.path.front(), at) || near(e.path.back(), at)) {
                continue;
            }
            for (std::size_t i = 1; i < e.path.size(); ++i) {
                if (on_segment(e.path[i - 1], e.path[i], at)) {
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
    // No dedup pass needed: (pin, other) pairs are unique by the rule
    // above, and a (pin, wire) pair is visited once.
    return out;
}

// --- the command ----------------------------------------------------------------

namespace {

using Step = LandedStep;

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

// The tie wire every landed step mints. ONE constructor for commit and
// replay: a tie is a plain two-node wire, free at both ends, no waypoints.
[[nodiscard]] Wire make_tie(WireId id, NodeId pin, NodeId other) {
    Wire t;
    t.id = id;
    t.a = pin;
    t.b = other;
    return t;
}

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
    NodeId tie_to = s.other;
    if (s.kind == Step::Kind::Split) {
        [[maybe_unused]] const auto split =
            d.split_wire(s.wire, s.at, SplitIds{s.junction, s.second});
        assert(split && "redo desync: a committed split no longer lands");
        tie_to = s.junction;
    }
    d.restore_wire(make_tie(s.tie, s.pin, tie_to));
}

}  // namespace

void UndoStack::push_entry(Entry e) {
    // Dropping the redo tail; erase (not resize) so Entry need not be
    // default-constructible.
    list_.erase(list_.begin() + static_cast<std::ptrdiff_t>(at_), list_.end());
    list_.push_back(std::move(e));
    if (list_.size() > kMaxEntries) {
        list_.erase(list_.begin());
    }
    at_ = list_.size();
}

bool commit_move(Doc& d, UndoStack& undo, const Overlay& overlay,
                 const std::vector<DropCandidate>& candidates) {
    if (overlay.empty() && candidates.empty()) {
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
            // A stale candidate (a node deleted since the drag showed it)
            // must not mint a wire to nowhere.
            if (d.find_node(c.pin) == nullptr ||
                d.find_node(c.other) == nullptr) {
                continue;
            }
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
    undo.push_entry(std::move(e));
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
    // (the restore_wire is only a defensive fallback).
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
