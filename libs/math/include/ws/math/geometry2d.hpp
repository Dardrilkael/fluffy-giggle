#pragma once

// 2D geometry helpers for hit-testing and measuring: distances to
// segments and polylines, point-in-polygon, segment/rect clipping and
// AABB bounds. Everything is plain float math over Vec2 (vecmath.hpp);
// nothing here allocates or throws - the callers are hit tests and draw
// culling on the frame path. Inputs are assumed FINITE; a NaN in gives
// an unspecified answer out (the widget/raster layers that feed these
// already guard non-finite geometry the way r2d's clip push does).
//
// Degenerate inputs are first-class, not asserts: a zero-length segment
// is a point, a one-point polyline is a point, an empty polygon contains
// nothing, and an empty bounds is a canonical +INF box that unions into
// whatever it meets. Callers on the frame path may hold live drag data,
// and "the user has not moved yet" must be an answer, not a crash.

#include <cstddef>
#include <cstdint>
#include <limits>

#include <ws/math/vecmath.hpp>

namespace ws::math {

// --- distances ---------------------------------------------------------------

// Distance from `p` to segment [a, b], plus (optionally) the closest
// point on the segment and its parameter: closest = a + (b - a) * t,
// t in [0, 1] (the segment's own span - the projection clamps to the
// nearer endpoint). A zero-length segment answers the point distance
// with t = 0.
float point_segment_distance(Vec2 p, Vec2 a, Vec2 b,
                             Vec2* closest = nullptr, float* t = nullptr);

// Distance from `p` to a polyline (its segment chain), plus (optionally)
// where the answer came from: the segment index (pts[i]..pts[i+1]) and
// the same clamped parameter/point point_segment_distance reports.
// count == 0 answers +INF with index 0 (nothing to be near); count == 1
// answers the point distance with index 0.
float polyline_distance(Vec2 p, const Vec2* pts, std::size_t count,
                        std::size_t* segment = nullptr, float* t = nullptr,
                        Vec2* closest = nullptr);

// --- containment --------------------------------------------------------------

// Even-odd containment for a SIMPLE polygon (no self-intersections;
// convex is the special case). The crossing rule is the classic raycast:
// a ray from `p` crosses each edge whose y-span straddles p.y, and an odd
// count means inside. A point exactly ON an edge is not guaranteed either
// answer across call sites - callers needing boundary-exact containment
// walk the edges with point_segment_distance == 0 instead. count < 3
// answers false (a point and a line segment are no polygon).
bool point_in_polygon(Vec2 p, const Vec2* pts, std::size_t count);

// --- clipping ------------------------------------------------------------------

// Clips segment [a, b] against the axis-aligned rect (Liang-Barsky).
// Returns true when any part of the segment lies inside; the clipped
// endpoints (in the same parameter space - out_a/out_b are points, and
// out_t0/out_t1 their parameters along the original segment, t0 <= t1)
// are written only when the answer is true. A fully outside segment
// answers false with the outs untouched; a degenerate (zero-length)
// segment answers whether the point itself lies inside the CLOSED rect
// (the clip keeps boundary-touching ends, so an edge point counts as
// in - Rect::contains is the half-open sibling, a different contract).
bool segment_clip_rect(Vec2 a, Vec2 b, float rect_x, float rect_y,
                       float rect_w, float rect_h, Vec2* out_a = nullptr,
                       Vec2* out_b = nullptr, float* out_t0 = nullptr,
                       float* out_t1 = nullptr);

// --- bounds ----------------------------------------------------------------------

// An axis-aligned box by its min/max corners. The canonical empty box is
// {+INF, +INF, -INF, -INF} - kEmptyBounds; it contains nothing and
// unions into whatever it meets, so a fold over "no points yet" starts
// there without a special case.
struct Box2 {
    float min_x = std::numeric_limits<float>::infinity();
    float min_y = std::numeric_limits<float>::infinity();
    float max_x = -std::numeric_limits<float>::infinity();
    float max_y = -std::numeric_limits<float>::infinity();

    bool valid() const { return min_x <= max_x && min_y <= max_y; }
    bool contains(Vec2 p) const {
        return valid() && p.x >= min_x && p.x < max_x && p.y >= min_y &&
               p.y < max_y;
    }
};

inline constexpr Box2 kEmptyBounds{};

// The smallest box covering both (either may be the empty box; two empty
// boxes answer the empty box).
[[nodiscard]] Box2 bounds_union(Box2 a, Box2 b);

// `a` grown to include point `p` (the fold step over a point set).
[[nodiscard]] Box2 bounds_expand(Box2 a, Vec2 p);

// `a` grown/shrunk by `margin` on every side. A negative margin past
// empty crosses the min/max (an invalid box contains nothing - the safe
// answer for an over-inset bounds, Rect::shrunk's rule).
[[nodiscard]] Box2 bounds_inflate(Box2 a, float margin);

// The smallest box covering the points (the empty box when count == 0).
[[nodiscard]] Box2 bounds_of_points(const Vec2* pts, std::size_t count);

// --- color ---------------------------------------------------------------------

// Unpacks a 0xRRGGBBAA packed color into four 0..1 floats in a Vec4
// (r, g, b, a). The packing order matches the raster's per-corner color
// attribute: a stroke's solid color, a fill's flat shade, a fringe's
// alpha-ramped edge. Straight alpha (the raster blends srcAlpha/1-srcAlpha,
// so a fringe fades its ALPHA only and leaves RGB untouched).
[[nodiscard]] inline Vec4 unpack_color(std::uint32_t rgba) {
    return Vec4{
        static_cast<float>((rgba >> 24) & 0xFFu) / 255.0f,
        static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f,
        static_cast<float>((rgba >> 8) & 0xFFu) / 255.0f,
        static_cast<float>(rgba & 0xFFu) / 255.0f,
    };
}

// --- curve flattening ----------------------------------------------------------

// The arc-step helper: the angle step whose chord sagitta is `tol` on a
// circle of radius `r`. The chord at angle step s has sagitta
// r * (1 - cos(s/2)); solving for s gives 2*acos(1 - tol/r). Clamped for
// degenerate radii (r <= tol buys the coarse quarter-circle step; a huge
// radius is capped so a near-straight arc cannot generate thousands of
// points). Always positive, always finite. This is the single source of
// the step size every flattener (quadratic/cubic bezier, ellipse arc,
// round-join fan, round-cap half-disc) uses, so the tolerance behavior is
// uniform across the canvas.
[[nodiscard]] float arc_step(float r, float tol);

}  // namespace ws::math
