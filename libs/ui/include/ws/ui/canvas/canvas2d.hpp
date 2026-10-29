#pragma once

// Canvas2D: the backend-agnostic 2D drawing interface the canvas view
// hands to a host's draw callback. Everything a host draws - strokes,
// fills, arcs, beziers, text, images - flows through this interface,
// and the implementation (Renderer2DCanvas over r2d, CommandListCanvas
// for tests) is the canvas's to swap. The host never touches the
// raster, the font system, or the GPU.
//
// Coordinates are LOCAL: the canvas owns a transform stack (push/pop,
// translate, scale, rotate) that maps local points to screen pixels.
// Stroke widths are SCREEN PIXELS (independent of the transform's
// scale and of the camera zoom - the SVG convention flipped: widths
// are screen, dash lengths are local). The canvas multiplies the
// camera zoom (set by CanvasView via set_view_scale) with its own
// transform scale to flatten curves at the right screen-space
// tolerance and to keep hairlines honest.
//
// The path API is the SVG/Canvas2D convention: begin_path, move_to,
// line_to, quadratic_to, cubic_to, arc, close_path, then stroke(paint)
// or fill(color). Convenience wrappers (stroke_line, stroke_polyline,
// fill_polygon, draw_circle, draw_rounded_rect, draw_text, draw_image)
// build a one-shot path and stroke/fill it in one call - the gallery
// page's stress test uses these to avoid per-primitive path overhead.

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

#include <ws/math/vecmath.hpp>
#include <ws/ui/canvas/tessellation.hpp>
#include <ws/ui/renderer2d.hpp>  // Font, Texture (the canvas's text/image seam)

namespace ws::ui::canvas {

// A 2D point in local space (the canvas transforms it to screen on
// stroke/fill). Alias of Vec2 - the same contiguous storage the
// tessellator reads.
using Point = ws::math::Vec2;

// The affine transform: [a b c d e f] maps (x, y) to
// (a*x + c*y + e, b*x + d*y + f). Shared by both concrete canvases
// (defined here so the header's private members can name it). The
// translate/scale/rotate methods post-multiply onto the current
// transform (a translate after a scale moves in the scaled space).
struct Affine {
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, e = 0.0f, f = 0.0f;
    Point apply(Point p) const {
        return {a * p.x + c * p.y + e, b * p.x + d * p.y + f};
    }
    float scale_factor() const {
        return std::sqrt(std::fabs(a * d - b * c));
    }
    void translate(float dx, float dy) {
        e += a * dx + c * dy;
        f += b * dx + d * dy;
    }
    void scale(float sx, float sy) {
        a *= sx; b *= sx; c *= sy; d *= sy;
    }
    void rotate(float rad);
};

// The canvas's draw interface. Every method is backend-agnostic; the
// implementation decides where the geometry lands (r2d's vertex quad,
// a recorded command list, a test buffer). The canvas owns the
// transform and clip stacks; the host drives the path API.
class Canvas2D {
public:
    virtual ~Canvas2D() = default;

    // --- view scale -----------------------------------------------------------
    // The product of the camera zoom and the canvas transform's scale.
    // The canvas uses it to flatten curves at the right screen-space
    // tolerance (tolerance / view_scale in local space) and to keep
    // hairline coverage honest. Set by CanvasView when the camera
    // moves; defaults to 1 (1 world unit = 1 screen pixel).
    virtual void set_view_scale(float scale) = 0;
    virtual float view_scale() const = 0;

    // --- transform stack ------------------------------------------------------
    // Pushes the current transform; pop restores it. translate/scale/
    // rotate multiply onto the current transform (local-to-screen).
    // The stack is bounded (32); a push past the cap is a warned no-op
    // (the canvas keeps drawing under the un-pushed transform).
    virtual void push_transform() = 0;
    virtual void pop_transform() = 0;
    virtual void translate(float dx, float dy) = 0;
    virtual void scale(float sx, float sy) = 0;
    virtual void rotate(float radians) = 0;

    // --- clip stack ----------------------------------------------------------
    // Pushes a LOCAL-space clip rect (the canvas transforms it to
    // screen). Draws until the matching pop_clip render only inside
    // the rect intersected with clips already active. The stack
    // empties at frame boundaries.
    virtual void push_clip(float x, float y, float w, float h) = 0;
    virtual void pop_clip() = 0;

