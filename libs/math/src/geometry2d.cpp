// geometry2d.hpp's out-of-line half. Pure float math, no allocations;
// see the header for the contracts (finite inputs assumed, degenerates
// first-class).

#include <ws/math/geometry2d.hpp>

#include <cmath>

namespace ws::math {

float point_segment_distance(Vec2 p, Vec2 a, Vec2 b, Vec2* closest,
                             float* t) {
    const Vec2 ab = b - a;
    const float len2 = ab.x * ab.x + ab.y * ab.y;
    float param;  // the clamped projection parameter, [0, 1]
    if (len2 <= 0.0f) {
        param = 0.0f;  // degenerate: the segment is the point a (== b)
    } else {
        param = ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2;
        if (param < 0.0f) {
            param = 0.0f;
        } else if (param > 1.0f) {
            param = 1.0f;
        }
    }
    const Vec2 c = a + ab * param;
    if (closest != nullptr) {
        *closest = c;
    }
    if (t != nullptr) {
        *t = param;
    }
    const Vec2 d = p - c;
    return std::sqrt(d.x * d.x + d.y * d.y);
}

float polyline_distance(Vec2 p, const Vec2* pts, std::size_t count,
                        std::size_t* segment, float* t, Vec2* closest) {
    if (count == 0) {
        // Nothing to be near. The outs stay untouched (the caller's
        // "where" is meaningless), matching segment_clip_rect's fully-
        // outside contract.
        return std::numeric_limits<float>::infinity();
    }
    if (count == 1) {
        if (segment != nullptr) {
            *segment = 0;
        }
        return point_segment_distance(p, pts[0], pts[0], closest, t);
    }
    float best = std::numeric_limits<float>::infinity();
    std::size_t best_i = 0;
    float best_t = 0.0f;
    Vec2 best_c{};
    for (std::size_t i = 0; i + 1 < count; ++i) {
        Vec2 c;
        float ti;
        const float d = point_segment_distance(p, pts[i], pts[i + 1], &c, &ti);
        if (d < best) {
            best = d;
            best_i = i;
            best_t = ti;
            best_c = c;
        }
    }
    if (segment != nullptr) {
        *segment = best_i;
    }
    if (t != nullptr) {
        *t = best_t;
    }
    if (closest != nullptr) {
        *closest = best_c;
    }
    return best;
}

bool point_in_polygon(Vec2 p, const Vec2* pts, std::size_t count) {
    if (count < 3) {
        return false;
    }
    // The crossing rule: cast a ray toward +x and count the edges it
    // crosses. An edge straddles p.y when its endpoints' y-order
    // disagrees; the crossing x is then interpolated and compared to
    // p.x. The strict/non-strict mix below is the classic formulation -
    // it keeps vertex-touching edges from double-counting, at the price
    // of boundary points being unspecified (the header's contract).
    bool inside = false;
    for (std::size_t i = 0, j = count - 1; i < count; j = i++) {
        const Vec2& a = pts[j];
        const Vec2& b = pts[i];
        const bool straddles =
            (a.y > p.y) != (b.y > p.y);
        if (!straddles) {
            continue;
        }
        const float x_at =
            a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y);
        if (p.x < x_at) {
            inside = !inside;
        }
    }
    return inside;
}

bool segment_clip_rect(Vec2 a, Vec2 b, float rect_x, float rect_y,
                       float rect_w, float rect_h, Vec2* out_a, Vec2* out_b,
                       float* out_t0, float* out_t1) {
    if (rect_w < 0.0f || rect_h < 0.0f) {
        return false;  // a backwards rect clips everything
    }
    // Liang-Barsky: walk the rect's four boundaries as half-plane
    // constraints on the parameter t. p/q per boundary, t entering
    // (p < 0) tightens t0, t leaving (p > 0) tightens t1; p == 0 means
    // the segment runs parallel to that boundary and is out when it sits
    // wholly on the outside half.
    const float right = rect_x + rect_w;
    const float bottom = rect_y + rect_h;
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    float t0 = 0.0f;
    float t1 = 1.0f;
    const float ps[4] = {-dx, dx, -dy, dy};
    const float qs[4] = {a.x - rect_x, right - a.x, a.y - rect_y,
                         bottom - a.y};
    for (int i = 0; i < 4; ++i) {
        if (ps[i] == 0.0f) {
            if (qs[i] < 0.0f) {
                return false;  // parallel and outside this boundary
            }
            continue;
        }
        const float r = qs[i] / ps[i];
        if (ps[i] < 0.0f) {
            if (r > t1) {
                return false;
            }
            if (r > t0) {
                t0 = r;
            }
        } else {
            if (r < t0) {
                return false;
            }
            if (r < t1) {
                t1 = r;
            }
        }
    }
    if (out_a != nullptr) {
        *out_a = a + Vec2{dx, dy} * t0;
    }
    if (out_b != nullptr) {
        *out_b = a + Vec2{dx, dy} * t1;
    }
    if (out_t0 != nullptr) {
        *out_t0 = t0;
    }
    if (out_t1 != nullptr) {
        *out_t1 = t1;
    }
    return true;
}

Box2 bounds_union(Box2 a, Box2 b) {
    Box2 out;
    out.min_x = a.min_x < b.min_x ? a.min_x : b.min_x;
    out.min_y = a.min_y < b.min_y ? a.min_y : b.min_y;
    out.max_x = a.max_x > b.max_x ? a.max_x : b.max_x;
    out.max_y = a.max_y > b.max_y ? a.max_y : b.max_y;
    return out;
}

Box2 bounds_expand(Box2 a, Vec2 p) {
    Box2 out = a;
    if (p.x < out.min_x) {
        out.min_x = p.x;
    }
    if (p.y < out.min_y) {
        out.min_y = p.y;
    }
    if (p.x > out.max_x) {
        out.max_x = p.x;
    }
    if (p.y > out.max_y) {
        out.max_y = p.y;
    }
    return out;
}

Box2 bounds_inflate(Box2 a, float margin) {
    Box2 out;
    out.min_x = a.min_x - margin;
    out.min_y = a.min_y - margin;
    out.max_x = a.max_x + margin;
    out.max_y = a.max_y + margin;
    return out;
}

Box2 bounds_of_points(const Vec2* pts, std::size_t count) {
    Box2 out = kEmptyBounds;
    for (std::size_t i = 0; i < count; ++i) {
        out = bounds_expand(out, pts[i]);
    }
    return out;
}

float arc_step(float r, float tol) {
    // r * (1 - cos(s/2)) = tol  =>  s = 2 * acos(1 - tol/r).
    // Degenerate radii (r <= tol) buy the coarse quarter-circle step:
    // the arc is sharper than the tolerance, so a coarse step keeps the
    // point count bounded while staying finite. A tiny floor on the step
    // keeps a near-straight giant-radius arc from flooding the path.
    if (r <= tol) {
        return 1.57079637f;  // pi/2: coarse but finite
    }
    const float ratio = std::max(-1.0f, std::min(1.0f, 1.0f - tol / r));
    const float step = 2.0f * std::acos(ratio);
    return std::max(step, 0.0001f);
}

}  // namespace ws::math
