// ws::sch model + nets tests (phase 1): the node graph, wires over node
// ids, split/merge as the ONLY T-connection, and the invariants that
// make the rewrite's promise true in the model itself:
//   * geometry never creates connectivity (crossing wires, a node on a
//     wire's path, coincident pins - none of it connects);
//   * the split IS the connection (no interior attachments anywhere);
//   * identical graphs answer identical nets (positions play no part).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <random>

#include <ws/schematic/model.hpp>

using ws::sch::Doc;
using ws::sch::NodeId;
using ws::sch::PinPair;

struct Fixture {
    Doc d;
    NodeId p1a{}, p1b{}, p2a{}, p2b{};

    Fixture() {
        p1a = d.add_pin(1, 0, {0.0, 0.0});
        p1b = d.add_pin(1, 1, {0.0, 3.0});
        p2a = d.add_pin(2, 0, {8.0, 0.0});
        p2b = d.add_pin(2, 1, {8.0, 3.0});
    }
};

TEST_CASE("nodes: kinds, ids, storage round trip") {
    Fixture f;
    REQUIRE(f.d.nodes.size() == 4);
    CHECK(f.d.find_node(f.p1a)->kind == ws::sch::NodeKind::Pin);
    CHECK(f.d.find_node(f.p1a)->part == 1);
    CHECK(f.d.find_node(f.p1a)->pin == 0);
    const NodeId j = f.d.add_junction({4.0, 0.0});
    CHECK(f.d.find_node(j)->kind == ws::sch::NodeKind::Junction);
    CHECK(f.d.find_node(j)->is_pin() == false);
    CHECK(f.d.find_node(9999) == nullptr);
}

TEST_CASE("wires: endpoint nodes are connectivity; waypoints are cosmetic") {
    Fixture f;
    const auto w = f.d.add_wire(f.p1a, f.p2a, {{4.0, 0.0}});
    REQUIRE(f.d.wires.size() == 1);
    CHECK(f.d.wires[0].a == f.p1a);
    const auto path = f.d.wire_path(w);
    REQUIRE(path.size() == 3);
    CHECK(path[1].x == 4.0);
}

TEST_CASE("geometry never creates connectivity: crossing wires stay split") {
    // REQUIRED TEST 2 (model half): a wire crossing another wire never
    // merges nets - even when the crossing point is a waypoint of one.
    Fixture f;
    f.d.add_wire(f.p1a, f.p2a, {{4.0, 0.0}});
    const NodeId t = f.d.add_junction({4.0, -4.0});
    const NodeId btm = f.d.add_junction({4.0, 4.0});
    f.d.add_wire(t, btm);

    const auto nets = ws::sch::build_nets(f.d);
    CHECK(nets.nets.size() == 4);  // the two wire nets + p1b/p2b singletons
    CHECK(pin_connections(f.d).size() == 1);  // only (p1a,p2a)
}

TEST_CASE("geometry never creates connectivity: a node ON a wire's path") {
    Fixture f;
    f.d.add_wire(f.p1a, f.p2a, {{4.0, 0.0}});
    const NodeId stray = f.d.add_junction({4.0, 0.0});
    CHECK(f.d.find_node(stray) != nullptr);
    const auto nets = ws::sch::build_nets(f.d);
    CHECK(nets.nets.size() == 4);  // the dot riding the path joins nothing
    CHECK(pin_connections(f.d).size() == 1);
}

TEST_CASE("geometry never creates connectivity: coincident pins") {
    Doc d;
    const NodeId a = d.add_pin(1, 0, {5.0, 5.0});
    const NodeId b = d.add_pin(2, 0, {5.0, 5.0});
    CHECK(d.find_node(a) != nullptr);
    CHECK(d.find_node(b) != nullptr);
    CHECK(ws::sch::build_nets(d).nets.size() == 2);
    CHECK(pin_connections(d).empty());
}

TEST_CASE("split_wire: the T-connection IS the split") {
    Fixture f;
    const auto w = f.d.add_wire(f.p1a, f.p2a, {{4.0, 0.0}});
    const auto s = f.d.split_wire(w, {4.0, 0.0});
    REQUIRE(s.has_value());
    CHECK(f.d.find_wire(w) == nullptr);  // wait: the slot was REUSED for the
    // first half - find_wire(w) returns the first half (same id? NO:
    // first.id is a FRESH id; the slot keeps the new id). Check by count.
    CHECK(f.d.wires.size() == 2);
    const auto* j = f.d.find_node(s->junction);
    REQUIRE(j != nullptr);
    CHECK(j->kind == ws::sch::NodeKind::Junction);
    CHECK(j->pos.x == 4.0);
    CHECK(f.d.find_wire(s->first)->a == f.p1a);
    CHECK(f.d.find_wire(s->first)->b == s->junction);
    CHECK(f.d.find_wire(s->second)->a == s->junction);
    CHECK(f.d.find_wire(s->second)->b == f.p2a);

    // A branch wire onto the junction: all pins, one net with them.
    const NodeId branch = f.d.add_pin(3, 0, {4.0, 6.0});
    f.d.add_wire(s->junction, branch);
    const auto nets = ws::sch::build_nets(f.d);
    CHECK(nets.nets.size() == 3);  // joined net + p1b/p2b singletons
    CHECK(pin_connections(f.d).size() == 3);  // p1a-p2a, p1a-branch, p2a-branch
}

