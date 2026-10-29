// geometry2d.hpp's suite: every helper through its paces, degenerates
// first-class (zero-length segments, one-point polylines, empty
// polygons, empty bounds, parallel/outside clips). No
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here - test_math.cpp owns doctest's
// main in the repo's math test executable; this file rides beside it
// (and beside test_vecmath/test_sparse, the same way).
#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include <ws/math/geometry2d.hpp>

using ws::math::Box2;
using ws::math::kEmptyBounds;
using ws::math::Vec2;
using ws::math::Vec4;

namespace {

// Vec2 has no operator== (vecmath keeps the set minimal) - the suite's
// own close-enough pair. A plain epsilon on both components.
bool near(Vec2 a, Vec2 b, float eps = 1e-4f) {
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps;
}

}  // namespace

TEST_CASE("point_segment_distance: interior, endpoints, degenerate") {
    const Vec2 a{0.0f, 0.0f};
    const Vec2 b{10.0f, 0.0f};

    SUBCASE("perpendicular projection lands inside") {
        Vec2 c;
        float t;
        const float d = ws::math::point_segment_distance({5.0f, 3.0f}, a, b,
                                                         &c, &t);
        CHECK(d == doctest::Approx(3.0f));
        CHECK(near(c, {5.0f, 0.0f}));
        CHECK(t == doctest::Approx(0.5f));
    }
    SUBCASE("projection past b clamps to b") {
        Vec2 c;
        float t;
        const float d = ws::math::point_segment_distance({15.0f, 0.0f}, a, b,
                                                         &c, &t);
        CHECK(d == doctest::Approx(5.0f));
        CHECK(near(c, b));
        CHECK(t == doctest::Approx(1.0f));
    }
    SUBCASE("projection before a clamps to a") {
        Vec2 c;
        float t;
        const float d = ws::math::point_segment_distance({-3.0f, -4.0f}, a, b,
                                                         &c, &t);
        CHECK(d == doctest::Approx(5.0f));
        CHECK(near(c, a));
        CHECK(t == doctest::Approx(0.0f));
    }
    SUBCASE("the point on the segment is zero away") {
        Vec2 c;
        const float d = ws::math::point_segment_distance({7.5f, 0.0f}, a, b,
                                                         &c, nullptr);
        CHECK(d == doctest::Approx(0.0f));
        CHECK(near(c, {7.5f, 0.0f}));
    }
    SUBCASE("a zero-length segment is its point") {
        Vec2 c;
        float t;
        const float d = ws::math::point_segment_distance({3.0f, 4.0f},
                                                         {0.0f, 0.0f},
                                                         {0.0f, 0.0f}, &c, &t);
        CHECK(d == doctest::Approx(5.0f));
        CHECK(near(c, {0.0f, 0.0f}));
        CHECK(t == doctest::Approx(0.0f));
    }
    SUBCASE("the outs are optional") {
        CHECK(ws::math::point_segment_distance({1.0f, 1.0f}, a, b) ==
              doctest::Approx(1.0f));
    }
}

TEST_CASE("polyline_distance: walks the chain, reports where") {
    const Vec2 pts[] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f}};

    SUBCASE("nearest lands on the second segment") {
        std::size_t seg;
        float t;
        Vec2 c;
        const float d = ws::math::polyline_distance({14.0f, 6.0f}, pts, 3,
                                                    &seg, &t, &c);
        CHECK(d == doctest::Approx(4.0f));
        CHECK(seg == 1);
        CHECK(t == doctest::Approx(0.6f));
        CHECK(near(c, {10.0f, 6.0f}));
    }
    SUBCASE("nearest lands on the first segment") {
        std::size_t seg;
        const float d = ws::math::polyline_distance({5.0f, -2.0f}, pts, 3,
                                                    &seg, nullptr, nullptr);
        CHECK(d == doctest::Approx(2.0f));
        CHECK(seg == 0);
    }
    SUBCASE("the shared corner answers the earlier segment") {
        // Distance 0 at the joint: the walk keeps the FIRST minimum, so
        // the corner belongs to segment 0 - the documented tie-break.
        std::size_t seg;
        const float d = ws::math::polyline_distance({10.0f, 0.0f}, pts, 3,
                                                    &seg);
        CHECK(d == doctest::Approx(0.0f));
        CHECK(seg == 0);
    }
    SUBCASE("empty answers +INF, outs untouched") {
        std::size_t seg = 123;
        CHECK(ws::math::polyline_distance({0.0f, 0.0f}, nullptr, 0, &seg) ==
              std::numeric_limits<float>::infinity());
        CHECK(seg == 123);
    }
    SUBCASE("one point is the point distance") {
        const Vec2 one[] = {{2.0f, 3.0f}};
        Vec2 c;
        const float d = ws::math::polyline_distance({5.0f, 3.0f}, one, 1,
                                                    nullptr, nullptr, &c);
        CHECK(d == doctest::Approx(3.0f));
        CHECK(near(c, one[0]));
    }
}

