// The stage-14 canvas suite, part 1: the pure tessellation half. The
// cases pin the emission contracts the header names - quad counts per
// family, the winding orders, the fringe alpha ramps - plus the
// flattening tolerance behavior, the dash walk and the fill's
// ear-clip/AA geometry. All headless: a TessBuffer is the whole raster
// backend these tests need.
//
// Part 2 (below) exercises the Canvas2D interface via CommandListCanvas:
// transform stack, path API, convenience wrappers, text/image recording.
//
// (test_ui_shell.cpp is part 3; both link the same gui archive + the
// r2d stub, the test_ui_gui precedent.)

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/ui/canvas/tessellation.hpp>
#include <ws/ui/canvas/canvas2d.hpp>
#include <ws/ui/canvas/view.hpp>

#include "ui_gui_stub.hpp"  // the r2d stub's counters (Renderer2DCanvas cases)

#include <cmath>
#include <vector>

using namespace ws::ui::canvas;
using ws::ui::r2d::Font;
using ws::ui::r2d::Texture;

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

TEST_CASE("dash: boundaries at interior vertices keep their runs") {
    // A pattern whose phase boundaries land exactly on the polyline's
    // interior vertices: every ON phase must still open a run there.
    // The walk used to consume the phase with run == nullptr whenever
    // the boundary sat exactly on an interior vertex (its run-opening
    // guard only fired with segment length remaining), silently
    // dropping that dash - a [4,4] pattern on a 16-unit-segment square
    // lost half its dashes.
    const Point sq[] = {{0.0f, 0.0f}, {16.0f, 0.0f},
                        {16.0f, 16.0f}, {0.0f, 16.0f}};
    DashSpec dash;
    dash.lengths[0] = 4.0f;
    dash.lengths[1] = 4.0f;
    dash.count = 2;

    std::vector<std::vector<Point>> runs;
    dash_polyline(sq, 4, dash, runs);
    // 3 segments x 16 units, period 8: two dashes per segment.
    REQUIRE(runs.size() == 6);
    for (const auto& run : runs) {
        REQUIRE(run.size() == 2);
        const float dx = run[1].x - run[0].x;
        const float dy = run[1].y - run[0].y;
        CHECK(std::sqrt(dx * dx + dy * dy) ==
              doctest::Approx(4.0f).epsilon(0.01));
    }

    // The interior-vertex runs open AT the vertex.
    CHECK(vnear({runs[2][0].x, runs[2][0].y}, 16.0f, 0.0f));
    CHECK(vnear({runs[4][0].x, runs[4][0].y}, 16.0f, 16.0f));

    // The path's own end still opens nothing (the phantom endpoint
    // contract the [10,5]-on-30 case pins above): a boundary landing
    // exactly at the LAST point stays run-less.
    const Point line[] = {{0.0f, 0.0f}, {30.0f, 0.0f}};
    DashSpec t;
    t.lengths[0] = 10.0f;
    t.lengths[1] = 5.0f;
    t.count = 2;
    std::vector<std::vector<Point>> two;
    dash_polyline(line, 2, t, two);
    CHECK(two.size() == 2);
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

// =============================================================================
// Part 2: the Canvas2D interface (CommandListCanvas recording)
// =============================================================================

TEST_CASE("canvas2d: transform stack maps local to screen") {
    CommandListCanvas c;

    SUBCASE("translate shifts points") {
        c.translate(5.0f, 10.0f);
        c.stroke_line({0.0f, 0.0f}, {10.0f, 0.0f}, Paint{});
        REQUIRE(c.cmds.size() == 1);
        CHECK(c.cmds[0].points[0].x == doctest::Approx(5.0f));
        CHECK(c.cmds[0].points[0].y == doctest::Approx(10.0f));
        CHECK(c.cmds[0].points[1].x == doctest::Approx(15.0f));
        CHECK(c.cmds[0].points[1].y == doctest::Approx(10.0f));
    }

    SUBCASE("scale multiplies coordinates") {
        c.scale(2.0f, 3.0f);
        c.stroke_line({1.0f, 1.0f}, {5.0f, 5.0f}, Paint{});
        REQUIRE(c.cmds.size() == 1);
        CHECK(c.cmds[0].points[0].x == doctest::Approx(2.0f));
        CHECK(c.cmds[0].points[0].y == doctest::Approx(3.0f));
        CHECK(c.cmds[0].points[1].x == doctest::Approx(10.0f));
        CHECK(c.cmds[0].points[1].y == doctest::Approx(15.0f));
    }

    SUBCASE("push/pop restores the transform") {
        c.translate(5.0f, 5.0f);
        c.push_transform();
        c.translate(100.0f, 100.0f);
        c.stroke_line({0.0f, 0.0f}, {1.0f, 0.0f}, Paint{});
        c.pop_transform();
        c.stroke_line({0.0f, 0.0f}, {1.0f, 0.0f}, Paint{});
        REQUIRE(c.cmds.size() == 2);
        // The first line (under both translates) is at (105, 105).
        CHECK(c.cmds[0].points[0].x == doctest::Approx(105.0f));
        // The second line (popped back to the first translate) is at (5, 5).
        CHECK(c.cmds[1].points[0].x == doctest::Approx(5.0f));
    }

    SUBCASE("view_scale tracks the transform scale") {
        CHECK(c.view_scale() == doctest::Approx(1.0f));
        c.scale(2.0f, 2.0f);
        CHECK(c.view_scale() == doctest::Approx(2.0f));
        c.push_transform();
        c.scale(3.0f, 3.0f);
        CHECK(c.view_scale() == doctest::Approx(6.0f));
        c.pop_transform();
        CHECK(c.view_scale() == doctest::Approx(2.0f));
    }
}

TEST_CASE("canvas2d: convenience wrappers record the right commands") {
    CommandListCanvas c;

    SUBCASE("stroke_line records one stroke_polyline") {
        c.stroke_line({0.0f, 0.0f}, {10.0f, 0.0f}, Paint{});
        CHECK(c.count(CanvasCmdKind::stroke_polyline) == 1);
        CHECK(c.cmds[0].points.size() == 2);
    }

    SUBCASE("fill_polygon records one fill_polygon") {
        const Point tri[] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {5.0f, 10.0f}};
        c.fill_polygon(tri, 3, 0xFF0000FFu);
        CHECK(c.count(CanvasCmdKind::fill_polygon) == 1);
        CHECK(c.cmds[0].color == 0xFF0000FFu);
        CHECK(c.cmds[0].points.size() == 4);  // 3 + close
    }

    SUBCASE("stroke_circle records a 32-segment closed polyline") {
        c.stroke_circle({0.0f, 0.0f}, 5.0f, Paint{});
        CHECK(c.count(CanvasCmdKind::stroke_polyline) == 1);
        CHECK(c.cmds[0].points.size() == 32);
        CHECK(c.cmds[0].closed);  // the tessellation joints the loop
    }

    SUBCASE("stroke_polyline records its closed flag") {
        // The flag decides whether the PAINT joints the path back to
        // its first point (a tessellation-time property; the recorded
        // points never carry the closing segment). The editor's symbol
        // path rides it: an open polyline must stay open - the stage-2
        // renderer once closed every poly and the resistor's zigzag
        // wore a phantom straight segment.
        const Point tri[] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {5.0f, 10.0f}};
        c.stroke_polyline(tri, 3, Paint{});
        CHECK(c.count(CanvasCmdKind::stroke_polyline) == 1);
        CHECK(c.cmds[0].points.size() == 3);
        CHECK_FALSE(c.cmds[0].closed);
        c.cmds.clear();
        c.stroke_polyline(tri, 3, Paint{}, true);
        CHECK(c.count(CanvasCmdKind::stroke_polyline) == 1);
        CHECK(c.cmds[0].points.size() == 3);  // the close is not a point
        CHECK(c.cmds[0].closed);
    }

    SUBCASE("fill_circle records a 32-point polygon") {
        c.fill_circle({0.0f, 0.0f}, 5.0f, 0xFFFFFFFFu);
        CHECK(c.count(CanvasCmdKind::fill_polygon) == 1);
        CHECK(c.cmds[0].points.size() == 33);  // 32 + close
    }

    SUBCASE("clip push/pop records commands") {
        c.push_clip(0.0f, 0.0f, 100.0f, 100.0f);
        c.pop_clip();
        CHECK(c.count(CanvasCmdKind::push_clip) == 1);
        CHECK(c.count(CanvasCmdKind::pop_clip) == 1);
    }
}

