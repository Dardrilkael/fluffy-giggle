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
    path_.clear(); has_path_ = false; path_closed_ = false;
}
void CommandListCanvas::move_to(Point p) {
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
    // Dash in local space, transform each run to screen, record.
    std::vector<std::vector<Vec2>> runs;
    dash_polyline(path_.data(), static_cast<int>(path_.size()),
                  paint.dash, runs);
    for (const auto& run : runs) {
        if (run.empty()) continue;
        // A run closes only when it wraps the whole ring: it started
        // at the path's first point AND walked to its last (the
        // close_path spelling's duplicate, or the ring's final
        // vertex). Every other run is an open fragment - stroking it
        // closed would splice a phantom chord from each dash's end
        // back to its start and drop its caps.
        const bool run_closed =
            path_closed_ && pts_near(run.front(), path_.front()) &&
            pts_near(run.back(), path_.back());
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
    path_.clear(); has_path_ = false; path_closed_ = false;
}

void CommandListCanvas::fill(std::uint32_t color) {
    if (!has_path_ || path_.size() < 3) {
        path_.clear(); has_path_ = false; path_closed_ = false;
        return;
    }
    CanvasCmd c;
    c.kind = CanvasCmdKind::fill_polygon;
    c.points.reserve(path_.size());
    for (const auto& p : path_) c.points.push_back(to_screen(p));
    c.color = color;
    cmds.push_back(std::move(c));
    path_.clear(); has_path_ = false; path_closed_ = false;
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
    for (int i = 0; i < 4; ++i) {
        move_to({corners[i].x + rad * std::cos(angles[i][0]),
                  corners[i].y + rad * std::sin(angles[i][0])});
        arc(corners[i], rad, rad, angles[i][0], angles[i][1]);
    }
    close_path();
    stroke(paint);
}

void CommandListCanvas::fill_rounded_rect(float x, float y, float w, float h,
                                          float r, std::uint32_t color) {
    begin_path();
    // See stroke_rounded_rect: the negative clamp, both spellings.
    const float rad = std::clamp(r, 0.0f, std::min(w, h) * 0.5f);
    const Point corners[4] = {{x + w - rad, y + rad}, {x + w - rad, y + h - rad},
                              {x + rad, y + h - rad}, {x + rad, y + rad}};
    const float angles[4][2] = {{-kHalfPi, 0.0f}, {0.0f, kHalfPi},
                                 {kHalfPi, kPi}, {kPi, 1.5f * kPi}};
    for (int i = 0; i < 4; ++i) {
        move_to({corners[i].x + rad * std::cos(angles[i][0]),
                  corners[i].y + rad * std::sin(angles[i][0])});
        arc(corners[i], rad, rad, angles[i][0], angles[i][1]);
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
    path_.clear(); has_path_ = false; path_closed_ = false;
}
void Renderer2DCanvas::move_to(Point p) {
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
    // Dash in local space, transform each run to screen, tessellate
    // through the r2d sink.
    std::vector<std::vector<Vec2>> runs;
    dash_polyline(path_.data(), static_cast<int>(path_.size()),
                  paint.dash, runs);
    for (const auto& run : runs) {
        if (run.empty()) continue;
        // Same wrap-the-ring rule as CommandListCanvas::stroke: dash
        // fragments stroke open (their caps, no phantom chords), a
        // full-ring run keeps its cyclic joints.
        const bool run_closed =
            path_closed_ && pts_near(run.front(), path_.front()) &&
            pts_near(run.back(), path_.back());
        std::vector<Vec2> screen_run(run.size());
        for (std::size_t i = 0; i < run.size(); ++i)
            screen_run[i] = to_screen(run[i]);
        // Call the tessellation free function (qualified, to disambiguate
        // from this class's own stroke_polyline member).
        ws::ui::canvas::stroke_polyline(screen_run.data(),
                                        static_cast<int>(screen_run.size()),
                                        paint, sink_, run_closed);
    }
    path_.clear(); has_path_ = false; path_closed_ = false;
}

void Renderer2DCanvas::fill(std::uint32_t color) {
    if (!has_path_ || path_.size() < 3) {
        path_.clear(); has_path_ = false; path_closed_ = false;
        return;
    }
    std::vector<Vec2> screen_path(path_.size());
    for (std::size_t i = 0; i < path_.size(); ++i)
        screen_path[i] = to_screen(path_[i]);
    ws::ui::canvas::fill_polygon(screen_path.data(),
                                 static_cast<int>(screen_path.size()),
                                 color, sink_);
    path_.clear(); has_path_ = false; path_closed_ = false;
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
    // the tessellator.
    std::vector<Vec2> screen_pts(count);
    for (int i = 0; i < count; ++i) screen_pts[i] = to_screen(pts[i]);
    ws::ui::canvas::stroke_polyline(screen_pts.data(), count, paint, sink_, closed);
}

void Renderer2DCanvas::fill_polygon(const Point* pts, int count,
                                    std::uint32_t color) {
    if (count < 3) return;
    std::vector<Vec2> screen_pts(count);
    for (int i = 0; i < count; ++i) screen_pts[i] = to_screen(pts[i]);
    ws::ui::canvas::fill_polygon(screen_pts.data(), count, color, sink_);
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
    // See CommandListCanvas::stroke_rounded_rect: the negative clamp.
    const float rad = std::clamp(r, 0.0f, std::min(w, h) * 0.5f);
    const Point corners[4] = {{x + w - rad, y + rad}, {x + w - rad, y + h - rad},
                              {x + rad, y + h - rad}, {x + rad, y + rad}};
    const float angles[4][2] = {{-kHalfPi, 0.0f}, {0.0f, kHalfPi},
                                 {kHalfPi, kPi}, {kPi, 1.5f * kPi}};
    for (int i = 0; i < 4; ++i) {
        move_to({corners[i].x + rad * std::cos(angles[i][0]),
                  corners[i].y + rad * std::sin(angles[i][0])});
        arc(corners[i], rad, rad, angles[i][0], angles[i][1]);
    }
    close_path();
    stroke(paint);
}

void Renderer2DCanvas::fill_rounded_rect(float x, float y, float w, float h,
                                         float r, std::uint32_t color) {
    begin_path();
    // See CommandListCanvas::stroke_rounded_rect: the negative clamp.
    const float rad = std::clamp(r, 0.0f, std::min(w, h) * 0.5f);
    const Point corners[4] = {{x + w - rad, y + rad}, {x + w - rad, y + h - rad},
                              {x + rad, y + h - rad}, {x + rad, y + rad}};
    const float angles[4][2] = {{-kHalfPi, 0.0f}, {0.0f, kHalfPi},
                                 {kHalfPi, kPi}, {kPi, 1.5f * kPi}};
    for (int i = 0; i < 4; ++i) {
        move_to({corners[i].x + rad * std::cos(angles[i][0]),
                  corners[i].y + rad * std::sin(angles[i][0])});
        arc(corners[i], rad, rad, angles[i][0], angles[i][1]);
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