    // --- path API ------------------------------------------------------------
    // Begins a new path (the old path is discarded). The path
    // accumulates in LOCAL space; stroke/fill transforms it to screen.
    virtual void begin_path() = 0;
    virtual void move_to(Point p) = 0;
    virtual void line_to(Point p) = 0;
    // Appends a quadratic Bezier from the current point through p1 to p2.
    virtual void quadratic_to(Point p1, Point p2) = 0;
    // Appends a cubic Bezier from the current point through p1, p2 to p3.
    virtual void cubic_to(Point p1, Point p2, Point p3) = 0;
    // Appends an ellipse arc around `center` with radii (rx, ry) from
    // a0 to a1 (radians). The arc starts at angle a0; if the current
    // point differs from the arc's start, a line_to connects them.
    virtual void arc(Point center, float rx, float ry, float a0, float a1) = 0;
    // Closes the path (line_to the last move_to point, marks the path
    // closed for stroke).
    virtual void close_path() = 0;
    // Strokes the current path with `paint` (screen-space width, joins,
    // caps, dashes). The path is consumed (a new begin_path is needed
    // for the next stroke).
    virtual void stroke(const Paint& paint) = 0;
    // Fills the current path with `color` (the path is treated as a
    // simple polygon; subpaths are filled independently).
    virtual void fill(std::uint32_t color) = 0;

    // --- convenience wrappers ------------------------------------------------
    // One-shot primitives that build a path and stroke/fill in one call.
    virtual void stroke_line(Point a, Point b, const Paint& paint) = 0;
    virtual void stroke_polyline(const Point* pts, int count,
                                 const Paint& paint, bool closed = false) = 0;
    virtual void fill_polygon(const Point* pts, int count,
                              std::uint32_t color) = 0;
    // A circle outline (closed polyline tessellated from the arc).
    virtual void stroke_circle(Point center, float r, const Paint& paint) = 0;
    // A filled circle (a 32+ segment polygon).
    virtual void fill_circle(Point center, float r, std::uint32_t color) = 0;
    // A rounded rectangle outline.
    virtual void stroke_rounded_rect(float x, float y, float w, float h,
                                     float r, const Paint& paint) = 0;
    // A filled rounded rectangle.
    virtual void fill_rounded_rect(float x, float y, float w, float h,
                                   float r, std::uint32_t color) = 0;

    // --- text ----------------------------------------------------------------
    // Draws one line of UTF-8 text at (x, y) [the line box's top-left
    // in LOCAL space] with `font` and `color`. Returns the pen's
    // advance. The canvas transforms the pen position; glyph quads
    // are emitted in screen space through the implementation's seam.
    virtual float draw_text(float x, float y, const char* utf8,
                            const r2d::Font& font,
                            std::uint32_t color = 0xFFFFFFFFu) = 0;
    // Measures the text's width in LOCAL space (accounts for the
    // transform's scale). The host never touches the font system
    // directly - this is the only measurement API.
    virtual float measure_text(const char* utf8,
                               const r2d::Font& font) = 0;

    // --- images --------------------------------------------------------------
    // Draws a textured quad at (x, y, w, h) in LOCAL space, tinted by
    // `color`. texture = {} draws a flat color quad.
    virtual void draw_image(float x, float y, float w, float h,
                            r2d::Texture texture,
                            std::uint32_t color = 0xFFFFFFFFu) = 0;
};

// --- CommandListCanvas --------------------------------------------------------
// A recording Canvas2D for tests: every draw call appends a command to
// a vector, no GPU, no raster. The tests assert the command stream
// (primitive counts, colors, points) directly - the same purity seam
// as the gui's r2d stub. The transform/clip stacks are real (the
// canvas applies them), so the recorded commands carry SCREEN-space
// geometry: a stroke_line at (0,0)-(10,0) under translate(5, 0)
// records a line at (5,0)-(15,0).

enum class CanvasCmdKind : std::uint8_t {
    stroke_polyline = 0,
    fill_polygon = 1,
    fill_circle = 2,
    draw_text = 3,
    draw_image = 4,
    push_clip = 5,
    pop_clip = 6,
};

struct CanvasCmd {
    CanvasCmdKind kind;
    std::vector<Point> points;  // stroke/fill polyline (screen space)
    std::uint32_t color = 0xFFFFFFFFu;
    float width = 1.0f;         // stroke width (screen px)
    StrokeJoin join = StrokeJoin::miter;
    StrokeCap cap = StrokeCap::butt;
    // text/image payload
    std::string text;
    r2d::Texture texture{};
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

class CommandListCanvas : public Canvas2D {
public:
    std::vector<CanvasCmd> cmds;

    void set_view_scale(float scale) override { view_scale_ = scale; }
    float view_scale() const override {
        return view_scale_ * xform_stack_.back().scale_factor();
    }

    void push_transform() override;
    void pop_transform() override;
    void translate(float dx, float dy) override;
    void scale(float sx, float sy) override;
    void rotate(float radians) override;

    void push_clip(float x, float y, float w, float h) override;
    void pop_clip() override;

    void begin_path() override;
    void move_to(Point p) override;
    void line_to(Point p) override;
    void quadratic_to(Point p1, Point p2) override;
    void cubic_to(Point p1, Point p2, Point p3) override;
    void arc(Point center, float rx, float ry, float a0, float a1) override;
    void close_path() override;
    void stroke(const Paint& paint) override;
    void fill(std::uint32_t color) override;

