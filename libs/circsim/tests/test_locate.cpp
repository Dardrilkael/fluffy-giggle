// locate (stage 3): the Problems panel's jump seam - a Diag's `where`
// spelling parsed back into the object it names, and the focus bounds
// the editor zooms to. One case per producer spelling.

#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ws/circsim/erc.hpp>
#include <ws/circsim/export.hpp>
#include <ws/circsim/locate.hpp>
#include <ws/circsim/nets.hpp>

using ws::circsim::Bounds;
using ws::circsim::Diag;
using ws::circsim::DiagTarget;
using ws::circsim::Junction;
using ws::circsim::JunctionRef;
using ws::circsim::Label;
using ws::circsim::Part;
using ws::circsim::PinRef;
using ws::circsim::Pos;
using ws::circsim::Schematic;
using ws::circsim::Wire;

namespace {

struct Bench {
    Schematic s;

    Part* part(std::string symbol, std::string ref, std::string value,
               Pos at) {
        return s.add_part(
            Part::make(std::move(symbol), std::move(ref),
                       std::move(value), at));
    }
    Part* r(std::string ref, Pos at) {
        return part("r", std::move(ref), "1k", at);
    }

    std::uint32_t pin_wire(PinRef a, PinRef b_) {
        Wire* w = s.add_wire({});
        w->a = a;
        w->b = b_;
        return w->id;
    }
    void j_wire(double ax, double ay, double bx, double by) {
        Wire* w = s.add_wire({});
        w->a = ws::circsim::JunctionRef{junction(ax, ay)};
        w->b = ws::circsim::JunctionRef{junction(bx, by)};
    }
    std::uint32_t junction(double x, double y) {
        Junction* j = s.add_junction({});
        j->pos = Pos{x, y};
        return j->id;
    }
    std::uint32_t label(double x, double y, std::string name) {
        Label* l = s.add_label({});
        l->pos = Pos{x, y};
        l->name = std::move(name);
        return l->id;
    }
};

Diag diag(std::string where) {
    Diag d;
    d.severity = ws::circsim::Severity::Error;
    d.code = "circsim.test";
    d.message = "the message";
    d.where = std::move(where);
    return d;
}

const ws::circsim::SpiceExport* export_of(const Schematic& s) {
    static ws::circsim::SpiceExport cached;
    const ws::circsim::NetBuild build = ws::circsim::build_nets(s);
    const auto deck =
        ws::circsim::export_deck(s, build.nets, nullptr, nullptr, nullptr);
    REQUIRE(deck.has_value());
    cached = *deck;
    return &cached;
}

}  // namespace

TEST_CASE("a ref where resolves to the part") {
    Bench b;
    Part* r1 = b.r("R1", {6.0, 0.0});
    (void)b.r("R2", {14.0, 0.0});

    const DiagTarget t =
        ws::circsim::locate_diag(diag("R1"), b.s, nullptr, nullptr);
    REQUIRE(t.kind == DiagTarget::Kind::Part);
    CHECK(t.part == r1->id);
    CHECK_FALSE(t.has_pin());
}

TEST_CASE("the export back-map carries the mangled names") {
    Bench b;
    // A ref ERC will flag (wrong prefix): the export mangles it, and
    // the run problems carry the MANGLED element name - locating it
    // must still find the part.
    Part* bad = b.part("r", "z1!", "1k", {0.0, 0.0});
    const ws::circsim::SpiceExport* map = export_of(b.s);
    REQUIRE(map->element_part.count("R_z1") == 1u);

    const DiagTarget t =
        ws::circsim::locate_diag(diag("R_z1"), b.s, nullptr, map);
    REQUIRE(t.kind == DiagTarget::Kind::Part);
    CHECK(t.part == bad->id);

    // The raw ref also resolves (ERC's own spelling, no map needed).
    const DiagTarget by_ref =
        ws::circsim::locate_diag(diag("z1!"), b.s, nullptr, nullptr);
    REQUIRE(by_ref.kind == DiagTarget::Kind::Part);
    CHECK(by_ref.part == bad->id);
}

TEST_CASE("the ref-less spellings: symbol id and part id") {
    Bench b;
    Part* g = b.part("gnd", "", "", {0.0, -6.0});
    Part* n = b.part("npn", "Q1", "", {10.0, 0.0});
    n->params["model"] = "npn";

    const DiagTarget t = ws::circsim::locate_diag(diag("gnd " +
                                                       std::to_string(g->id)),
                                                  b.s, nullptr, nullptr);
    REQUIRE(t.kind == DiagTarget::Kind::Part);
    CHECK(t.part == g->id);

    // The export's dangling spelling ("part 12") - the part exists.
    const DiagTarget p = ws::circsim::locate_diag(
        diag("part " + std::to_string(n->id)), b.s, nullptr, nullptr);
    REQUIRE(p.kind == DiagTarget::Kind::Part);
    CHECK(p.part == n->id);

    // A "part 99" for a part that is gone answers None.
    CHECK(ws::circsim::locate_diag(diag("part 99"), b.s, nullptr, nullptr)
              .kind == DiagTarget::Kind::None);
}