TEST_CASE("canvas2d: path API accumulates and strokes") {
    CommandListCanvas c;

    SUBCASE("a path with quadratic_to flattens") {
        c.begin_path();
        c.move_to({0.0f, 0.0f});
        c.quadratic_to({50.0f, 50.0f}, {100.0f, 0.0f});
        c.stroke(Paint{});
        // A curved quadratic flattens to multiple points; the dash
        // walk (solid pattern) emits one run.
        CHECK(c.count(CanvasCmdKind::stroke_polyline) == 1);
        CHECK(c.cmds[0].points.size() > 2);  // flattened
        // The last point is the curve's end.
        const auto& last = c.cmds[0].points.back();
        CHECK(last.x == doctest::Approx(100.0f));
        CHECK(last.y == doctest::Approx(0.0f));
    }

    SUBCASE("a path with cubic_to flattens") {
        c.begin_path();
        c.move_to({0.0f, 0.0f});
        c.cubic_to({0.0f, 100.0f}, {100.0f, 100.0f}, {100.0f, 0.0f});
        c.stroke(Paint{});
        CHECK(c.count(CanvasCmdKind::stroke_polyline) == 1);
        CHECK(c.cmds[0].points.size() > 3);  // flattened
    }

    SUBCASE("close_path joints back to the start") {
        c.begin_path();
        c.move_to({0.0f, 0.0f});
        c.line_to({10.0f, 0.0f});
        c.line_to({10.0f, 10.0f});
        c.close_path();
        c.stroke(Paint{});
        // The closed path's last point is the first move_to.
        const auto& pts = c.cmds[0].points;
        CHECK(pts.back().x == doctest::Approx(0.0f));
        CHECK(pts.back().y == doctest::Approx(0.0f));
    }

    SUBCASE("close_path closes to the ring's start, not the last move_to"
            " (the rounded-rect top-edge artifact)") {
        // The rounded rects build one ring out of four move_to + arc
        // pairs. close_path once appended the LAST move_to's target -
        // the top-left arc's start - so the ring finished through a
        // phantom chord from the top-left arc's end across to the
        // path's start, and every filled/stroked rounded rect wore a
        // diagonal smear across its top part. The close must finish
        // the ring at ITS OWN first point.
        c.begin_path();
        c.move_to({0.0f, 0.0f});    // the ring's start
        c.line_to({10.0f, 0.0f});
        c.move_to({10.0f, 10.0f});  // an interior jump, as the arcs do
        c.line_to({0.0f, 10.0f});
        c.close_path();
        c.stroke(Paint{});
        const auto& pts = c.cmds[0].points;
        CHECK(pts.back().x == doctest::Approx(0.0f));
        CHECK(pts.back().y == doctest::Approx(0.0f));
    }

    SUBCASE("fill_rounded_rect closes the ring along the top edge") {
        // fill_rounded_rect(10, 10, 40, 30, 6): the ring starts at the
        // top-right arc's start (44, 10) and the last arc ends at the
        // top-left arc's end (16, 10); the close must walk the TOP
        // edge (16, 10) -> (44, 10), ending at the ring's start.
        c.fill_rounded_rect(10.0f, 10.0f, 40.0f, 30.0f, 6.0f, 0xFF0000FFu);
        const CanvasCmd* fill =
            c.first(CanvasCmdKind::fill_polygon);
        REQUIRE(fill != nullptr);
        const auto& pts = fill->points;
        CHECK(pts.front().x == doctest::Approx(44.0f));
        CHECK(pts.front().y == doctest::Approx(10.0f));
        CHECK(pts.back().x == doctest::Approx(44.0f));
        CHECK(pts.back().y == doctest::Approx(10.0f));
        // The ring's vertices all lie in the rect's box (the phantom
        // chord's points did too - its damage was the diagonal EDGE,
        // which the ring's END above is the tell for; this loop pins
        // the arcs' flattening to the box regardless).
        for (const auto& p : pts) {
            CHECK(p.x >= 10.0f - 1e-3f);
            CHECK(p.x <= 50.0f + 1e-3f);
            CHECK(p.y >= 10.0f - 1e-3f);
            CHECK(p.y <= 40.0f + 1e-3f);
        }
    }

    SUBCASE("stroke_rounded_rect closes the ring along the top edge") {
        c.stroke_rounded_rect(10.0f, 10.0f, 40.0f, 30.0f, 6.0f, Paint{});
        const CanvasCmd* stroke_cmd = c.first(CanvasCmdKind::stroke_polyline);
        REQUIRE(stroke_cmd != nullptr);
        CHECK(stroke_cmd->closed);
        const auto& pts = stroke_cmd->points;
        CHECK(pts.front().x == doctest::Approx(44.0f));
        CHECK(pts.front().y == doctest::Approx(10.0f));
        CHECK(pts.back().x == doctest::Approx(44.0f));
        CHECK(pts.back().y == doctest::Approx(10.0f));
    }
}