TEST_CASE("split_wire: off-path point refuses; at-endpoint refuses") {
    Fixture f;
    const auto w = f.d.add_wire(f.p1a, f.p2a);
    CHECK(f.d.split_wire(w, {4.0, 7.0}) == std::nullopt);
    CHECK(f.d.wires.size() == 1);
    CHECK(f.d.split_wire(w, {0.0, 0.0}) == std::nullopt);  // endpoint a
    CHECK(f.d.split_wire(w, {8.0, 0.0}) == std::nullopt);  // endpoint b
}

TEST_CASE("merge_split: the inverse, and only for split pairs") {
    Fixture f;
    const auto w = f.d.add_wire(f.p1a, f.p2a, {{4.0, 0.0}});
    const auto s = f.d.split_wire(w, {4.0, 0.0});
    REQUIRE(s.has_value());

    // A third wire on the junction: merge refused.
    const NodeId third = f.d.add_pin(3, 0, {4.0, 6.0});
    f.d.add_wire(s->junction, third);
    CHECK(f.d.merge_split(s->junction) == 0);
    CHECK(f.d.wires.size() == 3);

    // Remove the branch; the merge restores ONE wire, path intact.
    f.d.erase_wire(f.d.wires.back().id);
    const auto merged = f.d.merge_split(s->junction);
    REQUIRE(merged != 0);
    CHECK(f.d.wires.size() == 1);
    CHECK(f.d.find_node(s->junction) == nullptr);
    const auto& path = f.d.wire_path(merged);
    REQUIRE(path.size() == 3);
    CHECK(path[1].x == 4.0);
    CHECK(pin_connections(f.d).size() == 1);
}

TEST_CASE("wire_path: a missing endpoint answers empty (dangling wire)") {
    Fixture f;
    const auto w = f.d.add_wire(f.p1a, f.p2a);
    f.d.erase_wire(w);
    CHECK(f.d.wire_path(w).empty());
}

TEST_CASE("labels: authored binding + same-name merge") {
    Fixture f;
    const auto w1 = f.d.add_wire(f.p1a, f.p1b);
    const auto w2 = f.d.add_wire(f.p2a, f.p2b);
    f.d.add_label({0.0, 1.0}, "out", w1);
    f.d.add_label({8.0, 1.0}, "OUT", w2);
    CHECK(ws::sch::build_nets(f.d).nets.size() == 1);  // all four pins
    CHECK(pin_connections(f.d).size() == 6);          // one net: every pair
}

TEST_CASE("labels: an UNBOUND label is a decoration") {
    Doc d;
    const NodeId p1 = d.add_pin(1, 0, {0.0, 0.0});
    const NodeId p2 = d.add_pin(2, 0, {10.0, 0.0});
    d.add_wire(p1, p2);
    d.add_label({5.0, 0.0}, "vcc", 0);
    CHECK(ws::sch::build_nets(d).nets.size() == 1);  // the wire net only
}

TEST_CASE("net_of: lookup by node") {
    Fixture f;
    f.d.add_wire(f.p1a, f.p1b);
    const auto nets = ws::sch::build_nets(f.d);
    const auto* n = nets.net_of(f.p1a);
    REQUIRE(n != nullptr);
    CHECK(nets.net_of(f.p1b) == n);
    CHECK(nets.net_of(f.p2a) != nullptr);  // a singleton net of its own
}

TEST_CASE("net ordering is stable (sorted members, order by first id)") {
    Fixture f;
    f.d.add_wire(f.p2b, f.p1b);
    f.d.add_wire(f.p1a, f.p2a);
    const auto nets = ws::sch::build_nets(f.d);
    REQUIRE(nets.nets.size() == 2);
    CHECK(std::is_sorted(nets.nets[0].begin(), nets.nets[0].end()));
    CHECK(nets.nets[0] < nets.nets[1]);
}

// --- the property half (phase 2 rides resolve_move; graph-level here) -------

static std::vector<std::vector<NodeId>> canon_nets(const Doc& d) {
    auto nets = ws::sch::build_nets(d).nets;
    std::sort(nets.begin(), nets.end());
    return nets;
}