TEST_CASE("the pin tail resolves by name or index") {
    Bench b;
    Part* q = b.part("npn", "Q1", "", {10.0, 0.0});
    q->params["model"] = "npn";

    const DiagTarget by_name =
        ws::circsim::locate_diag(diag("Q1 pin b"), b.s, nullptr, nullptr);
    REQUIRE(by_name.kind == DiagTarget::Kind::Part);
    CHECK(by_name.part == q->id);
    REQUIRE(by_name.has_pin());
    CHECK(by_name.pin == 1u);  // c(0) b(1) e(2)

    const DiagTarget by_index =
        ws::circsim::locate_diag(diag("Q1 pin 2"), b.s, nullptr, nullptr);
    REQUIRE(by_index.kind == DiagTarget::Kind::Part);
    CHECK(by_index.pin == 2u);

    // An unknown pin name answers the part with no pin.
    const DiagTarget unknown =
        ws::circsim::locate_diag(diag("Q1 pin zz"), b.s, nullptr, nullptr);
    REQUIRE(unknown.kind == DiagTarget::Kind::Part);
    CHECK_FALSE(unknown.has_pin());
}

TEST_CASE("net wheres resolve through the NetList and the node map") {
    Bench b;
    Part* r1 = b.r("R1", {6.0, 0.0});
    Part* r2 = b.r("R2", {14.0, 0.0});
    b.pin_wire(PinRef{r1->id, 1}, PinRef{r2->id, 0});
    b.label(10.0, 0.0, "out");
    const ws::circsim::NetBuild build = ws::circsim::build_nets(b.s);
    const ws::circsim::SpiceExport* map = export_of(b.s);

    const ws::circsim::Net* out = build.nets.by_name("out");
    REQUIRE(out != nullptr);

    const DiagTarget by_name = ws::circsim::locate_diag(diag("net out"),
                                                        b.s, &build.nets, map);
    REQUIRE(by_name.kind == DiagTarget::Kind::Net);
    CHECK(by_name.net == out->id);

    // The deck's node token through the back-map (no NetList needed).
    const std::string token =
        map->node_net.count("out") == 1u ? "out" : "N001";
    REQUIRE(map->node_net.count(token) == 1u);
    const DiagTarget by_token =
        ws::circsim::locate_diag(diag("net " + token), b.s, nullptr, map);
    REQUIRE(by_token.kind == DiagTarget::Kind::Net);
    CHECK(by_token.net == map->node_net.at(token));

    // A name nothing knows answers None.
    CHECK(ws::circsim::locate_diag(diag("net nope"), b.s, &build.nets, map)
              .kind == DiagTarget::Kind::None);
}

TEST_CASE("wire and label wheres resolve to the objects") {
    Bench b;
    Part* r1 = b.r("R1", {6.0, 0.0});
    Part* r2 = b.r("R2", {14.0, 0.0});
    const std::uint32_t wire =
        b.pin_wire(PinRef{r1->id, 1}, PinRef{r2->id, 0});
    const std::uint32_t lab = b.label(10.0, 0.0, "out");

    const DiagTarget w = ws::circsim::locate_diag(
        diag("wire " + std::to_string(wire)), b.s, nullptr, nullptr);
    REQUIRE(w.kind == DiagTarget::Kind::Wire);
    CHECK(w.obj == wire);

    const DiagTarget l = ws::circsim::locate_diag(diag("label out"), b.s,
                                                  nullptr, nullptr);
    REQUIRE(l.kind == DiagTarget::Kind::Label);
    CHECK(l.obj == lab);

    // A wire id that is gone, a label name nothing carries: None.
    CHECK(ws::circsim::locate_diag(diag("wire 99"), b.s, nullptr, nullptr)
              .kind == DiagTarget::Kind::None);
    CHECK(ws::circsim::locate_diag(diag("label nope"), b.s, nullptr, nullptr)
              .kind == DiagTarget::Kind::None);
}

TEST_CASE("comma lists answer the first token that resolves") {
    Bench b;
    Part* v1 = b.part("v", "V1", "5", {0.0, -4.0});
    (void)b.part("l", "L1", "1m", {0.0, 4.0});

    const DiagTarget t =
        ws::circsim::locate_diag(diag("NOPE, V1, L1"), b.s, nullptr, nullptr);
    REQUIRE(t.kind == DiagTarget::Kind::Part);
    CHECK(t.part == v1->id);
}

