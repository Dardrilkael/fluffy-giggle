// canvas2d.hpp's implementation. Both concrete canvases share the same
// affine transform (defined in the header), path accumulation and
// curve-flattening logic. The concrete classes differ only in where
// the tessellated quads land: a recorded command list for tests,
// r2d::draw_vertex_quad for the live renderer.
//
// The transform is an affine [a b c d e f] mapping local (x, y) to
// screen (a*x + c*y + e, b*x + d*y + f). The stack starts with the
// identity; push saves a copy, pop restores. The view_scale is the
// transform's scale factor (sqrt(|det|)) times the camera zoom -
// the canvas uses it to flatten curves at the right screen-space
// tolerance.

#include <ws/ui/canvas/canvas2d.hpp>

#include <algorithm>
#include <cmath>

#include <ws/math/geometry2d.hpp>

namespace ws::ui::canvas {

// The affine rotate (defined out-of-line per the header's declaration).
void Affine::rotate(float rad) {
    const float cs = std::cos(rad);
    const float sn = std::sin(rad);
    const float na = a * cs - b * sn;
    const float nb = a * sn + b * cs;
    const float nc = c * cs - d * sn;
    const float nd = c * sn + d * cs;
    a = na; b = nb; c = nc; d = nd;
}

namespace {

using ws::math::Vec2;
using ws::math::Vec4;

// The flattening tolerance in screen pixels. 0.25 is the middle of the
// sensible band: fine enough that a circle looks smooth at 1x zoom,
// coarse enough that a 100k-primitive stress test stays fast.
constexpr float kScreenTol = 0.25f;

// Builds a 32-segment circle polyline in local space.
std::vector<Vec2> circle_points(Vec2 center, float r) {
    std::vector<Vec2> pts(32);
    for (int i = 0; i < 32; ++i) {
        const float a = kTwoPi * i / 32.0f;
        pts[i] = {center.x + r * std::cos(a), center.y + r * std::sin(a)};
    }
    return pts;
}

// True when a and b sit within a float-noise whisker of each other.
bool pts_near(Vec2 a, Vec2 b) {
    return std::fabs(a.x - b.x) <= 1e-6f && std::fabs(a.y - b.y) <= 1e-6f;
}

}  // namespace

// ============================================================================
// CommandListCanvas
// ============================================================================

void CommandListCanvas::push_transform() {
    if (xform_stack_.size() < 32) xform_stack_.push_back(xform_stack_.back());
}
void CommandListCanvas::pop_transform() {
    if (xform_stack_.size() > 1) xform_stack_.pop_back();
}
void CommandListCanvas::translate(float dx, float dy) {
    xform_stack_.back().translate(dx, dy);
}
void CommandListCanvas::scale(float sx, float sy) {
    xform_stack_.back().scale(sx, sy);
}
void CommandListCanvas::rotate(float rad) {
    xform_stack_.back().rotate(rad);
}

void CommandListCanvas::push_clip(float x, float y, float w, float h) {
    // All four corners: under a rotated transform the fourth (br) can
    // extend past the min/max of the other three (a 45-degree turn
    // sticks it half a diagonal out), and a clip that misses it would
    // silently eat content. For the axis-aligned pan/zoom transforms
    // this is the exact same rect the three-corner form produced.
    const Vec2 tl = to_screen({x, y});
    const Vec2 tr = to_screen({x + w, y});
    const Vec2 bl = to_screen({x, y + h});
    const Vec2 br = to_screen({x + w, y + h});
    const float minx = std::min({tl.x, tr.x, bl.x, br.x});
    const float miny = std::min({tl.y, tr.y, bl.y, br.y});
    const float maxx = std::max({tl.x, tr.x, bl.x, br.x});
    const float maxy = std::max({tl.y, tr.y, bl.y, br.y});
    CanvasCmd c;
    c.kind = CanvasCmdKind::push_clip;
    c.x = minx; c.y = miny; c.w = maxx - minx; c.h = maxy - miny;
    cmds.push_back(std::move(c));
}

void CommandListCanvas::pop_clip() {
    CanvasCmd c;
    c.kind = CanvasCmdKind::pop_clip;
    cmds.push_back(std::move(c));
}

void CommandListCanvas::begin_path() {
    path_.clear(); substarts_.clear();
    has_path_ = false; path_closed_ = false;
}
void CommandListCanvas::move_to(Point p) {
    // A move_to opens a new subpath: fill and the dash walk treat the
    // ranges between substarts_ as independent shapes.
    substarts_.push_back(static_cast<int>(path_.size()));
    path_.push_back(p); has_path_ = true;
}
void CommandListCanvas::line_to(Point p) {
    if (!has_path_) { move_to(p); return; }
    path_.push_back(p);
}
void CommandListCanvas::quadratic_to(Point p1, Point p2) {
    if (!has_path_) { move_to(p1); return; }
    const float tol = kScreenTol / view_scale();
    flatten_quadratic(path_.back(), p1, p2, tol, path_);
}
void CommandListCanvas::cubic_to(Point p1, Point p2, Point p3) {
    if (!has_path_) { move_to(p1); return; }
    const float tol = kScreenTol / view_scale();
    flatten_cubic(path_.back(), p1, p2, p3, tol, path_);
}
void CommandListCanvas::arc(Point center, float rx, float ry, float a0, float a1) {
    if (!(rx > 0.0f) || !(ry > 0.0f)) return;
    const Vec2 start = {center.x + rx * std::cos(a0),
                        center.y + ry * std::sin(a0)};
    if (!has_path_) {
        move_to(start);
    } else {
        const Vec2 last = path_.back();
        if (std::fabs(last.x - start.x) > 1e-6f ||
            std::fabs(last.y - start.y) > 1e-6f) {
            line_to(start);
        }
    }
    const float tol = kScreenTol / view_scale();
    flatten_arc(center, rx, ry, a0, a1, tol, path_);
}
void CommandListCanvas::close_path() {
    if (has_path_ && !path_.empty()) {
        // The ring's OWN start, not the last move_to: a path may carry
        // several move_tos (the rounded rects' per-corner jumps), and
        // closing to the latest one would splice a phantom chord
        // across the shape instead of finishing the ring. Both
        // consumers already define closure as "back joins front" - the
        // stroker's synthetic closing segment and fill's ring walk -
        // so the first point is the only close target they agree on.
        path_.push_back(path_.front());
        path_closed_ = true;
    }
}

void CommandListCanvas::stroke(const Paint& paint) {
    if (!has_path_ || path_.empty()) return;
    // SOLID: the whole accumulated path is one chained polyline (the
    // documented solid stroke - the move_to connectors are drawn), so
    // the dash machinery is skipped entirely: no runs vector, no
    // per-run copy of the path. The single run trivially wraps (it
    // starts at the path's first point and ends at its last), so the
    // closed flag is the path's own.
    if (paint.dash.count <= 0) {
        CanvasCmd c;
        c.kind = CanvasCmdKind::stroke_polyline;
        c.points.reserve(path_.size());
        for (const auto& p : path_) c.points.push_back(to_screen(p));
        c.color = paint.color;
        c.width = paint.width;
        c.closed = path_closed_;
        c.join = paint.join;
        c.cap = paint.cap;
        cmds.push_back(std::move(c));
        path_.clear(); substarts_.clear();
        has_path_ = false; path_closed_ = false;
        return;
    }
    // DASHED: the pattern walks each SUBPATH - it never runs across a
    // move_to's jump (two disjoint rings dash as two rings, no phantom
    // connector dashes). A run closes only when it wraps its own
    // subpath's ring: it started at the subpath's first point AND
    // walked to its last (the close_path spelling's duplicate, or the
    // ring's final vertex). Every other run is an open fragment.
    std::vector<std::vector<Vec2>> runs;
    for (std::size_t s = 0; s < substarts_.size(); ++s) {
        const int first = substarts_[s];
        const int last = s + 1 == substarts_.size()
                             ? static_cast<int>(path_.size())
                             : substarts_[s + 1];
        if (last - first <= 0) continue;
        dash_polyline(path_.data() + first, last - first, paint.dash, runs);
        for (const auto& run : runs) {
            if (run.empty()) continue;
            const bool run_closed =
                path_closed_ && pts_near(run.front(), path_[first]) &&
                pts_near(run.back(), path_[last - 1]);
            CanvasCmd c;
            c.kind = CanvasCmdKind::stroke_polyline;
            c.points.reserve(run.size());
            for (const auto& p : run) c.points.push_back(to_screen(p));
            c.color = paint.color;
            c.width = paint.width;
            c.closed = run_closed;
            c.join = paint.join;
            c.cap = paint.cap;
            cmds.push_back(std::move(c));
        }
        runs.clear();  // per subpath: the ranges never share runs
    }
    path_.clear(); substarts_.clear();
    has_path_ = false; path_closed_ = false;
}

void CommandListCanvas::fill(std::uint32_t color) {
    if (!has_path_ || path_.size() < 3) {
        path_.clear(); substarts_.clear();
        has_path_ = false; path_closed_ = false;
        return;
    }
    // Every SUBPATH fills as its own simple polygon (the header's
    // contract): two rings in one path never share an edge. Before the
    // substarts were tracked the whole accumulated path went to the ear
    // clipper as one self-intersecting ring - two disjoint squares
    // filled through a phantom diagonal. A subpath with fewer than
    // three points is skipped.
    for (std::size_t s = 0; s < substarts_.size(); ++s) {
        const int first = substarts_[s];
        const int last = s + 1 == substarts_.size()
                             ? static_cast<int>(path_.size())
                             : substarts_[s + 1];
        if (last - first < 3) continue;
        CanvasCmd c;
        c.kind = CanvasCmdKind::fill_polygon;
        c.points.reserve(static_cast<std::size_t>(last - first));
        for (int i = first; i < last; ++i)
            c.points.push_back(to_screen(path_[i]));
        c.color = color;
        cmds.push_back(std::move(c));
    }
    path_.clear(); substarts_.clear();
    has_path_ = false; path_closed_ = false;
}

void CommandListCanvas::stroke_line(Point a, Point b, const Paint& paint) {
    const Point pts[] = {a, b};
    stroke_polyline(pts, 2, paint, false);
}

void CommandListCanvas::stroke_polyline(const Point* pts, int count,
                                        const Paint& paint, bool closed) {
    begin_path();
    for (int i = 0; i < count; ++i) {
        if (i == 0) move_to(pts[i]); else line_to(pts[i]);
    }
    path_closed_ = closed;
    stroke(paint);
}

void CommandListCanvas::fill_polygon(const Point* pts, int count,
                                     std::uint32_t color) {
    begin_path();
    for (int i = 0; i < count; ++i) {
        if (i == 0) move_to(pts[i]); else line_to(pts[i]);
    }
    close_path();
    fill(color);
}

void CommandListCanvas::stroke_circle(Point center, float r, const Paint& paint) {
    auto pts = circle_points(center, r);
    stroke_polyline(pts.data(), static_cast<int>(pts.size()), paint, true);
}

void CommandListCanvas::fill_circle(Point center, float r, std::uint32_t color) {
    auto pts = circle_points(center, r);
    fill_polygon(pts.data(), static_cast<int>(pts.size()), color);
}

void CommandListCanvas::stroke_rounded_rect(float x, float y, float w, float h,
                                            float r, const Paint& paint) {
    begin_path();
    // A negative radius clamps to 0 (the SVG rule): unfloored it
    // displaces the arc centers OUTSIDE the rect and the skipped arcs
    // splice a bowtie instead of the rect's outline.
    const float rad = std::clamp(r, 0.0f, std::min(w, h) * 0.5f);
    const Point corners[4] = {{x + w - rad, y + rad}, {x + w - rad, y + h - rad},
                              {x + rad, y + h - rad}, {x + rad, y + rad}};
    const float angles[4][2] = {{-kHalfPi, 0.0f}, {0.0f, kHalfPi},
                                 {kHalfPi, kPi}, {kPi, 1.5f * kPi}};
    // ONE ring: the first arc's start opens it and every later arc
    // line_to's to its own start (arc's own contract) - the connecting
    // segments are the rect's straight sides. The old four-move_to
    // spelling leaned on the solid stroke chaining across jumps, and
    // made fill's subpath-independent contract impossible to honor.
    // The clamp's floor is special: at radius 0 the arcs collapse
    // (arc() no-ops on a non-positive radius), so the sharp ring is
    // built explicitly - the same corner walk the arcs' starts traced.
    if (rad <= 0.0f) {
        move_to({x + w, y});
        line_to({x + w, y + h});
        line_to({x, y + h});
        line_to({x, y});
    } else {
        move_to({corners[0].x + rad * std::cos(angles[0][0]),
                 corners[0].y + rad * std::sin(angles[0][0])});
        for (int i = 0; i < 4; ++i) {
            arc(corners[i], rad, rad, angles[i][0], angles[i][1]);
        }
    }
    close_path();
    stroke(paint);
}

void CommandListCanvas::fill_rounded_rect(float x, float y, float w, float h,
                                          float r, std::uint32_t color) {
    begin_path();
    // See stroke_rounded_rect: the negative clamp, and the ONE-ring
    // arc walk (a single subpath, so the fill is one polygon); the
    // radius-0 floor builds the sharp ring explicitly.
    const float rad = std::clamp(r, 0.0f, std::min(w, h) * 0.5f);
    const Point corners[4] = {{x + w - rad, y + rad}, {x + w - rad, y + h - rad},
                              {x + rad, y + h - rad}, {x + rad, y + rad}};
    const float angles[4][2] = {{-kHalfPi, 0.0f}, {0.0f, kHalfPi},
                                 {kHalfPi, kPi}, {kPi, 1.5f * kPi}};
    if (rad <= 0.0f) {
        move_to({x + w, y});
        line_to({x + w, y + h});
        line_to({x, y + h});
        line_to({x, y});
    } else {
        move_to({corners[0].x + rad * std::cos(angles[0][0]),
                 corners[0].y + rad * std::sin(angles[0][0])});
        for (int i = 0; i < 4; ++i) {
            arc(corners[i], rad, rad, angles[i][0], angles[i][1]);
        }
    }
    close_path();
    fill(color);
}

void CommandListCanvas::fill_rects(const Point* corners, int count, float w,
                                   float h, std::uint32_t color) {
    if (count <= 0 || !(w > 0.0f) || !(h > 0.0f)) {
        return;
    }
    CanvasCmd c;
    c.kind = CanvasCmdKind::fill_rects;
    c.points.resize(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        c.points[static_cast<std::size_t>(i)] = to_screen(corners[i]);
    }
    c.w = w;
    c.h = h;
    c.color = color;
    cmds.push_back(std::move(c));
}

float CommandListCanvas::draw_text(float x, float y, const char* utf8,
                                   const r2d::Font& font, std::uint32_t color) {
    const Point screen = to_screen({x, y});
    CanvasCmd c;
    c.kind = CanvasCmdKind::draw_text;
    c.x = screen.x; c.y = screen.y;
    c.text = utf8 ? utf8 : "";
    c.color = color;
    cmds.push_back(std::move(c));
    return font.measure_text(utf8) * view_scale();
}

float CommandListCanvas::measure_text(const char* utf8, const r2d::Font& font) {
    return font.measure_text(utf8) * view_scale();
}

void CommandListCanvas::draw_image(float x, float y, float w, float h,
                                   r2d::Texture texture, std::uint32_t color) {
    const Point tl = to_screen({x, y});
    const Point br = to_screen({x + w, y + h});
    CanvasCmd c;
    c.kind = CanvasCmdKind::draw_image;
    c.x = tl.x; c.y = tl.y;
    c.w = br.x - tl.x; c.h = br.y - tl.y;
    c.texture = texture;
    c.color = color;
    cmds.push_back(std::move(c));
}

std::size_t CommandListCanvas::count(CanvasCmdKind kind) const {
    std::size_t n = 0;
    for (const auto& c : cmds) if (c.kind == kind) ++n;
    return n;
}

const CanvasCmd* CommandListCanvas::first(CanvasCmdKind kind) const {
    for (const auto& c : cmds) if (c.kind == kind) return &c;
    return nullptr;
}

Point CommandListCanvas::to_screen(Point p) const {
    return xform_stack_.back().apply(p);
}

// ============================================================================
// Renderer2DCanvas
// ============================================================================

void Renderer2DCanvas::push_transform() {
    if (xform_stack_.size() < 32) xform_stack_.push_back(xform_stack_.back());
}
void Renderer2DCanvas::pop_transform() {
    if (xform_stack_.size() > 1) xform_stack_.pop_back();
}
void Renderer2DCanvas::translate(float dx, float dy) {
    xform_stack_.back().translate(dx, dy);
}
void Renderer2DCanvas::scale(float sx, float sy) {
    xform_stack_.back().scale(sx, sy);
}
void Renderer2DCanvas::rotate(float rad) {
    xform_stack_.back().rotate(rad);
}

void Renderer2DCanvas::push_clip(float x, float y, float w, float h) {
    // All four corners - see CommandListCanvas::push_clip (the br
    // corner is the one a rotation sticks out past the other three).
    const Vec2 tl = to_screen({x, y});
    const Vec2 tr = to_screen({x + w, y});
    const Vec2 bl = to_screen({x, y + h});
    const Vec2 br = to_screen({x + w, y + h});
    const float minx = std::min({tl.x, tr.x, bl.x, br.x});
    const float miny = std::min({tl.y, tr.y, bl.y, br.y});
    const float maxx = std::max({tl.x, tr.x, bl.x, br.x});
    const float maxy = std::max({tl.y, tr.y, bl.y, br.y});
    r2d::push_clip(minx, miny, maxx - minx, maxy - miny);
}

void Renderer2DCanvas::pop_clip() { r2d::pop_clip(); }

void Renderer2DCanvas::begin_path() {
    path_.clear(); substarts_.clear();
    has_path_ = false; path_closed_ = false;
}
void Renderer2DCanvas::move_to(Point p) {
    // A move_to opens a new subpath (see the header): fill and the
    // dash walk treat the ranges between substarts_ as independent.
    substarts_.push_back(static_cast<int>(path_.size()));
    path_.push_back(p); has_path_ = true;
}
void Renderer2DCanvas::line_to(Point p) {
    if (!has_path_) { move_to(p); return; }
    path_.push_back(p);
}
void Renderer2DCanvas::quadratic_to(Point p1, Point p2) {
    if (!has_path_) { move_to(p1); return; }
    const float tol = kScreenTol / view_scale();
    flatten_quadratic(path_.back(), p1, p2, tol, path_);
}
void Renderer2DCanvas::cubic_to(Point p1, Point p2, Point p3) {
    if (!has_path_) { move_to(p1); return; }
    const float tol = kScreenTol / view_scale();
    flatten_cubic(path_.back(), p1, p2, p3, tol, path_);
}
void Renderer2DCanvas::arc(Point center, float rx, float ry, float a0, float a1) {
    if (!(rx > 0.0f) || !(ry > 0.0f)) return;
    const Vec2 start = {center.x + rx * std::cos(a0),
                        center.y + ry * std::sin(a0)};
    if (!has_path_) {
        move_to(start);
    } else {
        const Vec2 last = path_.back();
        if (std::fabs(last.x - start.x) > 1e-6f ||
            std::fabs(last.y - start.y) > 1e-6f) {
            line_to(start);
        }
    }
    const float tol = kScreenTol / view_scale();
    flatten_arc(center, rx, ry, a0, a1, tol, path_);
}
void Renderer2DCanvas::close_path() {
    if (has_path_ && !path_.empty()) {
        // See CommandListCanvas::close_path: close to the ring's start,
        // never to the last move_to (the rounded rects' per-corner
        // move_tos would otherwise close the ring across a phantom
        // chord through the shape's interior).
        path_.push_back(path_.front());
        path_closed_ = true;
    }
}

void Renderer2DCanvas::stroke(const Paint& paint) {
    if (!has_path_ || path_.empty()) return;
    // SOLID: one chained polyline straight to the tessellator. The
    // transform runs IN PLACE over the consumed path (fill/stroke
    // clear it afterwards anyway), so a steady frame's solid strokes
    // cost no allocation at all - the path buffer's capacity is the
    // canvas's, reused call after call.
    if (paint.dash.count <= 0) {
        for (auto& p : path_) p = to_screen(p);
        ws::ui::canvas::stroke_polyline(path_.data(),
                                        static_cast<int>(path_.size()),
                                        paint, sink_, path_closed_);
        path_.clear(); substarts_.clear();
        has_path_ = false; path_closed_ = false;
        return;
    }
    // DASHED: the pattern walks each SUBPATH (never across a move_to's
    // jump), each run transformed and tessellated through the sink.
    // Same wrap-the-ring rule as CommandListCanvas::stroke: dash
    // fragments stroke open (their caps, no phantom chords), a run
    // that wraps its own subpath's ring keeps its cyclic joints.
    std::vector<std::vector<Vec2>> runs;
    for (std::size_t s = 0; s < substarts_.size(); ++s) {
        const int first = substarts_[s];
        const int last = s + 1 == substarts_.size()
                             ? static_cast<int>(path_.size())
                             : substarts_[s + 1];
        if (last - first <= 0) continue;
        dash_polyline(path_.data() + first, last - first, paint.dash, runs);
        for (const auto& run : runs) {
            if (run.empty()) continue;
            const bool run_closed =
                path_closed_ && pts_near(run.front(), path_[first]) &&
                pts_near(run.back(), path_[last - 1]);
            std::vector<Vec2> screen_run(run.size());
            for (std::size_t i = 0; i < run.size(); ++i)
                screen_run[i] = to_screen(run[i]);
            ws::ui::canvas::stroke_polyline(screen_run.data(),
                                            static_cast<int>(screen_run.size()),
                                            paint, sink_, run_closed);
        }
        runs.clear();  // per subpath: the ranges never share runs
    }
    path_.clear(); substarts_.clear();
    has_path_ = false; path_closed_ = false;
}

void Renderer2DCanvas::fill(std::uint32_t color) {
    if (!has_path_ || path_.size() < 3) {
        path_.clear(); substarts_.clear();
        has_path_ = false; path_closed_ = false;
        return;
    }
    // Every SUBPATH fills as its own simple polygon (the header's
    // contract - the whole-path spelling spliced a phantom edge
    // between disjoint rings). The transform runs in place over each
    // consumed range: no per-fill screen buffer.
    for (std::size_t s = 0; s < substarts_.size(); ++s) {
        const int first = substarts_[s];
        const int last = s + 1 == substarts_.size()
                             ? static_cast<int>(path_.size())
                             : substarts_[s + 1];
        if (last - first < 3) continue;
        for (int i = first; i < last; ++i) path_[i] = to_screen(path_[i]);
        ws::ui::canvas::fill_polygon(path_.data() + first, last - first,
                                     color, sink_);
    }
    path_.clear(); substarts_.clear();
    has_path_ = false; path_closed_ = false;
}

void Renderer2DCanvas::stroke_line(Point a, Point b, const Paint& paint) {
    const Point pts[] = {a, b};
    stroke_polyline(pts, 2, paint, false);
}

void Renderer2DCanvas::stroke_polyline(const Point* pts, int count,
                                       const Paint& paint, bool closed) {
    // Convenience wrapper: transform local points to screen, then
    // tessellate through the r2d sink. A DASHED paint must route
    // through the path API: the pattern walks the local-space path,
    // so dashing is a stroke()-time property - the direct
    // tessellation below would silently draw the polyline solid while
    // CommandListCanvas's recording of the same call showed dashes.
    // (stroke_circle rides this wrapper, so a dashed circle outline
    // depends on the routing too.)
    if (count <= 0 || !(paint.width > 0.0f)) return;
    if (paint.dash.count > 0) {
        begin_path();
        for (int i = 0; i < count; ++i) {
            if (i == 0) move_to(pts[i]); else line_to(pts[i]);
        }
        path_closed_ = closed;
        stroke(paint);
        return;
    }
    // The undashed fast path: no path, no dash runs - straight to
    // the tessellator. The screen copy rides a stack buffer for the
    // common short input (a grid hairline is 2 points, a wire a
    // handful, a symbol poly a dozen); only a long polyline pays the
    // heap.
    constexpr int kStackPts = 64;
    Point stack_pts[kStackPts];
    std::vector<Point> heap_pts;
    Point* screen;
    if (count <= kStackPts) {
        screen = stack_pts;
    } else {
        heap_pts.resize(static_cast<std::size_t>(count));
        screen = heap_pts.data();
    }
    for (int i = 0; i < count; ++i) screen[i] = to_screen(pts[i]);
    ws::ui::canvas::stroke_polyline(screen, count, paint, sink_, closed);
}

void Renderer2DCanvas::fill_polygon(const Point* pts, int count,
                                    std::uint32_t color) {
    if (count < 3) return;
    // Same stack-buffer spelling as stroke_polyline's fast path: the
    // common fill is small, the heap is the long-polygon exception.
    constexpr int kStackPts = 64;
    Point stack_pts[kStackPts];
    std::vector<Point> heap_pts;
    Point* screen;
    if (count <= kStackPts) {
        screen = stack_pts;
    } else {
        heap_pts.resize(static_cast<std::size_t>(count));
        screen = heap_pts.data();
    }
    for (int i = 0; i < count; ++i) screen[i] = to_screen(pts[i]);
    ws::ui::canvas::fill_polygon(screen, count, color, sink_);
}

void Renderer2DCanvas::stroke_circle(Point center, float r, const Paint& paint) {
    auto pts = circle_points(center, r);
    stroke_polyline(pts.data(), static_cast<int>(pts.size()), paint, true);
}

void Renderer2DCanvas::fill_circle(Point center, float r, std::uint32_t color) {
    auto pts = circle_points(center, r);
    fill_polygon(pts.data(), static_cast<int>(pts.size()), color);
}

void Renderer2DCanvas::stroke_rounded_rect(float x, float y, float w, float h,
                                           float r, const Paint& paint) {
    begin_path();
    // See CommandListCanvas::stroke_rounded_rect: the negative clamp,
    // and the ONE-ring arc walk (arc chains via its line_to); the
    // radius-0 floor builds the sharp ring explicitly.
    const float rad = std::clamp(r, 0.0f, std::min(w, h) * 0.5f);
    const Point corners[4] = {{x + w - rad, y + rad}, {x + w - rad, y + h - rad},
                              {x + rad, y + h - rad}, {x + rad, y + rad}};
    const float angles[4][2] = {{-kHalfPi, 0.0f}, {0.0f, kHalfPi},
                                 {kHalfPi, kPi}, {kPi, 1.5f * kPi}};
    if (rad <= 0.0f) {
        move_to({x + w, y});
        line_to({x + w, y + h});
        line_to({x, y + h});
        line_to({x, y});
    } else {
        move_to({corners[0].x + rad * std::cos(angles[0][0]),
                 corners[0].y + rad * std::sin(angles[0][0])});
        for (int i = 0; i < 4; ++i) {
            arc(corners[i], rad, rad, angles[i][0], angles[i][1]);
        }
    }
    close_path();
    stroke(paint);
}

void Renderer2DCanvas::fill_rounded_rect(float x, float y, float w, float h,
                                         float r, std::uint32_t color) {
    begin_path();
    // See CommandListCanvas::stroke_rounded_rect: the negative clamp,
    // and the ONE-ring arc walk (a single subpath, one fill polygon);
    // the radius-0 floor builds the sharp ring explicitly.
    const float rad = std::clamp(r, 0.0f, std::min(w, h) * 0.5f);
    const Point corners[4] = {{x + w - rad, y + rad}, {x + w - rad, y + h - rad},
                              {x + rad, y + h - rad}, {x + rad, y + rad}};
    const float angles[4][2] = {{-kHalfPi, 0.0f}, {0.0f, kHalfPi},
                                 {kHalfPi, kPi}, {kPi, 1.5f * kPi}};
    if (rad <= 0.0f) {
        move_to({x + w, y});
        line_to({x + w, y + h});
        line_to({x, y + h});
        line_to({x, y});
    } else {
        move_to({corners[0].x + rad * std::cos(angles[0][0]),
                 corners[0].y + rad * std::sin(angles[0][0])});
        for (int i = 0; i < 4; ++i) {
            arc(corners[i], rad, rad, angles[i][0], angles[i][1]);
        }
    }
    close_path();
    fill(color);
}

void Renderer2DCanvas::fill_rects(const Point* corners, int count, float w,
                                  float h, std::uint32_t color) {
    // Each rect is ONE r2d quad - the draw_image contract (the
    // transformed top-left and bottom-right), no tessellation and no
    // per-rect vector. The batch's own storage is the caller's stack
    // buffer, so a 20k-dot grid frame allocates nothing per dot.
    if (count <= 0 || !(w > 0.0f) || !(h > 0.0f)) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        const Point& p = corners[i];
        const Point tl = to_screen(p);
        const Point br = to_screen({p.x + w, p.y + h});
        r2d::draw_quad(tl.x, tl.y, br.x - tl.x, br.y - tl.y, color);
    }
}

