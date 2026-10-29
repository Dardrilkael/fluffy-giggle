// tessellation.hpp's implementation. Everything here is plain float
// math over screen-space points; the only allocation is the caller's
// vectors (flattening outputs, dash runs) and the ear-clipper's index
// list. See the header for the contracts - this half is where the
// winding orders named there come from.
//
// Vector math (add/sub/scale via operator*, dot, cross, length,
// right_normal) comes from ws::math::vecmath; the 2D scalar cross and
// the right_normal are the same ones the math tests pin. unpack_color
// and arc_step come from ws::math::geometry2d. No local copies - one
// spelling of every vector op, shared with the rest of the workspace.

#include <ws/ui/canvas/tessellation.hpp>

#include <algorithm>
#include <cmath>

#include <ws/math/geometry2d.hpp>

namespace ws::ui::canvas {
namespace {

using ws::math::Vec2;
using ws::math::Vec4;
using ws::math::cross;
using ws::math::dot;
using ws::math::length;
using ws::math::right_normal;
using ws::math::unpack_color;
using ws::math::arc_step;

}  // namespace

// --- TessBuffer ---------------------------------------------------------------

void TessBuffer::quad(const Vec2 (&pos)[4], const Vec4 (&rgba)[4]) {
    for (int i = 0; i < 4; ++i) {
        TessVertex v;
        v.x = pos[i].x;
        v.y = pos[i].y;
        v.r = rgba[i].x;
        v.g = rgba[i].y;
        v.b = rgba[i].z;
        v.a = rgba[i].w;
        quads.push_back(v);
    }
}

// --- flattening ----------------------------------------------------------------

namespace {

// One quadratic subdivision step; true when the curve is flat enough
// (control point within `tol` of the chord).
bool quadratic_flat(Vec2 p0, Vec2 p1, Vec2 p2, float tol) {
    const Vec2 chord = p2 - p0;
    const float c = length(chord);
    if (c <= 1e-6f) {
        return true;  // degenerate: everything sits on one point
    }
    const float d = std::fabs(cross(chord, p1 - p0)) / c;
    return d <= tol;
}

void flatten_quadratic_rec(Vec2 p0, Vec2 p1, Vec2 p2, float tol,
                           std::vector<Vec2>& out, int depth) {
    if (depth >= 16 || quadratic_flat(p0, p1, p2, tol)) {
        out.push_back(p2);
        return;
    }
    // de Casteljau split at t = 0.5.
    const Vec2 p01 = (p0 + p1) * 0.5f;
    const Vec2 p12 = (p1 + p2) * 0.5f;
    const Vec2 mid = (p01 + p12) * 0.5f;
    flatten_quadratic_rec(p0, p01, mid, tol, out, depth + 1);
    flatten_quadratic_rec(mid, p12, p2, tol, out, depth + 1);
}

// Same for a cubic: both inner controls must sit within tol of the
// chord (the standard conservative test).
bool cubic_flat(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float tol) {
    const Vec2 chord = p3 - p0;
    const float c = length(chord);
    if (c <= 1e-6f) {
        return true;
    }
    const float d1 = std::fabs(cross(chord, p1 - p0)) / c;
    const float d2 = std::fabs(cross(chord, p2 - p0)) / c;
    return std::max(d1, d2) <= tol;
}

void flatten_cubic_rec(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float tol,
                       std::vector<Vec2>& out, int depth) {
    if (depth >= 16 || cubic_flat(p0, p1, p2, p3, tol)) {
        out.push_back(p3);
        return;
    }
    const Vec2 p01 = (p0 + p1) * 0.5f;
    const Vec2 p12 = (p1 + p2) * 0.5f;
    const Vec2 p23 = (p2 + p3) * 0.5f;
    const Vec2 p012 = (p01 + p12) * 0.5f;
    const Vec2 p123 = (p12 + p23) * 0.5f;
    const Vec2 mid = (p012 + p123) * 0.5f;
    flatten_cubic_rec(p0, p01, p012, mid, tol, out, depth + 1);
    flatten_cubic_rec(mid, p123, p23, p3, tol, out, depth + 1);
}

}  // namespace

void flatten_quadratic(Point p0, Point p1, Point p2, float tolerance,
                       std::vector<Point>& out) {
    if (tolerance < 0.01f) {
        tolerance = 0.01f;  // a sub-hundredth sagitta buys nothing
    }
    flatten_quadratic_rec(p0, p1, p2, tolerance, out, 0);
}

void flatten_cubic(Point p0, Point p1, Point p2, Point p3, float tolerance,
                   std::vector<Point>& out) {
    if (tolerance < 0.01f) {
        tolerance = 0.01f;
    }
    flatten_cubic_rec(p0, p1, p2, p3, tolerance, out, 0);
}

void flatten_arc(Point center, float rx, float ry, float a0, float a1,
                 float tolerance, std::vector<Point>& out) {
    if (!(rx > 0.0f) || !(ry > 0.0f)) {
        return;  // collapsed to the center; the anchor is already there
    }
    if (tolerance < 0.01f) {
        tolerance = 0.01f;
    }
    const float span = a1 - a0;
    if (std::fabs(span) < 1e-6f) {
        return;  // zero span: the anchor is the point
    }
    // The step from the LARGER radius (the worse-curving side of the
    // ellipse), capped in count so a giant radius cannot flood the path.
    const float step = arc_step(std::max(rx, ry), tolerance);
    int steps = static_cast<int>(std::ceil(std::fabs(span) / step));
    steps = std::max(1, std::min(steps, 2048));
    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float a = a0 + span * t;
        out.push_back({center.x + rx * std::cos(a),
                       center.y + ry * std::sin(a)});
    }
}