TEST_CASE("the block spellings and the empty where") {
    Bench b;
    (void)b.r("R1", {0.0, 0.0});

    const DiagTarget d = ws::circsim::locate_diag(diag("directives"), b.s,
                                                   nullptr, nullptr);
    CHECK(d.kind == DiagTarget::Kind::Directives);

    const DiagTarget a = ws::circsim::locate_diag(diag("analysis setup"),
                                                  b.s, nullptr, nullptr);
    CHECK(a.kind == DiagTarget::Kind::Analysis);

    // The temperature row's origin lands on the same block.
    const DiagTarget t = ws::circsim::locate_diag(diag("simulation setup"),
                                                  b.s, nullptr, nullptr);
    CHECK(t.kind == DiagTarget::Kind::Analysis);

    CHECK(ws::circsim::locate_diag(diag(""), b.s, nullptr, nullptr).kind ==
          DiagTarget::Kind::None);
    CHECK(ws::circsim::locate_diag(diag("gibberish"), b.s, nullptr, nullptr)
              .kind == DiagTarget::Kind::None);
}

TEST_CASE("focus_bounds answers the transformed part extent") {
    Bench b;
    // A resistor at the origin, rotated 90: its local x span (-3..3)
    // becomes the world y span, the y span (the 0.7-high zigzag teeth)
    // the x.
    Part* r1 = b.r("R1", {10.0, 20.0});
    r1->rot = 90;

    const DiagTarget t =
        ws::circsim::locate_diag(diag("R1"), b.s, nullptr, nullptr);
    const std::optional<Bounds> bounds = ws::circsim::focus_bounds(t, b.s);
    REQUIRE(bounds.has_value());
    // The pins bound the rotated y span (-3/+3 local); the teeth bound
    // the x (the 0.7 zigzag since 50bffe3, was 0.6).
    CHECK(bounds->min_x == doctest::Approx(10.0 - 0.7).epsilon(1e-9));
    CHECK(bounds->max_x == doctest::Approx(10.0 + 0.7).epsilon(1e-9));
    CHECK(bounds->min_y == doctest::Approx(20.0 - 3.0).epsilon(1e-9));
    CHECK(bounds->max_y == doctest::Approx(20.0 + 3.0).epsilon(1e-9));

    // Mirrored: the x span flips about the origin, the same extent.
    r1->rot = 0;
    r1->mirror = true;
    const std::optional<Bounds> flipped =
        ws::circsim::focus_bounds(t, b.s);
    REQUIRE(flipped.has_value());
    CHECK(flipped->min_x == doctest::Approx(10.0 - 3.0).epsilon(1e-9));
    CHECK(flipped->max_x == doctest::Approx(10.0 + 3.0).epsilon(1e-9));
}

