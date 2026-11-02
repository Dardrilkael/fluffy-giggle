// Net building: the connectivity rules (reference edges, exact-position
// coincidence, the crossing exception, labels, power/ground, naming).

#include <string>

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