TEST_CASE("canvas2d: text and image recording") {
    CommandListCanvas c;
    // A null font (never baked) measures 0 and draws nothing - the
    // canvas records the command anyway (the test asserts the stream,
    // not the raster).
    Font font;

    SUBCASE("draw_text records a draw_text command") {
        c.draw_text(10.0f, 20.0f, "hello", font, 0xFFFFFFFFu);
        CHECK(c.count(CanvasCmdKind::draw_text) == 1);
        CHECK(c.cmds[0].text == "hello");
        CHECK(c.cmds[0].x == doctest::Approx(10.0f));
        CHECK(c.cmds[0].y == doctest::Approx(20.0f));
    }

    SUBCASE("measure_text accounts for view scale") {
        c.scale(2.0f, 2.0f);
        // An empty font measures 0 regardless of scale.
        CHECK(c.measure_text("test", font) == doctest::Approx(0.0f));
    }

    SUBCASE("draw_image records a draw_image command") {
        Texture tex{42};
        c.draw_image(0.0f, 0.0f, 100.0f, 50.0f, tex, 0xFFFFFFFFu);
        CHECK(c.count(CanvasCmdKind::draw_image) == 1);
        CHECK(c.cmds[0].texture.id == 42);
        CHECK(c.cmds[0].w == doctest::Approx(100.0f));
        CHECK(c.cmds[0].h == doctest::Approx(50.0f));
    }
}

