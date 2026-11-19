// ws::sch commands + connect-on-drop tests (phase 3). THE REQUIRED TESTS:
//   * Undo/redo restores nodes, wires and nets EXACTLY (REQUIRED 6);
//   * connect-on-drop: during a drag the candidates are computed and
//     exposed; on release ONLY the shown candidates are created;
//   * wires moved over other wires never auto-connect;
//   * a PinOnWire candidate lands as the SPLIT (the T-connection) + tie,
//     a PinOnPin candidate lands as a wire - and both undo exactly.
// (main lives in test_model.cpp.)
#include <doctest/doctest.h>

#include <algorithm>

#include <ws/schematic/model.hpp>
#include <ws/schematic/commands.hpp>

using ws::sch::Doc;
using ws::sch::DropCandidate;
using ws::sch::MoveSession;
using ws::sch::NodeId;
using ws::sch::Overlay;
using ws::sch::UndoStack;
using ws::sch::Wire;

static bool same_pairs(const std::vector<ws::sch::PinPair>& a,
                       const std::vector<ws::sch::PinPair>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

static std::vector<ws::sch::PinPair> canon(std::vector<ws::sch::PinPair> ps) {
    for (auto& p : ps) {
        if (p.first > p.second) {
            std::swap(p.first, p.second);
        }
    }
    std::sort(ps.begin(), ps.end());
    return ps;
}

// The snapshot the exact-restore assertions compare: everything that
// makes a document (node list, wire list, net list) - id-KEYED (slot
// order is a storage detail; an undo's re-insert lands at the tail).
struct Snap {
    std::vector<std::pair<NodeId, ws::sch::Node>> nodes;
    std::vector<std::pair<ws::sch::WireId, Wire>> wires;
    std::vector<std::vector<NodeId>> nets;
    bool operator==(const Snap& o) const {
        return nodes == o.nodes && wires == o.wires && nets == o.nets;
    }
};

static Snap snap(const Doc& d) {
    Snap s;
    for (const auto& n : d.nodes) {
        s.nodes.emplace_back(n.id, n);
    }
    for (const auto& w : d.wires) {
        s.wires.emplace_back(w.id, w);
    }
    std::sort(s.nodes.begin(), s.nodes.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::sort(s.wires.begin(), s.wires.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    s.nets = ws::sch::build_nets(d).nets;
    return s;
}

TEST_CASE("drop_candidates: a pin dragged onto a wire's body offers PinOnWire") {
    Doc d;
    const NodeId p1 = d.add_pin(1, 0, {0.0, 0.0});
    const NodeId p2 = d.add_pin(2, 0, {20.0, 0.0});
    const auto w = d.add_wire(p1, p2, {{10.0, 0.0}});
    const NodeId mover = d.add_pin(3, 0, {10.0, 5.0});
    CHECK(d.find_wire(w) != nullptr);

    MoveSession s;
    s.land[mover] = {10.0, 0.0};  // dropped exactly on the wire's path
    const Overlay ov = ws::sch::resolve_move(d, s);

    const auto cands = ws::sch::drop_candidates(d, ov);
    REQUIRE(cands.size() == 1);
    CHECK(cands[0].kind == DropCandidate::Kind::PinOnWire);
    CHECK(cands[0].pin == mover);
    CHECK(cands[0].wire == w);
    CHECK(cands[0].at.x == 10.0);
    CHECK(cands[0].at.y == 0.0);
}

TEST_CASE("drop_candidates: a pin dragged onto another pin offers PinOnPin") {
    Doc d;
    const NodeId there = d.add_pin(1, 0, {5.0, 5.0});
    const NodeId mover = d.add_pin(2, 0, {0.0, 0.0});
    CHECK(d.find_node(there) != nullptr);

    MoveSession s;
    s.land[mover] = {5.0, 5.0};  // exactly on the other pin
    const Overlay ov = ws::sch::resolve_move(d, s);

    const auto cands = ws::sch::drop_candidates(d, ov);
    REQUIRE(cands.size() == 1);
    CHECK(cands[0].kind == DropCandidate::Kind::PinOnPin);
    // The pair, canonicalized to the smaller id as `pin`.
    const bool pair_a = cands[0].pin == mover && cands[0].other == there;
    const bool pair_b = cands[0].pin == there && cands[0].other == mover;
    const bool is_pair = pair_a || pair_b;
    CHECK(is_pair);
}

TEST_CASE("drop_candidates: a WIRE moved over another wire offers NOTHING") {
    // REQUIRED: wires moved over other wires never auto-connect - only
    // pins offer candidates.
    Doc d;
    const NodeId p1 = d.add_pin(1, 0, {0.0, 0.0});
    const NodeId p2 = d.add_pin(2, 0, {20.0, 0.0});
    d.add_wire(p1, p2);
    const NodeId q1 = d.add_pin(3, 0, {10.0, -8.0});
    const NodeId q2 = d.add_pin(4, 0, {10.0, 8.0});
    const auto crossing = d.add_wire(q1, q2);
    CHECK(d.find_wire(crossing) != nullptr);

    // Drag the vertical wire's end over the horizontal one (an end
    // move): the crossing point lies on both paths - still nothing.
    MoveSession s;
    s.ends.push_back({crossing, false, {10.0, 0.0}});
    const Overlay ov = ws::sch::resolve_move(d, s);
    CHECK(ws::sch::drop_candidates(d, ov).empty());
}

TEST_CASE("drop_candidates: a pin NOT on anything offers nothing") {
    Doc d;
    const NodeId p1 = d.add_pin(1, 0, {0.0, 0.0});
    const NodeId p2 = d.add_pin(2, 0, {20.0, 0.0});
    d.add_wire(p1, p2);
    const NodeId mover = d.add_pin(3, 0, {50.0, 50.0});

    MoveSession s;
    s.land[mover] = {60.0, 60.0};
    const Overlay ov = ws::sch::resolve_move(d, s);
    CHECK(ws::sch::drop_candidates(d, ov).empty());
}

// --- the command + undo stack ------------------------------------------------

TEST_CASE("commit: the overlay lands as ONE command; undo/redo exact (REQUIRED 6)") {
    Doc d;
    const NodeId p1 = d.add_pin(1, 0, {10.0, 0.0});
    const NodeId p2 = d.add_pin(2, 0, {24.0, 0.0});
    const auto w = d.add_wire(p1, p2, {{10.0, 6.0}, {24.0, 6.0}}, 90, 270);
    const NodeId stray = d.add_junction({40.0, 40.0});
    CHECK(d.find_wire(w) != nullptr);
    CHECK(d.find_node(stray) != nullptr);

    MoveSession s;
    s.land[p2] = {30.0, 4.0};
    const Overlay ov = ws::sch::resolve_move(d, s);
    const auto before = snap(d);
    const auto conns_before = canon(ws::sch::pin_connections(d));

    UndoStack undo;
    ws::sch::commit_move(d, undo, ov, {});  // no candidates shown
    CHECK(d.find_node(p2)->pos.x == 30.0);
    CHECK(d.find_node(p2)->pos.y == 4.0);
    REQUIRE(undo.size() == 1);
    CHECK(undo.name(0) == "Move");

    // Connections unchanged (a pure move).
    CHECK(same_pairs(canon(ws::sch::pin_connections(d)), conns_before));

    // UNDO restores nodes, wires and nets EXACTLY.
    CHECK(undo.undo(d));
    CHECK(snap(d) == before);
    // REDO restores the committed state exactly; UNDO again: exact.
    CHECK(undo.redo(d));
    CHECK(d.find_node(p2)->pos.x == 30.0);
    CHECK(undo.undo(d));
    CHECK(snap(d) == before);
}

TEST_CASE("commit: a shown PinOnWire candidate lands as the split; undo exact") {
    Doc d;
    const NodeId p1 = d.add_pin(1, 0, {0.0, 0.0});
    const NodeId p2 = d.add_pin(2, 0, {20.0, 0.0});
    const auto w = d.add_wire(p1, p2, {{10.0, 0.0}});
    const NodeId mover = d.add_pin(3, 0, {10.0, 5.0});
    const auto own = d.add_wire(mover, d.add_junction({10.0, 5.0}));
    CHECK(d.find_wire(w) != nullptr);
    CHECK(d.find_wire(own) != nullptr);

    MoveSession s;
    s.land[mover] = {10.0, 0.0};
    const Overlay ov = ws::sch::resolve_move(d, s);
    const auto cands = ws::sch::drop_candidates(d, ov);
    REQUIRE(cands.size() == 1);
    const auto before = snap(d);
    const auto conns_before = canon(ws::sch::pin_connections(d));

    UndoStack undo;
    ws::sch::commit_move(d, undo, ov, cands);
    // The wire was SPLIT (2 halves + a junction at the point) and the
    // dropped pin rides a tie wire to that junction: the explicit
    // connect-on-drop. Total wires: mover's own + 2 halves + tie = 4.
    CHECK(d.wires.size() == 4);
    const auto conns_after = canon(ws::sch::pin_connections(d));
    CHECK(conns_after.size() > conns_before.size());

    // Undo: the split un-happens EXACTLY (nodes, wires, nets).
    CHECK(undo.undo(d));
    CHECK(snap(d) == before);
    CHECK(undo.redo(d));
    CHECK(d.wires.size() == 4);
    CHECK(undo.undo(d));
    CHECK(snap(d) == before);
}

TEST_CASE("commit: an UNSEEN candidate never lands (only what was shown)") {
    // The drag showed nothing (the editor never rendered the snap
    // rings) but a candidate WOULD exist at the landing: the commit
    // creates only what was shown - nothing.
    Doc d;
    const NodeId p1 = d.add_pin(1, 0, {0.0, 0.0});
    const NodeId p2 = d.add_pin(2, 0, {20.0, 0.0});
    const auto w = d.add_wire(p1, p2, {{10.0, 0.0}});
    const NodeId mover = d.add_pin(3, 0, {10.0, 5.0});
    CHECK(d.find_wire(w) != nullptr);

    MoveSession s;
    s.land[mover] = {10.0, 0.0};
    const Overlay ov = ws::sch::resolve_move(d, s);
    REQUIRE(ws::sch::drop_candidates(d, ov).size() == 1);
    UndoStack undo;
    ws::sch::commit_move(d, undo, ov, {});
    // Nothing but the MOVE landed: no new wires, no new nodes, no new
    // nets - the pin sits on the path connected to nothing.
    CHECK(d.wires.size() == 1);
    CHECK(d.nodes.size() == 3);
    CHECK(ws::sch::build_nets(d).nets.size() == 2);
    CHECK(d.find_node(mover)->pos.x == 10.0);
    CHECK(d.find_node(mover)->pos.y == 0.0);
}

TEST_CASE("commit: a PinOnPin candidate lands as a wire between the pins") {
    Doc d;
    const NodeId there = d.add_pin(1, 0, {5.0, 5.0});
    const NodeId mover = d.add_pin(2, 0, {0.0, 0.0});
    CHECK(d.find_node(there) != nullptr);

    MoveSession s;
    s.land[mover] = {5.0, 5.0};
    const Overlay ov = ws::sch::resolve_move(d, s);
    const auto cands = ws::sch::drop_candidates(d, ov);
    REQUIRE(cands.size() == 1);

    const auto before = snap(d);
    UndoStack undo;
    ws::sch::commit_move(d, undo, ov, cands);
    CHECK(d.wires.size() == 1);
    CHECK(ws::sch::pin_connections(d).size() == 1);
    CHECK(undo.undo(d));
    CHECK(snap(d) == before);
    CHECK(d.wires.empty());
}

TEST_CASE("undo stack: multiple moves, full walk back and forth") {
    Doc d;
    const NodeId p1 = d.add_pin(1, 0, {0.0, 0.0});
    const NodeId p2 = d.add_pin(2, 0, {20.0, 0.0});
    const auto w = d.add_wire(p1, p2);
    CHECK(d.find_wire(w) != nullptr);
    UndoStack undo;
    for (int step = 1; step <= 5; ++step) {
        MoveSession s;
        s.land[p2] = {20.0 + step * 4.0, 0.0};
        const Overlay ov = ws::sch::resolve_move(d, s);
        ws::sch::commit_move(d, undo, ov, {});
    }
    CHECK(undo.size() == 5);
    while (undo.undo(d)) {
    }
    CHECK(d.find_node(p2)->pos.x == 20.0);
    while (undo.redo(d)) {
    }
    CHECK(d.find_node(p2)->pos.x == 40.0);  // 20 + 5 * 4
    CHECK(undo.undo(d));
    CHECK(d.find_node(p2)->pos.x == 36.0);
    CHECK(undo.redo(d));
    CHECK(d.find_node(p2)->pos.x == 40.0);
}

TEST_CASE("the stack pushes a fresh command only for a non-empty gesture") {
    Doc d;
    const NodeId p1 = d.add_pin(1, 0, {0.0, 0.0});
    CHECK(d.find_node(p1) != nullptr);
    UndoStack undo;
    MoveSession s;  // nothing in it
    const Overlay ov = ws::sch::resolve_move(d, s);
    ws::sch::commit_move(d, undo, ov, {});
    CHECK(undo.empty());  // a no-op gesture is no undo step
}

TEST_CASE("net exactness: undo/redo after a split restores the net list") {
    // REQUIRED 6 with a topology change: the split changes the nets;
    // undo restores the EXACT net list (node id sets), not just counts.
    Doc d;
    const NodeId p1 = d.add_pin(1, 0, {0.0, 0.0});
    const NodeId p2 = d.add_pin(2, 0, {20.0, 0.0});
    const auto w = d.add_wire(p1, p2, {{10.0, 0.0}});
    const NodeId mover = d.add_pin(3, 0, {10.0, 5.0});
    CHECK(d.find_wire(w) != nullptr);

    MoveSession s;
    s.land[mover] = {10.0, 0.0};
    const Overlay ov = ws::sch::resolve_move(d, s);
    const auto cands = ws::sch::drop_candidates(d, ov);
    const auto nets_before = ws::sch::build_nets(d).nets;

    UndoStack undo;
    ws::sch::commit_move(d, undo, ov, cands);
    const auto nets_after = ws::sch::build_nets(d).nets;
    // The drop JOINED the mover pin to the wire's net: one net now (the
    // explicit connection), not two.
    CHECK(nets_after.size() == 1);
    CHECK(nets_before.size() == 2);
    CHECK(undo.undo(d));
    CHECK(ws::sch::build_nets(d).nets == nets_before);
    CHECK(undo.redo(d));
    CHECK(ws::sch::build_nets(d).nets == nets_after);
}
