#pragma once

// Internal: shell-owned 2D renderer, CPU-facing half. This header never
// reaches apps - the public surface is <ws/ui/renderer2d.hpp>. The Vulkan
// side (pipeline, buffers, texture registry, the flush inside the render
// pass) extends this class in src/renderer2d.cpp; the app-facing methods
// below only record CPU-side batch data.

#include "batch_core.h"

#include <ws/ui/renderer2d.hpp>

namespace ws::ui::r2d {

class Renderer2D {
public:
    // --- shell plumbing (app.cpp) ---
    // Starts a frame: stores the extent (frame_size() for apps) and resets
    // the batch. Draws are accepted until the flush or a drop.
    void frame_begin(float fb_width, float fb_height);
    // Abandons the frame: the batch is dropped, never deferred - same
    // contract as RenderCommandQueue::clear().
    void frame_drop();

    // --- app-facing entry points (forwarded from the free functions) ---
    void begin_screen();
    void begin(const Camera2D& camera);
    void end();
    void draw_quad(float x, float y, float w, float h, std::uint32_t color,
                   Texture texture, float rotation);
    void draw_line(float x0, float y0, float x1, float y1, float thickness,
                   std::uint32_t color);
    FrameSize frame_size() const;
    Texture load_texture(const char* path);

private:
    // Central guard: false when calls must drop (no active frame) - warns
    // once per frame, for any reason.
    bool accept(const char* what);
    // Caps hit mid-frame: warn once per frame.
    void warn_drop(const char* what);

    batch::BatchCore batch_;
    float fb_width_ = 0.0f;
    float fb_height_ = 0.0f;
    bool frame_active_ = false;
    bool warned_ = false;
};

// Points the public free functions at the live renderer. app.cpp installs it
// for the duration of App::run() and clears it at shutdown.
void set_active(Renderer2D* renderer);

}  // namespace ws::ui::r2d