// --- dashing -------------------------------------------------------------------

void dash_polyline(const Point* pts, int count, const DashSpec& dash,
                   std::vector<std::vector<Point>>& runs) {
    if (count <= 0) {
        return;
    }
    // Solid: one run, the whole input.
    if (dash.count <= 0) {
        runs.emplace_back(pts, pts + count);
        return;
    }
    // SVG's odd-count rule: repeat the array to make pairs. The
    // effective pattern is 2*count entries (capped by kMax doubling).
    float pattern[DashSpec::kMax * 2];
    int pattern_count = dash.count;
    for (int i = 0; i < dash.count; ++i) {
        pattern[i] = dash.lengths[i];
    }
    if (pattern_count % 2 == 1) {
        if (pattern_count * 2 <= DashSpec::kMax * 2) {
            for (int i = 0; i < dash.count; ++i) {
                pattern[pattern_count + i] = dash.lengths[i];
            }
            pattern_count *= 2;
        } else {
            pattern[pattern_count] = 0.0f;  // cap reached: an off of 0
            ++pattern_count;                // closes the run cleanly
        }
    }
    float total = 0.0f;
    for (int i = 0; i < pattern_count; ++i) {
        if (!(pattern[i] > 0.0f)) {
            // A non-positive entry makes the pattern unusable as a
            // rhythm (or an infinite loop below): solid instead.
            runs.emplace_back(pts, pts + count);
            return;
        }
        total += pattern[i];
    }

    // Normalize the phase into [0, total) - negative offsets wrap.
    float phase = std::fmod(dash.offset, total);
    if (phase < 0.0f) {
        phase += total;
    }
    int idx = 0;
    float consumed = 0.0f;
    while (idx < pattern_count && phase >= consumed + pattern[idx]) {
        consumed += pattern[idx];
        ++idx;
    }
    bool on = (idx % 2) == 0;
    float left = pattern[idx] - (phase - consumed);

    std::vector<Point>* run = nullptr;
    if (on) {
        runs.emplace_back();
        run = &runs.back();
        run->push_back(pts[0]);
    }

    for (int i = 1; i < count; ++i) {
        float seg = length(pts[i] - pts[i - 1]);
        // Consume this segment against the pattern; a segment may cross
        // many phases, each crossing either closing a run or opening a
        // new one (possibly at an interpolated point).
        while (seg > 1e-9f) {
            if (left > seg) {
                left -= seg;
                seg = 0.0f;
                if (run != nullptr) {
                    run->push_back(pts[i]);
                }
            } else {
                seg -= left;
                const float seg_len = length(pts[i] - pts[i - 1]);
                const float t = seg > 1e-9f ? 1.0f - seg / seg_len : 1.0f;
                const Vec2 at = {
                    pts[i - 1].x + (pts[i].x - pts[i - 1].x) * t,
                    pts[i - 1].y + (pts[i].y - pts[i - 1].y) * t};
                if (run != nullptr) {
                    // Dedupe the boundary point when the segment's start
                    // already sits exactly there.
                    if (run->empty() ||
                        length(run->back() - at) > 1e-9f) {
                        run->push_back(at);
                    }
                    run = nullptr;  // phase over
                }
                idx = (idx + 1) % pattern_count;
                left = pattern[idx];
                on = !on;
                // Only open a new ON run when there's segment remaining
                // past the boundary. When seg ~ 0 the boundary sits at
                // the endpoint and the new phase has nothing to draw -
                // opening a run here would emit a spurious single-point
                // run at the endpoint (the [10,5] dash pattern on a
                // 30px line would produce a phantom third run at 30).
                if (on && seg > 1e-9f) {
                    runs.emplace_back();
                    run = &runs.back();
                    run->push_back(at);
                }
            }
        }
    }
    // A trailing zero-length ON phase still means a dot at the end.
    if (run != nullptr && run->empty()) {
        run->push_back(pts[count - 1]);
    }
}

// --- stroking -------------------------------------------------------------------

