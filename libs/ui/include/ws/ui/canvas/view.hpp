#pragma once

// ViewTransform2D: the camera a CanvasView drives. Pan, zoom (and
// optional rotation) with world<->screen mapping, zoom-at-point (the
// invariant: the world point under the cursor does not move), zoom
// limits, fit-to-rect. The adaptive grid renderer draws major/minor
// lines (or dots) that fade by zoom and snap in world units.
//
// All pure math - no gui, no input, no allocation. The CanvasView
// widget owns one of these, feeds it input events, and hands it (plus
// a Canvas2D&) to the host's draw callback each frame.

#include <cmath>
#include <cstdint>
#include <functional>

#include <ws/math/vecmath.hpp>
#include <ws/ui/canvas/canvas2d.hpp>
#include <ws/ui/gui/rect.hpp>  // gui::Rect (the layout rect the view occupies)

namespace ws::ui::canvas {

// The 2D view camera: pan (the world point at the viewport's center),
// zoom (world->screen scale), optional rotation (radians, positive =
// clockwise on the y-down screen).
struct ViewTransform2D {
    ws::math::Vec2 pan{0.0f, 0.0f};   // world point at the viewport center
    float zoom = 1.0f;                // world->screen scale
    float rotation = 0.0f;            // radians, 0 = no rotation

    // Zoom limits (clamped on every set_zoom / zoom_at). The defaults
    // cover a 1e-6..1e6 world-unit range - wide enough for a schematic
    // (microns to meters) and a map (meters to continents).
    float min_zoom = 1e-6f;
    float max_zoom = 1e6f;

    // The viewport's size in screen pixels. The transform maps world
    // points to screen pixels relative to the viewport's center.
    float viewport_w = 0.0f;
    float viewport_h = 0.0f;

    // --- world <-> screen mapping ---------------------------------------
    // screen = viewport_center + zoom * R * (world - pan)
    // world  = pan + R^-1 * (screen - viewport_center) / zoom
    // R is the rotation matrix (cos, -sin; sin, cos) for y-down CW.

    ws::math::Vec2 world_to_screen(ws::math::Vec2 world) const;
    ws::math::Vec2 screen_to_world(ws::math::Vec2 screen) const;

    // --- zoom-at-point ---------------------------------------------------
    // Zooms toward `anchor_screen` (a screen pixel): the world point
    // under that pixel stays put. Clamped to [min_zoom, max_zoom].
    void zoom_at(ws::math::Vec2 anchor_screen, float new_zoom);
    // Pan by a screen-pixel delta (dragging the view).
    void pan_by(ws::math::Vec2 screen_delta);
    // Reset to fit the given world rect (with optional margin). Sets
    // zoom so the rect fits inside the viewport, and pans to its center.
    void fit(float world_x, float world_y, float world_w, float world_h,
             float margin = 0.0f);

    // Clamps zoom to [min_zoom, max_zoom]; returns the clamped value.
    float clamp_zoom(float z) const {
        if (z < min_zoom) return min_zoom;
        if (z > max_zoom) return max_zoom;
        return z;
    }

private:
    // The rotation matrix entries (cached conceptually; computed fresh
        // each call - the cost is one sin/cos pair, negligible on the frame
        // path).
    float rot_cos() const { return std::cos(rotation); }
    float rot_sin() const { return std::sin(rotation); }
};

// --- adaptive grid -----------------------------------------------------------
// Draws a grid that adapts to the zoom: at any zoom, the grid spacing
// (in world units) is the largest power-of-10 multiple of the base
// spacing that yields ~2-10 screen pixels per cell. Major lines every
// 5 or 10 cells; minor lines fade by zoom (alpha ramps 0->1->0 as the
// spacing transitions between levels, so the grid never pops).
//
// `grid` is the Canvas2D to draw into (the canvas's transform should be
// the identity - the grid draws in SCREEN space, reading the view's
// world_to_screen to place lines). `base_spacing` is the smallest world
// unit the grid ever snaps to (1.0 for a unit grid, 0.001 for a mm
// grid).
struct GridStyle {
    float base_spacing = 1.0f;       // smallest world unit (snap)
    std::uint32_t minor_color = 0xFFFFFF20;  // faint
    std::uint32_t major_color = 0xFFFFFF60;  // stronger
    std::uint32_t origin_color = 0xFFFFFF80;  // the x=0 / y=0 axes
    int major_every = 5;             // major line every N minor cells
    float target_pixels = 80.0f;     // ~this many screen px per cell
    bool dots = false;               // dots at intersections (else lines)
};

// Draws the adaptive grid into `canvas` for the given view. The grid
// covers the viewport (view.viewport_w x view.viewport_h) with lines
// in world space, rendered through the canvas. The canvas's transform
// should be the identity (the grid uses world_to_screen directly).
void draw_grid(Canvas2D& canvas, const ViewTransform2D& view,
               const GridStyle& style = {});

// --- CanvasView --------------------------------------------------------------
// A widget that owns a view transform, applies a pan/zoom input policy,
// and hands a Canvas2D& + the transform to a host draw callback. The
// host draws its content in WORLD space (the canvas's transform is set
// from the view); the CanvasView handles the input, the clip, and the
// retained-layer cache.
//
// Input policy (default): wheel zooms toward the cursor (factor 1.1x
// per notch), middle-drag pans, space+left-drag pans. Touch: pinch
// zooms (two-finger), one-finger pans. The policy is configurable via
// the InputPolicy flags.
//
// Retained layers: the host tags content with a version counter; the
// CanvasView caches the recorded CommandList for that layer and replays
// it each frame, invalidating when the version changes or the view
// moves (for static layers) / always-redrawing (for dynamic layers).
// The CPU-side cache is the documented fallback (no offscreen render
// target in r2d v1); a future render-target extension plugs in at the
// same invalidation contract.

struct CanvasViewInputPolicy {
    bool wheel_zoom = true;          // scroll wheel zooms toward cursor
    bool middle_drag_pan = true;     // middle button drags
    bool space_drag_pan = true;      // space + left button drags
    bool touch_pinch_zoom = true;    // two-finger pinch
    bool touch_one_finger_pan = true; // one-finger pan
    float zoom_factor = 1.1f;        // per wheel notch
};

// The host's draw callback: receives a Canvas2D& (with the view's
// transform already applied - the host draws in WORLD space) and the
// view (for culling, measurement, etc.).
using CanvasDrawCallback = std::function<void(Canvas2D&, const ViewTransform2D&)>;

class CanvasView {
public:
    ViewTransform2D view;
    CanvasViewInputPolicy input_policy;