    void stroke_line(Point a, Point b, const Paint& paint) override;
    void stroke_polyline(const Point* pts, int count,
                         const Paint& paint, bool closed = false) override;
    void fill_polygon(const Point* pts, int count,
                      std::uint32_t color) override;
    void stroke_circle(Point center, float r, const Paint& paint) override;
    void fill_circle(Point center, float r, std::uint32_t color) override;
    void stroke_rounded_rect(float x, float y, float w, float h,
                             float r, const Paint& paint) override;
    void fill_rounded_rect(float x, float y, float w, float h,
                           float r, std::uint32_t color) override;

    float draw_text(float x, float y, const char* utf8,
                    const r2d::Font& font,
                    std::uint32_t color = 0xFFFFFFFFu) override;
    float measure_text(const char* utf8, const r2d::Font& font) override;

    void draw_image(float x, float y, float w, float h,
                    r2d::Texture texture,
                    std::uint32_t color = 0xFFFFFFFFu) override;

    // Test helpers: count commands of a kind, find the first of a kind.
    std::size_t count(CanvasCmdKind kind) const;
    const CanvasCmd* first(CanvasCmdKind kind) const;

private:
    float view_scale_ = 1.0f;
    std::vector<Affine> xform_stack_{1};  // identity at the bottom
    std::vector<Point> path_;
    bool path_closed_ = false;
    Point last_move_{};
    bool has_path_ = false;

    Point to_screen(Point p) const;
};

// --- Renderer2DCanvas ---------------------------------------------------------
// A Canvas2D that drives the r2d free-function seam. Strokes and fills
// tessellate through ws::ui::canvas's pure functions, then the quads
// land via r2d::draw_vertex_quad. Text and images delegate to r2d
// directly. The transform stack is real (the canvas applies it); clip
// pushes forward to r2d::push_clip in screen space. This is the
// implementation the gallery page and CanvasView use in a live app.
//
// Pure-CPU construction (no GPU touch at build time); the r2d calls
// are drop-safe outside a running app (logged once, ignored), so a
// headless test that constructs one and drives it records nothing
// without crashing - the same contract every r2d caller already has.

class Renderer2DCanvas : public Canvas2D {
public:
    void set_view_scale(float scale) override { view_scale_ = scale; }
    float view_scale() const override {
        return view_scale_ * xform_stack_.back().scale_factor();
    }

    void push_transform() override;
    void pop_transform() override;
    void translate(float dx, float dy) override;
    void scale(float sx, float sy) override;
    void rotate(float radians) override;

    void push_clip(float x, float y, float w, float h) override;
    void pop_clip() override;

    void begin_path() override;
    void move_to(Point p) override;
    void line_to(Point p) override;
    void quadratic_to(Point p1, Point p2) override;
    void cubic_to(Point p1, Point p2, Point p3) override;
    void arc(Point center, float rx, float ry, float a0, float a1) override;
    void close_path() override;
    void stroke(const Paint& paint) override;
    void fill(std::uint32_t color) override;

    void stroke_line(Point a, Point b, const Paint& paint) override;
    void stroke_polyline(const Point* pts, int count,
                         const Paint& paint, bool closed = false) override;
    void fill_polygon(const Point* pts, int count,
                      std::uint32_t color) override;
    void stroke_circle(Point center, float r, const Paint& paint) override;
    void fill_circle(Point center, float r, std::uint32_t color) override;
    void stroke_rounded_rect(float x, float y, float w, float h,
                             float r, const Paint& paint) override;
    void fill_rounded_rect(float x, float y, float w, float h,
                           float r, std::uint32_t color) override;

    float draw_text(float x, float y, const char* utf8,
                    const r2d::Font& font,
                    std::uint32_t color = 0xFFFFFFFFu) override;
    float measure_text(const char* utf8, const r2d::Font& font) override;

    void draw_image(float x, float y, float w, float h,
                    r2d::Texture texture,
                    std::uint32_t color = 0xFFFFFFFFu) override;

private:
    float view_scale_ = 1.0f;
    std::vector<Affine> xform_stack_{1};
    std::vector<Point> path_;
    bool path_closed_ = false;
    Point last_move_{};
    bool has_path_ = false;

    Point to_screen(Point p) const;

    // The r2d TessSink adapter: stroke_polyline/fill_polygon emit quads
    // to this sink, which forwards each to r2d::draw_vertex_quad.
    struct R2DSink : TessSink {
        void quad(const ws::math::Vec2 (&pos)[4],
                  const ws::math::Vec4 (&rgba)[4]) override;
    };
    R2DSink sink_;
};

}  // namespace ws::ui::canvas
