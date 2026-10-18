#pragma once

// The pure decision helpers behind ws::gfx's swapchain creation - kept free
// of every Vulkan type so tests cover the decision tables headless, no
// device faked. swapchain.cpp fills these inputs from the Vulkan queries
// (and supplies the typed constants); the helpers only decide.

#include <algorithm>  // std::clamp, std::min

#include <cstdint>

namespace ws::gfx {

// One surface format as the Vulkan query reports it: a VkFormat and a
// VkColorSpaceKHR value, each spelled as uint32.
struct SurfaceFormatChoice {
    uint32_t format = 0;
    uint32_t color_space = 0;
};

// The surface-format walk: the first preferred format the surface offers
// with the required color space wins. An EMPTY list means the surface has
// no format restrictions (the caller folds Vulkan's "any format" sentinel
// into empty) and is answered with `unrestricted`; a restricted surface
// offering none of the preferred formats falls back to its own first entry
// - the driver knows best, and it is virtually always the top preferred
// format anyway.
inline SurfaceFormatChoice choose_surface_format(
    const SurfaceFormatChoice* formats, uint32_t count, const uint32_t* preferred,
    uint32_t preferred_count, uint32_t required_color_space,
    SurfaceFormatChoice unrestricted) {
    if (count == 0) {
        return unrestricted;
    }
    for (uint32_t p = 0; p < preferred_count; ++p) {
        for (uint32_t i = 0; i < count; ++i) {
            if (formats[i].format == preferred[p] &&
                formats[i].color_space == required_color_space) {
                return formats[i];
            }
        }
    }
    return formats[0];
}

// The present-mode walk: the first of `preferences` the surface supports,
// `fallback` otherwise. swapchain.cpp passes {mailbox, immediate} with a
// FIFO fallback for vsync-off; vsync-on never walks at all - FIFO is the
// one mode the Vulkan spec guarantees, assigned directly. A missing FIFO
// is a broken driver and surfaces at vkCreateSwapchainKHR, not here: the
// choice never second-guesses the spec.
inline uint32_t choose_present_mode(const uint32_t* preferences,
                                    uint32_t preference_count,
                                    const uint32_t* available,
                                    uint32_t available_count, uint32_t fallback) {
    for (uint32_t p = 0; p < preference_count; ++p) {
        for (uint32_t i = 0; i < available_count; ++i) {
            if (preferences[p] == available[i]) {
                return preferences[p];
            }
        }
    }
    return fallback;
}

// The extent choice: the surface's fixed current extent, or - the
// 0xFFFFFFFF sentinel, "match the window" - the caller's framebuffer size
// clamped into the surface's min/max. False when the result is degenerate:
// a window with no visible area cannot back a swapchain, and the caller
// fails loudly rather than render into a zero-sized image.
struct ExtentChoice {
    uint32_t width = 0;
    uint32_t height = 0;
};
inline bool choose_extent(const ExtentChoice& current, const ExtentChoice& min_extent,
                          const ExtentChoice& max_extent, uint32_t fb_width,
                          uint32_t fb_height, ExtentChoice* out) {
    if (current.width != 0xFFFFFFFFu) {
        out->width = current.width;
        out->height = current.height;
    } else {
        out->width = std::clamp(fb_width, min_extent.width, max_extent.width);
        out->height = std::clamp(fb_height, min_extent.height, max_extent.height);
    }
    return out->width != 0 && out->height != 0;
}

// The image-count request: one more than the surface's minimum (room to
// render while presenting), clamped to its maximum when it has one
// (max == 0 means unlimited).
inline uint32_t choose_image_count(uint32_t min_images, uint32_t max_images) {
    uint32_t request = min_images + 1;
    if (max_images != 0) {
        request = std::min(request, max_images);
    }
    return request;
}

// The composite-alpha choice: opaque - what every window here wants - or
// else the lowest bit the surface supports (opaque is universal in
// practice). The bit values are VkCompositeAlphaFlagBitsKHR spelled as
// uint32.
inline uint32_t choose_composite_alpha(uint32_t supported_bits, uint32_t opaque_bit) {
    if ((supported_bits & opaque_bit) != 0) {
        return opaque_bit;
    }
    for (uint32_t bit = 1; bit != 0; bit <<= 1) {
        if ((supported_bits & bit) != 0) {
            return bit;
        }
    }
    return opaque_bit;
}

}  // namespace ws::gfx
