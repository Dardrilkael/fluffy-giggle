#pragma once

// The canvas layer's pure tessellation half: everything between "a
// polyline and a paint" and "vertex-colored quads a raster backend
// consumes". Public because the tests assert its output directly
// (vertex counts, winding, join/cap geometry, flattening tolerance) -
// the same purity seam as the gui's behavior/draw split. No renderer,
// no window, no allocation beyond the caller's vectors.
//
// ALL coordinates are SCREEN SPACE and all sizes SCREEN PIXELS: the
// canvas (canvas.hpp) is the one who knows about the world transform
// and the camera zoom - by the time these functions run, points are
// pixels and a "1" is one pixel. That is what makes stroke widths
// zoom-independent: the width never went through a transform.
//
// The emission contract: every primitive leaves as QUADS of four
// corners with four rgba rows (floats, 0..1, straight alpha - the
// raster blends srcAlpha/1-srcAlpha, so a fringe fades its ALPHA only).
// A triangle is a quad whose fourth corner duplicates the third; the
// batch's fixed index pattern (0,1,2 / 2,3,0) needs no triangle path.
//
// Vector math comes from ws::math, not a local copy: Point IS Vec2
// (an alias, not a wrapper), so a polyline's worth of points is one
// contiguous Vec2 array - no struct-of-arrays vs array-of-structs
// split, no field reordering at the seam. The same Vec2/Vec4 the
// raster reads is the one the stroker writes.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <ws/math/vecmath.hpp>

namespace ws::ui::canvas {

// A 2D point IS a ws::math::Vec2 - the alias keeps the canvas's own
// vocabulary ("a Point") while sharing storage and math with the rest
// of the workspace. Brace init `Point p{1.0f, 2.0f}` works the same
// as Vec2.
using Point = ws::math::Vec2;

// Angle constants the tests and the stroker share. Spelled from
// std::numbers so MSVC's /permissive- build is happy (M_PI is not
// standard).
inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = 2.0f * kPi;
inline constexpr float kHalfPi = 1.57079632679489661923f;

// --- the paint -----------------------------------------------------------------

enum class StrokeJoin : std::uint8_t {
    miter = 0,
    round = 1,
    bevel = 2,
};

enum class StrokeCap : std::uint8_t {
    butt = 0,
    round = 1,
    square = 2,
};

// A dash pattern: alternating ON/OFF lengths in the PATH's OWN units
// (the SVG convention - dashes travel with the geometry, unlike stroke
// widths, which are screen pixels). An odd count repeats the array
// (SVG's rule), so {10} is 10 on, 10 off. count 0 = solid.
struct DashSpec {
    static constexpr int kMax = 8;
    float lengths[kMax] = {};
    int count = 0;
    float offset = 0.0f;  // phase into the pattern, path units (may be
                          // negative; normalized on use)
};

// What a primitive is drawn with. The color is packed 0xRRGGBBAA
// (r2d::rgba's order). width is SCREEN PIXELS, independent of zoom and
// of the canvas transform's scale. miter_limit is the classic ratio
// (miter length / stroke width, SVG's default 4): past it a miter join
// falls back to bevel, the spike guard.
struct Paint {
    std::uint32_t color = 0xFFFFFFFFu;
    float width = 1.0f;
    StrokeJoin join = StrokeJoin::miter;
    StrokeCap cap = StrokeCap::butt;
    float miter_limit = 4.0f;
    DashSpec dash{};
};

// --- the sink --------------------------------------------------------------------
//
// Where tessellated geometry lands. One call = one quad; corners are
// the emitter's to order (each family documents its winding so the
// tests can pin it), colors are straight-alpha floats in ws::math::Vec4
// (r, g, b, a). The Vec2[4] / Vec4[4] form keeps the whole quad in four
// contiguous vector loads - the same layout the raster's per-vertex
// attribute feeds.
class TessSink {
public:
    virtual ~TessSink() = default;
    virtual void quad(const ws::math::Vec2 (&pos)[4],
                      const ws::math::Vec4 (&rgba)[4]) = 0;
};

// The recording sink: what the tests read. quads is flat - four
// TessVertex per quad, in call order.
struct TessVertex {
    float x = 0.0f;
    float y = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

class TessBuffer : public TessSink {
public:
    std::vector<TessVertex> quads;

