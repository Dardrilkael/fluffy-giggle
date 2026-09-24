#pragma once

#include <cstdint>

namespace ws::ui::r2d {

// 2D renderer: batched quads and lines, textured or flat. Apps record draws
// from on_update() only, exactly like the widgets.hpp calls; the shell owns
// every GPU detail (pipeline, buffers, flush point - 2D renders under the
// widget layer, command-queue overlays render on top). Draws are recorded
// CPU-side during on_update() and never outlive their frame: a frame that
// can't render drops them, it never defers them (same contract as the
// render command queue). Every call is drop-safe outside a running app -
// logged once, then ignored.

// --- camera ---------------------------------------------------------------------
// World-space camera. x/y = the world point shown at the window's CENTER;
// rotation = radians, positive rolls the view clockwise on screen; zoom =
// magnification (2 shows half the world span; 1 makes world units equal
// framebuffer pixels). World +y renders downward, matching screen coords.
struct Camera2D {
    float x = 0.0f;
    float y = 0.0f;
    float rotation = 0.0f;
    float zoom = 1.0f;
};

// --- textures ---------------------------------------------------------------------
// Opaque handle. 0 = the built-in 1x1 white texture (also the fallback for
// failed loads). Ids are stable for the process lifetime; v1 frees textures
// only at shutdown, so handles never dangle.
struct Texture {
    std::uint32_t id = 0;
};

// Packs channels as 0xRRGGBBAA. NOT ImGui's IM_COL32 order - don't paste
// ImGui color literals into r2d calls.
inline constexpr std::uint32_t rgba(std::uint8_t r, std::uint8_t g,
                                    std::uint8_t b, std::uint8_t a = 255) {
    return (std::uint32_t(r) << 24) | (std::uint32_t(g) << 16) |
           (std::uint32_t(b) << 8) | std::uint32_t(a);
}

// Framebuffer size in pixels - the space the 2D world is defined in.
// WindowResizeEvent carries *screen* coordinates, which differ under DPI
// scaling; this reports what the frame actually renders at. {0, 0} when no
// app is running.
struct FrameSize {
    float width = 0.0f;
    float height = 0.0f;
};
FrameSize frame_size();

// --- frame recording ----------------------------------------------------------------
// Screen mode: world (0,0) = window top-left, 1 world unit = 1 framebuffer
// pixel. Both begin() overloads close any open camera segment first, so
// calling them repeatedly per frame layers cameras back to front.
void begin();
void begin(const Camera2D& camera);

// Closes the open segment. Optional - an unclosed segment flushes as-is -
// but draws between end() and the next begin() are dropped.
void end();

// Top-left anchored quad; rotation pivots the center (positive = clockwise,
// radians). Flat colors pass texture = {}.
void draw_quad(float x, float y, float w, float h, std::uint32_t color,
               Texture texture = {}, float rotation = 0.0f);

// Quad of `thickness` pixels spanning the two endpoints (axis-crossed).
// Degenerate input (zero length, non-positive thickness) is dropped
// silently.
void draw_line(float x0, float y0, float x1, float y1, float thickness,
               std::uint32_t color);

// Loads an image file (PNG/JPG/..., whatever stb_image reads) and returns
// its handle. Valid from on_setup() onward. On failure logs and returns the
// white texture. Loading is synchronous - fine at load granularity.
Texture load_texture(const char* path);

}  // namespace ws::ui::r2d
