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
    // 50bffe3's even-slope zigzag: short flat leads, then the 0.7-high
    // teeth (every slope 0.375 : 0.7) - 10 points, not the old 7.
    REQUIRE(out[0].pts.size() == 10);
    CHECK(pts_eq(out[0], 0, Pos{-3.0, 0.0}));
    CHECK(pts_eq(out[0], 1, Pos{-2.25, 0.0}));
    CHECK(pts_eq(out[0], 4, Pos{-0.375, 0.7}));
    CHECK(pts_eq(out[0], 5, Pos{0.375, -0.7}));
    CHECK(pts_eq(out[0], 9, Pos{3.0, 0.0}));
}

TEST_CASE("render_symbol rides the part transform") {
    const SymbolDef* r = ws::circsim::find_symbol("r");
    REQUIRE(r != nullptr);

    // Rotation about the part position, y up.
    std::vector<SymbolStroke> out =
        render_symbol(*r, placement(Pos{10.0, 10.0}, 90));
    REQUIRE(out.size() == 1);
    CHECK(pts_eq(out[0], 0, Pos{10.0, 7.0}));   // (-3,0) rot90 -> (0,-3)
    CHECK(pts_eq(out[0], 9, Pos{10.0, 13.0}));  // (3,0) rot90 -> (0,3)

    // Mirror x-flips before the rotation.
    out = render_symbol(*r, placement(Pos{0.0, 0.0}, 0, true));
    REQUIRE(out.size() == 1);
    CHECK(pts_eq(out[0], 0, Pos{3.0, 0.0}));
    CHECK(pts_eq(out[0], 9, Pos{-3.0, 0.0}));
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

TEST_CASE("sources own their pins: leads, polarity, no stubs") {
    // The stage-17d redesign: pins moved out to +-3 with real lead
    // prims - a prim vertex ON each pin retires the safety net (the
    // stage-2 wart drew a 1.5-unit stub straight through each source
    // body because a pin ON the circle has no prim vertex). The +/-
    // polarity is line-drawn inside the circle (no font, no Text
    // prim), and the current source's arrow head is a triangle closed
    // by construction.
    const SymbolDef* v = ws::circsim::find_symbol("v");
    REQUIRE(v != nullptr);
    REQUIRE(v->pins.size() == 2);
    CHECK(v->pins[0].dx == -3.0);
    CHECK(v->pins[1].dx == 3.0);
    const std::vector<SymbolStroke> out =
        render_symbol(*v, placement(Pos{0.0, 0.0}));
    CHECK(count_kind(out, SymbolStroke::Kind::Circle) == 1);
    CHECK(count_kind(out, SymbolStroke::Kind::Text) == 0);
    CHECK(count_kind(out, SymbolStroke::Kind::Line) ==
          5);  // 2 leads + the + cross (2) + the - bar (1)
    CHECK(out.size() == 6);  // and nothing else - no safety-net stubs

    // The first lead: circle edge to the p pin.
    const SymbolStroke* l0 = find_kind(out, SymbolStroke::Kind::Line);
    REQUIRE(l0 != nullptr);
    CHECK(pts_eq(*l0, 0, Pos{-3.0, 0.0}));
    CHECK(pts_eq(*l0, 1, Pos{-1.0, 0.0}));

    // The polarity marks: + at the p side, - at the n side.
    bool plus_h = false, plus_v = false, minus = false;
    for (const SymbolStroke& s : out) {
        if (s.kind != SymbolStroke::Kind::Line) {
            continue;
        }
        if (pts_eq(s, 0, Pos{-0.65, 0.0}) && pts_eq(s, 1, Pos{-0.05, 0.0})) {
            plus_h = true;
        }
        if (pts_eq(s, 0, Pos{-0.35, -0.3}) && pts_eq(s, 1, Pos{-0.35, 0.3})) {
            plus_v = true;
        }
        if (pts_eq(s, 0, Pos{0.05, 0.0}) && pts_eq(s, 1, Pos{0.65, 0.0})) {
            minus = true;
        }
    }
    CHECK(plus_h);
    CHECK(plus_v);
    CHECK(minus);

    // The current source: the same lead contract, plus the arrow head
    // as a FILLED triangle (PolyFill - the fill closes it, no repeated
    // vertex, and the head reads solid at every zoom).
    const SymbolDef* i = ws::circsim::find_symbol("i");
    REQUIRE(i != nullptr);
    const std::vector<SymbolStroke> io =
        render_symbol(*i, placement(Pos{0.0, 0.0}));
    CHECK(count_kind(io, SymbolStroke::Kind::Line) == 3);  // 2 leads + shaft
    CHECK(io.size() == 5);                                 // + circle + head
    const SymbolStroke* head = find_kind(io, SymbolStroke::Kind::PolyFill);
    REQUIRE(head != nullptr);
    REQUIRE(head->pts.size() == 3);
    double top = head->pts[0].y;
    for (const Pos& p : head->pts) {
        top = std::max(top, p.y);
    }
    CHECK(head->pts[0].y == top);  // the head points +y
}

TEST_CASE("polyfill rides the part transform and fills, not strokes") {
    // The filled-polygon prim (the arrowhead kind): three points, no
    // repeated vertex, the placement transforms each, and the stroke
    // kind says FILL - the consumer never strokes it.
    SymbolDef s;
    s.key = "arrowish";
    DrawPrim d;
    d.kind = DrawPrim::Kind::PolyFill;
    d.pts = {Pos{0.0, 1.0}, Pos{-0.3, 0.0}, Pos{0.3, 0.0}};
    s.draw.push_back(d);

    const std::vector<SymbolStroke> out =
        render_symbol(s, placement(Pos{10.0, 10.0}, 90));
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == SymbolStroke::Kind::PolyFill);
    REQUIRE(out[0].pts.size() == 3);
    // (0,1) rot90 -> (-1,0), + (10,10)
    CHECK(pts_eq(out[0], 0, Pos{9.0, 10.0}));
    CHECK_FALSE(out[0].closed);  // fill closes; the flag never applies
}

