// ws::sch router + resolve_move tests (phase 2). THE REQUIRED TESTS:
//   Router goldens: both ends moved, one end moved, pin facing away
//   from target, collinear collapse, zero-length segment, loose-end
//   drag (6 cases).
//   Property: randomized move/rotate preserves the pin-pair set exactly;
//   a wire crossing another during a move never merges nets; moving a
//   part never opens a connection; preview overlay == committed geometry.
// (main lives in test_model.cpp.)
#include <doctest/doctest.h>

#include <algorithm>
#include <random>

#include <ws/schematic/model.hpp>

using ws::sch::Doc;
using ws::sch::MoveSession;
using ws::sch::Node;
using ws::sch::NodeId;
using ws::sch::Overlay;
using ws::sch::PinPair;
using ws::sch::Vec2;
using ws::sch::Wire;

static Doc apply(const Doc& d, const Overlay& ov) {
    Doc out = d;
    for (const auto& [id, pos] : ov.nodePos) {
        if (Node* n = out.find_node(id)) {
            n->pos = pos;
        }
    }
    for (const auto& [id, mid] : ov.wirePath) {
        if (Wire* w = out.find_wire(id)) {
            w->mid = mid;
        }
    }
    for (const auto& [k, dir] : ov.endDir) {
        if (Wire* w = out.find_wire(k.first)) {
            if (k.second) {
                w->a_dir = dir;
            } else {
                w->b_dir = dir;
            }
        }
    }
    return out;
}

static std::vector<Vec2> full_path(const Doc& d, ws::sch::WireId w,
                                    const Overlay& ov,
                                    const MoveSession* session = nullptr) {
    // The wire's path with the overlay applied (what the preview draws
    // and what the commit lands).
    const Wire* wire = d.find_wire(w);
    if (wire == nullptr) {
        return {};
    }
    std::vector<Vec2> path;
    const auto push_node = [&](NodeId n) {
        const auto it = ov.nodePos.find(n);
        const Node* node = d.find_node(n);
        if (node == nullptr) {
            return;
        }
        path.push_back(it != ov.nodePos.end() ? it->second : node->pos);
    };
    push_node(wire->a);
    const auto wit = ov.wirePath.find(w);
    if (wit != ov.wirePath.end()) {
        for (const Vec2& m : wit->second) {
            path.push_back(m);
        }
    } else {
        for (const Vec2& m : wire->mid) {
            path.push_back(m);
        }
    }
    push_node(wire->b);
    // A dragged loose end's landing (no node exists there yet - the
    // commit mints it): the session's point replaces that end.
    if (session != nullptr) {
        for (const auto& e : session->ends) {
            if (e.wire == w) {
                if (e.a_side) {
                    path.front() = e.to;
                } else {
                    path.back() = e.to;
                }
            }
        }
    }
    return path;
}

static std::size_t bends(const std::vector<Vec2>& p) {
    if (p.size() < 2u) {
        return 0;
    }
    std::size_t n = 0;
    for (std::size_t i = 1; i + 1 < p.size(); ++i) {
        const double cross = (p[i].x - p[i - 1].x) * (p[i + 1].y - p[i].y) -
                              (p[i].y - p[i - 1].y) * (p[i + 1].x - p[i].x);
        if (std::fabs(cross) > 1e-9) {
            ++n;
        }
    }
    return n;
}

static double length_of(const std::vector<Vec2>& p) {
    double s = 0.0;
    for (std::size_t i = 1; i < p.size(); ++i) {
        s += std::hypot(p[i].x - p[i - 1].x, p[i].y - p[i - 1].y);
    }
    return s;
}

static std::vector<PinPair> canon(std::vector<PinPair> ps) {
    for (auto& p : ps) {
        if (p.first > p.second) {
            std::swap(p.first, p.second);
        }
    }
    std::sort(ps.begin(), ps.end());
    return ps;
}

