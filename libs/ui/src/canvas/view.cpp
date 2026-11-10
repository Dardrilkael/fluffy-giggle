// view.hpp's implementation. ViewTransform2D is pure math (world <->
// screen mapping, zoom-at-point, fit); CanvasView owns a view, feeds
// it input, and drives a Renderer2DCanvas through the host's draw
// callback. The grid is grid.hpp's (one system for the whole
// library); this file only maps the view onto a GridFrame.

#include <ws/ui/canvas/view.hpp>

#include <algorithm>
#include <cmath>

namespace ws::ui::canvas {

using ws::math::Vec2;

// --- ViewTransform2D ---------------------------------------------------------

Vec2 ViewTransform2D::world_to_screen(Vec2 world) const {
    // world -> screen: translate by -pan, rotate, scale by zoom, offset
    // to viewport center.
    const Vec2 d = world - pan;
    const float cs = rot_cos();
    const float sn = rot_sin();
    const Vec2 rotated = {cs * d.x - sn * d.y, sn * d.x + cs * d.y};
    const Vec2 scaled = rotated * zoom;
    return {viewport_w * 0.5f + scaled.x, viewport_h * 0.5f + scaled.y};
}

Vec2 ViewTransform2D::screen_to_world(Vec2 screen) const {
    // screen -> world: offset from viewport center, scale by 1/zoom,
    // inverse-rotate, translate by +pan.
    const Vec2 d = {screen.x - viewport_w * 0.5f,
                    screen.y - viewport_h * 0.5f};
    const Vec2 scaled = d * (1.0f / zoom);
    const float cs = rot_cos();
    const float sn = rot_sin();
    // Inverse rotation: R^-1 = R^T = (cs, sn; -sn, cs).
    const Vec2 rotated = {cs * scaled.x + sn * scaled.y,
                          -sn * scaled.x + cs * scaled.y};
    return pan + rotated;
}

void ViewTransform2D::zoom_at(Vec2 anchor_screen, float new_zoom) {
    const float clamped = clamp_zoom(new_zoom);
    if (clamped == zoom) {
        return;
    }
    // The world point under the anchor stays put:
    //   world = screen_to_world(anchor, zoom)
    //   pan'  = world - R^-1 * (anchor - center) / zoom'
    // Substituting and simplifying: pan' = pan + R^-1 * (anchor - center) *
    // (1/zoom - 1/zoom').
    const Vec2 d = {anchor_screen.x - viewport_w * 0.5f,
                    anchor_screen.y - viewport_h * 0.5f};
    const float cs = rot_cos();
    const float sn = rot_sin();
    const Vec2 rotated = {cs * d.x + sn * d.y, -sn * d.x + cs * d.y};
    const float k = (1.0f / zoom) - (1.0f / clamped);
    pan = pan + rotated * k;
    zoom = clamped;
}

void ViewTransform2D::pan_by(Vec2 screen_delta) {
    // Panning by a screen delta moves the world the opposite direction:
    // the content follows the cursor. pan' = pan - R^-1 * delta / zoom.
    const float cs = rot_cos();
    const float sn = rot_sin();
    const Vec2 rotated = {cs * screen_delta.x + sn * screen_delta.y,
                          -sn * screen_delta.x + cs * screen_delta.y};
    pan = pan - rotated * (1.0f / zoom);
}

void ViewTransform2D::fit(float wx, float wy, float ww, float wh, float margin) {
    if (viewport_w <= 0.0f || viewport_h <= 0.0f || ww <= 0.0f || wh <= 0.0f) {
        return;
    }
    const float avail_w = viewport_w - 2.0f * margin;
    const float avail_h = viewport_h - 2.0f * margin;
    const float zx = avail_w / ww;
    const float zy = avail_h / wh;
    zoom = clamp_zoom(std::min(zx, zy));
    pan = {wx + ww * 0.5f, wy + wh * 0.5f};
}

// --- the view -> grid frame mapping -----------------------------------------

namespace {

// CanvasView's world is screen-convention (y down, rotation aside),
// so the frame passes no flip.
[[nodiscard]] GridFrame view_grid_frame(const ViewTransform2D& view,
                                        const ws::ui::gui::Rect& rect) {
    GridFrame frame;
    frame.center_x = view.pan.x;
    frame.center_y = view.pan.y;
    frame.zoom = view.zoom;
    frame.ox = rect.x;
    frame.oy = rect.y;
    frame.view_w = rect.width;
    frame.view_h = rect.height;
    frame.y_flip = false;
    return frame;
}

}  // namespace

// --- CanvasView --------------------------------------------------------------

void CanvasView::draw(const ws::ui::gui::Rect& rect, Canvas2D& canvas,
                      bool show_grid) {
    // Update the view's viewport size.
    view.viewport_w = rect.width;
    view.viewport_h = rect.height;

    // Apply pending input (wheel zoom, drag pan).
    apply_pending_input(rect);

    // Clip to the view's rect, then draw grid + host content.
    canvas.push_clip(rect.x, rect.y, rect.width, rect.height);

    if (show_grid && view.rotation == 0.0f) {
        // grid.hpp's system, the view mapped onto a frame. Rotated: no
        // grid (the header's documented limit - the lattice is
        // axis-aligned by contract).
        draw_grid(canvas, view_grid_frame(view, rect), grid);
    }

    // Set the world transform: world -> screen = rect_origin + viewport_center
    // + zoom * R * (world - pan). Composed as: translate(rect_center),
    // scale(zoom), rotate, translate(-pan).
    canvas.push_transform();
    canvas.translate(rect.x + view.viewport_w * 0.5f,
                     rect.y + view.viewport_h * 0.5f);
    canvas.scale(view.zoom, view.zoom);
    if (view.rotation != 0.0f) {
        canvas.rotate(view.rotation);
    }
    canvas.translate(-view.pan.x, -view.pan.y);

    // The canvas's view_scale is the transform's scale factor (zoom);
    // set_view_scale(1) so the effective scale is zoom * 1 = zoom. This
    // drives curve-flattening tolerance (finer at high zoom).
    canvas.set_view_scale(1.0f);

    // Draw the host's content in WORLD space.
    if (draw_cb_) {
        draw_cb_(canvas, view);
    }

    canvas.pop_transform();
    canvas.pop_clip();
}

void CanvasView::feed_mouse_moved(float x, float y) {
    mouse_x_ = x;
    mouse_y_ = y;
}

void CanvasView::feed_mouse_button(int button, bool pressed) {
    if (button == 2) {  // middle
        middle_down_ = pressed;
        if (pressed) {
            drag_start_x_ = mouse_x_;
            drag_start_y_ = mouse_y_;
            drag_start_pan_ = view.pan;
            dragging_ = true;
        } else {
            dragging_ = false;
        }
    }
    if (button == 0) {  // left
        left_down_ = pressed;
        if (pressed && space_down_) {
            drag_start_x_ = mouse_x_;
            drag_start_y_ = mouse_y_;
            drag_start_pan_ = view.pan;
            dragging_ = true;
        } else if (!pressed) {
            dragging_ = false;
        }
    }
}

void CanvasView::feed_scroll(float, float, float, float dy) {
    if (input_policy.wheel_zoom && dy != 0.0f) {
        // Zoom toward the cursor. The factor: dy > 0 zooms in.
        // Multiple notches can arrive before the next draw applies
        // them (a fast wheel, a busy frame), and each event computes
        // its target from the STILL-CURRENT view.zoom - so the target
        // must compound through the accumulated factor, or every
        // notch after the first overwrites the previous one and the
        // extra notches are silently lost.
        const float factor = std::pow(input_policy.zoom_factor, dy);
        pending_zoom_factor_ *= factor;
        pending_zoom_target_ = view.zoom * pending_zoom_factor_;
        has_pending_zoom_ = true;
    }
}

void CanvasView::feed_touch(std::uint32_t id, float x, float y, bool pressed) {
    // Find or allocate a slot.
    int slot = -1;
    for (int i = 0; i < kMaxTouches; ++i) {
        if (touches_[i].active && touches_[i].id == id) {
            slot = i;
            break;
        }
    }
    if (slot < 0 && pressed) {
        for (int i = 0; i < kMaxTouches; ++i) {
            if (!touches_[i].active) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) return;
    if (pressed) {
        touches_[slot] = {id, x, y, true};
    } else {
        touches_[slot].active = false;
    }
}

void CanvasView::feed_key(int key, bool pressed) {
    if (key == 32) {  // space
        space_down_ = pressed;
    }
}

void CanvasView::apply_pending_input(const ws::ui::gui::Rect& rect) {
    // Apply pending wheel zoom (toward the cursor, relative to the rect).
    if (has_pending_zoom_) {
        const Vec2 anchor = {mouse_x_ - rect.x, mouse_y_ - rect.y};
        view.zoom_at(anchor, pending_zoom_target_);
        has_pending_zoom_ = false;
        pending_zoom_factor_ = 1.0f;
    }

    // Apply drag pan (middle-drag or space+left-drag). pan_by, not
    // hand-rolled math: it applies the inverse rotation a rotated
    // view needs (pan -= R^-1 * delta / zoom), which the direct
    // `pan - delta/zoom` form gets wrong the moment rotation != 0.
    if (dragging_ && (middle_down_ || (space_down_ && left_down_))) {
        const float dx = mouse_x_ - drag_start_x_;
        const float dy = mouse_y_ - drag_start_y_;
        view.pan_by({dx, dy});
        // Update the start so the pan is incremental (continuous drag).
        drag_start_x_ = mouse_x_;
        drag_start_y_ = mouse_y_;
    }

    // Touch: two-finger pinch zooms, one-finger pans.
    int active = 0;
    for (int i = 0; i < kMaxTouches; ++i) if (touches_[i].active) ++active;
    if (active >= 2 && input_policy.touch_pinch_zoom) {
        // The two active touches.
        const Touch& a = touches_[0].active ? touches_[0] : touches_[1];
        const Touch& b = touches_[1].active ? touches_[1] : touches_[0];
        const float dist = std::sqrt((b.x - a.x) * (b.x - a.x) +
                                     (b.y - a.y) * (b.y - a.y));
        if (last_touch_dist_ > 0.0f && dist > 0.0f) {
            const float factor = dist / last_touch_dist_;
            const Vec2 mid = {(a.x + b.x) * 0.5f - rect.x,
                              (a.y + b.y) * 0.5f - rect.y};
            view.zoom_at(mid, view.zoom * factor);
        }
        last_touch_dist_ = dist;
    } else {
        last_touch_dist_ = 0.0f;
    }
    if (active == 1 && input_policy.touch_one_finger_pan) {
        const Touch& t = touches_[0].active ? touches_[0] : touches_[1];
        if (last_touch_x_ >= 0.0f) {
            const float dx = t.x - last_touch_x_;
            const float dy = t.y - last_touch_y_;
            view.pan_by({dx, dy});  // rotation-aware, like the drag pan
        }
        last_touch_x_ = t.x;
        last_touch_y_ = t.y;
    } else {
        last_touch_x_ = -1.0f;
        last_touch_y_ = -1.0f;
    }
}

}  // namespace ws::ui::canvas
