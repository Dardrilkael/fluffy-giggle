// ws::circsim - the schematic interaction geometry, headless (the
// suite geom.hpp promises). The cases here pin the LIB API directly;
// the editor app's own suites keep driving the same math through the
// Editor forwarders (test_canvas_panel.cpp) - the behaviors below are
// the ones that suite was born pinning, restated against the lib seam
// so a second consumer (a batch CLI, another frontend) can rely on
// them without the app.

#include <doctest/doctest.h>  // (this suite rides test_format.cpp's main - the lib suite's shape)

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <ws/circsim/geom.hpp>
#include <ws/circsim/library.hpp>

using ws::circsim::Pos;

namespace {

// A two-resistor sheet: R1 at the origin (pins left/right), R2 to the
// right, a wire joining them, a junction on the wire's mid, a label
// and a directive naming R1 (annotate's warning case).
[[nodiscard]] ws::circsim::Schematic two_resistors() {
    ws::circsim::Schematic s;
    const ws::circsim::SymbolLibrary lib;
    const ws::circsim::SymbolDef* r = lib.find("r");
    REQUIRE(r != nullptr);

    ws::circsim::Part p1{};
    p1.id = s.alloc_id();
    p1.symbol = r->key;
    p1.ref = "R1";
    p1.pos = {0.0, 0.0};
    s.parts_mut().push_back(p1);
    s.bump();

    ws::circsim::Part p2{};
    p2.id = s.alloc_id();
    p2.symbol = r->key;
    p2.ref = "R9";
    p2.pos = {8.0, 0.0};
    s.parts_mut().push_back(p2);
    s.bump();
    return s;
}

}  // namespace

// --- route_wire -------------------------------------------------------------

TEST_CASE("route_wire: a reversal target keeps the lead-out corner") {
    // A pin at (0,0) facing +x (dir 0) with the target at (-5,0): the
    // collinear fold once read the reversal as a continuation and
    // swallowed the (1,0) lead - the wire ran straight back through
    // the part body it just left.
    const std::vector<Pos> w =
        ws::circsim::route_wire({0.0, 0.0}, 0, {-5.0, 0.0}, -1);
    REQUIRE(w.size() == 1);
    CHECK(w[0].x == 1.0);
    CHECK(w[0].y == 0.0);
}

TEST_CASE("route_wire: 360 degrees routes exactly as 0") {
    const std::vector<Pos> a =
        ws::circsim::route_wire({0.0, 0.0}, 0, {4.0, 3.0}, -1);
    const std::vector<Pos> b =
        ws::circsim::route_wire({0.0, 0.0}, 360, {4.0, 3.0}, -1);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].x == b[i].x);
        CHECK(a[i].y == b[i].y);
    }
}

TEST_CASE("route_wire: aligned endpoints fold to nothing") {
    // Both on y=0, free ends: no corner, no waypoints.
    CHECK(ws::circsim::route_wire({0.0, 0.0}, -1, {5.0, 0.0}, -1)
              .empty());
}

// --- tidy_route ---------------------------------------------------------------

TEST_CASE("tidy_route: every diagonal leg earns its axis-aligned corner") {
    // (0,0) -> (2,1) -> (4,0): both legs are diagonal, so each earns a
    // corner (horizontal-first, the stock bend) - the author's vertex
    // (2,1) survives as the middle of the Z, the fewest bends that
    // keep the rest of the shape.
    const std::vector<Pos> w = ws::circsim::tidy_route(
        {{0.0, 0.0}, {2.0, 1.0}, {4.0, 0.0}});
    REQUIRE(w.size() == 3);
    CHECK(w[0] == Pos{2.0, 0.0});
    CHECK(w[1] == Pos{2.0, 1.0});
    CHECK(w[2] == Pos{4.0, 1.0});
}

TEST_CASE("tidy_route: duplicate points fold away") {
    const std::vector<Pos> w = ws::circsim::tidy_route(
        {{0.0, 0.0}, {0.0, 0.0}, {3.0, 0.0}});
    CHECK(w.empty());
}

// --- refdes -------------------------------------------------------------------

TEST_CASE("next_ref: the highest on the prefix, plus one") {
    const ws::circsim::Schematic s = two_resistors();
    const ws::circsim::SymbolLibrary lib;
    const ws::circsim::SymbolDef* r = lib.find("r");
    REQUIRE(r != nullptr);
    CHECK(ws::circsim::next_ref(s, *r) == "R10");
}

TEST_CASE("next_ref: a ref-less symbol answers empty") {
    const ws::circsim::SymbolLibrary lib;
    const ws::circsim::SymbolDef* gnd = lib.find("gnd");
    REQUIRE(gnd != nullptr);
    CHECK(ws::circsim::next_ref({}, *gnd).empty());
}