    // Sets the host's draw callback. Called every frame the view is
    // visible; the callback draws into the canvas (transform already
    // applied from the view).
    void set_draw_callback(CanvasDrawCallback cb) { draw_cb_ = std::move(cb); }

    // Drives the view for one frame: processes pending input (wheel,
    // drag), applies the view to the canvas, calls the draw callback,
    // and draws the grid (if show_grid). `rect` is the screen-space
    // rect the view occupies (from the gui layout). `canvas` is the
    // renderer canvas (the view sets its transform from `view`).
    //
    // Input is fed via the feed_* methods (the gui or the host calls
    // them from its event dispatch); the view accumulates pending
    // deltas and applies them on the next draw.
    void draw(const ws::ui::gui::Rect& rect, Renderer2DCanvas& canvas,
              bool show_grid = true);

    // Input feeders (called by the host's event handler; the view
    // accumulates and applies on the next draw).
    void feed_mouse_moved(float x, float y);
    void feed_mouse_button(int button, bool pressed);  // 0=left,1=right,2=middle
    void feed_scroll(float x, float y, float dx, float dy);
    void feed_touch(std::uint32_t id, float x, float y, bool pressed);
    void feed_key(int key, bool pressed);  // space=32

    // Retained-layer API: the host registers a layer with a version
    // counter. The view caches the layer's recorded commands and
    // replays them each frame; the cache invalidates when the version
    // changes or the view's transform changes (for static layers).
    // A dynamic layer (version = 0) always re-records.
    struct Layer {
        std::uint32_t id = 0;
        std::uint64_t version = 0;    // 0 = always re-record (dynamic)
        bool static_content = false;  // cached across view moves when true
        CanvasDrawCallback draw;
    };
    void set_layers(const Layer* layers, std::size_t count) {
        layers_ = layers; layer_count_ = count;
    }

private:
    CanvasDrawCallback draw_cb_;
    const Layer* layers_ = nullptr;
    std::size_t layer_count_ = 0;

    // Pending input state.
    float mouse_x_ = 0.0f, mouse_y_ = 0.0f;
    bool middle_down_ = false;
    bool space_down_ = false;
    bool left_down_ = false;
    float drag_start_x_ = 0.0f, drag_start_y_ = 0.0f;
    ws::math::Vec2 drag_start_pan_{};
    bool dragging_ = false;

    // Pending wheel zoom (applied on the next draw, when the rect is known).
    bool has_pending_zoom_ = false;
    float pending_zoom_target_ = 1.0f;
    float pending_zoom_factor_ = 1.0f;

    // Touch state (two-finger pinch + one-finger pan).
    static constexpr int kMaxTouches = 2;
    struct Touch {
        std::uint32_t id = 0;
        float x = 0.0f, y = 0.0f;
        bool active = false;
    };
    Touch touches_[kMaxTouches]{};
    float last_touch_dist_ = 0.0f;
    float last_touch_x_ = -1.0f, last_touch_y_ = -1.0f;

    void apply_pending_input(const ws::ui::gui::Rect& rect);
};

}  // namespace ws::ui::canvas