TEST_CASE("the inductor's coil owns its pins - no stubs") {
    // The pins sit ON the outermost arcs' curves (no prim vertex
    // anywhere near them), so the stage-2 vertex-only touch rule fired
    // the safety net and drew a 1.5-unit stub straight through the
    // coil from each end. The touch rule is geometric now: a pin on a
    // circle/arc's curve (or inside a dot) is touched.
    const SymbolDef* l = ws::circsim::find_symbol("l");
    REQUIRE(l != nullptr);
    const std::vector<SymbolStroke> out =
        render_symbol(*l, placement(Pos{0.0, 0.0}));
    CHECK(out.size() == 4);  // the four arcs, and nothing else
    CHECK(count_kind(out, SymbolStroke::Kind::Arc) == 4);
    CHECK(count_kind(out, SymbolStroke::Kind::Line) == 0);  // no stubs
}

TEST_CASE("transistors wear arrowheads that carry direction") {
    // The BJT emitter arrow (stage 17f): a FILLED head on the emitter
    // stroke - the tip toward the emitter pin (NPN) vs toward the bar
    // (PNP). The stage-2 table marked the spot with a dot, which
    // carries no direction at all.
    const Pos bar_end{-1.0, -0.8};   // the emitter stroke's start
    const Pos emitter_pin{2.0, -3.0};
    const auto dist = [](Pos a, Pos b) {
        return std::sqrt((a.x - b.x) * (a.x - b.x) +
                         (a.y - b.y) * (a.y - b.y));
    };
    for (const char* key : {"npn", "pnp"}) {
        const SymbolDef* t = ws::circsim::find_symbol(key);
        REQUIRE(t != nullptr);
        const std::vector<SymbolStroke> out =
            render_symbol(*t, placement(Pos{0.0, 0.0}));
        const SymbolStroke* head =
            find_kind(out, SymbolStroke::Kind::PolyFill);
        REQUIRE(head != nullptr);
        REQUIRE(head->pts.size() == 3);
        // The tip is the vertex farthest from the base edge's
        // midpoint (the lone apex of the triangle).
        std::size_t apex = 0;
        double best = -1.0;
        for (std::size_t i = 0; i < 3; ++i) {
            const Pos& a = head->pts[(i + 1) % 3];
            const Pos& b = head->pts[(i + 2) % 3];
            const Pos mid{(a.x + b.x) * 0.5, (a.y + b.y) * 0.5};
            const double d = dist(head->pts[i], mid);
            if (d > best) {
                best = d;
                apex = i;
            }
        }
        const bool npn = key[0] == 'n';
        // NPN: the apex sits closer to the emitter pin than to the
        // bar; PNP: closer to the bar than to the pin.
        if (npn) {
            CHECK(dist(head->pts[apex], emitter_pin) <
                  dist(head->pts[apex], bar_end));
        } else {
            CHECK(dist(head->pts[apex], bar_end) <
                  dist(head->pts[apex], emitter_pin));
        }
        // The head rides the stroke: its apex lies between the ends,
        // nearer the stroke line than its own half-width.
        double to_line = 1e9;
        for (std::size_t i = 0; i < 3; ++i) {
            const Pos& p = head->pts[i];
            const Pos ab{emitter_pin.x - bar_end.x,
                         emitter_pin.y - bar_end.y};
            const Pos ap{p.x - bar_end.x, p.y - bar_end.y};
            const double proj = (ap.x * ab.x + ap.y * ab.y) /
                             (ab.x * ab.x + ab.y * ab.y);
            if (proj < 0.0 || proj > 1.0) {
                continue;
            }
            const Pos q{bar_end.x + ab.x * proj, bar_end.y + ab.y * proj};
            to_line = std::min(to_line, dist(p, q));
        }
        CHECK(to_line < 0.3);
    }
}