TEST_CASE("point_in_polygon: convex, concave, degenerate") {
    SUBCASE("a square") {
        const Vec2 sq[] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f},
                           {0.0f, 10.0f}};
        CHECK(ws::math::point_in_polygon({5.0f, 5.0f}, sq, 4));
        CHECK_FALSE(ws::math::point_in_polygon({15.0f, 5.0f}, sq, 4));
        CHECK_FALSE(ws::math::point_in_polygon({-1.0f, -1.0f}, sq, 4));
    }
    SUBCASE("an L (simple concave): the notch is outside") {
        const Vec2 ell[] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f},
                            {6.0f, 10.0f}, {6.0f, 4.0f}, {0.0f, 4.0f}};
        CHECK(ws::math::point_in_polygon({3.0f, 2.0f}, ell, 6));
        CHECK(ws::math::point_in_polygon({8.0f, 8.0f}, ell, 6));
        CHECK(ws::math::point_in_polygon({8.0f, 2.0f}, ell, 6));  // the bar
        CHECK_FALSE(ws::math::point_in_polygon({3.0f, 7.0f}, ell, 6));
        CHECK_FALSE(ws::math::point_in_polygon({-1.0f, 2.0f}, ell, 6));
    }
    SUBCASE("a star (concave with reflex tips)") {
        const Vec2 star[] = {{0.0f, 0.0f},  {4.0f, 3.0f},  {8.0f, 0.0f},
                              {6.0f, 6.0f}, {8.0f, 10.0f}, {4.0f, 7.0f},
                              {0.0f, 10.0f}, {2.0f, 6.0f}};
        CHECK(ws::math::point_in_polygon({4.0f, 5.0f}, star, 8));
        CHECK_FALSE(ws::math::point_in_polygon({1.0f, 5.0f}, star, 8));
        CHECK_FALSE(ws::math::point_in_polygon({7.0f, 5.0f}, star, 8));
    }
    SUBCASE("fewer than three points contain nothing") {
        const Vec2 pair[] = {{0.0f, 0.0f}, {5.0f, 5.0f}};
        CHECK_FALSE(ws::math::point_in_polygon({1.0f, 1.0f}, pair, 2));
        CHECK_FALSE(ws::math::point_in_polygon({1.0f, 1.0f}, nullptr, 0));
    }
    SUBCASE("winding order does not matter (even-odd)") {
        const Vec2 cw[] = {{0.0f, 0.0f}, {0.0f, 10.0f}, {10.0f, 10.0f},
                           {10.0f, 0.0f}};
        const Vec2 ccw[] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f},
                            {0.0f, 10.0f}};
        CHECK(ws::math::point_in_polygon({5.0f, 5.0f}, cw, 4));
        CHECK(ws::math::point_in_polygon({5.0f, 5.0f}, ccw, 4));
    }
}

