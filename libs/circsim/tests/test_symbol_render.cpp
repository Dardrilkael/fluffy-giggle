// render (stage 17a): the symbol render seam - one pure flattening of
// a placed symbol's picture into world-space strokes, shared by the
// editor's canvas painting, the CLI's SVG dump and these tests. The
// cases here are CHARACTERIZATIONS of the stage-2 renderer's behavior
// the seam moved verbatim (including the known warts - every Poly
// closes, the sources ride the pin-lead safety net): the stage-17
// redesign commits flip exactly these expectations, one commit per
// fix, so the diff is the review.

#include <cmath>

#include <doctest/doctest.h>

#include <ws/circsim/render.hpp>
#include <ws/circsim/symbols.hpp>

using ws::circsim::DrawPrim;
using ws::circsim::Part;
using ws::circsim::PinDef;
using ws::circsim::Pos;
using ws::circsim::SymbolDef;
using ws::circsim::SymbolStroke;
using ws::circsim::render_symbol;

namespace {

Part placement(Pos pos, int rot = 0, bool mirror = false) {
    Part p;
    p.pos = pos;
    p.rot = rot;
    p.mirror = mirror;
    return p;
}

const SymbolStroke* find_kind(const std::vector<SymbolStroke>& strokes,
                              SymbolStroke::Kind kind, std::size_t nth = 0) {
    std::size_t seen = 0;
    for (const SymbolStroke& s : strokes) {
        if (s.kind == kind) {
            if (seen == nth) {
                return &s;
            }
            ++seen;
        }
    }
    return nullptr;
}

std::size_t count_kind(const std::vector<SymbolStroke>& strokes,
                       SymbolStroke::Kind kind) {
    std::size_t n = 0;
    for (const SymbolStroke& s : strokes) {
        if (s.kind == kind) {
            ++n;
        }
    }
    return n;
}

bool pts_eq(const SymbolStroke& s, std::size_t i, Pos want) {
    return s.pts.size() > i && s.pts[i].x == want.x && s.pts[i].y == want.y;
}

}  // namespace

TEST_CASE("render_symbol flattens the table's prims at identity") {
    const SymbolDef* r = ws::circsim::find_symbol("r");
    REQUIRE(r != nullptr);
    const std::vector<SymbolStroke> out =
        render_symbol(*r, placement(Pos{0.0, 0.0}));
    // The zigzag poly and NO stubs: both pin offsets carry prim
    // vertices.
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].kind == SymbolStroke::Kind::Poly);
    REQUIRE(out[0].pts.size() == 7);
    CHECK(pts_eq(out[0], 0, Pos{-3.0, 0.0}));
    CHECK(pts_eq(out[0], 3, Pos{0.0, 0.6}));
    CHECK(pts_eq(out[0], 6, Pos{3.0, 0.0}));
}

TEST_CASE("render_symbol rides the part transform") {
    const SymbolDef* r = ws::circsim::find_symbol("r");
    REQUIRE(r != nullptr);

    // Rotation about the part position, y up.
    std::vector<SymbolStroke> out =
        render_symbol(*r, placement(Pos{10.0, 10.0}, 90));
    REQUIRE(out.size() == 1);
    CHECK(pts_eq(out[0], 0, Pos{10.0, 7.0}));   // (-3,0) rot90 -> (0,-3)
    CHECK(pts_eq(out[0], 6, Pos{10.0, 13.0}));  // (3,0) rot90 -> (0,3)

    // Mirror x-flips before the rotation.
    out = render_symbol(*r, placement(Pos{0.0, 0.0}, 0, true));
    REQUIRE(out.size() == 1);
    CHECK(pts_eq(out[0], 0, Pos{3.0, 0.0}));
    CHECK(pts_eq(out[0], 6, Pos{-3.0, 0.0}));
}