TEST_CASE("the mosfet bulk tees off the channel, not the source") {
    // Stage 17f: the bulk route starts at the CHANNEL BAR's bottom
    // (0,-1.6). The stage-2 route teed at (0,-1) - the source
    // stroke's own start point - so the bulk looked tied to the
    // source at every zoom. The substrate arrow sits at the top of
    // the drop: up into the channel (NMOS), down away from it (PMOS);
    // and the b pin points LEFT (dir 180), the way its lead arrives.
    for (const char* key : {"nmos", "pmos"}) {
        const SymbolDef* t = ws::circsim::find_symbol(key);
        REQUIRE(t != nullptr);
        REQUIRE(t->pins.size() == 4);
        CHECK(t->pins[3].name == "b");
        CHECK(t->pins[3].dir == 180);

        const std::vector<SymbolStroke> out =
            render_symbol(*t, placement(Pos{0.0, 0.0}));
        // The bulk route: an open poly (0,-1.6) -> (0,-3) -> (-3,-3).
        const SymbolStroke* route = nullptr;
        for (const SymbolStroke& s : out) {
            if (s.kind == SymbolStroke::Kind::Poly) {
                route = &s;
            }
        }
        REQUIRE(route != nullptr);
        REQUIRE(route->pts.size() == 3);
        CHECK(pts_eq(*route, 0, Pos{0.0, -1.6}));  // the channel bottom
        CHECK(pts_eq(*route, 1, Pos{0.0, -3.0}));
        CHECK(pts_eq(*route, 2, Pos{-3.0, -3.0}));
        CHECK_FALSE(route->closed);

        // The substrate arrow at the top of the drop.
        const SymbolStroke* arrow =
            find_kind(out, SymbolStroke::Kind::PolyFill);
        REQUIRE(arrow != nullptr);
        REQUIRE(arrow->pts.size() == 3);
        double top = arrow->pts[0].y;
        std::size_t apex = 0;
        for (std::size_t i = 0; i < 3; ++i) {
            if (arrow->pts[i].y > top) {
                top = arrow->pts[i].y;
                apex = i;
            }
        }
        const bool nmos = key[0] == 'n';
        // NMOS: the apex is the TOP vertex (the arrow points up, into
        // the channel); PMOS: the apex is the BOTTOM one.
        if (nmos) {
            CHECK(arrow->pts[apex].y == top);
            CHECK(top < -1.5);  // just under the channel bottom
        } else {
            double bottom = arrow->pts[0].y;
            for (const Pos& p : arrow->pts) {
                bottom = std::min(bottom, p.y);
            }
            CHECK(arrow->pts[apex].y > bottom);  // apex is not the low point
            CHECK(bottom > -2.2);  // the head sits at the top of the drop
        }
    }
}

TEST_CASE("polylines render open; closed shapes close by construction") {
    // symbols.hpp's contract: Poly is an OPEN polyline. A shape that
    // means to close repeats its first vertex (the diode's triangle) -
    // the stroke's closed flag stays false and the shape is still
    // whole. The stage-2 renderer closed every poly unconditionally,
    // hanging a phantom segment off the resistor's zigzag from (3,0)
    // back to (-3,0); this case is the fix's pin.
    const SymbolDef* r = ws::circsim::find_symbol("r");
    REQUIRE(r != nullptr);
    const std::vector<SymbolStroke> rz =
        render_symbol(*r, placement(Pos{0.0, 0.0}));
    const SymbolStroke* poly = find_kind(rz, SymbolStroke::Kind::Poly);
    REQUIRE(poly != nullptr);
    // The even-slope zigzag since 50bffe3: 10 points, not the old 7.
    REQUIRE(poly->pts.size() == 10);
    CHECK_FALSE(poly->closed);
    // And the open path is exactly the table's zigzag - no phantom
    // closing segment, no reordered teeth.
    CHECK(pts_eq(*poly, 0, Pos{-3.0, 0.0}));
    CHECK(pts_eq(*poly, 1, Pos{-2.25, 0.0}));
    CHECK(pts_eq(*poly, 5, Pos{0.375, -0.7}));
    CHECK(pts_eq(*poly, 9, Pos{3.0, 0.0}));

    const SymbolDef* d = ws::circsim::find_symbol("d");
    REQUIRE(d != nullptr);
    const std::vector<SymbolStroke> dz =
        render_symbol(*d, placement(Pos{0.0, 0.0}));
    poly = find_kind(dz, SymbolStroke::Kind::Poly);
    REQUIRE(poly != nullptr);
    REQUIRE(poly->pts.size() == 4);
    CHECK(poly->pts.front().x == poly->pts.back().x);
    CHECK(poly->pts.front().y == poly->pts.back().y);
    CHECK_FALSE(poly->closed);  // closed by construction, not by flag
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