// =============================================================================
// Part 3: ViewTransform2D + adaptive grid
// =============================================================================

TEST_CASE("view_transform: world/screen round-trip") {
    ViewTransform2D v;
    v.viewport_w = 800.0f;
    v.viewport_h = 600.0f;

    SUBCASE("identity: world (0,0) maps to viewport center") {
        const Point s = v.world_to_screen({0.0f, 0.0f});
        CHECK(s.x == doctest::Approx(400.0f));
        CHECK(s.y == doctest::Approx(300.0f));
    }

    SUBCASE("round-trip at identity") {
        const Point world{123.4f, 567.8f};
        const Point screen = v.world_to_screen(world);
        const Point back = v.screen_to_world(screen);
        CHECK(back.x == doctest::Approx(world.x).epsilon(1e-4f));
        CHECK(back.y == doctest::Approx(world.y).epsilon(1e-4f));
    }

    SUBCASE("round-trip with pan and zoom") {
        v.pan = {50.0f, -30.0f};
        v.zoom = 2.5f;
        const Point world{100.0f, 200.0f};
        const Point screen = v.world_to_screen(world);
        const Point back = v.screen_to_world(screen);
        CHECK(back.x == doctest::Approx(world.x).epsilon(1e-4f));
        CHECK(back.y == doctest::Approx(world.y).epsilon(1e-4f));
    }

    SUBCASE("round-trip with rotation") {
        v.pan = {10.0f, 20.0f};
        v.zoom = 1.5f;
        v.rotation = 0.3f;
        const Point world{42.0f, -17.0f};
        const Point screen = v.world_to_screen(world);
        const Point back = v.screen_to_world(screen);
        CHECK(back.x == doctest::Approx(world.x).epsilon(1e-4f));
        CHECK(back.y == doctest::Approx(world.y).epsilon(1e-4f));
    }

    SUBCASE("zoom scales world to screen") {
        v.zoom = 2.0f;
        // A 10-unit world span = 20 screen px.
        const Point a = v.world_to_screen({0.0f, 0.0f});
        const Point b = v.world_to_screen({10.0f, 0.0f});
        CHECK((b.x - a.x) == doctest::Approx(20.0f));
    }
}

