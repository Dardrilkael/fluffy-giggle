#include "renderer2d.h"

#include <ws/core/log.hpp>

namespace ws::ui::r2d {

// --- Renderer2D: shell plumbing --------------------------------------------

void Renderer2D::frame_begin(float fb_width, float fb_height) {
    fb_width_ = fb_width;
    fb_height_ = fb_height;
    batch_.new_frame();
    frame_active_ = true;
    warned_ = false;
}

void Renderer2D::frame_drop() {
    frame_active_ = false;
    batch_.new_frame();
}

// --- Renderer2D: app-facing entry points -----------------------------------

bool Renderer2D::accept(const char* what) {
    if (frame_active_) {
        return true;
    }
    warn_drop(what);
    return false;
}

void Renderer2D::warn_drop(const char* what) {
    if (warned_) {
        return;
    }
    warned_ = true;
    ws::core::warn("ui: r2d::{} dropped - draws are only valid inside on_update()",
                   what);
}

void Renderer2D::begin_screen() {
    if (!accept("begin")) {
        return;
    }
    // Top-left origin: center the default camera on the extent midpoint.
    const Camera2D screen{fb_width_ * 0.5f, fb_height_ * 0.5f, 0.0f, 1.0f};
    if (!batch_.open_segment(batch::camera_vp(screen, fb_width_, fb_height_))) {
        warn_drop("begin");  // chunk cap
    }
}

void Renderer2D::begin(const Camera2D& camera) {
    if (!accept("begin")) {
        return;
    }
    if (!batch_.open_segment(batch::camera_vp(camera, fb_width_, fb_height_))) {
        warn_drop("begin");  // chunk cap
    }
}

void Renderer2D::end() {
    if (!accept("end")) {
        return;
    }
    batch_.close_segment();
}

void Renderer2D::draw_quad(float x, float y, float w, float h,
                           std::uint32_t color, Texture texture,
                           float rotation) {
    if (!accept("draw_quad")) {
        return;
    }
    float pos[4][2];
    batch::quad_corners(x, y, w, h, rotation, pos);
    const float uv[4][2] = {
        {0.0f, 0.0f},  // TL
        {1.0f, 0.0f},  // TR
        {1.0f, 1.0f},  // BR
        {0.0f, 1.0f},  // BL
    };
    float quad_color[4];
    batch::unpack_rgba(color, quad_color);
    if (!batch_.push_quad(pos, uv, quad_color, texture.id)) {
        warn_drop("draw_quad");  // caps
    }
}

void Renderer2D::draw_line(float x0, float y0, float x1, float y1,
                           float thickness, std::uint32_t color) {
    if (!accept("draw_line")) {
        return;
    }
    float pos[4][2];
    float uv[4][2];
    if (!batch::line_quad(x0, y0, x1, y1, thickness, pos, uv)) {
        return;  // degenerate: live app data, not worth a warning
    }
    float quad_color[4];
    batch::unpack_rgba(color, quad_color);
    if (!batch_.push_quad(pos, uv, quad_color, 0)) {
        warn_drop("draw_line");  // caps
    }
}

FrameSize Renderer2D::frame_size() const {
    return FrameSize{fb_width_, fb_height_};
}

Texture Renderer2D::load_texture(const char* path) {
    // The GPU texture registry lands with the Vulkan half; until then every
    // load falls back to white.
    ws::core::warn("ui: r2d::load_texture('{}') fell back to white - the GPU "
                   "texture registry is not wired yet", path);
    return Texture{};
}

// --- public free functions ----------------------------------------------------

namespace {

Renderer2D* g_active = nullptr;

// No app is running at all (or run() has returned): warn once per process -
// this is an app bug, not something to spam per call.
void warn_no_app(const char* what) {
    static bool warned = false;
    if (warned) {
        return;
    }
    warned = true;
    ws::core::warn("ui: r2d::{} dropped - no app is running (the 2D API only "
                   "draws while App::run() is active)", what);
}

}  // namespace

void set_active(Renderer2D* renderer) {
    g_active = renderer;
}

void begin() {
    if (g_active != nullptr) {
        g_active->begin_screen();
    } else {
        warn_no_app("begin");
    }
}

void begin(const Camera2D& camera) {
    if (g_active != nullptr) {
        g_active->begin(camera);
    } else {
        warn_no_app("begin");
    }
}

void end() {
    if (g_active != nullptr) {
        g_active->end();
    } else {
        warn_no_app("end");
    }
}

void draw_quad(float x, float y, float w, float h, std::uint32_t color,
               Texture texture, float rotation) {
    if (g_active != nullptr) {
        g_active->draw_quad(x, y, w, h, color, texture, rotation);
    } else {
        warn_no_app("draw_quad");
    }
}

void draw_line(float x0, float y0, float x1, float y1, float thickness,
               std::uint32_t color) {
    if (g_active != nullptr) {
        g_active->draw_line(x0, y0, x1, y1, thickness, color);
    } else {
        warn_no_app("draw_line");
    }
}

FrameSize frame_size() {
    if (g_active != nullptr) {
        return g_active->frame_size();
    }
    return FrameSize{};
}

Texture load_texture(const char* path) {
    if (g_active != nullptr) {
        return g_active->load_texture(path);
    }
    warn_no_app("load_texture");
    return Texture{};
}

}  // namespace ws::ui::r2d
