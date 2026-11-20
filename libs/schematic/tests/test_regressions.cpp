// ws::sch regression tests: one test per bug found in the review of
// libs/schematic, plus the canonical-document checks that make "undo is
// the exact inverse, redo rebuilds the identical document" a TESTED claim
// rather than a comment (the older tests compared nets / pin pairs only).
#include <doctest/doctest.h>

#include <algorithm>
#include <random>

#include <ws/schematic/commands.hpp>
#include <ws/schematic/model.hpp>

using namespace ws::sch;

namespace {

constexpr NodeId kLabelBit = 0x8000'0000u;

// Does the net containing `pin` also contain label slot `label_index`?
bool label_joins(const Doc& d, NodeId pin, std::size_t label_index) {
    for (const auto& net : build_nets(d).nets) {
        const bool has_pin =
            std::find(net.begin(), net.end(), pin) != net.end();
        const bool has_lbl = std::find(net.begin(), net.end(),
                                       kLabelBit | label_index) != net.end();
        if (has_pin && has_lbl) {
            return true;
        }
    }
    return false;
}

// The document in canonical form (entities sorted by id): vector order is
// not part of a document's identity, ids and contents are.
struct Canon {
    std::vector<Node> nodes;
    std::vector<Wire> wires;
    std::vector<Label> labels;
    bool operator==(const Canon&) const = default;
};

Canon canon(const Doc& d) {
    Canon c{d.nodes, d.wires, d.labels};
    std::sort(c.nodes.begin(), c.nodes.end(),
              [](const Node& a, const Node& b) { return a.id < b.id; });
    std::sort(c.wires.begin(), c.wires.end(),
              [](const Wire& a, const Wire& b) { return a.id < b.id; });
    std::sort(c.labels.begin(), c.labels.end(),
              [](const Label& a, const Label& b) { return a.id < b.id; });
    return c;
}

}  // namespace

// BUG 1 - build_nets: the first bound label's slot equalled the "unbound"
// sentinel (nodes.size()), so it was silently left out of every net.
TEST_CASE("regress: the FIRST bound label appears in its net") {
    Doc d;
    NodeId a = d.add_pin(1, 0, {0, 0}), b = d.add_pin(2, 0, {10, 0});
    WireId w = d.add_wire(a, b);
    d.add_label({5, 0}, "VCC", w);
    CHECK(label_joins(d, a, 0));
}

// BUG 2 - split_wire retired the wire's id, so every label bound to it
// dangled and its net name was lost.
TEST_CASE("regress: splitting a wire keeps its bound label on the net") {
    Doc d;
    NodeId c = d.add_pin(3, 0, {0, 5}), e = d.add_pin(4, 0, {10, 5});
    d.add_label({5, 5}, "X", d.add_wire(c, e));
    NodeId a = d.add_pin(1, 0, {0, 0}), b = d.add_pin(2, 0, {10, 0});
    WireId w = d.add_wire(a, b);
    d.add_label({5, 0}, "VCC", w);
    REQUIRE(label_joins(d, a, 1));
    const auto s = d.split_wire(w, {5, 0});
    REQUIRE(s.has_value());
    CHECK(s->first == w);
    CHECK(label_joins(d, a, 1));
    CHECK(label_joins(d, b, 1));  // still ONE net across the junction
}

TEST_CASE("regress: merge_split re-binds a label bound to the absorbed half") {
    Doc d;
    NodeId a = d.add_pin(1, 0, {0, 0}), b = d.add_pin(2, 0, {10, 0});
    WireId w = d.add_wire(a, b);
    d.add_label({0, 9}, "dummy", 0);  // unbound decoration, shifts indices
    const auto s = d.split_wire(w, {5, 0});
    REQUIRE(s.has_value());
    const LabelId on_second = d.add_label({8, 0}, "N1", s->second);
    REQUIRE(on_second != 0);
    const WireId merged = d.merge_split(s->junction);
    REQUIRE(merged == w);
    CHECK(d.find_label(on_second)->wire == w);
}

TEST_CASE("regress: a chain of splits merges back junction by junction") {
    Doc d;
    NodeId a = d.add_pin(1, 0, {0, 0}), b = d.add_pin(2, 0, {12, 0});
    WireId w = d.add_wire(a, b);
    const auto s1 = d.split_wire(w, {4, 0});
    REQUIRE(s1.has_value());
    const auto s2 = d.split_wire(s1->second, {8, 0});
    REQUIRE(s2.has_value());
    CHECK(d.wires.size() == 3);
    CHECK(d.merge_split(s2->junction) != 0);
    CHECK(d.merge_split(s1->junction) != 0);
    CHECK(d.wires.size() == 1);
    CHECK(d.wires.front().split_of == 0);  // no pieces left: no group
    CHECK(pin_connections(d).size() == 1);
}

