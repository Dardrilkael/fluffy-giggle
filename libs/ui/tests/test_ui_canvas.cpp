// The stage-14 canvas suite, part 1: the pure tessellation half. The
// cases pin the emission contracts the header names - quad counts per
// family, the winding orders, the fringe alpha ramps - plus the
// flattening tolerance behavior, the dash walk and the fill's
// ear-clip/AA geometry. All headless: a TessBuffer is the whole raster
// backend these tests need.
//
// (test_ui_shell.cpp is part 2; both link the same gui archive + the
// r2d stub, the test_ui_gui precedent.)

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/ui/canvas/tessellation.hpp>

#include <cmath>
#include <vector>

using namespace ws::ui::canvas;

namespace {

// TessVertex has no operator== - the suite's near-enough check.
bool vnear(const TessVertex& v, float x, float y, float eps = 1e-3f) {
    return std::fabs(v.x - x) <= eps && std::fabs(v.y - y) <= eps;
}

// Finds the first quad whose first corner is near (x, y).
int find_quad(const TessBuffer& buf, float x, float y) {
    for (std::size_t q = 0; q < buf.quad_count(); ++q) {
        if (vnear(buf.quads[q * 4], x, y)) {
            return static_cast<int>(q);
        }
    }
    return -1;
}

// True when some quad in the buffer has a corner exactly-ish at (x, y).
bool has_corner(const TessBuffer& buf, float x, float y) {
    for (const TessVertex& v : buf.quads) {
        if (vnear(v, x, y)) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("stroke: one segment, butt caps - counts and winding") {
    Paint paint;
    paint.width = 4.0f;  // half 2, core 1.5, fringe out to 2.5
    paint.cap = StrokeCap::butt;
    const Point line[] = {{0.0f, 0.0f}, {10.0f, 0.0f}};

    TessBuffer buf;
    stroke_polyline(line, 2, paint, buf);

    // 1 core quad + 2 longitudinal fringe bands (one per side) + 2 butt
    // cap bands (one per end) = 5.
    REQUIRE(buf.quad_count() == 5);

    // The core quad's winding contract: (A + n*hi, A - n*hi, B - n*hi,
    // B + n*hi), n = (-dy, dx) = (0, 1).
    const int core = find_quad(buf, 0.0f, 1.5f);
    REQUIRE(core >= 0);
    CHECK(vnear(buf.quads[core * 4 + 0], 0.0f, 1.5f));
    CHECK(vnear(buf.quads[core * 4 + 1], 0.0f, -1.5f));
    CHECK(vnear(buf.quads[core * 4 + 2], 10.0f, -1.5f));
    CHECK(vnear(buf.quads[core * 4 + 3], 10.0f, 1.5f));

    // The right-side fringe band: (inner-from, inner-to, outer-to,
    // outer-from) = walking +n.
    const int right = find_quad(buf, 0.0f, 1.5f + 0.0f);
    REQUIRE(right >= 0);
    // (it may BE the core's slot; find the band by its outer corner)
    int band = -1;
    for (std::size_t q = 0; q < buf.quad_count(); ++q) {
        if (vnear(buf.quads[q * 4], 0.0f, 1.5f) &&
            vnear(buf.quads[q * 4 + 1], 10.0f, 1.5f) &&
            vnear(buf.quads[q * 4 + 2], 10.0f, 2.5f)) {
            band = static_cast<int>(q);
            break;
        }
    }
    REQUIRE(band >= 0);
    CHECK(vnear(buf.quads[band * 4 + 3], 0.0f, 2.5f));
    // The alpha ramp: inner corners opaque, outer corners transparent.
    CHECK(buf.quads[band * 4 + 0].a == doctest::Approx(1.0f));
    CHECK(buf.quads[band * 4 + 1].a == doctest::Approx(1.0f));
    CHECK(buf.quads[band * 4 + 2].a == doctest::Approx(0.0f));
    CHECK(buf.quads[band * 4 + 3].a == doctest::Approx(0.0f));

    // The butt cap bands: one at each end, spanning +hi..-hi at the
    // inner edge and +ho..-ho at the outer.
    CHECK(has_corner(buf, 0.0f, -2.5f));
    CHECK(has_corner(buf, 10.0f, -2.5f));
}

TEST_CASE("stroke: miter join fills the corner and the fringe mitres") {
    Paint paint;
    paint.width = 4.0f;  // hi 1.5, ho 2.5
    paint.join = StrokeJoin::miter;
    paint.cap = StrokeCap::butt;
    const Point ell[] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f}};

    TessBuffer buf;
    stroke_polyline(ell, 3, paint, buf);

    // Core: 2 segment quads + the joint's base wedge (A, P, B) + the
    // miter wedge (A, M, B) = 4. Fringes: 4 longitudinal bands + 2
    // miter bands = 6. Caps: 2. Total 12.
    REQUIRE(buf.quad_count() == 12);

    // The joint sits at (10, 0); the turn east -> south is clockwise,
    // so the OUTER side is -n (north). The miter point at radius 1 is
    // (11, -1) (offset lines y = -1 and x = 11), so at the core radius
    // 1.5 it is (11.5, -1.5) and at the fringe radius 2.5 it is
    // (12.5, -2.5) - the fringe band must share those corners.
    CHECK(has_corner(buf, 11.5f, -1.5f));
    CHECK(has_corner(buf, 12.5f, -2.5f));

    // The base wedge triangle: (A, P, B) with the third corner
    // duplicated - A = P - n_east*hi = (10, -1.5), P = (10, 0),
    // B = P - n_south*hi = (11.5, 0).
    const int wedge = find_quad(buf, 10.0f, -1.5f);
    REQUIRE(wedge >= 0);
    bool found_base = false;
    for (std::size_t q = 0; q < buf.quad_count(); ++q) {
        if (vnear(buf.quads[q * 4], 10.0f, -1.5f) &&
            vnear(buf.quads[q * 4 + 1], 10.0f, 0.0f) &&
            vnear(buf.quads[q * 4 + 2], 11.5f, 0.0f) &&
            vnear(buf.quads[q * 4 + 3], 11.5f, 0.0f)) {
            found_base = true;
        }
    }
    CHECK(found_base);
}

TEST_CASE("stroke: bevel join, round join, round caps - the counts") {
    const Point ell[] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f}};

    SUBCASE("bevel: base wedge only, one bevel band") {
        Paint paint;
        paint.width = 4.0f;
        paint.join = StrokeJoin::bevel;
        TessBuffer buf;
        stroke_polyline(ell, 3, paint, buf);
        // 3 core (2 segments + base wedge) + 5 fringe (4 longitudinal +
        // 1 bevel band) + 2 caps = 10.
        CHECK(buf.quad_count() == 10);
    }

    SUBCASE("round join: fan replaces the wedges") {
        Paint paint;
        paint.width = 4.0f;
        paint.join = StrokeJoin::round;
        TessBuffer buf;
        stroke_polyline(ell, 3, paint, buf);
        // Core: 2 segments + fan. The fan spans 90 degrees at radius
        // 1.5: step = 2*acos(1 - 0.25/1.5) ~ 1.171 rad -> 2 steps.
        // Fringe: 4 longitudinal + 2 arc bands (same 2-step split).
        // Caps: 2. Total = 4 + 6 + 2 = 12.
        CHECK(buf.quad_count() == 12);
    }

    SUBCASE("round caps: the half-disc bands at both ends") {
        Paint paint;
        paint.width = 4.0f;
        paint.join = StrokeJoin::miter;
        paint.cap = StrokeCap::round;
        TessBuffer buf;
        stroke_polyline(ell, 3, paint, buf);
        // Same as the miter case (12) but each butt cap band (1) became
        // a half-disc band of 4 quads (span pi at radius 2.5:
        // step ~ 0.902 rad -> 4 steps): +3 per cap, +6 total.
        CHECK(buf.quad_count() == 18);
    }

    SUBCASE("square caps: three band quads per end") {
        Paint paint;
        paint.width = 4.0f;
        paint.join = StrokeJoin::miter;
        paint.cap = StrokeCap::square;
        TessBuffer buf;
        stroke_polyline(ell, 3, paint, buf);
        // Each butt band (1) became 3 (two sides + the far edge): +2
        // per cap, +4 total.
        CHECK(buf.quad_count() == 16);
    }
}