TEST_CASE("is_auto_ref: prefix + digits only") {
    CHECK(ws::circsim::is_auto_ref("R12", "R"));
    CHECK_FALSE(ws::circsim::is_auto_ref("R_load", "R"));
    CHECK_FALSE(ws::circsim::is_auto_ref("TP1a", "TP"));
    CHECK_FALSE(ws::circsim::is_auto_ref("R", "R"));
}

TEST_CASE("mentions_token: whole words only, ASCII case-insensitive") {
    CHECK(ws::circsim::mentions_token(".meas I(R1)", "R1"));
    CHECK_FALSE(ws::circsim::mentions_token(".meas I(R10)", "R1"));
    CHECK_FALSE(ws::circsim::mentions_token("XR1", "R1"));
    CHECK(ws::circsim::mentions_token("i(r1) = 1", "R1"));
    CHECK_FALSE(ws::circsim::mentions_token("anything", ""));
}

TEST_CASE("annotation_order: rows top to bottom, left to right") {
    // Four parts: two on the top row (B left of A on disk), two below.
    const std::vector<Pos> at{{6.0, 5.0}, {0.0, 5.0}, {6.0, 0.0},
                              {0.0, 0.0}};
    const std::vector<std::size_t> order =
        ws::circsim::annotation_order(at, 1.0);
    REQUIRE(order.size() == 4);
    CHECK(order[0] == 1);  // top row, left
    CHECK(order[1] == 0);  // top row, right
    CHECK(order[2] == 3);  // bottom row, left
    CHECK(order[3] == 2);  // bottom row, right
}

// --- bounds -------------------------------------------------------------------

TEST_CASE("part_world_bounds: the transformed symbol box, per part id") {
    const ws::circsim::Schematic s = two_resistors();
    const ws::circsim::SymbolLibrary lib;
    const ws::circsim::Part* p1 = s.find_part(
        s.parts().front().id);
    REQUIRE(p1 != nullptr);
    const std::optional<ws::circsim::Bounds> b =
        ws::circsim::part_world_bounds(s, lib, p1->id);
    REQUIRE(b.has_value());
    // The r symbol spans roughly [-3,3] x [-1,1] locally; unrotated at
    // the origin the world box is the local box.
    CHECK(b->min_x <= -2.0);
    CHECK(b->max_x >= 2.0);
    CHECK(b->min_y <= -0.5);
    CHECK(b->max_y >= 0.5);
    // A missing id answers nothing.
    CHECK_FALSE(
        ws::circsim::part_world_bounds(s, lib, 999999u).has_value());
}

TEST_CASE("content_bounds: covers parts, junctions, labels and wires") {
    ws::circsim::Schematic s = two_resistors();
    const ws::circsim::SymbolLibrary lib;

    ws::circsim::Junction j{};
    j.id = s.alloc_id();
    j.pos = {-20.0, 7.0};
    s.junctions_mut().push_back(j);
    s.bump();

    ws::circsim::Label l{};
    l.id = s.alloc_id();
    l.pos = {25.0, -9.0};
    l.name = "net";
    s.labels_mut().push_back(l);
    s.bump();

    const std::optional<ws::circsim::Bounds> b =
        ws::circsim::content_bounds(s, lib);
    REQUIRE(b.has_value());
    CHECK(b->min_x <= -20.0);
    CHECK(b->max_x >= 25.0);
    CHECK(b->min_y <= -9.0);
    CHECK(b->max_y >= 7.0);
}

TEST_CASE("wire_path: endpoints through their refs, waypoints between") {
    ws::circsim::Schematic s;
    const ws::circsim::SymbolLibrary lib;

    ws::circsim::Junction a{};
    a.id = s.alloc_id();
    a.pos = {0.0, 0.0};
    s.junctions_mut().push_back(a);
    s.bump();
    ws::circsim::Junction b{};
    b.id = s.alloc_id();
    b.pos = {4.0, 0.0};
    s.junctions_mut().push_back(b);
    s.bump();

    ws::circsim::Wire w = ws::circsim::Wire::endpoints(
        ws::circsim::JunctionRef{a.id}, ws::circsim::JunctionRef{b.id});
    w.id = s.alloc_id();
    w.waypoints = {{2.0, 2.0}};
    s.wires_mut().push_back(w);
    s.bump();

    const std::vector<Pos> path = ws::circsim::wire_path(s, lib, w);
    REQUIRE(path.size() == 3);
    CHECK(path[0] == Pos{0.0, 0.0});
    CHECK(path[1] == Pos{2.0, 2.0});
    CHECK(path[2] == Pos{4.0, 0.0});
}

// --- picking ------------------------------------------------------------------