namespace {

// One valid segment of the walk: direction and right normal are
// precomputed; a and b index into the caller's points (kept as points
// too, so the closure segment can be synthetic).
struct StrokeSeg {
    Vec2 a;
    Vec2 b;
    Vec2 d;  // unit direction a -> b
    Vec2 n;  // right normal of d
};

bool seg_valid(Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    return dot(ab, ab) > 1e-12f;
}

// The joined corner at a joint, on side s (+1 = right/-n... the sign is
// the caller's), at radius r: the intersection of the two offset lines.
// Scales linearly with r, so this solves it at unit radius and the
// callers scale. False when the lines are near-parallel (collinear or a
// 180-degree reversal) - the caller falls back to the plain corner.
bool offset_intersection(Vec2 p, Vec2 n_prev, Vec2 n_cur, float s,
                         Vec2& out) {
    // Lines: p + s*n_prev + t*d_prev and p + s*n_cur + u*d_cur. Their
    // direction vectors are the segments' own; here they are reconstructed
    // from the normals (d = rot90ccw(n) in y-down terms: n = (-dy, dx)
    // => d = (ny, -nx)).
    const Vec2 d_prev = {n_prev.y, -n_prev.x};
    const Vec2 d_cur = {n_cur.y, -n_cur.x};
    const float det = -cross(d_prev, d_cur);
    if (std::fabs(det) < 1e-6f) {
        return false;
    }
    const Vec2 rhs = (n_cur * s) - (n_prev * s);
    // Solve t*d_prev - u*d_cur = s*(n_cur - n_prev) by Cramer.
    const float t = cross(rhs, d_cur * -1.0f) / det;
    out = p + (n_prev * s) + (d_prev * t);
    return true;
}

// Emits one quad: four corners, one rgba per corner.
void emit_quad(TessSink& sink, const Vec2 (&p)[4], const Vec4& c0,
               const Vec4& c1, const Vec4& c2, const Vec4& c3) {
    const Vec2 pos[4] = {p[0], p[1], p[2], p[3]};
    const Vec4 rgba[4] = {c0, c1, c2, c3};
    sink.quad(pos, rgba);
}

// The stroker's per-joint resolution: which side is outer, and what the
// outer corner looks like (miter point, per-segment corners for
// bevel/round).
struct Joint {
    bool active = false;    // a real turn (not collinear)
    float outer = 1.0f;     // the outer side's sign on the normal
    bool miter = false;     // outer corner is the miter point (else the
                            // segments' own corners + a bridging band)
    Vec2 miter_at_1{};      // the miter point at radius 1 (outer side)
    Vec2 inner_at_1{};      // the inner trim intersection at radius 1
    bool inner_ok = false;  // inner_at_1 is valid (else collinear)
};

// Resolves the joint between seg_prev (ending at the shared point) and
// seg_cur. hi/ho only matter for the round-join step count, computed by
// the fringe emitter; resolution itself is radius-free.
Joint resolve_joint(const StrokeSeg& prev, const StrokeSeg& cur,
                    const Paint& paint) {
    Joint j;
    const float turn = cross(prev.d, cur.d);
    const float along = dot(prev.d, cur.d);
    if (std::fabs(turn) < 1e-6f && along > 0.0f) {
        j.active = false;  // collinear: nothing to fill, nothing to trim
        j.inner_ok = false;
        return j;
    }
    j.active = true;
    j.outer = turn > 0.0f ? -1.0f : 1.0f;
    // Outer miter: the offset-line intersection on the outer side. The
    // miter LENGTH at radius r is |miter_at_1| * r, so the classic
    // ratio (miter length / stroke width = |m| * r / 2r) is |m| / 2.
    Vec2 m{};
    if (offset_intersection(cur.a, prev.n, cur.n, j.outer, m)) {
        const float ratio = length(m - cur.a) * 0.5f;
        if (ratio <= paint.miter_limit) {
            j.miter = true;
            j.miter_at_1 = m - cur.a;  // relative to the joint point
        }
    }
    // Inner trim: the intersection on the inner side (radius-free).
    j.inner_ok =
        offset_intersection(cur.a, prev.n, cur.n, -j.outer, j.inner_at_1);
    if (j.inner_ok) {
        j.inner_at_1 = j.inner_at_1 - cur.a;
    }
    return j;
}

}  // namespace