TEST_CASE("focus_bounds answers the net, wire and label extents") {
    Bench b;
    Part* r1 = b.r("R1", {6.0, 0.0});
    Part* r2 = b.r("R2", {14.0, 0.0});
    b.pin_wire(PinRef{r1->id, 1}, PinRef{r2->id, 0});
    // A T-join up to the label: the junction at (10, 0) is a wire
    // VERTEX lying on the R1-R2 wire's polyline (the T-junction rule),
    // so the stub unions with the divider's net; the label sits on the
    // stub's far junction and names the net "out".
    b.j_wire(10.0, 5.0, 10.0, 0.0);
    b.label(10.0, 5.0, "out");
    const ws::circsim::NetBuild build = ws::circsim::build_nets(b.s);
    const ws::circsim::Net* out = build.nets.by_name("out");
    REQUIRE(out != nullptr);

    // The net: the extent spans the wire (pins at 9 and 11) and the
    // label's junction at (10, 5).
    DiagTarget net;
    net.kind = DiagTarget::Kind::Net;
    net.net = out->id;
    const std::optional<Bounds> nb =
        ws::circsim::focus_bounds(net, b.s, &build.nets);
    REQUIRE(nb.has_value());
    CHECK(nb->min_x == doctest::Approx(9.0).epsilon(1e-9));
    CHECK(nb->max_x == doctest::Approx(11.0).epsilon(1e-9));
    CHECK(nb->min_y == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(nb->max_y == doctest::Approx(5.0).epsilon(1e-9));

    // The same answer without the supplied NetList (the built path).
    const std::optional<Bounds> built =
        ws::circsim::focus_bounds(net, b.s);
    REQUIRE(built.has_value());
    CHECK(built->min_x == nb->min_x);
    CHECK(built->max_y == nb->max_y);

    // The wire: its two endpoints.
    DiagTarget wire;
    wire.kind = DiagTarget::Kind::Wire;
    wire.obj = b.s.wires().front().id;
    const std::optional<Bounds> wb =
        ws::circsim::focus_bounds(wire, b.s, &build.nets);
    REQUIRE(wb.has_value());
    CHECK(wb->min_x == doctest::Approx(9.0).epsilon(1e-9));
    CHECK(wb->max_x == doctest::Approx(11.0).epsilon(1e-9));

    // The label: one unit of air around the anchor.
    DiagTarget lab;
    lab.kind = DiagTarget::Kind::Label;
    lab.obj = b.label(0.0, 40.0, "far");
    const std::optional<Bounds> lb =
        ws::circsim::focus_bounds(lab, b.s, &build.nets);
    REQUIRE(lb.has_value());
    CHECK(lb->min_x == doctest::Approx(-1.0).epsilon(1e-9));
    CHECK(lb->max_x == doctest::Approx(1.0).epsilon(1e-9));
    CHECK(lb->min_y == doctest::Approx(39.0).epsilon(1e-9));
    CHECK(lb->max_y == doctest::Approx(41.0).epsilon(1e-9));

    // The no-geometry targets answer nothing.
    DiagTarget none;
    none.kind = DiagTarget::Kind::Directives;
    CHECK_FALSE(ws::circsim::focus_bounds(none, b.s).has_value());
    none.kind = DiagTarget::Kind::Analysis;
    CHECK_FALSE(ws::circsim::focus_bounds(none, b.s).has_value());
    none.kind = DiagTarget::Kind::None;
    CHECK_FALSE(ws::circsim::focus_bounds(none, b.s).has_value());
}

// --- the hardening batch: the structured target path --------------------------
// Producers now fill Diag::target at the source; locate_diag answers it
// verbatim, before (and instead of) parsing the `where` string - the
// legacy parse stays for producers that have not migrated.

TEST_CASE("a structured target answers before the where string is read") {
    Bench b;
    Part* r1 = b.r("R1", {0.0, 0.0});
    Part* r2 = b.r("R2", {10.0, 0.0});
    b.pin_wire(PinRef{r1->id, 0}, PinRef{r2->id, 0});

    Diag d;
    d.where = "total garbage the parser can never resolve";
    d.target.kind = DiagTarget::Kind::Part;
    d.target.part = r2->id;
    d.target.pin = 1;
    const DiagTarget t = ws::circsim::locate_diag(d, b.s, nullptr, nullptr);
    REQUIRE(t.kind == DiagTarget::Kind::Part);
    CHECK(t.part == r2->id);
    CHECK(t.pin == 1);

    // A target survives even when `where` is empty (file-level rows
    // that still know their object).
    Diag quiet;
    quiet.where = "";
    quiet.target.kind = DiagTarget::Kind::Net;
    quiet.target.net = 7;
    const DiagTarget t2 =
        ws::circsim::locate_diag(quiet, b.s, nullptr, nullptr);
    CHECK(t2.kind == DiagTarget::Kind::Net);
    CHECK(t2.net == 7);

    // No target: the legacy string parse still carries the row.
    Diag legacy;
    legacy.where = "R1";
    const DiagTarget t3 =
        ws::circsim::locate_diag(legacy, b.s, nullptr, nullptr);
    REQUIRE(t3.kind == DiagTarget::Kind::Part);
    CHECK(t3.part == r1->id);
}

TEST_CASE("ERC findings arrive with their targets already filled") {
    Schematic s;
    Part* r = s.add_part(
        Part::make("r", "X9", "1k", {0.0, 0.0}));  // wrong prefix
    (void)r;
    const std::vector<Diag> erc = ws::circsim::run_erc(s);
    bool saw_ref_prefix = false;
    for (const Diag& d : erc) {
        if (d.code == "circsim.erc.ref_prefix") {
            saw_ref_prefix = true;
            REQUIRE(d.target.kind == DiagTarget::Kind::Part);
            CHECK(d.target.part == r->id);
        }
        if (d.code == "circsim.erc.missing_ground") {
            CHECK(d.target.kind == DiagTarget::Kind::None);
        }
    }
    CHECK(saw_ref_prefix);

    // A floating pin names the part AND the pin index.
    Schematic two;
    Part* a = two.add_part(Part::make("r", "R1", "1k", {0.0, 0.0}));
    Part* c = two.add_part(Part::make("c", "C1", "1n", {10.0, 0.0}));
    two.add_part(Part::make("gnd", "", "", {5.0, -5.0}));
    two.add_wire(Wire::endpoints(PinRef{a->id, 0}, PinRef{c->id, 0}));
    const std::vector<Diag> erc2 = ws::circsim::run_erc(two);
    for (const Diag& d : erc2) {
        if (d.code == "circsim.erc.floating_pin") {
            REQUIRE(d.target.kind == DiagTarget::Kind::Part);
            CHECK(d.target.has_pin());
        }
    }
}
