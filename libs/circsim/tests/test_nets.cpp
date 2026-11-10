// Net building: the connectivity rules (reference edges, exact-position
// coincidence, the crossing exception, labels, power/ground, naming).

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include <ws/circsim/nets.hpp>

using ws::circsim::Junction;
using ws::circsim::Label;
using ws::circsim::NetBuild;
using ws::circsim::Part;
using ws::circsim::PinRef;
using ws::circsim::Pos;
using ws::circsim::Schematic;
using ws::circsim::Wire;

namespace {

// A bare bench: a schematic with the wiring helpers. Parts are added per
// test - nets count pins, so a shared part set would pollute every count.
struct Bench {
    Schematic s;

    Part* part(std::string symbol, std::string ref, Pos at) {
        return s.add_part(Part::make(std::move(symbol), std::move(ref),
                                     "1k", at));
    }
    // A junction at (x, y), returning its id.
    std::uint32_t junction(double x, double y) {
        Junction* j = s.add_junction({});
        j->pos = Pos{x, y};
        return j->id;
    }
    // A wire between two junctions with optional waypoints.
    std::uint32_t wire_j(double ax, double ay, double bx, double by,
                         std::vector<Pos> wps = {}) {
        Wire* w = s.add_wire({});
        w->a = ws::circsim::JunctionRef{junction(ax, ay)};
        w->b = ws::circsim::JunctionRef{junction(bx, by)};
        w->waypoints = std::move(wps);
        return w->id;
    }
    std::uint32_t label(double x, double y, std::string name) {
        Label* l = s.add_label({});
        l->pos = Pos{x, y};
        l->name = std::move(name);
        return l->id;
    }
    std::uint32_t pin_wire(PinRef a, PinRef b) {
        Wire* w = s.add_wire({});
        w->a = a;
        w->b = b;
        return w->id;
    }
};

bool has_diag(const NetBuild& b, const std::string& code) {
    for (const ws::circsim::Diag& d : b.diags) {
        if (d.code == code) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("pin_pos applies mirror then rotation") {
    const ws::circsim::SymbolDef* r = ws::circsim::find_symbol("r");
    REQUIRE(r != nullptr);
    Part p = Part::make("r", "R1", "1k", {10.0, 20.0});
    p.rot = 0;
    CHECK(ws::circsim::pin_pos(*r, p, 0) == Pos{7.0, 20.0});
    CHECK(ws::circsim::pin_pos(*r, p, 1) == Pos{13.0, 20.0});
    p.rot = 90;  // (-3, 0) -> (0, -3)
    CHECK(ws::circsim::pin_pos(*r, p, 0) == Pos{10.0, 17.0});
    p.rot = 180;
    CHECK(ws::circsim::pin_pos(*r, p, 0) == Pos{13.0, 20.0});
    p.rot = 270;  // (-3, 0) -> (0, 3)
    CHECK(ws::circsim::pin_pos(*r, p, 0) == Pos{10.0, 23.0});
    p.rot = 0;
    p.mirror = true;  // (-3, 0) mirrors to (3, 0)
    CHECK(ws::circsim::pin_pos(*r, p, 0) == Pos{13.0, 20.0});
    p.rot = 90;  // mirror first: (3, 0) rotates to (0, 3)
    CHECK(ws::circsim::pin_pos(*r, p, 0) == Pos{10.0, 23.0});
}

TEST_CASE("wires crossing where neither has a vertex never connect") {
    Bench b;
    // Vertical wire crossing a horizontal wire at (5, 0), no shared vertex.
    b.wire_j(5.0, -5.0, 5.0, 5.0);
    b.wire_j(0.0, 0.0, 10.0, 0.0);
    const NetBuild nets = ws::circsim::build_nets(b.s);
    REQUIRE(nets.nets.nets.size() == 2);
}

TEST_CASE("a wire endpoint on another wire's interior connects (T)") {
    Bench b;
    const std::uint32_t horizontal = b.wire_j(0.0, 0.0, 10.0, 0.0);
    const std::uint32_t drop = b.wire_j(5.0, 0.0, 5.0, -4.0);  // lands on it
    const NetBuild nets = ws::circsim::build_nets(b.s);
    REQUIRE(nets.nets.nets.size() == 1);
    const auto* net = nets.nets.net_of_wire(horizontal);
    REQUIRE(net != nullptr);
    CHECK(nets.nets.net_of_wire(drop)->id == net->id);
}

TEST_CASE("a waypoint on another wire's interior connects") {
    Bench b;
    b.wire_j(0.0, 0.0, 10.0, 0.0);
    // This wire's only vertex on the other path is its elbow at (5, 4).
    b.wire_j(5.0, 8.0, 5.0, -8.0, {Pos{5.0, 4.0}});
    // Elbow at (5, 4) is not on the horizontal (y = 0): no connection...
    // unless the vertical leg crosses it at (5, 0) - which is an INTERIOR
    // point of the vertical and of the horizontal: a crossing, not a T.
    const NetBuild nets = ws::circsim::build_nets(b.s);
    CHECK(nets.nets.nets.size() == 2);
}

TEST_CASE("a wire elbow landing on another wire connects") {
    Bench b;
    b.wire_j(0.0, 0.0, 10.0, 0.0);
    // Elbow exactly at (5, 0), on the horizontal wire: a T with a corner.
    b.wire_j(5.0, 8.0, 5.0, -8.0, {Pos{5.0, 0.0}});
    const NetBuild nets = ws::circsim::build_nets(b.s);
    CHECK(nets.nets.nets.size() == 1);
}

TEST_CASE("coincident pins connect without any wire") {
    Bench b;
    Part* r1 = b.part("r", "R1", {0.0, 0.0});
    // R2 moved so its pin 0 sits exactly on R1's pin 1.
    Part* r2 = b.part("r", "R2", {6.0, 0.0});
    const NetBuild nets = ws::circsim::build_nets(b.s);
    const auto* a = nets.nets.net_of_pin(PinRef{r1->id, 1});
    const auto* c = nets.nets.net_of_pin(PinRef{r2->id, 0});
    REQUIRE(a != nullptr);
    REQUIRE(c != nullptr);
    CHECK(a->id == c->id);
}

TEST_CASE("a pin lying on a wire's interior connects") {
    Bench b;
    Part* r1 = b.part("r", "R1", {0.0, 0.0});
    // A vertical wire through R1's right pin at (3, 0).
    b.wire_j(3.0, 3.0, 3.0, -3.0);
    const NetBuild nets = ws::circsim::build_nets(b.s);
    const auto* pin_net = nets.nets.net_of_pin(PinRef{r1->id, 1});
    REQUIRE(pin_net != nullptr);
    CHECK(pin_net->wires.size() == 1);
}

TEST_CASE("same-name labels merge nets, case-insensitively") {
    Bench b;
    b.wire_j(0.0, 0.0, 10.0, 0.0);
    b.wire_j(20.0, 0.0, 30.0, 0.0);
    b.label(5.0, 0.0, "out");
    b.label(25.0, 0.0, "OUT");
    const NetBuild nets = ws::circsim::build_nets(b.s);
    REQUIRE(nets.nets.nets.size() == 1);
    CHECK(nets.nets.nets[0].name == "out");  // lowest id's spelling wins
    CHECK(nets.nets.nets[0].labels.size() == 2);
}

TEST_CASE("a label on a wire's interior attaches; a crossing does not") {
    Bench b;
    b.wire_j(0.0, 0.0, 10.0, 0.0);
    b.label(5.0, 0.0, "mid");
    const NetBuild nets = ws::circsim::build_nets(b.s);
    REQUIRE(nets.nets.nets.size() == 1);
    CHECK(nets.nets.nets[0].name == "mid");

    Bench c;
    c.wire_j(0.0, 0.0, 10.0, 0.0);
    c.wire_j(5.0, -5.0, 5.0, 5.0);
    c.label(5.0, 0.0, "cross");
    const NetBuild crossed = ws::circsim::build_nets(c.s);
    CHECK(has_diag(crossed, "circsim.nets.label_at_crossing"));
    // The label names nothing: it joins no net.
    for (const auto& n : crossed.nets.nets) {
        CHECK(n.name != "cross");
    }
}

TEST_CASE("a label on empty space warns and names nothing") {
    Bench b;
    b.wire_j(0.0, 0.0, 10.0, 0.0);
    b.label(50.0, 50.0, "nowhere");
    const NetBuild nets = ws::circsim::build_nets(b.s);
    CHECK(has_diag(nets, "circsim.nets.label_unattached"));
    for (const auto& n : nets.nets.nets) {
        CHECK(n.name != "nowhere");
    }
}

TEST_CASE("label conflicts warn and the lowest id wins") {
    Bench b;
    b.wire_j(0.0, 0.0, 10.0, 0.0);
    b.label(5.0, 0.0, "alpha");
    b.label(6.0, 0.0, "beta");
    const NetBuild nets = ws::circsim::build_nets(b.s);
    REQUIRE(nets.nets.nets.size() == 1);
    CHECK(nets.nets.nets[0].name == "alpha");
    CHECK(has_diag(nets, "circsim.nets.label_conflict"));
}

TEST_CASE("ground: gnd parts, 0/gnd labels, one global net") {
    SUBCASE("two gnd symbols on separate islands are one net") {
        Bench b;
        b.s.add_part(Part::make("gnd", "", "", {0.0, -5.0}));
        b.s.add_part(Part::make("gnd", "", "", {20.0, -5.0}));
        const NetBuild nets = ws::circsim::build_nets(b.s);
        REQUIRE(nets.nets.nets.size() == 1);
        CHECK(nets.nets.nets[0].is_ground);
        CHECK(nets.nets.nets[0].name == "0");
    }
    SUBCASE("a gnd label merges with the gnd-symbol net") {
        Bench b;
        b.s.add_part(Part::make("gnd", "", "", {0.0, -5.0}));
        b.wire_j(20.0, 0.0, 30.0, 0.0);
        b.label(25.0, 0.0, "GND");
        const NetBuild nets = ws::circsim::build_nets(b.s);
        REQUIRE(nets.nets.nets.size() == 1);
        CHECK(nets.nets.nets[0].name == "0");
    }
    SUBCASE("a label named 0 is ground and beats other labels") {
        Bench b;
        b.wire_j(0.0, 0.0, 10.0, 0.0);
        b.label(5.0, 0.0, "sig");
        b.label(6.0, 0.0, "0");
        const NetBuild nets = ws::circsim::build_nets(b.s);
        REQUIRE(nets.nets.nets.size() == 1);
        CHECK(nets.nets.nets[0].is_ground);
        CHECK(nets.nets.nets[0].name == "0");
    }
}

TEST_CASE("power symbols: fixed net names merge across the sheet") {
    Bench b;
    b.s.add_part(Part::make("vcc", "", "", {0.0, 5.0}));
    b.s.add_part(Part::make("vcc", "", "", {20.0, 5.0}));
    b.wire_j(0.0, 0.0, 10.0, 0.0);
    b.wire_j(20.0, 0.0, 30.0, 0.0);
    const NetBuild nets = ws::circsim::build_nets(b.s);
    REQUIRE(nets.nets.nets.size() == 3);  // VCC + the two islands
    const auto* vcc = nets.nets.by_name("VCC");
    REQUIRE(vcc != nullptr);
    CHECK(vcc->pins.size() == 2);  // both vcc pins, no wires between them
}

TEST_CASE("auto names are deterministic and never collide with labels") {
    Bench b;
    Part* r1 = b.part("r", "R1", {0.0, 0.0});    // id 1
    Part* r2 = b.part("r", "R2", {10.0, 0.0});   // id 2
    b.pin_wire(PinRef{r1->id, 0}, PinRef{r1->id, 1});  // id 3, anchor 1
    b.pin_wire(PinRef{r2->id, 0}, PinRef{r2->id, 1});  // id 4, anchor 2
    b.wire_j(20.0, 0.0, 30.0, 0.0);                    // j5 j6 w7
    b.label(25.0, 0.0, "n001");  // would collide with the first auto name
    const NetBuild nets = ws::circsim::build_nets(b.s);
    REQUIRE(nets.nets.nets.size() == 3);
    // ci by name: n001 (the label), N002, N003 (N001 skipped, taken).
    CHECK(nets.nets.nets[0].name == "n001");
    CHECK(nets.nets.nets[1].name == "N002");
    CHECK(nets.nets.nets[2].name == "N003");
}

TEST_CASE("an empty schematic builds zero nets") {
    Schematic s;
    const NetBuild nets = ws::circsim::build_nets(s);
    CHECK(nets.nets.nets.empty());
    CHECK(nets.nets.ground() == nullptr);
    CHECK(nets.diags.empty());
}

TEST_CASE("unknown symbols join no net; their wires drop with a warning") {
    Bench b;
    Part* r1 = b.part("r", "R1", {0.0, 0.0});
    Part* mystery =
        b.s.add_part(Part::make("mystery", "U1", "??", {20.0, 0.0}));
    b.pin_wire(PinRef{mystery->id, 0}, PinRef{r1->id, 0});
    const NetBuild nets = ws::circsim::build_nets(b.s);
    CHECK(nets.nets.net_of_pin(PinRef{mystery->id, 0}) == nullptr);
    CHECK(has_diag(nets, "circsim.nets.dangling_wire"));
}

TEST_CASE("the netlist lookups answer for pins, wires and junctions") {
    Bench b;
    const std::uint32_t j = b.junction(0.0, 0.0);
    const std::uint32_t w = b.wire_j(0.0, 0.0, 10.0, 0.0);
    const NetBuild nets = ws::circsim::build_nets(b.s);
    const auto* jn = nets.nets.net_of_junction(j);
    const auto* wn = nets.nets.net_of_wire(w);
    REQUIRE(jn != nullptr);
    REQUIRE(wn != nullptr);
    CHECK(jn->id == wn->id);
    CHECK(nets.nets.net_of_pin(PinRef{9999, 0}) == nullptr);
    const bool named = nets.nets.by_name("N001") == wn ||
                        nets.nets.by_name("n001") == wn;
    CHECK(named);
}

// --- silent joins (stage 18): the connections the drawing never shows ---------

TEST_CASE("silent joins: a pin on a wire's interior is reported") {
    Bench b;
    Part* r1 = b.part("r", "R1", {0.0, 0.0});  // pin 1 at (3, 0)
    const std::uint32_t w = b.wire_j(3.0, 3.0, 3.0, -3.0);
    const NetBuild nets = ws::circsim::build_nets(b.s);
    REQUIRE(nets.silent_joins.size() == 1);
    const auto& join = nets.silent_joins[0];
    CHECK(join.kind == ws::circsim::SilentJoin::Kind::PinOnWire);
    CHECK(join.pos == Pos{3.0, 0.0});
    CHECK(join.part == r1->id);
    CHECK(join.pin == 1);
    CHECK(join.wire == w);
}

TEST_CASE("silent joins: a wire's vertex riding another wire is reported") {
    Bench b;
    const std::uint32_t horizontal = b.wire_j(0.0, 0.0, 10.0, 0.0);
    // The elbow at (5, 0) lands on the horizontal's interior: a T.
    const std::uint32_t drop = b.wire_j(5.0, 8.0, 5.0, -8.0, {Pos{5.0, 0.0}});
    const NetBuild nets = ws::circsim::build_nets(b.s);
    REQUIRE(nets.silent_joins.size() == 1);
    const auto& join = nets.silent_joins[0];
    CHECK(join.kind == ws::circsim::SilentJoin::Kind::WireOnWire);
    CHECK(join.pos == Pos{5.0, 0.0});
    CHECK(join.wire == drop);
    CHECK(join.other == horizontal);
}

TEST_CASE("silent joins: a pin on the rider wins over the wire entry") {
    Bench b;
    const std::uint32_t horizontal = b.wire_j(0.0, 0.0, 10.0, 0.0);
    Part* r2 = b.part("r", "R2", {2.0, 0.0});  // pin 1 at (5, 0)
    // The rider's elbow lands exactly on that pin: a WireOnWire (the
    // elbow riding the horizontal) and a PinOnWire (the pin picked up
    // by both wires) compete for one position - the pin wins, and the
    // position reports once.
    const std::uint32_t drop = b.wire_j(5.0, 8.0, 5.0, -8.0, {Pos{5.0, 0.0}});
    const NetBuild nets = ws::circsim::build_nets(b.s);
    REQUIRE(nets.silent_joins.size() == 1);  // one position, one entry
    CHECK(nets.silent_joins[0].kind ==
          ws::circsim::SilentJoin::Kind::PinOnWire);
    CHECK(nets.silent_joins[0].pos == Pos{5.0, 0.0});
    CHECK(nets.silent_joins[0].part == r2->id);
    CHECK(nets.silent_joins[0].wire == horizontal);
    (void)drop;
}

TEST_CASE("silent joins: landings, crossings and touches stay silent-free") {
    // A wire END on a pin is a landing, not a pickup.
    Bench landing;
    Part* r1 = landing.part("r", "R1", {0.0, 0.0});
    Part* r2 = landing.part("r", "R2", {10.0, 0.0});
    landing.pin_wire(PinRef{r1->id, 1}, PinRef{r2->id, 0});
    CHECK(ws::circsim::build_nets(landing.s).silent_joins.empty());

    // Two wires CROSSING with no shared vertex connect nothing - and
    // report nothing.
    Bench crossing;
    crossing.wire_j(5.0, -5.0, 5.0, 5.0);
    crossing.wire_j(0.0, 0.0, 10.0, 0.0);
    CHECK(ws::circsim::build_nets(crossing.s).silent_joins.empty());

    // Coincident pins are their own (visible) mark - the tline deck's
    // grounding idiom rides exactly this.
    Bench touching;
    touching.part("r", "R1", {0.0, 0.0});
    touching.part("r", "R2", {6.0, 0.0});  // pin 0 on R1's pin 1
    CHECK(ws::circsim::build_nets(touching.s).silent_joins.empty());
}

TEST_CASE("silent joins: an authored junction suppresses the record") {
    // A junction entity lying on a wire's interior draws its own dot.
    Bench b;
    b.wire_j(0.0, 0.0, 10.0, 0.0);
    b.junction(5.0, 0.0);
    CHECK(ws::circsim::build_nets(b.s).silent_joins.empty());

    // Even a pin pickup hides behind a junction at the same spot.
    Bench c;
    c.wire_j(3.0, 3.0, 3.0, -3.0);
    c.part("r", "R1", {0.0, 0.0});  // pin 1 at (3, 0), picked up...
    c.junction(3.0, 0.0);           // ...but the junction says it first
    CHECK(ws::circsim::build_nets(c.s).silent_joins.empty());
}

// --- the hardening batch: the large-schematic grid path -----------------------
// Above 500 wires the wire-vertex and pin-on-wire coincidence rules
// switch from the brute-force scan to the segment grid; the two must
// answer identically. A comb - one spine, N teeth whose waypoints
// cross it - is the cleanest proof: every tooth unions through the
// spine (one net), every crossing waypoint records a silent join.

namespace {

struct CombAnswer {
    std::size_t nets = 0;
    std::size_t joins = 0;
    std::vector<std::pair<double, double>> join_positions;
};

CombAnswer comb(std::size_t teeth) {
    Bench b;
    // The spine runs one unit past the last tooth so no crossing
    // waypoint sits on a spine endpoint (those are visible landings,
    // not silent joins).
    b.wire_j(0, 0, static_cast<double>(teeth) + 1.0, 0);
    for (std::size_t i = 1; i <= teeth; ++i) {
        const double x = static_cast<double>(i);
        b.wire_j(x, -5, x, 5, {Pos{x, 0}});
    }
    const NetBuild built = ws::circsim::build_nets(b.s);
    CombAnswer out;
    out.nets = built.nets.nets.size();
    out.joins = built.silent_joins.size();
    for (const auto& j : built.silent_joins) {
        out.join_positions.emplace_back(j.pos.x, j.pos.y);
    }
    std::sort(out.join_positions.begin(), out.join_positions.end());
    return out;
}

}  // namespace

TEST_CASE("the segment grid answers what the brute force answers") {
    // Below the threshold: the historical brute-force path.
    const CombAnswer small = comb(60);
    REQUIRE(small.nets == 1);
    REQUIRE(small.joins == 60);
    CHECK(small.join_positions.front() == std::make_pair(1.0, 0.0));
    CHECK(small.join_positions.back() ==
          std::make_pair(60.0, 0.0));

    // Above the threshold: the grid path, same comb shape.
    const CombAnswer large = comb(600);
    REQUIRE(large.nets == 1);
    REQUIRE(large.joins == 600);
    CHECK(large.join_positions.front() == std::make_pair(1.0, 0.0));
    CHECK(large.join_positions.back() ==
          std::make_pair(600.0, 0.0));
    // No duplicates, no gaps: exactly the x = 1..600 interior points.
    for (std::size_t i = 0; i < large.join_positions.size(); ++i) {
        CHECK(large.join_positions[i] ==
              std::make_pair(static_cast<double>(i) + 1.0, 0.0));
    }
}