    void quad(const ws::math::Vec2 (&pos)[4],
              const ws::math::Vec4 (&rgba)[4]) override;
    std::size_t quad_count() const { return quads.size() / 4; }
    void clear() { quads.clear(); }
};

// --- flattening --------------------------------------------------------------------
// Each helper walks its curve and APPENDS the flattened points to
// `out`, EXCLUDING the curve's start point (the anchor is already in
// the caller's path - every use is "extend this polyline along the
// curve"). The end point IS appended. `tolerance` is the flattening
// sagitta in screen px (0.25-0.5 is the sensible band); the segment
// count comes from the curve's screen-space size, so a zoomed-in curve
// flattens finer and a zoomed-out one coarser. Degenerate control
// points collapse silently (a quadratic whose handle sits on the chord
// appends just the end point).

// Appends a quadratic Bezier from p0 (excluded) through p1 to p2.
void flatten_quadratic(Point p0, Point p1, Point p2, float tolerance,
                       std::vector<Point>& out);

// Appends a cubic Bezier from p0 (excluded) through p1, p2 to p3.
void flatten_cubic(Point p0, Point p1, Point p2, Point p3, float tolerance,
                   std::vector<Point>& out);

// Appends an ellipse arc around `center` with radii (rx, ry) from a0
// (excluded) to a1, angles in radians. y grows DOWN, so a positive
// angle sweeps CLOCKWISE on screen: the point at angle a is
// (cx + rx*cos a, cy + ry*sin a). a1 < a0 sweeps backward (negative
// span); a span of 2pi is the full ellipse (the duplicate closing point
// is appended - a closed path wants it). A non-positive radius appends
// nothing (the arc collapsed to the center, and the anchor is already
// there).
void flatten_arc(Point center, float rx, float ry, float a0, float a1,
                 float tolerance, std::vector<Point>& out);

// --- dashing -----------------------------------------------------------------------
// Splits a polyline at the dash pattern's OFF phases, in the points'
// own units (the canvas dashes in LOCAL space, where the lengths mean
// what the author drew, then transforms and strokes each run). Each
// emitted run is a sub-polyline of the input; a run of exactly ONE
// point is a zero-length ON phase - the stroker turns it into cap
// geometry (a round cap makes the dot of a dotted line). A solid
// pattern (count 0, or a non-positive total length) emits one run: the
// whole input. Consecutive duplicate input points collapse within a
// run. `runs` is appended to, not cleared - callers pipeline.
void dash_polyline(const Point* pts, int count, const DashSpec& dash,
                   std::vector<std::vector<Point>>& runs);

// --- stroking -------------------------------------------------------------------
// Strokes a polyline: the paint's width in screen px, joins at interior
// vertices (miter with the limit's bevel fallback / round / bevel),
// caps at both ends (butt / round / square), and 1px antialiasing
// fringes along the whole outline. `closed` joints the last vertex back
// to the first (a circle outline) and caps nothing.
//
// Geometry: the core spans half-width max(w/2 - 0.5, 0); the fringes
// run from there out to w/2 + 0.5 with corner alpha ramping 1 -> 0 (a
// hairline - width below 1px - has no core: two bands from the center
// line out, peaking at alpha = width, the coverage-honest answer). The
// fringes walk the JOIN-RESOLVED outline (the miter/bevel/arc chain),
// so consecutive fringe quads share corners: no gaps, no double-drawn
// inner corners.
//
// Winding contracts the tests pin:
//   - a segment's core quad names corners (A + n*hi, A - n*hi,
//     B - n*hi, B + n*hi) with A the start, B the end and n the right
//     normal ((-dy, dx)); triangles duplicate the THIRD corner.
//   - a fringe quad names corners in outline-walk order: (inner-from,
//     inner-to, outer-to, outer-from).
//
// Degenerates: count <= 0 emits nothing; count == 1 emits cap geometry
// only (butt: nothing, round: the dot, square: the square); zero-length
// segments are skipped, their neighbors jointed directly. Paint.width
// <= 0 emits nothing.
void stroke_polyline(const Point* pts, int count, const Paint& paint,
                     TessSink& sink, bool closed = false);

// --- filling ---------------------------------------------------------------------
// Fills a SIMPLE polygon (no self-intersections; convex is the special
// case) with `color`, antialiased: the core is the ear-clipped
// triangulation of the polygon shrunk 0.5px inward per vertex (the
// offset clamped by half the shorter adjacent edge, so a sliver cannot
// invert), and each boundary edge carries a fringe from the shrunk edge
// (alpha 1) to 0.5px past the true edge (alpha 0) - 50% coverage right
// at the geometric edge. Triangles are degenerate quads (third corner
// duplicated). count < 3 emits nothing; collinear vertices are dropped
// by the clipper. Winding either way - the fill normalizes.
void fill_polygon(const Point* pts, int count, std::uint32_t color,
                  TessSink& sink);

}  // namespace ws::ui::canvas