TEST_CASE("view_transform: zoom-at-point keeps the cursor's world point fixed") {
    ViewTransform2D v;
    v.viewport_w = 800.0f;
    v.viewport_h = 600.0f;
    v.pan = {0.0f, 0.0f};
    v.zoom = 1.0f;

    // The cursor sits at screen (200, 150) - offset from the viewport
    // center (400, 300) by (-200, -150).
    const Point cursor_screen{200.0f, 150.0f};
    const Point world_under_cursor = v.screen_to_world(cursor_screen);

    // Zoom in 4x toward the cursor.
    v.zoom_at(cursor_screen, 4.0f);
    CHECK(v.zoom == doctest::Approx(4.0f));

    // The world point under the cursor must NOT have moved.
    const Point world_after = v.screen_to_world(cursor_screen);
    CHECK(world_after.x == doctest::Approx(world_under_cursor.x).epsilon(1e-4f));
    CHECK(world_after.y == doctest::Approx(world_under_cursor.y).epsilon(1e-4f));

    // But the viewport center's world point DID move (the view panned
    // to keep the cursor fixed).
    const Point center_world_before = v.screen_to_world({400.0f, 300.0f});
    // (sanity: at zoom 4, the center is 200/4 = 50 world units right of
    // the cursor's world point, which was at -200 world. So center is
    // at -200 + 50 = -150. Not asserting the exact value - the invariant
    // is the cursor, not the center.)
    CHECK(center_world_before.x != doctest::Approx(0.0f));
}

TEST_CASE("view_transform: zoom limits clamp") {
    ViewTransform2D v;
    v.min_zoom = 0.1f;
    v.max_zoom = 10.0f;
    v.viewport_w = 100.0f;
    v.viewport_h = 100.0f;

    v.zoom_at({50.0f, 50.0f}, 100.0f);
    CHECK(v.zoom == doctest::Approx(10.0f));
    v.zoom_at({50.0f, 50.0f}, 0.001f);
    CHECK(v.zoom == doctest::Approx(0.1f));
}

TEST_CASE("view_transform: fit centers and scales to the rect") {
    ViewTransform2D v;
    v.viewport_w = 800.0f;
    v.viewport_h = 600.0f;
    v.min_zoom = 0.01f;
    v.max_zoom = 100.0f;

    // Fit a 400x300 world rect with no margin.
    v.fit(0.0f, 0.0f, 400.0f, 300.0f);
    // zoom = min(800/400, 600/300) = min(2, 2) = 2.
    CHECK(v.zoom == doctest::Approx(2.0f));
    // pan = center of the rect = (200, 150).
    CHECK(v.pan.x == doctest::Approx(200.0f));
    CHECK(v.pan.y == doctest::Approx(150.0f));

    // The rect's corners map to the viewport's corners.
    CHECK(v.world_to_screen({0.0f, 0.0f}).x == doctest::Approx(0.0f));
    CHECK(v.world_to_screen({400.0f, 300.0f}).x == doctest::Approx(800.0f));
}