void stroke_polyline(const Point* pts, int count, const Paint& paint,
                     TessSink& sink, bool closed) {
    if (count <= 0 || !(paint.width > 0.0f)) {
        return;
    }

    const Vec4 col = unpack_color(paint.color);

    const float half = paint.width * 0.5f;
    const float ho = half + 0.5f;              // outer fringe radius
    const float hi = std::max(half - 0.5f, 0.0f);  // core radius
    const bool hairline = half < 0.5f;
    // Named alpha_in, not a_in: the join bands below declare corner
    // points named a_in/b_in (A/B at the inner radius), and MSVC /W4
    // (C4456) rejects the shadow even though the types differ.
    const float alpha_in = hairline
                               ? std::min(paint.width, 1.0f)
                               : 1.0f;  // the fringe's inner alpha
    const Vec4 c_in{col.x, col.y, col.z, alpha_in};
    const Vec4 c_out{col.x, col.y, col.z, 0.0f};
    const Vec4 c_solid = col;

    // The one-point polyline: cap geometry only, both caps at the same
    // point (round makes the dot, square the square, butt nothing).
    // The direction is arbitrary - there is no segment to derive one
    // from. A butt cap on a single point is nothing at all (a flat edge
    // of zero width) - the header's contract.
    if (count == 1) {
        if (paint.cap == StrokeCap::butt) {
            return;
        }
        StrokeSeg s{};
        s.a = pts[0];
        s.b = pts[0];
        s.d = {1.0f, 0.0f};
        s.n = {0.0f, 1.0f};
        // The caps emit their fringe bands; a core would be nothing
        // (both degenerate). One helper, two calls: the end cap walks
        // its arc through +d, the start cap through -d - two
        // half-geometries back to back make the full dot/square.
        auto cap_bands = [&](Vec2 v, Vec2 d) {
            const Vec2 n = {0.0f, 1.0f};
            if (paint.cap == StrokeCap::butt) {
                if (hairline) {
                    const Vec2 q[4] = {v, v, v - n * ho,
                                        v + n * ho};
                    emit_quad(sink, q, c_in, c_in, c_out, c_out);
                } else {
                    const Vec2 q[4] = {v + n * hi,
                                        v - n * hi,
                                        v - n * ho,
                                        v + n * ho};
                    emit_quad(sink, q, c_in, c_in, c_out, c_out);
                }
            } else if (paint.cap == StrokeCap::square) {
                const Vec2 e = d * half;
                const Vec2 p0 = v + n * hi;
                const Vec2 p1 = (v + e) + n * hi;
                const Vec2 p2 = (v + e) - n * hi;
                const Vec2 p3 = v - n * hi;
                const Vec2 q0o = v + n * ho;
                const Vec2 q1o = (v + e) + n * ho;
                const Vec2 q2o = (v + e) - n * ho;
                const Vec2 q3o = v - n * ho;
                const Vec2 side_a[4] = {p0, p1, q1o, q0o};
                emit_quad(sink, side_a, c_in, c_in, c_out, c_out);
                const Vec2 far[4] = {p1, p2, q2o, q1o};
                emit_quad(sink, far, c_in, c_in, c_out, c_out);
                const Vec2 side_b[4] = {p2, p3, q3o, q2o};
                emit_quad(sink, side_b, c_in, c_in, c_out, c_out);
            } else {  // round: the half-disc fan from v, through +d
                // A fan of triangles from v out to the outer radius:
                // v carries the inner alpha (solid for thick strokes,
                // the coverage-honest peak for hairlines), the arc edge
                // fades to 0. This fills the dot (v is a corner of every
                // fan quad) at the cost of approximate AA on thick
                // strokes - the fade spans the cap radius, not 1px.
                const float a0 = std::atan2(n.y, n.x);
                const float span = -kPi;
                const float step = arc_step(ho, 0.25f);
                int steps = static_cast<int>(
                    std::ceil(std::fabs(span) / step));
                steps = std::max(1, std::min(steps, 64));
                Vec2 prev_out = v + n * ho;
                for (int i = 1; i <= steps; ++i) {
                    const float t =
                        static_cast<float>(i) / static_cast<float>(steps);
                    const float a = a0 + span * t;
                    const Vec2 dir = {std::cos(a), std::sin(a)};
                    const Vec2 cur_out = v + dir * ho;
                    const Vec2 q[4] = {v, prev_out, cur_out, cur_out};
                    emit_quad(sink, q, c_in, c_out, c_out, c_out);
                    prev_out = cur_out;
                }
            }
        };
        cap_bands(pts[0], {1.0f, 0.0f});
        cap_bands(pts[0], {-1.0f, 0.0f});
        return;
    }

    // ---- the walk ---------------------------------------------------------------
    // Valid segments in order (zero-length spans skipped); for a closed
    // path whose last point differs from its first, one synthetic
    // closing segment joins them. The walk keeps the FIRST valid segment
    // so the closing joint can reference it.
    //   segs are generated lazily by advancing a cursor; "next valid
    //   segment after index i" = the pair (i, j) with j the first index
    //   > i whose span from pts[i] is non-degenerate... rather: advance
    //   i to the first point that starts a valid span, then j to the
    //   first point after i that ends it.

    // Collect valid segments into a local buffer. A closed path may add
    // one synthetic closer; an open path walks them straight. Bounded by
    // count+1.
    // (The vector is the stroker's one allocation; the 100k stress uses
    // short primitives, and long paths stroke once per frame - amortized
    // fine against the per-quad math it feeds.)
    std::vector<StrokeSeg> segs;
    segs.reserve(static_cast<std::size_t>(count));
    {
        int i = 0;
        while (i < count - 1) {
            if (!seg_valid(pts[i], pts[i + 1])) {
                ++i;
                continue;
            }
            StrokeSeg s;
            s.a = pts[i];
            s.b = pts[i + 1];
            s.d = (s.b - s.a) * (1.0f / length(s.b - s.a));
            s.n = right_normal(s.d);
            segs.push_back(s);
            ++i;
        }
    }
    if (segs.empty()) {
        // Every span was degenerate: the whole path is one point.
        stroke_polyline(pts, 1, paint, sink, false);
        return;
    }
    if (closed) {
        const StrokeSeg& last = segs.back();
        if (length(last.b - segs.front().a) > 1e-9f) {
            StrokeSeg s;
            s.a = last.b;
            s.b = segs.front().a;
            s.d = (s.b - s.a) * (1.0f / length(s.b - s.a));
            s.n = right_normal(s.d);
            segs.push_back(s);
        }
    }
    const int n = static_cast<int>(segs.size());
    const bool loop = closed;  // joints run cyclically

    // ---- pass 1: the core (skipped for hairlines) ------------------------------
    if (!hairline) {
        for (int i = 0; i < n; ++i) {
            const StrokeSeg& s = segs[i];
            const Vec2 q[4] = {s.a + s.n * hi,
                                s.a - s.n * hi,
                                s.b - s.n * hi,
                                s.b + s.n * hi};
            emit_quad(sink, q, c_solid, c_solid, c_solid, c_solid);
        }
        // Joint wedges between consecutive segments (cyclically when
        // closed). The notch triangle (A, P, B) fills every style's
        // base; miter adds (A, M, B); round replaces the base with the
        // fan.
        for (int i = 0; i < n; ++i) {
            const int next = loop ? (i + 1) % n : i + 1;
            if (!loop && next >= n) {
                break;
            }
            const Joint j = resolve_joint(segs[i], segs[next], paint);
            if (!j.active) {
                continue;
            }
            const Vec2 p = segs[next].a;
            const Vec2 a = p + segs[i].n * (j.outer * hi);
            const Vec2 b = p + segs[next].n * (j.outer * hi);
            if (paint.join == StrokeJoin::round) {
                // Fan around p from a to b through the outer side.
                const float a0 = std::atan2(a.y - p.y, a.x - p.x);
                const float a1 = std::atan2(b.y - p.y, b.x - p.x);
                float span = a1 - a0;
                // rotate the short outer way: sign matches the turn
                if (span > kPi) {
                    span -= 2.0f * kPi;
                }
                if (span < -kPi) {
                    span += 2.0f * kPi;
                }
                (void)a1;
                const float step = arc_step(hi, 0.25f);
                int steps = static_cast<int>(
                    std::ceil(std::fabs(span) / step));
                steps = std::max(1, std::min(steps, 64));
                Vec2 prev = a;
                for (int k = 1; k <= steps; ++k) {
                    const float t =
                        static_cast<float>(k) / static_cast<float>(steps);
                    const float ang = a0 + span * t;
                    const Vec2 cur = p + Vec2{std::cos(ang), std::sin(ang)} * hi;
                    const Vec2 q[4] = {p, prev, cur, cur};
                    emit_quad(sink, q, c_solid, c_solid, c_solid, c_solid);
                    prev = cur;
                }
            } else {
                // The base wedge (A, P, B) - the bevel's whole join,
                // the miter's foundation.
                const Vec2 q[4] = {a, p, b, b};
                emit_quad(sink, q, c_solid, c_solid, c_solid, c_solid);
                // The miter wedge is ONLY for miter joins - bevel stops
                // at the base wedge. (resolve_joint still computes
                // j.miter for the fringe pass below, but the core only
                // adds the wedge when the paint actually calls for it.)
                if (paint.join == StrokeJoin::miter && j.miter) {
                    const Vec2 m = p + j.miter_at_1 * hi;
                    const Vec2 qm[4] = {a, m, b, b};
                    emit_quad(sink, qm, c_solid, c_solid, c_solid, c_solid);
                }
            }
        }
    }

    // ---- pass 2: the fringes ---------------------------------------------------
    // Longitudinal bands: one per segment per side, corners trimmed at
    // the joints (inner side: the offset intersection; outer side: the
    // miter point when mitred, else the segment's own corner - the
    // join/cap bands bridge from there).
    // The joined START corner of segment i on side s (at radius r):
    //   joint between seg[i-1] and seg[i]; for i == 0 (open): the cap
    //   corner = a + s*n*r.
    auto corner_at = [&](int seg_index, bool at_end, float s, float r,
                         Vec2& out) {
        const StrokeSeg& sref = segs[seg_index];
        // which joint bounds this end?
        int jprev;
        if (at_end) {
            jprev = seg_index;  // joint between seg_index and its next
        } else {
            jprev = loop ? (seg_index - 1 + n) % n : seg_index - 1;
        }
        const bool has_joint =
            at_end ? (loop || seg_index + 1 < n) : jprev >= 0;
        if (!has_joint) {
            // an open end: the segment's own corner
            const Vec2 v = at_end ? sref.b : sref.a;
            out = v + sref.n * (s * r);
            return;
        }
        const Joint j = resolve_joint(segs[jprev], segs[at_end ? (jprev + 1) % n : seg_index], paint);
        const Vec2 v = at_end ? segs[jprev].b : segs[seg_index].a;
        if (!j.active) {
            out = v + sref.n * (s * r);
            return;
        }
        if (s == j.outer) {
            if (j.miter) {
                out = v + j.miter_at_1 * r;
            } else {
                out = v + sref.n * (s * r);
            }
        } else {
            if (j.inner_ok) {
                out = v + j.inner_at_1 * r;
            } else {
                out = v + sref.n * (s * r);
            }
        }
    };

    for (int i = 0; i < n; ++i) {
        for (int side = 0; side < 2; ++side) {
            const float sgn = side == 0 ? 1.0f : -1.0f;
            Vec2 i0{}, i1{}, o0{}, o1{};
            corner_at(i, false, sgn, hi, i0);
            corner_at(i, true, sgn, hi, i1);
            corner_at(i, false, sgn, ho, o0);
            corner_at(i, true, sgn, ho, o1);
            const Vec2 q[4] = {i0, i1, o1, o0};
            emit_quad(sink, q, c_in, c_in, c_out, c_out);
        }
    }

    // Join bands on the outer side (bevel edge / miter edges / round
    // arc), spanning [hi, ho] like every fringe. Hairlines skip them:
    // no core means no join geometry to bridge, and the longitudinal
    // bands already cross at the joint.
    if (!hairline) {
        for (int i = 0; i < n; ++i) {
            const int next = loop ? (i + 1) % n : i + 1;
            if (!loop && next >= n) {
                break;
            }
            const Joint j = resolve_joint(segs[i], segs[next], paint);
            if (!j.active) {
                continue;
            }
            const Vec2 p = segs[next].a;
            const Vec2 a_in = p + segs[i].n * (j.outer * hi);
            const Vec2 b_in = p + segs[next].n * (j.outer * hi);
            const Vec2 a_out = p + segs[i].n * (j.outer * ho);
            const Vec2 b_out = p + segs[next].n * (j.outer * ho);
            if (paint.join == StrokeJoin::round) {
                // The arc band, steps sized by the OUTER radius.
                const float a0 =
                    std::atan2(a_in.y - p.y, a_in.x - p.x);
                const float b0 = std::atan2(b_in.y - p.y, b_in.x - p.x);
                float span = b0 - a0;
                if (span > kPi) {
                    span -= 2.0f * kPi;
                }
                if (span < -kPi) {
                    span += 2.0f * kPi;
                }
                const float step = arc_step(ho, 0.25f);
                int steps =
                    static_cast<int>(std::ceil(std::fabs(span) / step));
                steps = std::max(1, std::min(steps, 64));
                Vec2 pin = a_in;
                Vec2 pout = a_out;
                for (int k = 1; k <= steps; ++k) {
                    const float t =
                        static_cast<float>(k) / static_cast<float>(steps);
                    const float ang = a0 + span * t;
                    const Vec2 dir = {std::cos(ang), std::sin(ang)};
                    const Vec2 cin = p + dir * hi;
                    const Vec2 cout = p + dir * ho;
                    const Vec2 q[4] = {pin, cin, cout, pout};
                    emit_quad(sink, q, c_in, c_in, c_out, c_out);
                    pin = cin;
                    pout = cout;
                }
            } else if (paint.join == StrokeJoin::miter && j.miter) {
                const Vec2 m_in = p + j.miter_at_1 * hi;
                const Vec2 m_out = p + j.miter_at_1 * ho;
                const Vec2 qa[4] = {a_in, m_in, m_out, a_out};
                emit_quad(sink, qa, c_in, c_in, c_out, c_out);
                const Vec2 qb[4] = {m_in, b_in, b_out, m_out};
                emit_quad(sink, qb, c_in, c_in, c_out, c_out);
            } else {
                // The bevel edge's band (bevel join, or a miter join
                // whose miter exceeded the limit and fell back).
                const Vec2 q[4] = {a_in, b_in, b_out, a_out};
                emit_quad(sink, q, c_in, c_in, c_out, c_out);
            }
        }
    }

    // Cap bands at the two ends of an open path.
    if (!loop) {
        auto emit_cap = [&](const StrokeSeg& s, bool at_end) {
            const Vec2 v = at_end ? s.b : s.a;
            const Vec2 d = at_end ? s.d : s.d * -1.0f;
            const Vec2 nrm = s.n;
            if (paint.cap == StrokeCap::butt) {
                if (hairline) {
                    const Vec2 q[4] = {v, v, v - nrm * ho,
                                        v + nrm * ho};
                    emit_quad(sink, q, c_in, c_in, c_out, c_out);
                } else {
                    const Vec2 q[4] = {v + nrm * hi,
                                        v - nrm * hi,
                                        v - nrm * ho,
                                        v + nrm * ho};
                    emit_quad(sink, q, c_in, c_in, c_out, c_out);
                }
            } else if (paint.cap == StrokeCap::square) {
                const Vec2 e = d * half;
                const Vec2 p0 = v + nrm * hi;
                const Vec2 p1 = (v + e) + nrm * hi;
                const Vec2 p2 = (v + e) - nrm * hi;
                const Vec2 p3 = v - nrm * hi;
                const Vec2 q0o = v + nrm * ho;
                const Vec2 q1o = (v + e) + nrm * ho;
                const Vec2 q2o = (v + e) - nrm * ho;
                const Vec2 q3o = v - nrm * ho;
                const Vec2 side_a[4] = {p0, p1, q1o, q0o};
                emit_quad(sink, side_a, c_in, c_in, c_out, c_out);
                const Vec2 far[4] = {p1, p2, q2o, q1o};
                emit_quad(sink, far, c_in, c_in, c_out, c_out);
                const Vec2 side_b[4] = {p2, p3, q3o, q2o};
                emit_quad(sink, side_b, c_in, c_in, c_out, c_out);
            } else {  // round: the half-disc fan from v, through +d
                // Same fan as the single-point cap_bands above: v is a
                // corner of every fan quad, the arc edge fades to 0.
                const float a0 = std::atan2(nrm.y, nrm.x);
                const float span = -kPi;
                const float step = arc_step(ho, 0.25f);
                int steps =
                    static_cast<int>(std::ceil(std::fabs(span) / step));
                steps = std::max(1, std::min(steps, 64));
                Vec2 prev_out = v + nrm * ho;
                for (int i = 1; i <= steps; ++i) {
                    const float t =
                        static_cast<float>(i) / static_cast<float>(steps);
                    const float a = a0 + span * t;
                    const Vec2 dir = {std::cos(a), std::sin(a)};
                    const Vec2 cur_out = v + dir * ho;
                    const Vec2 q[4] = {v, prev_out, cur_out, cur_out};
                    emit_quad(sink, q, c_in, c_out, c_out, c_out);
                    prev_out = cur_out;
                }
            }
        };
        emit_cap(segs.front(), false);
        emit_cap(segs.back(), true);
    }
}