TEST_CASE("stroke: hairlines, degenerates and the miter limit") {
    SUBCASE("a hairline is two alpha-ramped bands, no core") {
        Paint paint;
        paint.width = 0.5f;  // half 0.25 < 0.5: no core
        paint.cap = StrokeCap::butt;
        const Point line[] = {{0.0f, 0.0f}, {10.0f, 0.0f}};
        TessBuffer buf;
        stroke_polyline(line, 2, paint, buf);
        // 2 longitudinal bands (inner corners ON the center line) + 2
        // degenerate cap bands = 4.
        REQUIRE(buf.quad_count() == 4);
        // The inner alpha is the width (the coverage-honest peak).
        CHECK(buf.quads[0].a == doctest::Approx(0.5f));
        // The inner corners sit at y = 0 (radius 0 = the center line).
        CHECK(vnear(buf.quads[0], 0.0f, 0.0f));
    }

    SUBCASE("a single point emits cap geometry only") {
        Paint paint;
        paint.width = 4.0f;
        paint.cap = StrokeCap::round;
        const Point dot[] = {{5.0f, 5.0f}};
        TessBuffer buf;
        stroke_polyline(dot, 1, paint, buf);
        // Two half-disc bands (4 quads each) around the point.
        REQUIRE(buf.quad_count() == 8);
        CHECK(has_corner(buf, 5.0f, 5.0f));
        CHECK(has_corner(buf, 5.0f, 5.0f - 2.5f));
        CHECK(has_corner(buf, 5.0f, 5.0f + 2.5f));
    }

    SUBCASE("a single point with a butt cap emits nothing") {
        Paint paint;
        paint.width = 4.0f;
        paint.cap = StrokeCap::butt;
        const Point dot[] = {{5.0f, 5.0f}};
        TessBuffer buf;
        stroke_polyline(dot, 1, paint, buf);
        CHECK(buf.quad_count() == 0);
    }

    SUBCASE("zero-length segments are skipped, neighbors jointed") {
        Paint paint;
        paint.width = 4.0f;
        paint.join = StrokeJoin::miter;
        const Point zig[] = {{0.0f, 0.0f}, {0.0f, 0.0f},
                             {10.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f}};
        TessBuffer buf;
        stroke_polyline(zig, 5, paint, buf);
        // Same geometry as the plain L: 12 quads.
        CHECK(buf.quad_count() == 12);
    }

    SUBCASE("width zero and empty input emit nothing") {
        Paint paint;
        const Point line[] = {{0.0f, 0.0f}, {10.0f, 0.0f}};
        TessBuffer buf;
        paint.width = 0.0f;
        stroke_polyline(line, 2, paint, buf);
        stroke_polyline(nullptr, 0, paint, buf);
        CHECK(buf.quad_count() == 0);
    }

    SUBCASE("the miter limit falls back to bevel") {
        Paint paint;
        paint.width = 4.0f;
        paint.join = StrokeJoin::miter;
        paint.miter_limit = 0.1f;  // anything spikier than a bevel
        const Point sharp[] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.5f, 0.5f}};
        TessBuffer buf;
        stroke_polyline(sharp, 3, paint, buf);
        // The 87-degree turn's miter ratio ~0.727 exceeds 0.1: the join
        // is a bevel (base wedge only, one bevel band).
        // Core: 2 segments + 1 wedge; fringe: 4 bands + 1 bevel band;
        // caps: 2. Total 10 - the bevel count, not the miter 12.
        CHECK(buf.quad_count() == 10);
    }
}