TEST_CASE("identical graphs answer identical nets (the property's core)") {
    // Property test 1's model half: connectivity is a function of the
    // GRAPH, so the same graph - however drawn, wherever positioned -
    // answers the same nets.
    Doc a;
    {
        const NodeId p1 = a.add_pin(1, 0, {0.0, 0.0});
        const NodeId p2 = a.add_pin(2, 0, {30.0, 0.0});
        const NodeId p3 = a.add_pin(3, 0, {60.0, 0.0});
        a.add_wire(p1, p2, {{15.0, 0.0}});
        a.add_wire(p2, p3, {{45.0, 0.0}});
    }
    Doc b;
    {
        const NodeId p1 = b.add_pin(1, 0, {-100.0, 900.0});
        const NodeId p2 = b.add_pin(2, 0, {7.0, 3.0});
        const NodeId p3 = b.add_pin(3, 0, {12.0, -6.0});
        b.add_wire(p1, p2, {{-50.0, 900.0}, {-50.0, 3.0}, {7.0, 3.0}});
        b.add_wire(p2, p3, {{7.0, -6.0}, {12.0, -6.0}});
    }
    CHECK(pin_connections(a).size() == 3);
    CHECK(pin_connections(a) == pin_connections(b));
    CHECK(canon_nets(a).size() == canon_nets(b).size());
}

TEST_CASE("a wire dragged over another wire never merges (randomized)") {
    // Property test 2, as a pure graph property: wire PATHS are
    // irrelevant to nets, so however a randomized layout throws wires
    // over each other, the net count is the wire-component count.
    std::mt19937 rng(7);
    for (int iter = 0; iter < 200; ++iter) {
        Doc d;
        std::vector<NodeId> pins;
        for (std::uint32_t part = 1; part <= 4; ++part) {
            pins.push_back(
                d.add_pin(part, 0, {static_cast<double>(rng() % 20 - 10),
                                    static_cast<double>(rng() % 20 - 10)}));
        }
        d.add_wire(pins[0], pins[1], {{3.0, 3.0}, {-3.0, -3.0}});
        d.add_wire(pins[2], pins[3], {{-3.0, 3.0}, {3.0, -3.0}});
        CHECK(ws::sch::build_nets(d).nets.size() == 2);
    }
}

TEST_CASE("moving a part never opens a connection (randomized)") {
    // Property test 3's model half: simulate a move by rewriting pin
    // node POSITIONS (what a move is in graph terms - ids never change)
    // and assert the nets are the same VALUE, not just count.
    std::mt19937 rng(11);
    for (int iter = 0; iter < 200; ++iter) {
        Doc d;
        std::vector<NodeId> pins;
        for (std::uint32_t part = 1; part <= 5; ++part) {
            pins.push_back(
                d.add_pin(part, 0, {static_cast<double>(rng() % 30 - 15),
                                    static_cast<double>(rng() % 30 - 15)}));
        }
        const int nw = 2 + static_cast<int>(rng() % 4);
        for (int i = 0; i < nw; ++i) {
            const NodeId a = pins[rng() % pins.size()];
            NodeId b = pins[rng() % pins.size()];
            if (b == a) {
                b = pins[(b + 1) % pins.size()];
            }
            std::vector<ws::sch::Vec2> mid;
            for (int k = 0; k < static_cast<int>(rng() % 3); ++k) {
                mid.push_back({static_cast<double>(rng() % 20 - 10),
                               static_cast<double>(rng() % 20 - 10)});
            }
            d.add_wire(a, b, mid);
        }
        const auto before = canon_nets(d);
        const auto conns = pin_connections(d);
        // THE MOVE: pins wander; wires keep their endpoint node ids.
        for (auto& n : d.nodes) {
            if (n.is_pin()) {
                n.pos = {static_cast<double>(rng() % 200 - 100),
                         static_cast<double>(rng() % 200 - 100)};
            }
        }
        for (auto& w : d.wires) {
            for (auto& m : w.mid) {
                m = {static_cast<double>(rng() % 50 - 25),
                     static_cast<double>(rng() % 50 - 25)};
            }
        }
        CHECK(canon_nets(d) == before);
        CHECK(pin_connections(d) == conns);
    }
}

TEST_CASE("split changes nets exactly by the new junction's joins") {
    // The split is an EDIT (authored), not geometry: nets change because
    // the graph changed, and only for the wires that now MEET at the
    // junction.
    Doc d;
    const NodeId p1 = d.add_pin(1, 0, {0.0, 0.0});
    const NodeId p2 = d.add_pin(2, 0, {10.0, 0.0});
    const auto w = d.add_wire(p1, p2);
    const auto before = canon_nets(d);

    const auto s = d.split_wire(w, {5.0, 0.0});
    REQUIRE(s.has_value());
    const auto after = canon_nets(d);
    REQUIRE(after.size() == 1);
    CHECK(after[0].size() == before[0].size() + 1);  // the junction joined
    CHECK(pin_connections(d).size() == 1);
}
