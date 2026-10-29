// view.hpp's implementation. ViewTransform2D is pure math (world <->
// screen mapping, zoom-at-point, fit); the adaptive grid reads the
// view to place world-space lines in screen space; CanvasView owns a
// view, feeds it input, and drives a Renderer2DCanvas through the
// host's draw callback.

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

// --- adaptive grid -----------------------------------------------------------

void draw_grid(Canvas2D& canvas, const ViewTransform2D& view,
               const GridStyle& style) {
    if (view.viewport_w <= 0.0f || view.viewport_h <= 0.0f ||
        style.base_spacing <= 0.0f) {
        return;
    }

    // Pick the grid spacing: snap the ideal (target_pixels / zoom) to
    // the nearest power-of-10 multiple of base_spacing. This keeps the
    // screen-cell-size close to target_pixels at every zoom, so the
    // visual density is stable.
    const float ideal = style.target_pixels / view.zoom;
    float exponent = 0.0f;
    if (ideal > 0.0f && style.base_spacing > 0.0f) {
        exponent = std::round(std::log10(ideal / style.base_spacing));
    }
    float spacing = style.base_spacing *
                    std::pow(10.0f, exponent);
    if (spacing < style.base_spacing) {
        spacing = style.base_spacing;  // never finer than the base
    }

    // The world bounds visible in the viewport (the four corners).
    const Vec2 tl = view.screen_to_world({0.0f, 0.0f});
    const Vec2 tr = view.screen_to_world({view.viewport_w, 0.0f});
    const Vec2 bl = view.screen_to_world({0.0f, view.viewport_h});
    const Vec2 br = view.screen_to_world({view.viewport_w, view.viewport_h});
    const float min_x = std::min({tl.x, tr.x, bl.x, br.x});
    const float max_x = std::max({tl.x, tr.x, bl.x, br.x});
    const float min_y = std::min({tl.y, tr.y, bl.y, br.y});
    const float max_y = std::max({tl.y, tr.y, bl.y, br.y});

    // Snap the line positions to the spacing (so lines land on world
    // units, not arbitrary fractions).
    const float first_x = std::ceil(min_x / spacing) * spacing;
    const float first_y = std::ceil(min_y / spacing) * spacing;

    // The fade: alpha ramps 0->1 as the spacing's screen size grows from
    // half the target to the target. This prevents pop-in when the spacing
    // level transitions (the spacing jumps 10x at each level, so the fade
    // smooths the visual step).
    const float screen_spacing = spacing * view.zoom;
    float fade = (screen_spacing - style.target_pixels * 0.5f) /
                 (style.target_pixels * 0.5f);
    fade = std::max(0.0f, std::min(1.0f, fade));

    // The minor color's alpha scales by fade; major lines are full strength.
    auto scale_alpha = [](std::uint32_t color, float a) {
        const std::uint32_t alpha = static_cast<std::uint32_t>(
            (color & 0xFFu) * a);
        return (color & 0xFFFFFF00u) | alpha;
    };
    const std::uint32_t minor = scale_alpha(style.minor_color, fade);
    const std::uint32_t major = scale_alpha(style.major_color, fade);

    // Draw vertical lines (world x = first_x + i*spacing).
    Paint paint;
    paint.width = 1.0f;
    paint.cap = StrokeCap::butt;

    // Vertical lines.
    for (float x = first_x; x <= max_x; x += spacing) {
        const bool is_major =
            static_cast<int>(std::round(x / spacing)) % style.major_every == 0;
        const bool is_origin = std::fabs(x) < spacing * 0.01f;
        paint.color = is_origin ? style.origin_color
                                : (is_major ? major : minor);
        if (paint.color == 0) continue;  // skip fully-transparent lines
        const Vec2 a = view.world_to_screen({x, min_y});
        const Vec2 b = view.world_to_screen({x, max_y});
        canvas.stroke_line(a, b, paint);
    }
    // Horizontal lines.
    for (float y = first_y; y <= max_y; y += spacing) {
        const bool is_major =
            static_cast<int>(std::round(y / spacing)) % style.major_every == 0;
        const bool is_origin = std::fabs(y) < spacing * 0.01f;
        paint.color = is_origin ? style.origin_color
                                : (is_major ? major : minor);
        if (paint.color == 0) continue;
        const Vec2 a = view.world_to_screen({min_x, y});
        const Vec2 b = view.world_to_screen({max_x, y});
        canvas.stroke_line(a, b, paint);
    }
}

// --- CanvasView --------------------------------------------------------------

void CanvasView::draw(const ws::ui::gui::Rect& rect, Renderer2DCanvas& canvas,
                      bool show_grid) {
    // Update the view's viewport size.
    view.viewport_w = rect.width;
    view.viewport_h = rect.height;

    // Apply pending input (wheel zoom, drag pan).
    apply_pending_input(rect);

    // Clip to the view's rect, then draw grid + host content.
    canvas.push_clip(rect.x, rect.y, rect.width, rect.height);

    if (show_grid) {
        // The grid draws in viewport-relative screen space (0..vw),
        // so offset by the rect's origin.
        canvas.push_transform();
        canvas.translate(rect.x, rect.y);
        draw_grid(canvas, view);
        canvas.pop_transform();
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
        const float factor = std::pow(input_policy.zoom_factor, dy);
        const float new_zoom = view.zoom * factor;
        // The anchor is the mouse position relative to the view's rect.
        // We need the rect here, but feed_scroll doesn't have it. Store
        // the pending zoom and apply it in apply_pending_input (which
        // has the rect). For now, store the delta.
        pending_zoom_factor_ *= factor;
        pending_zoom_target_ = new_zoom;
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

    // Apply drag pan (middle-drag or space+left-drag).
    if (dragging_ && (middle_down_ || (space_down_ && left_down_))) {
        const float dx = mouse_x_ - drag_start_x_;
        const float dy = mouse_y_ - drag_start_y_;
        view.pan = view.pan - Vec2{dx, dy} * (1.0f / view.zoom);
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
            view.pan = view.pan - Vec2{dx, dy} * (1.0f / view.zoom);
        }
        last_touch_x_ = t.x;
        last_touch_y_ = t.y;
    } else {
        last_touch_x_ = -1.0f;
        last_touch_y_ = -1.0f;
    }
}

}  // namespace ws::ui::canvas