TEST_CASE("view_transform: pan_by moves the content with the cursor") {
    ViewTransform2D v;
    v.viewport_w = 800.0f;
    v.viewport_h = 600.0f;
    v.zoom = 2.0f;

    const Point world_before = v.screen_to_world({400.0f, 300.0f});
    // Pan by (20, 10) screen px: the content should follow the cursor,
    // so the world point at the center moves by (-20/zoom, -10/zoom)
    // = (-10, -5) in world space.
    v.pan_by({20.0f, 10.0f});
    const Point world_after = v.screen_to_world({400.0f, 300.0f});
    CHECK(world_after.x == doctest::Approx(world_before.x - 10.0f).epsilon(1e-4f));
    CHECK(world_after.y == doctest::Approx(world_before.y - 5.0f).epsilon(1e-4f));
}

TEST_CASE("grid: the view draws the shared grid system") {
    // CanvasView::draw rides grid.hpp's system (one implementation
    // for the whole library - the stage-16 rework). The view's own
    // GridSpec member restyles it; the drawn lattice stays bounded
    // and non-empty at every zoom, extreme or idle.
    CommandListCanvas c;
    CanvasView view;
    GridSpec spec;  // defaults: the dot lattice
    spec.show_dots = false;  // ...restyled to hairlines here
    view.grid = spec;

    const ws::ui::gui::Rect rect{0.0f, 0.0f, 800.0f, 600.0f};

    // Idle zoom: a reasonable, bounded line count.
    view.view.viewport_w = 800.0f;
    view.view.viewport_h = 600.0f;
    view.view.zoom = 26.0f;
    c.cmds.clear();
    view.draw(rect, c, true);
    const std::size_t lines_zoom26 = c.count(CanvasCmdKind::stroke_polyline);
    CHECK(lines_zoom26 > 10);   // a real grid, axes included
    CHECK(lines_zoom26 < 400);  // bounded by the level ladder + cap

    // Deep zoom-out: the ladder coarsens, the count stays bounded
    // (no dense gray block), and the grid never disappears.
    view.view.zoom = 0.001f;
    view.view.min_zoom = 1e-6f;
    c.cmds.clear();
    view.draw(rect, c, true);
    const std::size_t lines_out = c.count(CanvasCmdKind::stroke_polyline);
    CHECK(lines_out > 0);
    CHECK(lines_out < 400);

    // Deep zoom-in: the base grid is never subdivided, so the count
    // only falls as the cells grow past the screen.
    view.view.zoom = 200.0f;
    view.view.max_zoom = 1e6f;
    c.cmds.clear();
    view.draw(rect, c, true);
    const std::size_t lines_in = c.count(CanvasCmdKind::stroke_polyline);
    CHECK(lines_in > 0);
    CHECK(lines_in < lines_zoom26);

    // The default spec draws dots (the schematic look), through the
    // batched fill_rects path - never a fill_circle per dot.
    CanvasView dots_view;
    dots_view.view.viewport_w = 800.0f;
    dots_view.view.viewport_h = 600.0f;
    dots_view.view.zoom = 26.0f;
    c.cmds.clear();
    dots_view.draw(rect, c, true);
    CHECK(c.count(CanvasCmdKind::fill_rects) > 0);
    CHECK(c.count(CanvasCmdKind::fill_circle) == 0);
    CHECK(c.count(CanvasCmdKind::fill_polygon) == 0);
}