TEST_CASE("segment_clip_rect: the Liang-Barsky matrix") {
    const float rx = 0.0f, ry = 0.0f, rw = 10.0f, rh = 10.0f;

    SUBCASE("a crossing segment clips to the intersection") {
        Vec2 a, b;
        float t0, t1;
        CHECK(ws::math::segment_clip_rect({-5.0f, 5.0f}, {15.0f, 5.0f}, rx,
                                          ry, rw, rh, &a, &b, &t0, &t1));
        CHECK(near(a, {0.0f, 5.0f}));
        CHECK(near(b, {10.0f, 5.0f}));
        CHECK(t0 == doctest::Approx(0.25f));
        CHECK(t1 == doctest::Approx(0.75f));
    }
    SUBCASE("a diagonal fully inside passes with t 0..1") {
        Vec2 a, b;
        float t0, t1;
        CHECK(ws::math::segment_clip_rect({1.0f, 1.0f}, {9.0f, 9.0f}, rx, ry,
                                          rw, rh, &a, &b, &t0, &t1));
        CHECK(near(a, {1.0f, 1.0f}));
        CHECK(near(b, {9.0f, 9.0f}));
        CHECK(t0 == doctest::Approx(0.0f));
        CHECK(t1 == doctest::Approx(1.0f));
    }
    SUBCASE("a fully outside segment refuses, outs untouched") {
        Vec2 a{7.0f, 7.0f}, b{8.0f, 8.0f};
        CHECK_FALSE(ws::math::segment_clip_rect({12.0f, 12.0f},
                                                {20.0f, 20.0f}, rx, ry, rw,
                                                rh, &a, &b));
        CHECK(near(a, {7.0f, 7.0f}));  // not written on refusal
    }
    SUBCASE("parallel-and-outside refuses; parallel-and-inside passes") {
        CHECK_FALSE(ws::math::segment_clip_rect({-5.0f, -3.0f},
                                                {15.0f, -3.0f}, rx, ry, rw,
                                                rh));
        Vec2 a, b;
        CHECK(ws::math::segment_clip_rect({-5.0f, 3.0f}, {15.0f, 3.0f}, rx,
                                          ry, rw, rh, &a, &b));
        CHECK(near(a, {0.0f, 3.0f}));
        CHECK(near(b, {10.0f, 3.0f}));
    }
    SUBCASE("a corner-crossing diagonal clips at both boundaries") {
        Vec2 a, b;
        CHECK(ws::math::segment_clip_rect({-10.0f, -10.0f}, {20.0f, 20.0f},
                                          rx, ry, rw, rh, &a, &b));
        CHECK(near(a, {0.0f, 0.0f}));
        CHECK(near(b, {10.0f, 10.0f}));
    }
    SUBCASE("a degenerate segment is a closed point containment test") {
        CHECK(ws::math::segment_clip_rect({5.0f, 5.0f}, {5.0f, 5.0f}, rx, ry,
                                          rw, rh));
        // The clip is CLOSED (the touching-diagonal case above keeps its
        // boundary end), so a point on any edge - the right/bottom ones
        // included - counts as inside.
        CHECK(ws::math::segment_clip_rect({10.0f, 5.0f}, {10.0f, 5.0f},
                                          rx, ry, rw, rh));
        CHECK_FALSE(ws::math::segment_clip_rect({-1.0f, 5.0f},
                                                {-1.0f, 5.0f}, rx, ry, rw,
                                                rh));
    }
    SUBCASE("a backwards rect clips everything") {
        CHECK_FALSE(ws::math::segment_clip_rect({5.0f, 5.0f}, {5.0f, 5.0f},
                                                0.0f, 0.0f, -10.0f, 10.0f));
    }
    SUBCASE("touching the boundary from inside still counts") {
        // t-range collapses to the boundary point but the segment IS
        // inside the closed rect (Liang-Barsky keeps the touching end).
        Vec2 a, b;
        float t0, t1;
        CHECK(ws::math::segment_clip_rect({0.0f, -5.0f}, {0.0f, 5.0f}, rx,
                                          ry, rw, rh, &a, &b, &t0, &t1));
        CHECK(near(a, {0.0f, 0.0f}));
        CHECK(near(b, {0.0f, 5.0f}));
    }
}