TEST_CASE("arcs fold mirror and rotation into their angles") {
    const SymbolDef* l = ws::circsim::find_symbol("l");
    REQUIRE(l != nullptr);
    // The table's first inductor arc: center (-2.25,0), 0..180 deg.
    const DrawPrim& table_arc = l->draw[0];
    REQUIRE(table_arc.kind == DrawPrim::Kind::Arc);
    CHECK(table_arc.a0 == 0.0);
    CHECK(table_arc.a1 == 180.0);

    std::vector<SymbolStroke> out =
        render_symbol(*l, placement(Pos{0.0, 0.0}));
    const SymbolStroke* arc = find_kind(out, SymbolStroke::Kind::Arc);
    REQUIRE(arc != nullptr);
    CHECK(arc->a0 == 0.0);
    CHECK(arc->a1 == 180.0);
    CHECK(pts_eq(*arc, 0, Pos{-2.25, 0.0}));

    // Mirror reflects about the y axis and swaps the sweep.
    out = render_symbol(*l, placement(Pos{0.0, 0.0}, 0, true));
    arc = find_kind(out, SymbolStroke::Kind::Arc);
    REQUIRE(arc != nullptr);
    CHECK(arc->a0 == 0.0);     // 180 - 180
    CHECK(arc->a1 == 180.0);   // 180 - 0
    CHECK(pts_eq(*arc, 0, Pos{2.25, 0.0}));

    // Rotation adds to both ends.
    out = render_symbol(*l, placement(Pos{0.0, 0.0}, 90));
    arc = find_kind(out, SymbolStroke::Kind::Arc);
    REQUIRE(arc != nullptr);
    CHECK(arc->a0 == 90.0);
    CHECK(arc->a1 == 270.0);
}

TEST_CASE("rects flatten to closed quads") {
    // The .csym tline example carries the only table Rect; a
    // hand-built one covers the kind here (corner order: a, bx, b, ay
    // - a closed quad, not an axis-aligned rect call, since the
    // placement may rotate the box).
    SymbolDef s;
    s.key = "rectsym";
    DrawPrim d;
    d.kind = DrawPrim::Kind::Rect;
    d.pts = {Pos{-1.0, -2.0}, Pos{1.0, 2.0}};
    s.draw.push_back(d);
    const std::vector<SymbolStroke> out =
        render_symbol(s, placement(Pos{5.0, 5.0}));
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == SymbolStroke::Kind::Poly);
    REQUIRE(out[0].pts.size() == 4);
    CHECK(pts_eq(out[0], 0, Pos{4.0, 3.0}));
    CHECK(pts_eq(out[0], 1, Pos{6.0, 3.0}));
    CHECK(pts_eq(out[0], 2, Pos{6.0, 7.0}));
    CHECK(pts_eq(out[0], 3, Pos{4.0, 7.0}));
    CHECK(out[0].closed);
}

TEST_CASE("the pin-lead safety net fires only for untouched pins") {
    // A symbol whose prims fall short of its pins: the .csym safety
    // net draws a stub into the body, opposite the pin's direction.
    // The reach math's outside-bounds branches are dead code (the
    // bounds include the pins, so a pin can never sit outside them)
    // - every stub is the 1.5-unit default, kept verbatim by the
    // seam. A stub is a LEAD, not a connection: net building never
    // reads it.
    SymbolDef s;
    s.key = "stubbed";
    s.pins = {PinDef{.name = "a", .dx = -4.0, .dy = 0.0, .dir = 180}};
    DrawPrim body;
    body.kind = DrawPrim::Kind::Circle;
    body.pts = {Pos{0.0, 0.0}};
    body.r = 1.0;
    s.draw.push_back(body);

    const std::vector<SymbolStroke> out =
        render_symbol(s, placement(Pos{0.0, 0.0}));
    REQUIRE(out.size() == 2);
    const SymbolStroke& stub = out[1];
    REQUIRE(stub.kind == SymbolStroke::Kind::Line);
    // Pin at (-4,0), dir 180: the stub points INTO the body (+x),
    // 1.5 units long.
    CHECK(pts_eq(stub, 0, Pos{-4.0, 0.0}));
    CHECK(pts_eq(stub, 1, Pos{-2.5, 0.0}));

    // A prim vertex ON the pin (a lead) retires the stub.
    DrawPrim lead;
    lead.kind = DrawPrim::Kind::Line;
    lead.pts = {Pos{-4.0, 0.0}, Pos{-1.0, 0.0}};
    s.draw.push_back(lead);
    CHECK(render_symbol(s, placement(Pos{0.0, 0.0})).size() == 2);
}

