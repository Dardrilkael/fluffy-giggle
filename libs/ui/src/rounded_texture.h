#pragma once

#include <cstdint>
#include <vector>

namespace ws::ui::r2d::rounded {

// The builtin rounded-rect mask draw_rounded_quad 9-slices: one square
// texture with a fixed baked corner radius. The four corner slices carry
// the arcs (scaled to whatever radius a call asks for), the strips and
// the center are solid - so only the corners need real resolution.
inline constexpr int kTexSize = 32;   // square, pixels
inline constexpr int kTexRadius = 8;  // baked corner radius, pixels

// Bakes the mask: white inside the rounded rect, transparent outside, a
// one-texel coverage ramp across the arcs so the corners land
// anti-aliased however they are later scaled. RGBA8, row-major from the
// top-left, kTexSize * kTexSize * 4 bytes. Pure CPU - the renderer
// uploads it once at init, the tests assert the pixels headlessly.
std::vector<std::uint8_t> bake();

}  // namespace ws::ui::r2d::rounded