// --- filling --------------------------------------------------------------------

void fill_polygon(const Point* pts, int count, uint32_t color, TessSink& sink) {
    if (count < 3) {
        return;
    }
    const Vec4 col = unpack_color(color);
    const Vec4 c_fade{col.x, col.y, col.z, 0.0f};

    // Normalize the winding: area2 > 0 is the canonical direction the
    // clipper below reasons in (a square [0,0],[10,0],[10,10],[0,10]
    // walks it positively). A reversed input is copied once.
    float area2 = 0.0f;
    for (int i = 0; i < count; ++i) {
        const Vec2& a = pts[i];
        const Vec2& b = pts[(i + 1) % count];
        area2 += cross(a, b);
    }
    std::vector<Vec2> work;
    const Vec2* poly = pts;
    int poly_count = count;
    if (area2 < 0.0f) {
        work.assign(pts, pts + count);
        std::reverse(work.begin(), work.end());
        poly = work.data();
    }

    // Drop collinear and duplicate vertices: they are not ears and only
    // confuse containment tests.
    // (built in place in `work2`; avoids mutating the caller's array)
    std::vector<Vec2> clean;
    clean.reserve(static_cast<std::size_t>(poly_count));
    for (int i = 0; i < poly_count; ++i) {
        const Vec2& prev = poly[(i + poly_count - 1) % poly_count];
        const Vec2& cur = poly[i];
        const Vec2& next = poly[(i + 1) % poly_count];
        if (std::fabs(cross(cur - prev, next - cur)) > 1e-9f) {
            clean.push_back(cur);
        }
    }
    if (clean.size() < 3) {
        return;  // a degenerate outline fills nothing
    }
    const int n = static_cast<int>(clean.size());

    // The AA shrink: each vertex moves 0.5px inward along the average of
    // its adjacent edge normals (inward for the canonical winding = the
    // edge direction's RIGHT normal, (−dy, dx) - the square's bottom
    // edge heading east shrinks up into y+), clamped by a quarter of
    // the shorter adjacent edge so a sliver cannot invert. The fringe
    // then spans shrunk edge -> 0.5px OUTSIDE the true edge, alpha
    // 1 -> 0: 50% coverage at the geometric edge.
    std::vector<Vec2> shrunk(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const Vec2& prev = clean[(i + n - 1) % n];
        const Vec2& cur = clean[i];
        const Vec2& next = clean[(i + 1) % n];
        Vec2 e0 = cur - prev;
        Vec2 e1 = next - cur;
        const float l0 = length(e0);
        const float l1 = length(e1);
        e0 = e0 * (1.0f / l0);
        e1 = e1 * (1.0f / l1);
        // Inward normals: the right normal of each edge direction.
        const Vec2 n0 = right_normal(e0);
        const Vec2 n1 = right_normal(e1);
        Vec2 avg = n0 + n1;
        const float al = length(avg);
        if (al > 1e-6f) {
            avg = avg * (1.0f / al);
        } else {
            avg = n1;  // the two edges cancel (a needle vertex): either
        }
        // The vertex moves along avg far enough that each EDGE lands
        // 0.5px inward: the move is 0.5/cos(theta/2), theta the angle
        // between the adjacent edge normals (a right corner moves 0.707
        // so both its edges see the full 0.5; a straight vertex moves
        // exactly 0.5). cos floors at 0.5, so a needle vertex moves at
        // most 1.0px before the edge-length clamp below has its say.
        const float edge_dot = std::max(dot(e0, e1), -1.0f);
        const float cos_half =
            std::max(std::sqrt((1.0f + edge_dot) * 0.5f), 0.5f);
        const float clamp =
            std::min(0.5f / cos_half, std::min(l0, l1) * 0.25f);
        shrunk[static_cast<std::size_t>(i)] = cur + avg * clamp;
    }

    // Ear clipping over the SHRUNK polygon (the core), with the fringe
    // emitted per boundary edge of the ORIGINAL outline. Index list
    // into `clean`/`shrunk` (same indexing).
    std::vector<int> index;
    index.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        index.push_back(i);
    }

    // Point-in-triangle (boundary counts as inside - a vertex resting
    // exactly on an ear's edge still blocks it, the conservative answer
    // that keeps the clipper from producing slivers).
    auto tri_contains = [](Vec2 a, Vec2 b, Vec2 c, Vec2 p) {
        const float d1 = cross(b - a, p - a);
        const float d2 = cross(c - b, p - b);
        const float d3 = cross(a - c, p - c);
        const bool neg = (d1 < 0.0f) || (d2 < 0.0f) || (d3 < 0.0f);
        const bool pos = (d1 > 0.0f) || (d2 > 0.0f) || (d3 > 0.0f);
        return !(neg && pos);
    };

    auto emit_tri = [&](Vec2 a, Vec2 b, Vec2 c) {
        const Vec2 q[4] = {a, b, c, c};
        emit_quad(sink, q, col, col, col, col);
    };

    int guard = 0;
    const int guard_max = 2 * n * n + 8;  // the O(n^2) worst case, doubled
    while (index.size() > 3 && guard++ < guard_max) {
        bool clipped = false;
        const int m = static_cast<int>(index.size());
        for (int i = 0; i < m && !clipped; ++i) {
            const int ia = index[(i + m - 1) % m];
            const int ib = index[i];
            const int ic = index[(i + 1) % m];
            const Vec2& a = shrunk[static_cast<std::size_t>(ia)];
            const Vec2& b = shrunk[static_cast<std::size_t>(ib)];
            const Vec2& c = shrunk[static_cast<std::size_t>(ic)];
            // Convex for the canonical winding: the cross of the corner
            // is positive.
            if (cross(b - a, c - b) <= 0.0f) {
                continue;
            }
            // No other surviving vertex inside the ear.
            bool blocked = false;
            for (int k = 0; k < m && !blocked; ++k) {
                const int ik = index[k];
                if (ik == ia || ik == ib || ik == ic) {
                    continue;
                }
                if (tri_contains(a, b, c,
                                 shrunk[static_cast<std::size_t>(ik)])) {
                    blocked = true;
                }
            }
            if (blocked) {
                continue;
            }
            emit_tri(a, b, c);
            index.erase(index.begin() + i);
            clipped = true;
        }
        if (!clipped) {
            break;  // degenerate remainder: emit it as a fan and stop
        }
    }
    if (index.size() == 3) {
        emit_tri(shrunk[static_cast<std::size_t>(index[0])],
                 shrunk[static_cast<std::size_t>(index[1])],
                 shrunk[static_cast<std::size_t>(index[2])]);
    } else if (index.size() > 3) {
        // The guard tripped (numerically degenerate): fan the remainder
        // around the first vertex - overlapping triangles beat holes.
        for (std::size_t k = 1; k + 1 < index.size(); ++k) {
            emit_tri(shrunk[static_cast<std::size_t>(index[0])],
                     shrunk[static_cast<std::size_t>(index[k])],
                     shrunk[static_cast<std::size_t>(index[k + 1])]);
        }
    }

    // The fringe: one band per ORIGINAL boundary edge, from the shrunk
    // edge (alpha 1... the fill's own alpha) out to 0.5px past the true
    // edge (alpha 0).
    for (int i = 0; i < n; ++i) {
        const Vec2& cur = clean[static_cast<std::size_t>(i)];
        const Vec2& next = clean[static_cast<std::size_t>((i + 1) % n)];
        const Vec2& scur = shrunk[static_cast<std::size_t>(i)];
        const Vec2& snext = shrunk[static_cast<std::size_t>((i + 1) % n)];
        // The outward normal of this edge (inward was the right normal
        // (−dy, dx), so outward is its negation).
        Vec2 e = next - cur;
        const float el = length(e);
        if (el <= 1e-9f) {
            continue;
        }
        e = e * (1.0f / el);
        const Vec2 out_n = {e.y, -e.x};
        const Vec2 o_cur = cur + out_n * 0.5f;
        const Vec2 o_next = next + out_n * 0.5f;
        const Vec2 q[4] = {scur, snext, o_next, o_cur};
        emit_quad(sink, q, col, col, c_fade, c_fade);
    }
}

}  // namespace ws::ui::canvas