TEST_CASE("canvas2d: dashed closed paths stroke fragments open") {
    // The dash walk carves a closed ring into open fragments; only a
    // run that wraps the whole ring (the solid case, or a dash longer
    // than the perimeter) may joint its ends. Stroking every fragment
    // closed spliced a phantom chord from each dash's end back to its
    // start and dropped the fragment's caps.
    Paint paint;
    paint.width = 2.0f;
    paint.cap = StrokeCap::round;
    paint.dash.lengths[0] = 4.0f;
    paint.dash.lengths[1] = 4.0f;
    paint.dash.count = 2;
    const Point square[] = {{0.0f, 0.0f}, {16.0f, 0.0f},
                            {16.0f, 16.0f}, {0.0f, 16.0f}};

    SUBCASE("the wrapper spelling: fragments open, solid closed") {
        CommandListCanvas c;
        c.stroke_polyline(square, 4, paint, true);
        // 3 segments x 16 units, period 8: six 4-unit dashes.
        REQUIRE(c.cmds.size() == 6);
        for (const auto& cmd : c.cmds) {
            CHECK_FALSE(cmd.closed);
        }

        CommandListCanvas solid;
        solid.stroke_polyline(square, 4, Paint{}, true);
        REQUIRE(solid.cmds.size() == 1);
        CHECK(solid.cmds[0].closed);
    }

    SUBCASE("the close_path spelling behaves the same") {
        // close_path appends the duplicate closing point: 4 segments,
        // perimeter 64, eight dashes - all open.
        CommandListCanvas c;
        c.begin_path();
        c.move_to(square[0]);
        for (int i = 1; i < 4; ++i) {
            c.line_to(square[i]);
        }
        c.close_path();
        c.stroke(paint);
        REQUIRE(c.cmds.size() == 8);
        for (const auto& cmd : c.cmds) {
            CHECK_FALSE(cmd.closed);
        }

        // ...and the solid closed ring still joints.
        CommandListCanvas solid;
        solid.begin_path();
        solid.move_to(square[0]);
        for (int i = 1; i < 4; ++i) {
            solid.line_to(square[i]);
        }
        solid.close_path();
        solid.stroke(Paint{});
        REQUIRE(solid.cmds.size() == 1);
        CHECK(solid.cmds[0].closed);
    }

    SUBCASE("a dash longer than the perimeter degenerates to one ring") {
        CommandListCanvas c;
        Paint wrap = paint;
        wrap.dash.lengths[0] = 100.0f;  // > perimeter 48
        c.stroke_polyline(square, 4, wrap, true);
        REQUIRE(c.cmds.size() == 1);
        CHECK(c.cmds[0].closed);
    }

    SUBCASE("through the renderer canvas: exactly the fragments' quads") {
        // End-to-end over the r2d seam: the dashed ring must cost
        // exactly what stroking its fragments individually costs -
        // no phantom closing chords (each would add a doubled core
        // segment plus joint bands, and drop the caps).
        std::vector<std::vector<Point>> runs;
        dash_polyline(square, 4, paint.dash, runs);
        REQUIRE(runs.size() == 6);

        ws::ui::gui_test::stub.reset();
        {
            Renderer2DCanvas rc;
            for (const auto& run : runs) {
                rc.stroke_polyline(run.data(),
                                   static_cast<int>(run.size()), paint,
                                   false);
            }
        }
        const std::uint32_t fragment_quads = ws::ui::gui_test::stub.quads;
        CHECK(fragment_quads > 0);

        ws::ui::gui_test::stub.reset();
        {
            Renderer2DCanvas rc;
            rc.stroke_polyline(square, 4, paint, true);
        }
        CHECK(ws::ui::gui_test::stub.quads == fragment_quads);
    }
}

TEST_CASE("canvas2d: push_clip bounds all four transformed corners") {
    CommandListCanvas c;

    SUBCASE("axis-aligned transforms keep the exact rect") {
        c.translate(10.0f, 20.0f);
        c.scale(2.0f, 2.0f);
        c.push_clip(0.0f, 0.0f, 10.0f, 10.0f);
        REQUIRE(c.cmds.size() == 1);
        CHECK(c.cmds[0].x == doctest::Approx(10.0f));
        CHECK(c.cmds[0].y == doctest::Approx(20.0f));
        CHECK(c.cmds[0].w == doctest::Approx(20.0f));
        CHECK(c.cmds[0].h == doctest::Approx(20.0f));
    }

    SUBCASE("a 45-degree rotation bounds the br corner too") {
        // The old three-corner bounds lost the rotated br corner -
        // half the diagonal. All four corners: the full 14.14 square.
        c.rotate(kPi / 4.0f);
        c.push_clip(0.0f, 0.0f, 10.0f, 10.0f);
        REQUIRE(c.cmds.size() == 1);
        CHECK(c.cmds[0].x == doctest::Approx(-7.0711f).epsilon(0.001));
        CHECK(c.cmds[0].y == doctest::Approx(0.0f).epsilon(0.001));
        CHECK(c.cmds[0].w == doctest::Approx(14.1421f).epsilon(0.001));
        CHECK(c.cmds[0].h == doctest::Approx(14.1421f).epsilon(0.001));
    }
}