TEST_CASE("characterization: sources ride the safety net today") {
    // The v source's pins sit ON its circle - no prim VERTEX there -
    // so the stage-2 safety net draws a stub from each pin INTO the
    // body: a diameter line straight through the source. This case
    // pins the wart the stage-17d redesign removes (the pins move out
    // to +-3 with real lead prims).
    const SymbolDef* v = ws::circsim::find_symbol("v");
    REQUIRE(v != nullptr);
    const std::vector<SymbolStroke> out =
        render_symbol(*v, placement(Pos{0.0, 0.0}));
    REQUIRE(count_kind(out, SymbolStroke::Kind::Circle) == 1);
    REQUIRE(count_kind(out, SymbolStroke::Kind::Text) == 1);
    const std::size_t lines = count_kind(out, SymbolStroke::Kind::Line);
    REQUIRE(lines == 2);  // one stub per pin - the wart
    const SymbolStroke* first =
        find_kind(out, SymbolStroke::Kind::Line);
    REQUIRE(first != nullptr);
    // Pin p at (-1,0) dir 180: the stub crosses 1.5 units into the
    // body - past the center, over the polarity text's anchor.
    CHECK(pts_eq(*first, 0, Pos{-1.0, 0.0}));
    CHECK(pts_eq(*first, 1, Pos{0.5, 0.0}));
}

TEST_CASE("characterization: every Poly closes today") {
    // The stage-2 renderer closed every polyline (the editor passed
    // closed=true unconditionally): the resistor's OPEN zigzag wore a
    // phantom closing segment back through its teeth, and the diode's
    // triangle - closed by construction, its first vertex repeated at
    // the end - closed again. The stage-17c fix flips this flag to
    // false (closed shapes close by construction); until then this
    // case is the honest characterization.
    const SymbolDef* r = ws::circsim::find_symbol("r");
    REQUIRE(r != nullptr);
    const std::vector<SymbolStroke> rz =
        render_symbol(*r, placement(Pos{0.0, 0.0}));
    const SymbolStroke* poly = find_kind(rz, SymbolStroke::Kind::Poly);
    REQUIRE(poly != nullptr);
    REQUIRE(poly->pts.size() == 7);
    CHECK(poly->closed);  // the bug, characterized

    const SymbolDef* d = ws::circsim::find_symbol("d");
    REQUIRE(d != nullptr);
    const std::vector<SymbolStroke> dz =
        render_symbol(*d, placement(Pos{0.0, 0.0}));
    poly = find_kind(dz, SymbolStroke::Kind::Poly);
    REQUIRE(poly != nullptr);
    REQUIRE(poly->pts.size() == 4);
    CHECK(poly->pts.front().x == poly->pts.back().x);
    CHECK(poly->pts.front().y == poly->pts.back().y);
    CHECK(poly->closed);
}

TEST_CASE("malformed prims are skipped, not repaired") {
    SymbolDef s;
    s.key = "broken";
    DrawPrim short_line;
    short_line.kind = DrawPrim::Kind::Line;
    short_line.pts = {Pos{0.0, 0.0}};
    s.draw.push_back(short_line);
    DrawPrim empty_circle;
    empty_circle.kind = DrawPrim::Kind::Circle;
    s.draw.push_back(empty_circle);
    s.pins = {PinDef{.name = "a", .dx = 0.0, .dy = 0.0, .dir = 180}};
    // The empty-circle center at (0,0) is no vertex; the pin at the
    // origin IS the short line's vertex - no stub, no strokes that
    // could render.
    const std::vector<SymbolStroke> out =
        render_symbol(s, placement(Pos{0.0, 0.0}));
    CHECK(out.empty());
}
