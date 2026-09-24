#include "atlas_packer.h"

#include <algorithm>
#include <numeric>

namespace ws::ui::r2d::atlas {
namespace {

// Writes img's cell (sprite + gutter ring) into the canvas at (x, y) - the
// cell's top-left, gutter included. Source reads clamp to the image's own
// edge pixels, so the ring duplicates the sprite's border rather than
// copying transparent canvas or reaching into a shelf neighbour.
void blit(std::uint8_t* canvas, std::int64_t page_size,
          const SourceImage& img, std::int64_t x, std::int64_t y) {
    const std::int64_t w = img.w;
    const std::int64_t h = img.h;
    for (std::int64_t gy = 0; gy < h + 2 * kGutter; ++gy) {
        const std::int64_t sy = std::clamp(gy - kGutter, std::int64_t{0}, h - 1);
        std::uint8_t* dst = canvas + ((y + gy) * page_size + x) * 4;
        const std::uint8_t* src = img.pixels + (sy * w) * 4;
        for (std::int64_t gx = 0; gx < w + 2 * kGutter; ++gx) {
            const std::int64_t sx = std::clamp(gx - kGutter, std::int64_t{0}, w - 1);
            dst[gx * 4 + 0] = src[sx * 4 + 0];
            dst[gx * 4 + 1] = src[sx * 4 + 1];
            dst[gx * 4 + 2] = src[sx * 4 + 2];
            dst[gx * 4 + 3] = src[sx * 4 + 3];
        }
    }
}

// Fills a whole cell white: the fallback sprite and its gutter.
void fill_white(std::uint8_t* canvas, std::int64_t page_size, std::int64_t x,
                std::int64_t y, std::int64_t cell_w, std::int64_t cell_h) {
    for (std::int64_t gy = 0; gy < cell_h; ++gy) {
        std::uint8_t* dst = canvas + ((y + gy) * page_size + x) * 4;
        for (std::int64_t gx = 0; gx < cell_w; ++gx) {
            dst[gx * 4 + 0] = 255;
            dst[gx * 4 + 1] = 255;
            dst[gx * 4 + 2] = 255;
            dst[gx * 4 + 3] = 255;
        }
    }
}

}  // namespace

Packing pack(const SourceImage* images, std::size_t count,
             std::uint32_t page_size) {
    Packing out;
    out.assigned.resize(count);

    // Tallest first packs shelves tightly (the classic shelf heuristic);
    // stable, so equal-height images keep their input order.
    std::vector<std::uint32_t> order(count);
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(),
                     [images](std::uint32_t a, std::uint32_t b) {
                         return images[a].h > images[b].h;
                     });

    const std::uint32_t cell_pad = 2 * kGutter;
    std::uint32_t cursor_x = 0;
    std::uint32_t cursor_y = 0;
    std::uint32_t shelf_h = 0;

    for (std::uint32_t idx : order) {
        const SourceImage& img = images[idx];
        // Degenerate or page-oversized inputs degrade to a white cell
        // instead of failing the whole atlas.
        const bool ok = img.pixels != nullptr && img.w > 0 && img.h > 0 &&
                        img.w + cell_pad <= page_size &&
                        img.h + cell_pad <= page_size;
        const std::uint32_t w = ok ? img.w : kFallbackSize;
        const std::uint32_t h = ok ? img.h : kFallbackSize;
        const std::uint32_t cell_w = w + cell_pad;
        const std::uint32_t cell_h = h + cell_pad;

        if (cursor_x + cell_w > page_size) {  // shelf full: wrap
            cursor_x = 0;
            cursor_y += shelf_h;
            shelf_h = 0;
        }
        if (out.pages.empty() || cursor_y + cell_h > page_size) {  // page full
            cursor_x = 0;
            cursor_y = 0;
            shelf_h = 0;
            Page page;
            page.size = page_size;
            page.pixels.assign(static_cast<std::size_t>(page_size) * page_size *
                                   4,
                               0);
            out.pages.push_back(std::move(page));
        }
        Page& page = out.pages.back();
        if (ok) {
            blit(page.pixels.data(), page_size, img, cursor_x, cursor_y);
        } else {
            fill_white(page.pixels.data(), page_size, cursor_x, cursor_y,
                       cell_w, cell_h);
        }

        const float inv = 1.0f / static_cast<float>(page_size);
        Rect rect;
        rect.u0 = static_cast<float>(cursor_x + kGutter) * inv;
        rect.v0 = static_cast<float>(cursor_y + kGutter) * inv;
        rect.u1 = static_cast<float>(cursor_x + kGutter + w) * inv;
        rect.v1 = static_cast<float>(cursor_y + kGutter + h) * inv;
        rect.w = w;
        rect.h = h;
        page.rects.push_back(rect);
        out.assigned[idx] = Placement{
            static_cast<std::uint32_t>(out.pages.size() - 1),
            static_cast<std::uint32_t>(page.rects.size() - 1)};

        cursor_x += cell_w;
        shelf_h = std::max(shelf_h, cell_h);
    }
    return out;
}

}  // namespace ws::ui::r2d::atlas
