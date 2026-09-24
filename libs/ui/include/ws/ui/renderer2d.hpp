#pragma once

#include <cstdint>
#include <initializer_list>
#include <vector>

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

// An atlas sprite: one sub-rect of one packed atlas page (load_atlas).
// page is the page texture's id, so every sprite of a page shares one
// batch material no matter how interleaved the draw calls are; index
// selects the sub-rect. {} is the white sentinel failed entries degrade
// to - it draws plain white, like Texture{}.
struct Sprite {
    std::uint32_t page = 0;
    std::uint32_t index = 0;
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
// pixel. Every begin() opens a new camera block, so calling them repeatedly
// per frame layers blocks back to front.
//
// At the frame's end the shell sorts the recorded draws - explicit layer
// first, then camera block, then texture - and only then batches them. The
// sort is stable: overlapping draws that share layer, camera and texture
// always keep their submission order. Draws differing only in texture may
// be grouped (reordered); when the overlap order of two different-texture
// draws matters, give them an explicit set_layer().
void begin();
void begin(const Camera2D& camera);

// Ends the current camera block. Optional - an unclosed block flushes
// as-is - but draws between end() and the next begin() are dropped.
void end();

// Sorts all subsequent draws into `layer` (default 0). Higher layers draw
// later - on top - regardless of submission order or camera block. Sticky:
// stays until the next set_layer(), across frames.
void set_layer(std::uint32_t layer);

// Top-left anchored quad; rotation pivots the center (positive = clockwise,
// radians). Flat colors pass texture = {}.
void draw_quad(float x, float y, float w, float h, std::uint32_t color,
               Texture texture = {}, float rotation = 0.0f);

// Draws an atlas sprite: same quad contract as draw_quad, but the texture
// is the sprite's page and the UVs are its sub-rect - a page's sprites all
// batch into one segment. Rotation pivots the center. Unknown or sentinel
// sprites draw white.
void draw_sprite(float x, float y, float w, float h, std::uint32_t color,
                 Sprite sprite, float rotation = 0.0f);

// Quad of `thickness` pixels spanning the two endpoints (axis-crossed).
// Degenerate input (zero length, non-positive thickness) is dropped
// silently.
void draw_line(float x0, float y0, float x1, float y1, float thickness,
               std::uint32_t color);

// Loads an image file (PNG/JPG/..., whatever stb_image reads) and returns
// its handle. Valid from on_setup() onward. On failure logs and returns the
// white texture. Loading is synchronous - fine at load granularity.
Texture load_texture(const char* path);

// Packs the given image files into atlas page textures (2048x2048, shelf
// packed tallest-first, duplicated-edge gutters) and returns one Sprite
// per input, in input order. All sprites on a page share that page's
// texture, so interleaved sprite draws stay in one batch segment; pages
// spill when full and batch apart, degrading gracefully. Missing/failed
// images become Sprite{} (white), like load_texture. Valid from on_setup()
// onward; loading is synchronous.
std::vector<Sprite> load_atlas(std::initializer_list<const char*> paths);

// The sprite's original pixel size - atlas packing never scales. {0, 0}
// for unknown or sentinel sprites.
FrameSize sprite_size(Sprite sprite);

}  // namespace ws::ui::r2d