TEST_CASE("bounds: union, expand, inflate, of_points") {
    SUBCASE("the empty box contains nothing and unions into anything") {
        CHECK_FALSE(kEmptyBounds.valid());
        CHECK_FALSE(kEmptyBounds.contains({0.0f, 0.0f}));
        const Box2 u = ws::math::bounds_union(
            kEmptyBounds, Box2{1.0f, 2.0f, 3.0f, 4.0f});
        CHECK(u.min_x == doctest::Approx(1.0f));
        CHECK(u.min_y == doctest::Approx(2.0f));
        CHECK(u.max_x == doctest::Approx(3.0f));
        CHECK(u.max_y == doctest::Approx(4.0f));
        CHECK(ws::math::bounds_union(kEmptyBounds, kEmptyBounds).min_x ==
              std::numeric_limits<float>::infinity());
    }
    SUBCASE("union takes the wider span on each axis") {
        const Box2 u = ws::math::bounds_union(Box2{0.0f, 0.0f, 5.0f, 5.0f},
                                              Box2{-2.0f, 3.0f, 2.0f, 9.0f});
        CHECK(u.min_x == doctest::Approx(-2.0f));
        CHECK(u.min_y == doctest::Approx(0.0f));
        CHECK(u.max_x == doctest::Approx(5.0f));
        CHECK(u.max_y == doctest::Approx(9.0f));
    }
    SUBCASE("expand grows toward the point, both directions") {
        Box2 b{0.0f, 0.0f, 5.0f, 5.0f};
        b = ws::math::bounds_expand(b, {-3.0f, 7.0f});
        CHECK(b.min_x == doctest::Approx(-3.0f));
        CHECK(b.max_y == doctest::Approx(7.0f));
        b = ws::math::bounds_expand(b, {2.0f, 2.0f});  // interior: no move
        CHECK(b.min_x == doctest::Approx(-3.0f));
        CHECK(b.max_y == doctest::Approx(7.0f));
        CHECK(ws::math::bounds_expand(kEmptyBounds, {4.0f, 4.0f}).valid());
    }
    SUBCASE("inflate grows and shrinks by the margin") {
        const Box2 g = ws::math::bounds_inflate(
            Box2{0.0f, 0.0f, 5.0f, 5.0f}, 2.0f);
        CHECK(g.min_x == doctest::Approx(-2.0f));
        CHECK(g.max_y == doctest::Approx(7.0f));
        const Box2 s = ws::math::bounds_inflate(
            Box2{0.0f, 0.0f, 5.0f, 5.0f}, -10.0f);
        CHECK_FALSE(s.valid());  // over-inverted: contains nothing
        CHECK_FALSE(s.contains({2.0f, 2.0f}));
    }
    SUBCASE("of_points folds empty and full sets") {
        CHECK_FALSE(ws::math::bounds_of_points(nullptr, 0).valid());
        const Vec2 pts[] = {{3.0f, -1.0f}, {-2.0f, 8.0f}, {5.0f, 5.0f}};
        const Box2 b = ws::math::bounds_of_points(pts, 3);
        CHECK(b.min_x == doctest::Approx(-2.0f));
        CHECK(b.min_y == doctest::Approx(-1.0f));
        CHECK(b.max_x == doctest::Approx(5.0f));
        CHECK(b.max_y == doctest::Approx(8.0f));
        CHECK(b.contains({3.0f, -1.0f}));
    }
    SUBCASE("contains is half-open on the max corners") {
        const Box2 b{0.0f, 0.0f, 5.0f, 5.0f};
        CHECK(b.contains({0.0f, 0.0f}));
        CHECK(b.contains({4.999f, 4.999f}));
        CHECK_FALSE(b.contains({5.0f, 2.0f}));
        CHECK_FALSE(b.contains({2.0f, 5.0f}));
    }
}

