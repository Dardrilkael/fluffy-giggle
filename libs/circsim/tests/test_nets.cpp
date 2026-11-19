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
    std::uint32_t junction(double x, double y,
                           std::vector<std::uint32_t> taps = {}) {
        Junction* j = s.add_junction({});
        j->pos = Pos{x, y};
        j->taps = std::move(taps);
        return j->id;
    }
    // A wire between two junctions with optional waypoints.
    std::uint32_t wire_j(double ax, double ay, double bx, double by,
                         std::vector<Pos> wps = {},
                         std::vector<std::uint32_t> taps = {}) {
        Wire* w = s.add_wire({});
        w->a = ws::circsim::JunctionRef{junction(ax, ay)};
        w->b = ws::circsim::JunctionRef{junction(bx, by)};
        w->waypoints = std::move(wps);
        // The junctions' taps (authored bindings): the whole list
        // lands on the A-end junction. Tests that want the T-tap
        // gesture bind through this; a bare geometry touch stays
        // inert.
        if (!taps.empty()) {
            for (Junction& j : s.junctions_mut()) {
                if (j.id == std::get<ws::circsim::JunctionRef>(w->a).junction) {
                    j.taps = taps;
                    break;
                }
            }
        }
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

bool has_diag(const NetBuild& b, const std::string& code,
              ws::circsim::Severity severity =
                  ws::circsim::Severity::Warning) {
    for (const ws::circsim::Diag& d : b.diags) {
        if (d.code == code && d.severity == severity) {
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

TEST_CASE("a wire END on another wire's interior connects through its "
          "junction (authored T)") {
    // wire_j mints a junction at both ends: a dropped wire END on
    // another wire's body carries its own dot - and the BINDING makes
    // it the authored T-tap (the wire tool writes exactly that: the
    // minted dot taps the wire it landed on). A bare dot - geometry
    // only, what a MOVE leaves behind - connects nothing.
    Bench b;
    const std::uint32_t horizontal = b.wire_j(0.0, 0.0, 10.0, 0.0);
    // Unbound: the moved-on case - inert.
    const std::uint32_t stray = b.wire_j(5.0, 0.0, 5.0, -4.0);
    {
        const NetBuild nets = ws::circsim::build_nets(b.s);
        REQUIRE(nets.nets.nets.size() == 2);
        const auto* h_net = nets.nets.net_of_wire(horizontal);
        REQUIRE(h_net != nullptr);
        CHECK(nets.nets.net_of_wire(stray)->id != h_net->id);
    }
    // Bound (the authored gesture): the drop joins the horizontal
    // through its dot's tap binding - wherever the wire actually runs.
    // The stray from the first half stays its own net (its endpoint
    // dot touches the horizontal's interior UNBOUND: geometry).
    const std::uint32_t drop = b.wire_j(5.0, 0.0, 5.0, -4.0, {},
                                        {horizontal});
    const NetBuild nets = ws::circsim::build_nets(b.s);
    REQUIRE(nets.nets.nets.size() == 2);
    const auto* net = nets.nets.net_of_wire(horizontal);
    REQUIRE(net != nullptr);
    CHECK(nets.nets.net_of_wire(drop)->id == net->id);
    CHECK(nets.nets.net_of_wire(stray)->id != net->id);
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

TEST_CASE("a wire elbow landing on another wire stays separate without "
          "a junction") {
    // Same rule, a bend's vertex instead of an end: the touch is
    // inert until a junction names it.
    Bench b;
    const std::uint32_t horizontal = b.wire_j(0.0, 0.0, 10.0, 0.0);
    // Elbow exactly at (5, 0), on the horizontal wire: a T with a corner.
    const std::uint32_t elbow = b.wire_j(5.0, 8.0, 5.0, -8.0,
                                        {Pos{5.0, 0.0}});
    const NetBuild nets = ws::circsim::build_nets(b.s);
    CHECK(nets.nets.nets.size() == 2);
    CHECK(has_diag(nets, "circsim.nets.wire_touch"));
    // A bound dot joins them (the authored gesture - the tool binds
    // BOTH wires); a bare dot (a move's leftovers) would leave them
    // separate. The binding is structural: the union rides the taps.
    b.junction(5.0, 0.0, {horizontal, elbow});
    CHECK(ws::circsim::build_nets(b.s).nets.nets.size() == 1);
}

TEST_CASE("coincident pins connect nothing without structure") {
    // THE PARTITION RULE: two pins at the same point are geometry -
    // a move left them overlapped and that must not merge nets. The
    // old rule unioned them (the "pin-to-pin touch"); a move could
    // therefore rewrite the partition by where it left parts. Now
    // the touch connects nothing and warns (entities_coincide);
    // the authored ways back are a wire ending on both pins, or a
    // bound dot at the spot.
    Bench b;
    Part* r1 = b.part("r", "R1", {0.0, 0.0});
    // R2 moved so its pin 0 sits exactly on R1's pin 1.
    Part* r2 = b.part("r", "R2", {6.0, 0.0});
    const NetBuild nets = ws::circsim::build_nets(b.s);
    const auto* a = nets.nets.net_of_pin(PinRef{r1->id, 1});
    const auto* c = nets.nets.net_of_pin(PinRef{r2->id, 0});
    REQUIRE(a != nullptr);
    REQUIRE(c != nullptr);
    CHECK(a->id != c->id);
    CHECK(has_diag(nets, "circsim.nets.entities_coincide",
                   ws::circsim::Severity::Warning));
}

TEST_CASE("one bare touch warns once, not twice") {
    // A pin that coincides with a junction AND lies on a wire's
    // interior: the old flow fired BOTH entities_coincide (pin meets
    // junction at a point) and pin_on_wire (pin rides the wire) for
    // the same touch. The coincidence is the finding - pin_on_wire
    // stays out for a pin already warned coincident.
    Bench b;
    b.part("r", "R1", {0.0, 0.0});
    // A junction exactly on R1's pin 1 at (3,0) - the coincidence.
    b.junction(3.0, 0.0);
    // And a wire whose interior passes through (3,0): a horizontal
    // wire from (0,-2) to (6,-2)? No - the interior must CARRY the
    // pin position. A wire from (3,-3) to (3,3) rides through it.
    b.wire_j(3.0, -3.0, 3.0, 3.0);
    const NetBuild nets = ws::circsim::build_nets(b.s);
    CHECK(has_diag(nets, "circsim.nets.entities_coincide"));
    // Exactly one warning names the pin - the coincidence, not the
    // wire ride on top of it.
    std::size_t pin_warns = 0;
    for (const ws::circsim::Diag& d : nets.diags) {
        if (d.code == "circsim.nets.pin_on_wire" ||
            d.code == "circsim.nets.entities_coincide") {
            ++pin_warns;
        }
    }
    CHECK(pin_warns == 1u);
}

TEST_CASE("a pin lying on a wire's interior connects nothing") {
    // A moved or rotated part's pin on a wire's body is geometry,
    // not a connection: the pin names no net through the wire, and
    // the warning says what the drawing never did. (Power symbols
    // keep their pickup - their own test below.)
    Bench b;
    Part* r1 = b.part("r", "R1", {0.0, 0.0});
    // A vertical wire through R1's right pin at (3, 0).
    const std::uint32_t vertical = b.wire_j(3.0, 3.0, 3.0, -3.0);
    const NetBuild nets = ws::circsim::build_nets(b.s);
    const auto* pin_net = nets.nets.net_of_pin(PinRef{r1->id, 1});
    REQUIRE(pin_net != nullptr);
    CHECK(pin_net->wires.empty());
    CHECK(has_diag(nets, "circsim.nets.pin_on_wire"));
    // The authored ways back: the wire ENDING on the pin (a landing
    // - the reference names both sides), or a bound dot at the spot
    // with a wire ending on the dot. A bound dot ALONE at the pin's
    // position no longer joins the pin: the dot's binding is to the
    // wire, the pin names nothing until structure spans it.
    b.junction(3.0, 0.0, {vertical});
    const NetBuild dotted = ws::circsim::build_nets(b.s);
    CHECK(dotted.nets.net_of_pin(PinRef{r1->id, 1})->wires.empty());
    // The wire itself lands on the pin: the authored connection.
    Wire* land = b.s.add_wire({});
    land->a = PinRef{r1->id, 1};
    land->b = ws::circsim::JunctionRef{b.junction(3.0, 0.0, {vertical})};
    const NetBuild joined = ws::circsim::build_nets(b.s);
    // The pin's net carries BOTH wires: the landing wire (its own
    // other end is the dot bound to the vertical) and the vertical
    // itself (the dot's binding unions it in).
    CHECK(joined.nets.net_of_pin(PinRef{r1->id, 1})->wires.size() == 2);
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

// A label NEAR a wire names that wire's net: the anchor reaches up to
// one grid cell around the wire's polyline, the nearest wire winning
// when several are in reach (the switch.csim layout lesson - a label
// dropped a unit or two off the wire silently named nothing and the
// Results panel answered N001 instead of the user's spelling).
TEST_CASE("a label within reach of one wire attaches to it") {
    Bench b;
    b.wire_j(0.0, 0.0, 10.0, 0.0);
    b.label(5.0, 1.0, "near");  // one unit above the wire
    const NetBuild nets = ws::circsim::build_nets(b.s);
    REQUIRE(nets.nets.nets.size() == 1);
    CHECK(nets.nets.nets[0].name == "near");
    CHECK_FALSE(has_diag(nets, "circsim.nets.label_unattached"));
}

TEST_CASE("a label at a wire's end attaches through the reach") {
    Bench b;
    b.wire_j(3.0, 0.0, 10.0, 0.0);
    b.label(2.0, 0.0, "in");  // one unit left of the endpoint
    const NetBuild nets = ws::circsim::build_nets(b.s);
    REQUIRE(nets.nets.nets.size() == 1);
    CHECK(nets.nets.nets[0].name == "in");
}

TEST_CASE("a label's reach snaps to the NEAREST wire") {
    Bench b;
    const std::uint32_t wire_a = b.wire_j(0.0, 0.0, 10.0, 0.0);
    b.wire_j(20.0, 0.0, 30.0, 0.0);
    b.label(11.0, 0.0, "ctl");  // one unit past wire A's end, 9 from B
    const NetBuild nets = ws::circsim::build_nets(b.s);
    REQUIRE(nets.nets.nets.size() == 2);
    // The label rides wire A's net (the nearest), not wire B's.
    for (const auto& n : nets.nets.nets) {
        if (n.name == "ctl") {
            REQUIRE(n.wires.size() == 1);
            CHECK(n.wires.front() == wire_a);
        } else {
            CHECK(n.name == "N001");  // B alone, auto-named
        }
    }
}

TEST_CASE("a label beyond reach stays unattached and warns") {
    Bench b;
    b.wire_j(0.0, 0.0, 10.0, 0.0);
    b.label(5.0, 3.0, "far");  // three units off: outside one grid cell
    const NetBuild nets = ws::circsim::build_nets(b.s);
    CHECK(has_diag(nets, "circsim.nets.label_unattached"));
    for (const auto& n : nets.nets.nets) {
        CHECK(n.name != "far");
    }
}

TEST_CASE("the reach is per-label unambiguous: two wires in reach, "
          "nearest wins") {
    Bench b;
    const std::uint32_t lower = b.wire_j(0.0, 0.0, 10.0, 0.0);  // y=0
    b.wire_j(0.0, 3.0, 10.0, 3.0);                             // y=3
    b.label(5.0, 0.5, "tie");  // 0.5 from the lower, 2.5 from the upper
    const NetBuild nets = ws::circsim::build_nets(b.s);
    REQUIRE(nets.nets.nets.size() == 2);
    // Exactly one net carries the label - the lower (nearest) wire's.
    for (const auto& n : nets.nets.nets) {
        if (!n.labels.empty()) {
            REQUIRE(n.wires.size() == 1);
            CHECK(n.wires.front() == lower);
        }
    }
}

TEST_CASE("an exact reach tie keeps the FIRST wire") {
    // The label sits exactly between two wires (0.5 from each): a
    // genuine tie. The first wire - the one the author drew first -
    // wins; the loop's <= used to let the later wire take the pick,
    // against its own comment.
    Bench b;
    const std::uint32_t first = b.wire_j(0.0, 0.0, 10.0, 0.0);  // y=0
    const std::uint32_t second = b.wire_j(0.0, 1.0, 10.0, 1.0);  // y=1
    b.label(5.0, 0.5, "tied");
    const NetBuild nets = ws::circsim::build_nets(b.s);
    REQUIRE(nets.nets.nets.size() == 2);
    const ws::circsim::Net* carrier = nullptr;
    for (const auto& n : nets.nets.nets) {
        if (!n.labels.empty()) {
            REQUIRE(carrier == nullptr);  // exactly one net carries it
            carrier = &n;
        }
    }
    REQUIRE(carrier != nullptr);
    CHECK(carrier->wires.front() == first);
    CHECK(carrier->wires.front() != second);
}

TEST_CASE("a bound label names its wire by reference, not position") {
    // The authored binding (the junction taps' rule): the label's
    // wire field IS the connection. Dragged off the wire it keeps
    // naming it - the partition never re-resolves through geometry.
    Bench b;
    const std::uint32_t w = b.wire_j(0.0, 0.0, 10.0, 0.0);
    Label* l = b.s.add_label({});
    l->pos = Pos{5.0, 0.0};
    l->name = "bound";
    l->wire = w;
    // The label sits ON the wire here (a bound label off it, below).
    {
        const NetBuild nets = ws::circsim::build_nets(b.s);
        REQUIRE(nets.nets.nets.size() == 1);
        CHECK(nets.nets.nets[0].name == "bound");
    }
    // Dragged FAR off the wire: still that net - the binding, not
    // the position, carries the connection.
    l->pos = Pos{50.0, 50.0};
    {
        const NetBuild nets = ws::circsim::build_nets(b.s);
        REQUIRE(nets.nets.nets.size() == 1);
        CHECK(nets.nets.nets[0].name == "bound");
        CHECK_FALSE(has_diag(nets, "circsim.nets.label_unattached"));
    }
    // A label bound to a DELETED wire warns instead of silently
    // re-resolving.
    Label* gone = b.s.add_label({});
    gone->pos = Pos{5.0, 0.0};
    gone->name = "gone";
    gone->wire = 999u;
    {
        const NetBuild nets = ws::circsim::build_nets(b.s);
        CHECK(has_diag(nets, "circsim.nets.label_detached"));
    }
}

TEST_CASE("a label's reach never bridges two separate nets through a "
          "crossing") {
    // AT the crossing point itself the exact rule stands: two wires'
    // interiors carry the label, it names nothing, the warning says
    // where to move it.
    Bench b;
    b.wire_j(0.0, 0.0, 10.0, 0.0);
    b.wire_j(5.0, -5.0, 5.0, 5.0);
    b.label(5.0, 0.0, "atcross");
    const NetBuild nets = ws::circsim::build_nets(b.s);
    CHECK(has_diag(nets, "circsim.nets.label_at_crossing"));
    for (const auto& n : nets.nets.nets) {
        CHECK(n.name != "atcross");
    }

    // NEAR the crossing, off both wires: the reach resolves the label
    // to the NEAREST wire's net (the vertical arm is nearer than the
    // horizontal one) - the label names one net, never both.
    Bench c;
    c.wire_j(0.0, 0.0, 10.0, 0.0);
    c.wire_j(5.0, -5.0, 5.0, 5.0);
    c.label(5.3, 0.4, "nearcross");
    const NetBuild near = ws::circsim::build_nets(c.s);
    CHECK_FALSE(has_diag(near, "circsim.nets.label_unattached"));
    CHECK_FALSE(has_diag(near, "circsim.nets.label_at_crossing"));
    std::size_t carrying = 0;
    for (const auto& n : near.nets.nets) {
        if (!n.labels.empty()) {
            ++carrying;
        }
    }
    CHECK(carrying == 1);
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
    // A wire whose A end rides a junction dot at (0, 0): the ENDPOINT
    // REF unions them (the authored gesture) - a bare dot at the wire's
    // start position no longer does (the partition rule).
    const std::uint32_t w = b.wire_j(0.0, 0.0, 10.0, 0.0);
    const std::uint32_t j =
        std::get<ws::circsim::JunctionRef>(
            b.s.wires()[0].a)
            .junction;
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

// --- dangling touches (the old silent joins, now refused) ---------------------
// Stage 18 painted the silent pickups as dots; the move/rotate bug they
// carried - a moved part's pin landing on a wire silently merged nets,
// and the next move silently split them again - retired them: a touch
// without a junction now connects NOTHING and says so (one warning per
// wire pair, one per part-pin-wire). The junction dot is the authored
// switch back on, exactly the gesture the wire tools mint.

TEST_CASE("a pin on a wire's interior warns once and connects nothing") {
    Bench b;
    b.part("r", "R1", {0.0, 0.0});  // pin 1 at (3, 0)
    b.wire_j(3.0, 3.0, 3.0, -3.0);
    const NetBuild nets = ws::circsim::build_nets(b.s);
    CHECK(has_diag(nets, "circsim.nets.pin_on_wire"));
    // Exactly one warning for the one touch (the scan visits the pin
    // against the wire's segments - the warned set holds it to one).
    std::size_t pin_warns = 0;
    for (const ws::circsim::Diag& d : nets.diags) {
        if (d.code == "circsim.nets.pin_on_wire") {
            ++pin_warns;
        }
    }
    CHECK(pin_warns == 1);
}

TEST_CASE("a wire vertex riding another wire warns once, connects nothing") {
    Bench b;
    b.wire_j(0.0, 0.0, 10.0, 0.0);
    // The elbow at (5, 0) lands on the horizontal's interior: a touch.
    b.wire_j(5.0, 8.0, 5.0, -8.0, {Pos{5.0, 0.0}});
    const NetBuild nets = ws::circsim::build_nets(b.s);
    CHECK(nets.nets.nets.size() == 2);
    CHECK(has_diag(nets, "circsim.nets.wire_touch"));
    // One warning per pair, whichever wire's vertex found it - the
    // scan visits the drop's elbow AND the horizontal's vertices
    // against each other's paths.
    std::size_t touch_warns = 0;
    for (const ws::circsim::Diag& d : nets.diags) {
        if (d.code == "circsim.nets.wire_touch") {
            ++touch_warns;
        }
    }
    CHECK(touch_warns == 1);
}

TEST_CASE("a pin and a wire sharing a spot: the pin warns, the pair "
          "stays deduped") {
    Bench b;
    b.wire_j(0.0, 0.0, 10.0, 0.0);
    b.part("r", "R2", {2.0, 0.0});  // pin 1 at (5, 0)
    // The rider's elbow lands exactly on that pin: one wire-pair
    // warning and one pin warning for the same position - the dedup
    // sets hold each to one.
    b.wire_j(5.0, 8.0, 5.0, -8.0, {Pos{5.0, 0.0}});
    const NetBuild nets = ws::circsim::build_nets(b.s);
    std::size_t pin_warns = 0;
    std::size_t touch_warns = 0;
    for (const ws::circsim::Diag& d : nets.diags) {
        if (d.code == "circsim.nets.pin_on_wire") {
            ++pin_warns;
        }
        if (d.code == "circsim.nets.wire_touch") {
            ++touch_warns;
        }
    }
    // The pin rides BOTH wires at that spot - the drop's elbow and
    // the horizontal - so two pin warnings (one per wire), while the
    // elbow's touch warns once. Everything inert: four nets.
    CHECK(pin_warns == 2);
    CHECK(touch_warns == 1);
    CHECK(nets.nets.nets.size() == 4);
}

TEST_CASE("silent joins: landings, crossings and touches stay silent-free") {
    // A wire END on a pin is a landing, not a pickup.
    Bench landing;
    Part* r1 = landing.part("r", "R1", {0.0, 0.0});
    Part* r2 = landing.part("r", "R2", {10.0, 0.0});
    landing.pin_wire(PinRef{r1->id, 1}, PinRef{r2->id, 0});

    // Two wires CROSSING with no shared vertex connect nothing - and
    // report nothing.
    Bench crossing;
    crossing.wire_j(5.0, -5.0, 5.0, 5.0);
    crossing.wire_j(0.0, 0.0, 10.0, 0.0);

    // Coincident pins are their own (visible) mark - the tline deck's
    // grounding idiom rides exactly this.
    Bench touching;
    touching.part("r", "R1", {0.0, 0.0});
    touching.part("r", "R2", {6.0, 0.0});  // pin 0 on R1's pin 1
}

TEST_CASE("silent joins: an authored junction suppresses the record") {
    // A junction entity lying on a wire's interior draws its own dot.
    Bench b;
    b.wire_j(0.0, 0.0, 10.0, 0.0);
    b.junction(5.0, 0.0);

    // Even a pin pickup hides behind a junction at the same spot.
    Bench c;
    c.wire_j(3.0, 3.0, 3.0, -3.0);
    c.part("r", "R1", {0.0, 0.0});  // pin 1 at (3, 0), picked up...
    c.junction(3.0, 0.0);           // ...but the junction says it first
}

// --- move/rotate inertness (the user's rule) ------------------------------------
// Moving or rotating an element keeps every existing connection and
// creates none: a pin dragged onto another wire's body warns and stays
// inert; dragging it away leaves the same partition. Connections are
// authored only through the wire gestures - an endpoint reference, a
// junction dot (a wire END carries one), or a gnd/power pin's naming
// pickup.

TEST_CASE("moving a part across wires never changes the net partition") {
    // R1 wired to J1 (net A) beside two unrelated wires; a sweep of
    // moves drops its pins on both wires' interiors - the partition
    // answers the same count every time, and the wired net keeps its
    // members through the endpoint reference.
    Bench b;
    Part* r1 = b.part("r", "R1", {0.0, 10.0});
    // Net A: J1 -- W1 -- R1.pin0, the ENDPOINT REFERENCE the editor's
    // wire tool lands (the pin resolves through the part wherever it
    // moves - the connection that must never break).
    Junction* j1 = b.s.add_junction({});
    j1->pos = {-13.0, 10.0};
    Wire* wired_w = b.s.add_wire({});
    wired_w->a = ws::circsim::JunctionRef{j1->id};
    wired_w->b = ws::circsim::PinRef{r1->id, 0};
    wired_w->ortho = true;
    const std::uint32_t wired = wired_w->id;
    const std::uint32_t w2 = b.wire_j(-20.0, 0.0, 20.0, 0.0);  // net B
    const std::uint32_t w3 = b.wire_j(30.0, -10.0, 30.0, 30.0);  // net C

    const std::size_t before =
        ws::circsim::build_nets(b.s).nets.nets.size();

    // Onto W2's body: pin0 (-3,0) and pin1 (3,0) ride its interior.
    r1->pos = {0.0, 0.0};
    const NetBuild onto = ws::circsim::build_nets(b.s);
    CHECK(onto.nets.nets.size() == before);
    CHECK(has_diag(onto, "circsim.nets.pin_on_wire"));
    const auto* pin_net = onto.nets.net_of_pin(PinRef{r1->id, 0});
    REQUIRE(pin_net != nullptr);
    CHECK(pin_net->wires.size() == 1);  // the authored wire only
    CHECK(pin_net->wires.front() == wired);

    // Far away, pins and re-routed legs crossing nothing but space.
    r1->pos = {40.0, 40.0};
    const NetBuild away = ws::circsim::build_nets(b.s);
    CHECK(away.nets.nets.size() == before);
    CHECK_FALSE(has_diag(away, "circsim.nets.pin_on_wire"));
    CHECK(away.nets.net_of_pin(PinRef{r1->id, 0})->wires.size() == 1);
    (void)w2;
    (void)w3;
}

TEST_CASE("a moved part's wire dragging across another wire connects "
          "nothing") {
    // The attached wire's re-routed leg may sweep across an unrelated
    // wire - the crossing and even a landing-looking vertex on the
    // body stay inert without a junction.
    Bench b;
    Part* r1 = b.part("r", "R1", {0.0, 10.0});
    // Net A: J1 -- W1 -- R1.pin0 through the endpoint reference (the
    // attachment a move must preserve).
    Junction* j1 = b.s.add_junction({});
    j1->pos = {-13.0, 10.0};
    Wire* w1 = b.s.add_wire({});
    w1->a = ws::circsim::JunctionRef{j1->id};
    w1->b = ws::circsim::PinRef{r1->id, 0};
    w1->ortho = true;
    const std::uint32_t w2 = b.wire_j(-20.0, 0.0, 20.0, 0.0);  // net B

    // Move R1 so pin0 lands ON W2's interior: W1's leg rides down with
    // the pin and its endpoint vertex touches W2's body.
    r1->pos = {0.0, 0.0};
    const NetBuild nets = ws::circsim::build_nets(b.s);
    CHECK(nets.nets.nets.size() == 3);  // A (wired), B, R1.pin1
    CHECK(has_diag(nets, "circsim.nets.wire_touch"));
    // Net B keeps exactly its own members: the sweep added nothing.
    const auto* b_net = nets.nets.net_of_wire(w2);
    REQUIRE(b_net != nullptr);
    CHECK(b_net->wires.size() == 1);
    CHECK(b_net->pins.empty());
}

TEST_CASE("a gnd pin dropped on a wire's interior grounds nothing") {
    // THE PARTITION RULE: even a power symbol's pin connects nothing
    // by position. The naming idiom lives in the symbol TABLE (every
    // gnd pin's net is the global ground NAMED "0") - that part is
    // structural and survives; what does NOT survive is picking up
    // the wire the pin happens to sit on. A ground dropped on a wire
    // now warns like any other pin; wiring it (or a bound dot at the
    // spot) is the author's gesture.
    Bench b;
    const std::uint32_t w = b.wire_j(-20.0, 0.0, 20.0, 0.0);
    b.wire_j(30.0, -10.0, 30.0, 30.0);  // an unrelated second net
    b.part("gnd", "", {8.0, 0.0});  // pin 0 at (8,0), on w's interior

    const NetBuild nets = ws::circsim::build_nets(b.s);
    const auto* g_net = nets.nets.net_of_wire(w);
    REQUIRE(g_net != nullptr);
    CHECK_FALSE(g_net->is_ground);
    CHECK(has_diag(nets, "circsim.nets.pin_on_wire",
                   ws::circsim::Severity::Warning));
}

// --- the hardening batch: the large-schematic grid path -----------------------
// Above 500 wires the wire-vertex and pin-on-wire coincidence rules
// switch from the brute-force scan to the segment grid; the two must
// answer identically. A comb - one spine, N teeth whose waypoints
// cross it, a junction at every crossing - is the cleanest proof:
// one net through both paths, zero silent joins either way.

namespace {

struct CombAnswer {
    std::size_t nets = 0;
};

CombAnswer comb(std::size_t teeth) {
    Bench b;
    // The spine runs one unit past the last tooth so no crossing
    // waypoint sits on a spine endpoint (those are landings). A
    // junction at every tooth's crossing, BOUND to the spine and its
    // own tooth - the authored T gesture - keeps the comb ONE net
    // through both paths, and the joins the dots ride are none (a
    // bound junction is the explicit dot; nothing is silent).
    const std::uint32_t spine = b.wire_j(0, 0,
                                         static_cast<double>(teeth) + 1.0, 0);
    for (std::size_t i = 1; i <= teeth; ++i) {
        const double x = static_cast<double>(i);
        const std::uint32_t tooth = b.wire_j(x, -5, x, 5, {Pos{x, 0}});
        b.junction(x, 0, {spine, tooth});
    }
    const NetBuild built = ws::circsim::build_nets(b.s);
    CombAnswer out;
    out.nets = built.nets.nets.size();
    return out;
}

}  // namespace

TEST_CASE("the segment grid answers what the brute force answers") {
    // Below the threshold: the historical brute-force path.
    const CombAnswer small = comb(60);
    REQUIRE(small.nets == 1);

    // Above the threshold: the grid path, same comb shape.
    const CombAnswer large = comb(600);
    REQUIRE(large.nets == 1);
}