float Renderer2DCanvas::draw_text(float x, float y, const char* utf8,
                                  const r2d::Font& font, std::uint32_t color) {
    const Point screen = to_screen({x, y});
    return r2d::draw_text(screen.x, screen.y, utf8, font, color);
}

float Renderer2DCanvas::measure_text(const char* utf8, const r2d::Font& font) {
    return font.measure_text(utf8) * view_scale();
}

void Renderer2DCanvas::draw_image(float x, float y, float w, float h,
                                  r2d::Texture texture, std::uint32_t color) {
    const Point tl = to_screen({x, y});
    const Point br = to_screen({x + w, y + h});
    r2d::draw_quad(tl.x, tl.y, br.x - tl.x, br.y - tl.y, color, texture);
}

Point Renderer2DCanvas::to_screen(Point p) const {
    return xform_stack_.back().apply(p);
}

void Renderer2DCanvas::R2DSink::quad(const Vec2 (&pos)[4],
                                     const Vec4 (&rgba)[4]) {
    float p[4][2];
    float c[4][4];
    for (int i = 0; i < 4; ++i) {
        p[i][0] = pos[i].x; p[i][1] = pos[i].y;
        c[i][0] = rgba[i].x; c[i][1] = rgba[i].y;
        c[i][2] = rgba[i].z; c[i][3] = rgba[i].w;
    }
    const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    r2d::draw_vertex_quad(p, uv, c);
}

}  // namespace ws::ui::canvas