TEST_CASE("stroke: closed paths joint the loop and cap nothing") {
    Paint paint;
    paint.width = 4.0f;
    paint.join = StrokeJoin::miter;
    paint.cap = StrokeCap::round;  // would add caps if the path were open

    // A square that already repeats its first point.
    const Point sq[] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f},
                        {0.0f, 10.0f}, {0.0f, 0.0f}};
    TessBuffer buf;
    stroke_polyline(sq, 5, paint, buf, true);
    // 4 segments. Core: 4 quads + 4 joints x (base + miter) = 12.
    // Fringe: 8 longitudinal + 4 joints x 2 miter bands = 16. No caps.
    // Total 28.
    REQUIRE(buf.quad_count() == 28);

    // The same square WITHOUT the repeated closing point: the stroker
    // adds the synthetic closing segment itself.
    TessBuffer buf2;
    stroke_polyline(sq, 4, paint, buf2, true);
    CHECK(buf2.quad_count() == 28);
}

TEST_CASE("flattening: tolerance drives the segment count") {
    SUBCASE("a quadratic with a collinear handle appends just the end") {
        std::vector<Point> out;
        flatten_quadratic({0.0f, 0.0f}, {50.0f, 0.0f}, {100.0f, 0.0f},
                          0.5f, out);
        REQUIRE(out.size() == 1);
        CHECK(vnear({out[0].x, out[0].y}, 100.0f, 0.0f));
    }

    SUBCASE("a curved quadratic flattens finer as tolerance shrinks") {
        std::vector<Point> coarse;
        flatten_quadratic({0.0f, 0.0f}, {50.0f, 50.0f}, {100.0f, 0.0f},
                          2.0f, coarse);
        std::vector<Point> fine;
        flatten_quadratic({0.0f, 0.0f}, {50.0f, 50.0f}, {100.0f, 0.0f},
                          0.1f, fine);
        CHECK(coarse.size() >= 2);
        CHECK(fine.size() > coarse.size());
        // Both end exactly at p2.
        CHECK(vnear({coarse.back().x, coarse.back().y}, 100.0f, 0.0f));
        CHECK(vnear({fine.back().x, fine.back().y}, 100.0f, 0.0f));
    }

    SUBCASE("a cubic mirrors the quadratic's behavior") {
        std::vector<Point> out;
        flatten_cubic({0.0f, 0.0f}, {0.0f, 100.0f}, {100.0f, 100.0f},
                      {100.0f, 0.0f}, 0.5f, out);
        CHECK(out.size() >= 4);
        CHECK(vnear({out.back().x, out.back().y}, 100.0f, 0.0f));
        // Every point stays inside the convex hull (the 100x100 box).
        for (const Point& p : out) {
            CHECK(p.x >= -1e-3f);
            CHECK(p.x <= 100.0f + 1e-3f);
            CHECK(p.y >= -1e-3f);
            CHECK(p.y <= 100.0f + 1e-3f);
        }
    }

    SUBCASE("an arc walks the circle at the tolerance's step") {
        std::vector<Point> out;
        flatten_arc({0.0f, 0.0f}, 10.0f, 10.0f, 0.0f, kTwoPi, 0.25f, out);
        // step = 2*acos(1 - 0.25/10) ~ 0.4475 rad -> ceil(2pi/0.4475)
        // = 15 points; the closing point duplicates the start.
        REQUIRE(out.size() == 15);
        for (const Point& p : out) {
            const float r = std::sqrt(p.x * p.x + p.y * p.y);
            CHECK(r == doctest::Approx(10.0f).epsilon(0.01));
        }
    }

    SUBCASE("an ellipse arc respects the radii and the span") {
        std::vector<Point> out;
        flatten_arc({5.0f, 5.0f}, 20.0f, 10.0f, 0.0f, kHalfPi, 0.5f, out);
        REQUIRE(!out.empty());
        // The end sits at angle pi/2: (5 + 20*0, 5 + 10*1) = (5, 15).
        // (y grows down, so +pi/2 sweeps to south = +y.)
        CHECK(vnear({out.back().x, out.back().y}, 5.0f, 15.0f));
        for (const Point& p : out) {
            const float nx = (p.x - 5.0f) / 20.0f;
            const float ny = (p.y - 5.0f) / 10.0f;
            CHECK(nx * nx + ny * ny == doctest::Approx(1.0f).epsilon(0.01));
        }
    }

    SUBCASE("a negative span sweeps backward") {
        std::vector<Point> out;
        flatten_arc({0.0f, 0.0f}, 10.0f, 10.0f, 0.0f, -kHalfPi, 0.5f, out);
        REQUIRE(!out.empty());
        // The end sits at angle -pi/2: (0, -10).
        CHECK(vnear({out.back().x, out.back().y}, 0.0f, -10.0f));
    }

    SUBCASE("degenerate radii and spans append nothing") {
        std::vector<Point> out;
        flatten_arc({0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, 1.0f, 0.5f, out);
        flatten_arc({0.0f, 0.0f}, 10.0f, 10.0f, 0.5f, 0.5f, 0.5f, out);
        CHECK(out.empty());
    }
}

TEST_CASE("dash: the pattern walk") {
    SUBCASE("an even pattern splits a line into runs") {
        const Point line[] = {{0.0f, 0.0f}, {30.0f, 0.0f}};
        DashSpec dash;
        dash.lengths[0] = 10.0f;
        dash.lengths[1] = 5.0f;
        dash.count = 2;

        std::vector<std::vector<Point>> runs;
        dash_polyline(line, 2, dash, runs);
        REQUIRE(runs.size() == 2);
        REQUIRE(runs[0].size() == 2);
        CHECK(vnear({runs[0][0].x, runs[0][0].y}, 0.0f, 0.0f));
        CHECK(vnear({runs[0][1].x, runs[0][1].y}, 10.0f, 0.0f));
        REQUIRE(runs[1].size() == 2);
        CHECK(vnear({runs[1][0].x, runs[1][0].y}, 15.0f, 0.0f));
        CHECK(vnear({runs[1][1].x, runs[1][1].y}, 25.0f, 0.0f));
    }

    SUBCASE("an offset phases into the pattern") {
        // 25px line (not 30): the offset shifts the pattern so the line
        // ends mid-off-phase, keeping the run count at 2. A 30px line
        // would end mid-on and produce a third run [25, 30].
        const Point line[] = {{0.0f, 0.0f}, {25.0f, 0.0f}};
        DashSpec dash;
        dash.lengths[0] = 10.0f;
        dash.lengths[1] = 5.0f;
        dash.count = 2;
        dash.offset = 5.0f;  // 5 into the ON phase: 5 left of it

        std::vector<std::vector<Point>> runs;
        dash_polyline(line, 2, dash, runs);
        REQUIRE(runs.size() == 2);
        CHECK(vnear({runs[0][0].x, runs[0][0].y}, 0.0f, 0.0f));
        CHECK(vnear({runs[0][1].x, runs[0][1].y}, 5.0f, 0.0f));
        CHECK(vnear({runs[1][0].x, runs[1][0].y}, 10.0f, 0.0f));
        CHECK(vnear({runs[1][1].x, runs[1][1].y}, 20.0f, 0.0f));
    }

    SUBCASE("a negative offset wraps") {
        const Point line[] = {{0.0f, 0.0f}, {25.0f, 0.0f}};
        DashSpec dash;
        dash.lengths[0] = 10.0f;
        dash.lengths[1] = 5.0f;
        dash.count = 2;
        dash.offset = -25.0f;  // == +5 (mod 15)

        std::vector<std::vector<Point>> runs;
        dash_polyline(line, 2, dash, runs);
        REQUIRE(runs.size() == 2);
        CHECK(vnear({runs[0][1].x, runs[0][1].y}, 5.0f, 0.0f));
    }

    SUBCASE("an odd count doubles, per the SVG rule") {
        const Point line[] = {{0.0f, 0.0f}, {30.0f, 0.0f}};
        DashSpec dash;
        dash.lengths[0] = 10.0f;
        dash.count = 1;  // effective 10 on, 10 off

        std::vector<std::vector<Point>> runs;
        dash_polyline(line, 2, dash, runs);
        REQUIRE(runs.size() == 2);
        CHECK(vnear({runs[1][0].x, runs[1][0].y}, 20.0f, 0.0f));
    }

    SUBCASE("a zero ON length makes one-point dot runs") {
        const Point line[] = {{0.0f, 0.0f}, {10.0f, 0.0f}};
        DashSpec dash;
        dash.lengths[0] = 0.0f;  // hmm: zero entries make the pattern
        dash.lengths[1] = 5.0f;  // unusable per the header - use 0.001
        dash.count = 2;
        // Per the contract a non-positive entry reads as solid:
        std::vector<std::vector<Point>> runs;
        dash_polyline(line, 2, dash, runs);
        REQUIRE(runs.size() == 1);
        REQUIRE(runs[0].size() == 2);

        // The honest dot spelling: a tiny ON (0.001) with round caps.
        // A 0.001 on phase is NOT zero-length - it produces a 2-point
        // run (the start and 0.001 later). The stroker's round caps
        // turn that near-zero segment into a dot.
        DashSpec dotted;
        dotted.lengths[0] = 0.001f;
        dotted.lengths[1] = 5.0f;
        dotted.count = 2;
        std::vector<std::vector<Point>> dots;
        dash_polyline(line, 2, dotted, dots);
        REQUIRE(dots.size() == 2);
        REQUIRE(dots[0].size() == 2);  // the near-zero on phase
        CHECK(vnear({dots[0][0].x, dots[0][0].y}, 0.0f, 0.0f));
        CHECK(vnear({dots[0][1].x, dots[0][1].y}, 0.001f, 0.0f));
    }

    SUBCASE("solid patterns emit the whole input as one run") {
        const Point line[] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 8.0f}};
        std::vector<std::vector<Point>> runs;
        dash_polyline(line, 3, DashSpec{}, runs);
        REQUIRE(runs.size() == 1);
        REQUIRE(runs[0].size() == 3);
    }
}