static bool same_pairs(const std::vector<PinPair>& a,
                       const std::vector<PinPair>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

struct RtFixture {
    Doc d;
    NodeId p1{}, p2{};
    ws::sch::WireId w{};

    RtFixture() {
        p1 = d.add_pin(1, 0, {10.0, 0.0});
        p2 = d.add_pin(2, 0, {24.0, 0.0});
        w = d.add_wire(p1, p2, {{10.0, 6.0}, {24.0, 6.0}}, 90, 270);
    }
};

TEST_CASE("golden 1: both ends moved -> rigid translate") {
    RtFixture f;
    MoveSession s;
    s.land[f.p1] = {10.0, -4.0};
    s.land[f.p2] = {24.0, -4.0};

    const Overlay ov = ws::sch::resolve_move(f.d, s);
    const auto path = full_path(f.d, f.w, ov);
    REQUIRE(path.size() == 4);
    CHECK(path[0].y == -4.0);
    CHECK(path[1].x == 10.0);
    CHECK(path[1].y == 2.0);
    CHECK(path[2].x == 24.0);
    CHECK(path[2].y == 2.0);
    CHECK(path[3].y == -4.0);
    CHECK(bends(path) == 2);  // the Z shape kept, translated
    CHECK(ov.nodePos.at(f.p1).y == -4.0);
    REQUIRE(ov.wirePath.count(f.w) == 1);
    REQUIRE(ov.wirePath.at(f.w).size() == 2);
    CHECK(ov.wirePath.at(f.w)[0].y == 2.0);
}

TEST_CASE("golden 2: one end moved -> fixed end's stub kept, slid waypoint") {
    RtFixture f;
    MoveSession s;
    s.land[f.p2] = {24.0, 4.0};

    const Overlay ov = ws::sch::resolve_move(f.d, s);
    const auto path = full_path(f.d, f.w, ov);
    REQUIRE(path.size() == 4);
    CHECK(path[0].x == 10.0);
    CHECK(path[0].y == 0.0);
    CHECK(path[1].x == 10.0);
    CHECK(path[1].y == 6.0);
    CHECK(path[2].x == 24.0);
    CHECK(path[3].x == 24.0);
    CHECK(path[3].y == 4.0);
    CHECK(bends(path) == 2);
}

TEST_CASE("golden 3: pin facing away from target -> legal pattern never backwards") {
    // The fixed pin faces +y (north) but the target lies SOUTH: the L
    // would leave the pin backwards (into its body). The cheapest LEGAL
    // pattern wins; the first leg must leave the pin along its face.
    Doc d;
    const NodeId p1 = d.add_pin(1, 0, {0.0, 10.0});
    const NodeId p2 = d.add_pin(2, 0, {0.0, 0.0});
    const auto w = d.add_wire(p1, p2, {}, 90, 270);

    MoveSession s;  // p2 slides 8 east
    s.land[p2] = {8.0, 0.0};
    const Overlay ov = ws::sch::resolve_move(d, s);
    const auto path = full_path(d, w, ov);

    REQUIRE(path.size() >= 3);
    CHECK(path[1].y > 10.0);  // first leg leaves along the face (+y)
    CHECK(bends(path) <= 3);  // an L or a U, never more
    for (std::size_t i = 1; i < path.size(); ++i) {
        const bool axis = (std::fabs(path[i].x - path[i - 1].x) < 1e-9) ||
                          (std::fabs(path[i].y - path[i - 1].y) < 1e-9);
        CHECK(axis);
    }
}

TEST_CASE("golden 4: collinear collapse (a landing in line folds the corner)") {
    Doc d;
    const NodeId p1 = d.add_pin(1, 0, {10.0, 0.0});
    const NodeId p2 = d.add_pin(2, 0, {24.0, 0.0});
    const auto w = d.add_wire(p1, p2, {}, -1, -1);
    MoveSession s;
    s.land[p2] = {36.0, 0.0};
    const Overlay ov = ws::sch::resolve_move(d, s);
    const auto path = full_path(d, w, ov);
    REQUIRE(path.size() == 2);
    CHECK(path[0].x == 10.0);
    CHECK(path[1].x == 36.0);
    CHECK(bends(path) == 0);  // straight, no matter the stored mids
}

TEST_CASE("golden 5: zero-length segment folds away") {
    RtFixture f;
    // Pin 2 lands where the corridor's corner used to be: the slid
    // waypoint equals an endpoint's position - no duplicate vertices,
    // no NaN.
    MoveSession s;
    s.land[f.p2] = {24.0, 6.0};
    const Overlay ov = ws::sch::resolve_move(f.d, s);
    REQUIRE(ov.wirePath.count(f.w) == 1);
    const auto& mid = ov.wirePath.at(f.w);
    for (std::size_t i = 1; i < mid.size(); ++i) {
        const bool dup = (mid[i].x == mid[i - 1].x) && (mid[i].y == mid[i - 1].y);
        CHECK(!dup);
    }
    for (const Vec2& m : mid) {
        CHECK(std::isfinite(m.x));
        CHECK(std::isfinite(m.y));
    }
}

TEST_CASE("golden 6: a wire dragged by its loose end (EndMove) routes free") {
    // The b end of a wire is dragged to a free point: the a end (a pin
    // with an exit dir) keeps its exit direction, the b end follows the
    // session's landing, cheapest pattern wins.
    RtFixture f;
    MoveSession s;
    s.ends.push_back({f.w, false, {40.0, 8.0}});
    const Overlay ov = ws::sch::resolve_move(f.d, s);
    const auto path = full_path(f.d, f.w, ov, &s);
    REQUIRE(path.size() >= 3);
    CHECK(path.back().x == 40.0);
    CHECK(path.back().y == 8.0);
    // The fixed end's PIN EXIT DIRECTION kept: the first leg leaves the
    // pin along its face (north), never diagonal.
    REQUIRE(path[1].x == 10.0);
    CHECK(path[1].y > 0.0);
    for (std::size_t i = 1; i < path.size(); ++i) {
        const bool axis = (std::fabs(path[i].x - path[i - 1].x) < 1e-9) ||
                          (std::fabs(path[i].y - path[i - 1].y) < 1e-9);
        CHECK(axis);
    }
}

TEST_CASE("property 1: random move/rotate preserves pin pairs exactly") {
    // REQUIRED TEST 1: randomized layouts, move/rotate random parts by
    // random deltas: the set of pin-to-pin connections before == after
    // (resolve_move alone never connects).
    std::mt19937 rng(1234);
    for (int iter = 0; iter < 300; ++iter) {
        Doc d;
        std::vector<NodeId> pins;
        std::vector<Vec2> pin_pos;
        const int n_parts = 2 + static_cast<int>(rng() % 4);
        for (std::uint32_t part = 1; part <= static_cast<std::uint32_t>(n_parts); ++part) {
            for (std::uint32_t pin = 0; pin < 2; ++pin) {
                const Vec2 at{static_cast<double>(rng() % 40 - 20),
                              static_cast<double>(rng() % 40 - 20)};
                pins.push_back(d.add_pin(part, pin, at));
                pin_pos.push_back(at);
            }
        }
        const int n_wires = 1 + static_cast<int>(rng() % 5);
        for (int i = 0; i < n_wires; ++i) {
            NodeId a = pins[rng() % pins.size()];
            NodeId b = pins[rng() % pins.size()];
            if (a == b) {
                continue;
            }
            std::vector<Vec2> mid;
            if (rng() % 2) {
                mid.push_back({static_cast<double>(rng() % 20 - 10),
                               static_cast<double>(rng() % 20 - 10)});
            }
            d.add_wire(a, b, mid, -1, -1);
        }
        if (rng() % 2) {
            const NodeId j1 = d.add_junction({0.0, 50.0});
            const NodeId j2 = d.add_junction({10.0, 50.0});
            d.add_wire(j1, j2);
            d.add_wire(j1, pins[0]);
        }

        const auto before = canon(ws::sch::pin_connections(d));

        MoveSession s;
        for (std::uint32_t part = 1; part <= static_cast<std::uint32_t>(n_parts); ++part) {
            s.parts.push_back({part, {static_cast<double>(rng() % 60 - 30),
                                      static_cast<double>(rng() % 60 - 30)}});
        }
        for (std::size_t i = 0; i < pins.size(); ++i) {
            s.land[pins[i]] = {pin_pos[i].x + static_cast<double>(rng() % 40 - 20),
                               pin_pos[i].y + static_cast<double>(rng() % 40 - 20)};
            if (rng() % 2) {
                s.land_dir[pins[i]] = 90 * static_cast<int>(rng() % 4);
            }
        }
        const Overlay ov = ws::sch::resolve_move(d, s);
        const Doc after = apply(d, ov);
        CHECK(same_pairs(canon(ws::sch::pin_connections(after)), before));
    }
}

TEST_CASE("property 2: a wire crossing another during a move never merges") {
    std::mt19937 rng(99);
    for (int iter = 0; iter < 200; ++iter) {
        Doc d;
        const NodeId p1 = d.add_pin(1, 0, {0.0, 0.0});
        const NodeId p2 = d.add_pin(1, 1, {12.0, 0.0});
        const NodeId p3 = d.add_pin(2, 0, {6.0, -8.0});
        const NodeId p4 = d.add_pin(2, 1, {6.0, 8.0});
        d.add_wire(p1, p2, {}, -1, -1);
        d.add_wire(p3, p4, {}, -1, -1);
        const auto before = ws::sch::pin_connections(d);

        MoveSession s;
        s.land[p1] = {0.0, 5.0};
        s.land[p2] = {12.0, 5.0};  // the corridor crosses the vertical wire
        const Overlay ov = ws::sch::resolve_move(d, s);
        const Doc after = apply(d, ov);
        CHECK(same_pairs(canon(ws::sch::pin_connections(after)), canon(before)));
        const std::size_t na = ws::sch::build_nets(after).nets.size();
        const std::size_t nb = ws::sch::build_nets(d).nets.size();
        CHECK(na == nb);
    }
}

TEST_CASE("property 3: moving a part never opens a connection") {
    std::mt19937 rng(2024);
    for (int iter = 0; iter < 200; ++iter) {
        Doc d;
        std::vector<NodeId> part_pins;
        for (std::uint32_t pin = 0; pin < 3; ++pin) {
            part_pins.push_back(d.add_pin(1, pin, {static_cast<double>(pin * 6),
                                                   0.0}));
        }
        const NodeId far = d.add_pin(2, 0, {6.0, 30.0});
        for (NodeId p : part_pins) {
            d.add_wire(p, far, {}, -1, -1);
        }
        const auto before = canon(ws::sch::pin_connections(d));

        MoveSession s;
        for (NodeId p : part_pins) {
            s.land[p] = {static_cast<double>(rng() % 40 - 20),
                         static_cast<double>(rng() % 40 - 20)};
        }
        const Overlay ov = ws::sch::resolve_move(d, s);
        const Doc after = apply(d, ov);
        const auto pairs_after = canon(ws::sch::pin_connections(after));
        CHECK(same_pairs(pairs_after, before));
    }
}

TEST_CASE("property 4: preview overlay == committed geometry (the SAME overlay)") {
    // What the preview drew is what the commit lands - by construction,
    // because the commit applies the SAME Overlay value the preview drew.
    RtFixture f;
    MoveSession s;
    s.land[f.p2] = {30.0, 4.0};
    const Overlay preview = ws::sch::resolve_move(f.d, s);
    const Doc committed = apply(f.d, preview);
    const auto pv = full_path(f.d, f.w, preview);
    const auto cm = committed.wire_path(f.w);
    REQUIRE(pv.size() == cm.size());
    for (std::size_t i = 0; i < pv.size(); ++i) {
        CHECK(pv[i].x == doctest::Approx(cm[i].x));
        CHECK(pv[i].y == doctest::Approx(cm[i].y));
    }
    for (const auto& [id, pos] : preview.nodePos) {
        const Node* n = committed.find_node(id);
        REQUIRE(n != nullptr);
        CHECK(n->pos.x == doctest::Approx(pos.x));
        CHECK(n->pos.y == doctest::Approx(pos.y));
    }
}

TEST_CASE("router: deterministic (same session, same answer, twice)") {
    RtFixture f;
    MoveSession s;
    s.land[f.p2] = {30.0, 10.0};
    const auto a = ws::sch::resolve_move(f.d, s);
    const auto b = ws::sch::resolve_move(f.d, s);
    REQUIRE(a.wirePath.size() == b.wirePath.size());
    REQUIRE(a.wirePath.at(f.w).size() == b.wirePath.at(f.w).size());
    for (std::size_t i = 0; i < a.wirePath.at(f.w).size(); ++i) {
        CHECK(a.wirePath.at(f.w)[i].x == b.wirePath.at(f.w)[i].x);
        CHECK(a.wirePath.at(f.w)[i].y == b.wirePath.at(f.w)[i].y);
    }
}

TEST_CASE("router: an untouched wire keeps its shape (no entry)") {
    RtFixture f;
    const NodeId q1 = f.d.add_pin(3, 0, {50.0, 50.0});
    const NodeId q2 = f.d.add_pin(4, 0, {60.0, 60.0});
    const auto other = f.d.add_wire(q1, q2, {{55.0, 55.0}});
    CHECK(f.d.find_wire(other) != nullptr);
    MoveSession s;
    s.land[f.p2] = {24.0, 4.0};
    const Overlay ov = ws::sch::resolve_move(f.d, s);
    CHECK(ov.wirePath.count(other) == 0);
    CHECK(ov.nodePos.count(q1) == 0);
}

TEST_CASE("router: the cheap pattern is the fewest bends first") {
    Doc d;
    const NodeId p1 = d.add_pin(1, 0, {0.0, 0.0});
    const NodeId p2 = d.add_pin(2, 0, {10.0, 0.0});
    const auto w = d.add_wire(p1, p2);
    MoveSession s;
    s.land[p2] = {10.0, 4.0};
    const Overlay ov = ws::sch::resolve_move(d, s);
    const auto path = full_path(d, w, ov);
    CHECK(bends(path) == 1);  // the single L, not a Z detour
    CHECK(length_of(path) ==
          doctest::Approx(std::fabs(10.0 - 0.0) + std::fabs(4.0 - 0.0)));
}

TEST_CASE("router: rotation flips the stub (land_dir honored)") {
    Doc d;
    const NodeId p1 = d.add_pin(1, 0, {0.0, 0.0});
    const NodeId p2 = d.add_pin(2, 0, {10.0, 0.0});
    const auto w = d.add_wire(p1, p2, {}, 0, 180);
    MoveSession s;
    s.land_dir[p1] = 90;  // rotated: the pin now faces north
    const Overlay ov = ws::sch::resolve_move(d, s);
    const auto path = full_path(d, w, ov);
    REQUIRE(path.size() >= 3);
    CHECK(path[1].y > 0.0);
    CHECK(path[1].x == 0.0);
    CHECK(ov.endDir.count({w, true}) == 1);
    CHECK(ov.endDir.at({w, true}) == 90);
    CHECK(bends(path) >= 1);
}
