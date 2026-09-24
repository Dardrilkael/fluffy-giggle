#pragma once

// Internal: pure-CPU sprite atlas packer. Loads nothing, uploads nothing -
// it takes in-memory RGBA8 images and hands back packed page canvases plus
// per-image sub-rects, ready for Renderer2D::upload_texture(). Shelf
// packing (rows, tallest image first), the stb_rect_pack / ImGui font
// atlas strategy: tight enough for icon sets without Guillotine/MaxRects
// machinery. This header never reaches apps.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ws::ui::r2d::atlas {

// Square page side, px (default; pack() accepts smaller for tests). A
// 2048 page holds ~1000 64px icons.
inline constexpr std::uint32_t kPageSize = 2048;
// Duplicated-edge border around every sprite: magnified edges never sample
// a shelf neighbour's texels (or the transparent canvas) under the linear
// sampler.
inline constexpr std::uint32_t kGutter = 1;
// Cell a failed or page-oversized image degrades to (solid white - the
// load_texture fallback contract, applied per sprite).
inline constexpr std::uint32_t kFallbackSize = 2;

// One packed sprite: the normalized sub-rect within its page plus the
// source image's pixel size (packing never scales). u/v 0 = page top-left,
// the same y-down convention the draw commands already use.
struct Rect {
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
};

// Borrowed input: RGBA8, row-major, top row first (exactly stb_image's
// layout). pixels == null marks a failed load; it degrades to a white cell.
struct SourceImage {
    const std::uint8_t* pixels = nullptr;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
};

// One packed output page: pixels is size*size*4 bytes, ready to upload as a
// single texture. rects is in placement order.
struct Page {
    std::vector<std::uint8_t> pixels;  // zero-init canvas, sprites blitted in
    std::uint32_t size = 0;            // square: width == height == size
    std::vector<Rect> rects;
};

// Where input i landed: page index + that rect's index within the page.
struct Placement {
    std::uint32_t page = 0;
    std::uint32_t index = 0;
};

struct Packing {
    std::vector<Page> pages;
    std::vector<Placement> assigned;  // one per input, in input order
};

// Packs count images into square pages. Every input gets a placement -
// failed, degenerate, or larger-than-page images get a white fallback cell
// instead of failing the atlas. Pages spill when a shelf no longer fits.
Packing pack(const SourceImage* images, std::size_t count,
             std::uint32_t page_size = kPageSize);

}  // namespace ws::ui::r2d::atlas
