#include "rounded_texture.h"

#include <algorithm>
#include <cmath>

namespace ws::ui::r2d::rounded {

std::vector<std::uint8_t> bake() {
    const float n = static_cast<float>(kTexSize);
    const float r = static_cast<float>(kTexRadius);
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(kTexSize) * kTexSize * 4, 0);
    for (int y = 0; y < kTexSize; ++y) {
        for (int x = 0; x < kTexSize; ++x) {
            // Texel center, top-left origin.
            const float tx = static_cast<float>(x) + 0.5f;
            const float ty = static_cast<float>(y) + 0.5f;
            // Clamping into the [r, n-r] box maps a corner texel to its
            // quarter-circle's center; a texel already inside the box sits
            // on a flat edge and is solid.
            const float cx = std::clamp(tx, r, n - r);
            const float cy = std::clamp(ty, r, n - r);
            float coverage = 1.0f;
            if (cx != tx || cy != ty) {
                const float dx = tx - cx;
                const float dy = ty - cy;
                coverage = std::clamp(r - std::sqrt(dx * dx + dy * dy) +
                                          0.5f,
                                      0.0f, 1.0f);
            }
            std::uint8_t* px =
                &pixels[(static_cast<std::size_t>(y) * kTexSize + x) * 4];
            px[0] = 255;  // white fill - only alpha carries the mask
            px[1] = 255;
            px[2] = 255;
            px[3] = static_cast<std::uint8_t>(coverage * 255.0f + 0.5f);
        }
    }
    return pixels;
}

}  // namespace ws::ui::r2d::rounded
