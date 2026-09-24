#include "renderer2d.h"

#include <algorithm>

#include <ws/core/log.hpp>

namespace ws::ui::r2d {

namespace {

// TL, TR, BR, BL - matches the shared index pattern (0,1,2 / 2,3,0).
constexpr float kQuadUV[4][2] = {
    {0.0f, 0.0f},  // TL
    {1.0f, 0.0f},  // TR
    {1.0f, 1.0f},  // BR
    {0.0f, 1.0f},  // BL
};

}  // namespace

// --- Renderer2D: shell plumbing --------------------------------------------

void Renderer2D::frame_begin(float fb_width, float fb_height) {
    fb_width_ = fb_width;
    fb_height_ = fb_height;
    batch_.new_frame();
    commands_.clear();  // capacity kept - the frame reuses the arena
    current_epoch_ = 0;
    camera_stashed_ = false;
    frame_active_ = true;
    warned_ = false;
}

void Renderer2D::frame_end() {
    frame_active_ = false;
    if (commands_.empty()) {
        return;
    }
    // Layer first (the explicit z escape hatch), then begin() block, then
    // material. Stable, because reordering must never flip two overlapping
    // same-state draws.
    std::stable_sort(commands_.begin(), commands_.end(),
                     [](const DrawCommand& a, const DrawCommand& b) {
                         if (a.sort_layer != b.sort_layer) {
                             return a.sort_layer < b.sort_layer;
                         }
                         if (a.camera_epoch != b.camera_epoch) {
                             return a.camera_epoch < b.camera_epoch;
                         }
                         return a.material_key < b.material_key;
                     });

    // Replay: a segment opens per begin() block; the block's 32-slot texture
    // table absorbs material variety, so material changes never split. Runs
    // of the same block (same camera, different layers) share the segment -
    // their sorted vertex order already paints correctly.
    std::uint32_t open_epoch = 0;  // 0 never matches a recorded block
    for (const DrawCommand& cmd : commands_) {
        if (cmd.camera_epoch != open_epoch) {
            batch_.close_segment();
            batch_.open_segment(
                batch::camera_vp(cmd.camera, fb_width_, fb_height_));
            open_epoch = cmd.camera_epoch;
        }
        float color[4];
        batch::unpack_rgba(cmd.color, color);
        if (!batch_.push_quad(cmd.pos, cmd.uv, color, cmd.texture.id)) {
            warn_caps();
        }
    }
    batch_.close_segment();
    commands_.clear();
}

void Renderer2D::frame_drop() {
    frame_active_ = false;
    camera_stashed_ = false;
    commands_.clear();
    batch_.new_frame();
}

// --- Renderer2D: guards -----------------------------------------------------

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
    ws::core::warn("ui: r2d::{} dropped - draws are only valid inside "
                   "on_update(), within a begin()/end() block",
                   what);
}

void Renderer2D::warn_caps() {
    if (warned_) {
        return;
    }
    warned_ = true;
    ws::core::warn("ui: r2d draws dropped - frame batch caps exhausted "
                   "({} segments, {} vertices)",
                   batch::kMaxChunksPerFrame, batch::kMaxVerticesPerFrame);
}

// --- Renderer2D: app-facing entry points -----------------------------------

void Renderer2D::stash_camera(const Camera2D& camera) {
    ++current_epoch_;
    current_camera_ = camera;
    camera_stashed_ = true;
}

void Renderer2D::record(const char* what, const float (&pos)[4][2],
                        const float (&uv)[4][2], std::uint32_t color,
                        Texture texture) {
    if (!camera_stashed_) {
        warn_drop(what);  // outside any begin()/end() block
        return;
    }
    DrawCommand cmd;
    cmd.sort_layer = current_layer_;
    cmd.camera_epoch = current_epoch_;
    cmd.material_key = make_material_key(texture);
    cmd.camera = current_camera_;
    cmd.texture = texture;
    cmd.color = color;
    for (int i = 0; i < 4; ++i) {
        cmd.pos[i][0] = pos[i][0];
        cmd.pos[i][1] = pos[i][1];
        cmd.uv[i][0] = uv[i][0];
        cmd.uv[i][1] = uv[i][1];
    }
    commands_.push_back(cmd);
}

void Renderer2D::begin_screen() {
    if (!accept("begin")) {
        return;
    }
    // Top-left origin: center the default camera on the extent midpoint.
    const Camera2D screen{fb_width_ * 0.5f, fb_height_ * 0.5f, 0.0f, 1.0f};
    stash_camera(screen);
}

void Renderer2D::begin(const Camera2D& camera) {
    if (!accept("begin")) {
        return;
    }
    stash_camera(camera);
}

void Renderer2D::end() {
    if (!accept("end")) {
        return;
    }
    camera_stashed_ = false;  // draws after end() drop until the next begin()
}

void Renderer2D::set_layer(std::uint32_t layer) {
    if (!accept("set_layer")) {
        return;
    }
    current_layer_ = layer;
}

void Renderer2D::draw_quad(float x, float y, float w, float h,
                           std::uint32_t color, Texture texture,
                           float rotation) {
    if (!accept("draw_quad")) {
        return;
    }
    float pos[4][2];
    batch::quad_corners(x, y, w, h, rotation, pos);
    record("draw_quad", pos, kQuadUV, color, texture);
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
    record("draw_line", pos, uv, color, Texture{});  // lines are solid color
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

void set_layer(std::uint32_t layer) {
    if (g_active != nullptr) {
        g_active->set_layer(layer);
    } else {
        warn_no_app("set_layer");
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