TEST_CASE("canvas2d: rounded rects clamp a negative radius to zero") {
    // A negative radius used to displace the arc centers outside the
    // rect and skip the arcs, splicing a bowtie; the SVG rule is a
    // sharp rect.
    CommandListCanvas c;
    c.stroke_rounded_rect(0.0f, 0.0f, 10.0f, 10.0f, -5.0f, Paint{});
    REQUIRE(c.cmds.size() == 1);
    REQUIRE(c.cmds[0].points.size() == 5);  // 4 corners + the close
    // The ring starts at the top-right arc's start and walks the
    // rect's corners clockwise (y-down).
    CHECK(vnear({c.cmds[0].points[0].x, c.cmds[0].points[0].y}, 10.0f, 0.0f));
    CHECK(vnear({c.cmds[0].points[1].x, c.cmds[0].points[1].y}, 10.0f, 10.0f));
    CHECK(vnear({c.cmds[0].points[2].x, c.cmds[0].points[2].y}, 0.0f, 10.0f));
    CHECK(vnear({c.cmds[0].points[3].x, c.cmds[0].points[3].y}, 0.0f, 0.0f));

    CommandListCanvas f;
    f.fill_rounded_rect(0.0f, 0.0f, 10.0f, 10.0f, -5.0f, 0xFFFFFFFFu);
    REQUIRE(f.cmds.size() == 1);
    REQUIRE(f.cmds[0].points.size() == 5);
}

TEST_CASE("canvas view: pending input applies once, compounded") {
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    SUBCASE("one wheel notch before the draw") {
        CanvasView view;
        view.feed_mouse_moved(50.0f, 50.0f);
        view.feed_scroll(0.0f, 0.0f, 0.0f, 1.0f);
        CommandListCanvas c;
        view.draw(rect, c, false);
        CHECK(view.view.zoom == doctest::Approx(1.1f).epsilon(0.001));
    }

    SUBCASE("two notches in one frame compound, not overwrite") {
        // Each event computes its target from the still-current zoom,
        // so the second notch used to overwrite the first: two notches
        // applied 1.1x once. The pending factor must compound.
        CanvasView view;
        view.feed_mouse_moved(50.0f, 50.0f);
        view.feed_scroll(0.0f, 0.0f, 0.0f, 1.0f);
        view.feed_scroll(0.0f, 0.0f, 0.0f, 1.0f);
        CommandListCanvas c;
        view.draw(rect, c, false);
        CHECK(view.view.zoom == doctest::Approx(1.21f).epsilon(0.001));
    }

    SUBCASE("a middle-drag pan follows the rotation") {
        // pan_by applies the inverse rotation; the hand-rolled
        // `pan - delta/zoom` it replaced ignored rotation entirely and
        // dragged a 90-degree view along the wrong axis.
        CanvasView view;
        view.view.rotation = kHalfPi;
        view.feed_mouse_moved(50.0f, 50.0f);
        view.feed_mouse_button(2, true);  // middle press starts the drag
        view.feed_mouse_moved(60.0f, 50.0f);  // +10 screen x
        CommandListCanvas c;
        view.draw(rect, c, false);
        // R^-1 * (10, 0) at rotation pi/2 = (0, -10); pan -= that:
        // the pan moves on y, not x.
        CHECK(view.view.pan.x == doctest::Approx(0.0f).epsilon(0.001));
        CHECK(view.view.pan.y == doctest::Approx(10.0f).epsilon(0.001));
    }
}