TEST_CASE("hit_pin and hit_junction: the nearest within the radius") {
    ws::circsim::Schematic s = two_resistors();
    const ws::circsim::SymbolLibrary lib;
    const ws::circsim::Part* p1 = &s.parts().front();

    // R1's left pin sits near (-3, 0) at the stock symbol.
    const std::optional<ws::circsim::PinHit> hit =
        ws::circsim::hit_pin(s, lib, {-3.0, 0.2}, 1.0);
    REQUIRE(hit.has_value());
    CHECK(hit->ref.part == p1->id);
    CHECK(std::fabs(hit->pos.x - (-3.0)) < 1.0);
    CHECK(std::fabs(hit->pos.y - 0.0) < 1.0);
    CHECK(hit->dir == 180);  // the left pin faces west

    // Far away: nothing.
    CHECK_FALSE(
        ws::circsim::hit_pin(s, lib, {50.0, 50.0}, 1.0).has_value());

    ws::circsim::Junction j{};
    j.id = s.alloc_id();
    j.pos = {2.0, 5.0};
    s.junctions_mut().push_back(j);
    s.bump();
    CHECK(ws::circsim::hit_junction(s, {2.3, 5.2}, 0.6).has_value());
    CHECK_FALSE(ws::circsim::hit_junction(s, {2.3, 5.2}, 0.2).has_value());
}

TEST_CASE("hit_wire_point: the snap is bit-equal to a path vertex") {
    // A two-segment path (an L): (0,0) -> (3,0) -> (3,3). A click just
    // past the corner, off the grid: the snap lands on the nearer
    // ENDPOINT (a path vertex, always grid-aligned) when the grid snap
    // would drift off the segment - the junction minted there is
    // accepted by nets.cpp's exact-double on_polyline.
    ws::circsim::Schematic s;
    const ws::circsim::SymbolLibrary lib;

    ws::circsim::Junction a{};
    a.id = s.alloc_id();
    a.pos = {0.0, 0.0};
    s.junctions_mut().push_back(a);
    s.bump();
    ws::circsim::Junction b{};
    b.id = s.alloc_id();
    b.pos = {3.0, 3.0};
    s.junctions_mut().push_back(b);
    s.bump();

    ws::circsim::Wire w = ws::circsim::Wire::endpoints(
        ws::circsim::JunctionRef{a.id}, ws::circsim::JunctionRef{b.id});
    w.id = s.alloc_id();
    w.waypoints = {{3.0, 0.0}};
    s.wires_mut().push_back(w);
    s.bump();

    // A click near the vertical leg's lower end: the snap is the
    // corner vertex (3,0) exactly.
    const std::optional<ws::circsim::WirePointSnap> hit =
        ws::circsim::hit_wire_point(s, lib, {3.2, 0.1}, 0.4, 1.0);
    REQUIRE(hit.has_value());
    CHECK(hit->wire == w.id);
    CHECK(hit->pos.x == 3.0);
    CHECK(hit->pos.y == 0.0);

    // Far from every wire: nothing.
    CHECK_FALSE(ws::circsim::hit_wire_point(s, lib, {20.0, 20.0}, 0.4, 1.0)
                    .has_value());
}

// --- placement ----------------------------------------------------------------

TEST_CASE("magnet_shift: no pin in range answers the base unchanged") {
    const ws::circsim::Schematic s = two_resistors();
    const ws::circsim::SymbolLibrary lib;
    const ws::circsim::SymbolDef* r = lib.find("r");
    REQUIRE(r != nullptr);
    const Pos base{0.0, 20.0};  // far from every pin
    const Pos at = ws::circsim::magnet_shift(s, lib, *r, base, 0, false,
                                             2.0);
    CHECK(at.x == base.x);
    CHECK(at.y == base.y);
}

TEST_CASE("attached_wire_routes: no wires, no routes") {
    const ws::circsim::Schematic s = two_resistors();
    const ws::circsim::SymbolLibrary lib;
    const ws::circsim::Part* p1 = &s.parts().front();
    CHECK(ws::circsim::attached_wire_routes(
              s, lib, ws::circsim::AttachedKind::Part, p1->id)
              .empty());
}

// --- colors ----------------------------------------------------------------

TEST_CASE("series_color: the wheel cycles every eight") {
    CHECK(ws::circsim::series_color(0) == ws::circsim::series_color(8));
    CHECK(ws::circsim::series_color(3) != ws::circsim::series_color(4));
    // The head the Results plot always painted first: red,
    // 0xE05A4EFF (the r2d packing).
    CHECK(ws::circsim::series_color(0) == 0xE05A4EFFu);
    CHECK(ws::circsim::series_color(1) == 0x4EA1E0FFu);
}