TEST_CASE("unpack_color: the 0xRRGGBBAA layout") {
    SUBCASE("white opaque is all ones") {
        const Vec4 c = ws::math::unpack_color(0xFFFFFFFFu);
        CHECK(c.x == doctest::Approx(1.0f));
        CHECK(c.y == doctest::Approx(1.0f));
        CHECK(c.z == doctest::Approx(1.0f));
        CHECK(c.w == doctest::Approx(1.0f));
    }
    SUBCASE("black transparent is all zeros") {
        const Vec4 c = ws::math::unpack_color(0x00000000u);
        CHECK(c.x == doctest::Approx(0.0f));
        CHECK(c.y == doctest::Approx(0.0f));
        CHECK(c.z == doctest::Approx(0.0f));
        CHECK(c.w == doctest::Approx(0.0f));
    }
    SUBCASE("red / green / blue / alpha channels land in their slots") {
        const Vec4 r = ws::math::unpack_color(0xFF0000FFu);
        CHECK(r.x == doctest::Approx(1.0f));
        CHECK(r.y == doctest::Approx(0.0f));
        CHECK(r.z == doctest::Approx(0.0f));
        CHECK(r.w == doctest::Approx(1.0f));

        const Vec4 g = ws::math::unpack_color(0x00FF00FFu);
        CHECK(g.x == doctest::Approx(0.0f));
        CHECK(g.y == doctest::Approx(1.0f));
        CHECK(g.z == doctest::Approx(0.0f));
        CHECK(g.w == doctest::Approx(1.0f));

        const Vec4 b = ws::math::unpack_color(0x0000FFFFu);
        CHECK(b.x == doctest::Approx(0.0f));
        CHECK(b.y == doctest::Approx(0.0f));
        CHECK(b.z == doctest::Approx(1.0f));
        CHECK(b.w == doctest::Approx(1.0f));

        const Vec4 a = ws::math::unpack_color(0x00000080u);
        CHECK(a.w == doctest::Approx(0.5f).epsilon(0.01));
    }
    SUBCASE("a 50% gray at 50% alpha lands in the middle of every channel") {
        const Vec4 c = ws::math::unpack_color(0x80808080u);
        CHECK(c.x == doctest::Approx(0.5f).epsilon(0.01));
        CHECK(c.y == doctest::Approx(0.5f).epsilon(0.01));
        CHECK(c.z == doctest::Approx(0.5f).epsilon(0.01));
        CHECK(c.w == doctest::Approx(0.5f).epsilon(0.01));
    }
}

TEST_CASE("arc_step: the sagitta-to-step mapping") {
    SUBCASE("a larger radius buys a smaller step at the same tolerance") {
        // The sagitta s = r*(1 - cos(step/2)) is held to `tol`, so as r
        // grows the step shrinks (a gentle arc needs finer steps to keep
        // the same chord error). r=1 buys ~1.445 rad; r=100 buys ~0.141.
        const float small = ws::math::arc_step(1.0f, 0.25f);
        const float large = ws::math::arc_step(100.0f, 0.25f);
        CHECK(small > 0.0f);
        CHECK(large > 0.0f);
        CHECK(large < small);
    }
    SUBCASE("a tighter tolerance buys a smaller step at the same radius") {
        const float coarse = ws::math::arc_step(10.0f, 1.0f);
        const float fine = ws::math::arc_step(10.0f, 0.1f);
        CHECK(fine < coarse);
    }
    SUBCASE("the closed-form sagitta holds: step = 2*acos(1 - tol/r)") {
        // For r=10, tol=0.25: step = 2*acos(1 - 0.025) = 2*acos(0.975).
        const float step = ws::math::arc_step(10.0f, 0.25f);
        const float expected = 2.0f * std::acos(1.0f - 0.25f / 10.0f);
        CHECK(step == doctest::Approx(expected).epsilon(0.001));
    }
    SUBCASE("a degenerate radius (r <= tol) buys the coarse pi/2 step") {
        const float step = ws::math::arc_step(0.1f, 0.25f);
        CHECK(step == doctest::Approx(1.57079637f));
    }
    SUBCASE("the step is always positive and finite") {
        CHECK(ws::math::arc_step(1.0f, 0.25f) > 0.0f);
        CHECK(ws::math::arc_step(1e6f, 0.25f) > 0.0f);
        CHECK(ws::math::arc_step(0.0f, 0.25f) > 0.0f);
        CHECK(std::isfinite(ws::math::arc_step(1e6f, 0.25f)));
    }
}
