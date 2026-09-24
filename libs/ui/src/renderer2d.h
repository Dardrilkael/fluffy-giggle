#pragma once

// Internal: shell-owned 2D renderer, CPU-facing half. This header never
// reaches apps - the public surface is <ws/ui/renderer2d.hpp>. App-facing
// methods only RECORD commands (a draw is data until the frame ends);
// frame_end() sorts the frame's commands - explicit layer first, then
// begin() block, then material (stable, so same-state draws never flip) -
// and replays them into the batch. The Vulkan half extends this class in
// src/renderer2d.cpp and flushes that batch inside the render pass.

#include "batch_core.h"

#include <cstdint>
#include <vector>

#include <ws/ui/renderer2d.hpp>

namespace ws::ui::r2d {

// One recorded quad - lines are expanded into quads at record time. Kept
// as data so the whole frame can be sorted before batching: state changes,
// not submission order alone, decide the segment structure.
struct DrawCommand {
    // Sort key components, most significant first:
    std::uint32_t sort_layer = 0;    // set_layer(): the explicit z escape hatch
    std::uint32_t camera_epoch = 0;  // which begin() block recorded this
    std::uint64_t material_key = 0;  // texture (+ future blend/shader bits)

    Camera2D camera;           // camera state at record time
    Texture texture;            // {} = flat color (the white texture)
    std::uint32_t color = 0;  // packed 0xRRGGBBAA, unpacked at replay

    float pos[4][2] = {};  // TL, TR, BR, BL
    float uv[4][2] = {};
};

// v1 packs just the texture id; blend mode and shader id are the reserved
// upper bits (shader << 40 | blend << 32) for the day they exist - the
// sort comparator's shape never changes.
inline std::uint64_t make_material_key(Texture texture) {
    return static_cast<std::uint64_t>(texture.id);
}

class Renderer2D {
public:
    // --- shell plumbing (app.cpp) ---
    // Starts a frame: stores the extent, resets the batch and the command
    // list (capacity kept). Draws are accepted until frame_end/frame_drop.
    void frame_begin(float fb_width, float fb_height);
    // The batching point: sorts the recorded commands and replays them
    // into the batch. Call once per frame, after on_update() returned.
    void frame_end();
    // Abandons the frame: commands and batch are dropped, never deferred -
    // same contract as RenderCommandQueue::clear().
    void frame_drop();

    // --- app-facing entry points (forwarded from the free functions) ---
    void begin_screen();
    void begin(const Camera2D& camera);
    void end();
    void set_layer(std::uint32_t layer);
    void draw_quad(float x, float y, float w, float h, std::uint32_t color,
                   Texture texture = {}, float rotation = 0.0f);
    void draw_line(float x0, float y0, float x1, float y1, float thickness,
                   std::uint32_t color);
    FrameSize frame_size() const;
    Texture load_texture(const char* path);

    // Test/observability seam (internal header only): the built batch as
    // frame_end() left it.
    const batch::BatchCore& batch() const { return batch_; }

private:
    // Central guard: false when calls must drop (no active frame) - warns
    // once per frame, for any reason.
    bool accept(const char* what);
    // Misuse warn (draw outside on_update() / begin() block): once per frame.
    void warn_drop(const char* what);
    // Caps hit during replay: once per frame.
    void warn_caps();
    // Stashes camera state and opens a new begin() block (bumps the epoch).
    void stash_camera(const Camera2D& camera);
    // Appends one fully-expanded quad command; drops it (with the once-per-
    // frame warn) when no begin() block is open.
    void record(const char* what, const float (&pos)[4][2],
                const float (&uv)[4][2], std::uint32_t color, Texture texture);

    std::vector<DrawCommand> commands_;  // capacity reused across frames
    batch::BatchCore batch_;
    Camera2D current_camera_{};
    std::uint32_t current_layer_ = 0;  // sticky across frames until set_layer()
    std::uint32_t current_epoch_ = 0;  // bumped by every begin()
    float fb_width_ = 0.0f;
    float fb_height_ = 0.0f;
    bool camera_stashed_ = false;  // set by begin(), cleared by end()
    bool frame_active_ = false;
    bool warned_ = false;
};

// Points the public free functions at the live renderer. app.cpp installs it
// for the duration of App::run() and clears it at shutdown.
void set_active(Renderer2D* renderer);

}  // namespace ws::ui::r2d