TEST_CASE("regress: merge_split of an allow_any dot drawn head-to-head") {
    Doc d;
    NodeId a = d.add_pin(1, 0, {0, 0}), b = d.add_pin(2, 0, {10, 0});
    NodeId j = d.add_junction({5, 0});
    d.add_wire(a, j);
    d.add_wire(b, j);  // both wires END at the junction
    const WireId m = d.merge_split(j, /*allow_any=*/true);
    REQUIRE(m != 0);
    CHECK(d.find_node(j) == nullptr);
    const Wire* w = d.find_wire(m);
    REQUIRE(w != nullptr);
    CHECK(d.find_node(w->a) != nullptr);  // no endpoint names the removed node
    CHECK(d.find_node(w->b) != nullptr);
}

// BUG 3 - two pins dropped on one wire: the second candidate named a wire
// id the first split had retired and was silently not connected.
TEST_CASE("regress: two pins dropped on one wire both connect; undo/redo exact") {
    Doc d;
    NodeId a = d.add_pin(1, 0, {0, 0}), b = d.add_pin(2, 0, {10, 0});
    d.add_wire(a, b);
    NodeId p = d.add_pin(3, 0, {3, 5}), q = d.add_pin(4, 0, {7, 5});
    const Canon before = canon(d);

    Overlay ov;
    ov.nodePos[p] = {3, 0};
    ov.nodePos[q] = {7, 0};
    const auto cands = drop_candidates(d, ov);
    REQUIRE(cands.size() == 2);

    UndoStack us;
    REQUIRE(commit_move(d, us, ov, cands));
    CHECK(pin_connections(d).size() == 6);  // 4 pins, one net
    const Canon committed = canon(d);

    REQUIRE(us.undo(d));
    CHECK(canon(d) == before);  // the EXACT document, ids and all

    REQUIRE(us.redo(d));
    CHECK(canon(d) == committed);  // redo rebuilds the identical document
    CHECK(pin_connections(d).size() == 6);
}

TEST_CASE("regress: two pins dropped on the SAME spot of one wire") {
    Doc d;
    NodeId a = d.add_pin(1, 0, {0, 0}), b = d.add_pin(2, 0, {10, 0});
    d.add_wire(a, b);
    NodeId p = d.add_pin(3, 0, {5, 5}), q = d.add_pin(4, 0, {5, -5});
    const Canon before = canon(d);
    Overlay ov;
    ov.nodePos[p] = {5, 0};
    ov.nodePos[q] = {5, 0};
    const auto cands = drop_candidates(d, ov);
    UndoStack us;
    REQUIRE(commit_move(d, us, ov, cands));
    CHECK(pin_connections(d).size() == 6);  // all four on one net
    const Canon committed = canon(d);
    REQUIRE(us.undo(d));
    CHECK(canon(d) == before);
    REQUIRE(us.redo(d));
    CHECK(canon(d) == committed);
}

// BUG 4 - drop_candidates scanned ALL pins, so unrelated coincident pins
// offered a "connection" on every drag anywhere.
TEST_CASE("regress: stationary coincident pins offer nothing") {
    Doc d;
    d.add_pin(1, 0, {0, 0});
    d.add_pin(2, 0, {0, 0});
    NodeId far = d.add_pin(3, 0, {9, 9});
    Overlay ov;
    ov.nodePos[far] = {9, 8};
    CHECK(drop_candidates(d, ov).empty());
}

// BUG 5 - same scan: a wire rerouted over a STATIONARY pin offered
// PinOnWire although only dragged pins may create candidates.
TEST_CASE("regress: a rerouted wire over a stationary pin offers nothing") {
    Doc d;
    NodeId a = d.add_pin(1, 0, {0, 0}), b = d.add_pin(2, 0, {10, 0});
    WireId w = d.add_wire(a, b);
    d.add_pin(3, 0, {5, 4});
    Overlay ov;
    ov.wirePath[w] = {{0, 4}, {10, 4}};
    CHECK(drop_candidates(d, ov).empty());
}

