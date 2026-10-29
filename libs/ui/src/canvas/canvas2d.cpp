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
    const Vec2 tl = to_screen({x, y});
    const Vec2 tr = to_screen({x + w, y});
    const Vec2 bl = to_screen({x, y + h});
    const float minx = std::min({tl.x, tr.x, bl.x});
    const float miny = std::min({tl.y, tr.y, bl.y});
    const float maxx = std::max({tl.x, tr.x, bl.x});
    const float maxy = std::max({tl.y, tr.y, bl.y});
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
    path_.push_back(p); last_move_ = p; has_path_ = true;
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
        path_.push_back(last_move_);
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
        CanvasCmd c;
        c.kind = CanvasCmdKind::stroke_polyline;
        c.points.reserve(run.size());
        for (const auto& p : run) c.points.push_back(to_screen(p));
        c.color = paint.color;
        c.width = paint.width;
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
    const float rad = std::min(r, std::min(w, h) * 0.5f);
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
    const float rad = std::min(r, std::min(w, h) * 0.5f);
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
    const Vec2 tl = to_screen({x, y});
    const Vec2 tr = to_screen({x + w, y});
    const Vec2 bl = to_screen({x, y + h});
    const float minx = std::min({tl.x, tr.x, bl.x});
    const float miny = std::min({tl.y, tr.y, bl.y});
    const float maxx = std::max({tl.x, tr.x, bl.x});
    const float maxy = std::max({tl.y, tr.y, bl.y});
    r2d::push_clip(minx, miny, maxx - minx, maxy - miny);
}

void Renderer2DCanvas::pop_clip() { r2d::pop_clip(); }

void Renderer2DCanvas::begin_path() {
    path_.clear(); has_path_ = false; path_closed_ = false;
}
void Renderer2DCanvas::move_to(Point p) {
    path_.push_back(p); last_move_ = p; has_path_ = true;
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
        path_.push_back(last_move_);
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
        std::vector<Vec2> screen_run(run.size());
        for (std::size_t i = 0; i < run.size(); ++i)
            screen_run[i] = to_screen(run[i]);
        // Call the tessellation free function (qualified, to disambiguate
        // from this class's own stroke_polyline member).
        ws::ui::canvas::stroke_polyline(screen_run.data(),
                                        static_cast<int>(screen_run.size()),
                                        paint, sink_, path_closed_);
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
    // tessellate through the r2d sink. (No dashing - the path-based
    // stroke() handles dashes; this is the fast path for one-shot
    // polylines.)
    if (count <= 0 || !(paint.width > 0.0f)) return;
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
    const float rad = std::min(r, std::min(w, h) * 0.5f);
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
    const float rad = std::min(r, std::min(w, h) * 0.5f);
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