TEST_CASE("fill: the ear-clipped core and the AA fringe") {
    SUBCASE("a square: two triangles plus four fringe bands") {
        const Point sq[] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f},
                            {0.0f, 10.0f}};
        TessBuffer buf;
        fill_polygon(sq, 4, 0xFF00FF00u, buf);
        REQUIRE(buf.quad_count() == 6);

        // The core triangles are full alpha, the fringe ramps to 0. The
        // shrunk square's corners: 0.5px in along the bisector means
        // (0.5, 0.5) at each corner (the move is 0.5/cos(45) = 0.707
        // along the diagonal = 0.5 on each axis).
        CHECK(has_corner(buf, 0.5f, 0.5f));
        CHECK(has_corner(buf, 9.5f, 9.5f));
        // The fringe's outer corners sit at the VERTICES, 0.5px OUTSIDE
        // the true edges: (0, -0.5) at the bottom-left corner's outward,
        // (10.5, 0) at the top-right corner's outward. (Edge midpoints
        // like (5, -0.5) are ON the fringe quad but not a vertex.)
        CHECK(has_corner(buf, 0.0f, -0.5f));
        CHECK(has_corner(buf, 10.5f, 0.0f));
    }

    SUBCASE("winding either way fills the same") {
        const Point cw[] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f},
                            {0.0f, 10.0f}};
        const Point ccw[] = {{0.0f, 0.0f}, {0.0f, 10.0f}, {10.0f, 10.0f},
                             {10.0f, 0.0f}};
        TessBuffer a;
        TessBuffer b;
        fill_polygon(cw, 4, 0xFFFFFFFFu, a);
        fill_polygon(ccw, 4, 0xFFFFFFFFu, b);
        CHECK(a.quad_count() == b.quad_count());
    }

    SUBCASE("a concave L fills more triangles than a square") {
        const Point ell[] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f},
                             {6.0f, 10.0f}, {6.0f, 4.0f}, {0.0f, 4.0f}};
        TessBuffer buf;
        fill_polygon(ell, 6, 0xFFFFFFFFu, buf);
        // 6 vertices -> 4 core triangles + 6 fringe bands = 10.
        CHECK(buf.quad_count() == 10);
    }

    SUBCASE("collinear vertices are dropped by the clipper") {
        const Point box[] = {{0.0f, 0.0f}, {5.0f, 0.0f}, {10.0f, 0.0f},
                             {10.0f, 10.0f}, {5.0f, 10.0f}, {0.0f, 10.0f}};
        TessBuffer buf;
        fill_polygon(box, 6, 0xFFFFFFFFu, buf);
        // Same as the plain square: 2 triangles + 4 fringes.
        CHECK(buf.quad_count() == 6);
    }

    SUBCASE("degenerates fill nothing") {
        TessBuffer buf;
        const Point pair[] = {{0.0f, 0.0f}, {10.0f, 0.0f}};
        fill_polygon(pair, 2, 0xFFFFFFFFu, buf);
        fill_polygon(nullptr, 0, 0xFFFFFFFFu, buf);
        const Point line3[] = {{0.0f, 0.0f}, {5.0f, 0.0f}, {10.0f, 0.0f}};
        fill_polygon(line3, 3, 0xFFFFFFFFu, buf);
        CHECK(buf.quad_count() == 0);
    }
}