// BUG 6 - split_wire stamped the junction ends as pinned and replay_split
// (redo) assigned directions differently, so redo did not rebuild the
// identical document. Redo now calls split_wire itself with the minted ids.
TEST_CASE("regress: split halves leave the junction end free") {
    Doc d;
    NodeId a = d.add_pin(1, 0, {0, 0}), b = d.add_pin(2, 0, {10, 0});
    WireId w = d.add_wire(a, b, {}, 0, 180);
    const auto s = d.split_wire(w, {5, 0});
    REQUIRE(s.has_value());
    CHECK(d.find_wire(s->first)->a_dir == 0);
    CHECK(d.find_wire(s->first)->b_dir == kDirFree);
    CHECK(d.find_wire(s->second)->a_dir == kDirFree);
    CHECK(d.find_wire(s->second)->b_dir == 180);
}

TEST_CASE("regress: commit/undo/redo of a bent-wire split is canonical") {
    Doc d;
    NodeId a = d.add_pin(1, 0, {0, 0}), b = d.add_pin(2, 0, {10, 6});
    d.add_wire(a, b, {{0, 6}}, 0, 180);  // L-shaped, exits pinned
    NodeId p = d.add_pin(3, 0, {4, 6}), r = d.add_pin(4, 0, {0, 3});
    const Canon before = canon(d);
    Overlay ov;
    ov.nodePos[p] = {4, 6};  // lands on the horizontal leg
    ov.nodePos[r] = {0, 3};  // lands on the vertical leg
    // (pins already at those spots in the doc: move them a step first)
    d.find_node(p)->pos = {4, 9};
    d.find_node(r)->pos = {3, 3};
    const Canon start = canon(d);
    (void)before;

    const auto cands = drop_candidates(d, ov);
    REQUIRE(cands.size() == 2);
    UndoStack us;
    REQUIRE(commit_move(d, us, ov, cands));
    const Canon committed = canon(d);
    CHECK(pin_connections(d).size() == 6);
    REQUIRE(us.undo(d));
    CHECK(canon(d) == start);
    REQUIRE(us.redo(d));
    CHECK(canon(d) == committed);
    REQUIRE(us.undo(d));
    CHECK(canon(d) == start);
}

// The property the whole command design rides on: for ANY sequence of
// drops (several pins on one wire, several on one spot, pins that land on
// nothing), undo walks back through EXACTLY the documents it came from and
// redo rebuilds EXACTLY the documents it left - ids, shapes, directions.
TEST_CASE("regress: random drops - undo/redo are canonical at every step") {
    for (unsigned seed = 1; seed <= 40; ++seed) {
        std::mt19937 rng(seed);
        const auto pick = [&](int lo, int hi) {
            return std::uniform_int_distribution<int>(lo, hi)(rng);
        };

        Doc d;
        constexpr int kWires = 4;
        for (int i = 0; i < kWires; ++i) {
            const double y = 10.0 * i;
            const NodeId a = d.add_pin(100 + i, 0, {0, y});
            const NodeId b = d.add_pin(200 + i, 0, {20, y});
            const WireId w = d.add_wire(a, b, {}, 0, 180);
            if (i % 2 == 0) {
                d.add_label({10, y}, "N" + std::to_string(i), w);
            }
        }
        std::vector<NodeId> loose;
        for (int i = 0; i < 12; ++i) {
            loose.push_back(d.add_pin(300 + i, 0, {double(pick(1, 19)), 5.0}));
        }

        std::vector<Canon> states{canon(d)};
        UndoStack us;
        for (int step = 0; step < 6; ++step) {
            Overlay ov;
            for (const NodeId p : loose) {
                if (pick(0, 2) == 0) {
                    ov.nodePos[p] = {double(pick(0, 20)),
                                     double(10 * pick(0, kWires - 1))};
                }
            }
            const auto cands = drop_candidates(d, ov);
            if (commit_move(d, us, ov, cands)) {
                states.push_back(canon(d));
            }
        }
        for (std::size_t i = states.size() - 1; i > 0; --i) {
            REQUIRE(us.undo(d));
            REQUIRE(canon(d) == states[i - 1]);
        }
        for (std::size_t i = 1; i < states.size(); ++i) {
            REQUIRE(us.redo(d));
            REQUIRE(canon(d) == states[i]);
        }
        // Every wire endpoint still names a live node (no dangling ends).
        for (const Wire& w : d.wires) {
            REQUIRE(d.find_node(w.a) != nullptr);
            REQUIRE(d.find_node(w.b) != nullptr);
        }
    }
}
