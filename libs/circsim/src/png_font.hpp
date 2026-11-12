#pragma once

#include <array>
#include <cstdint>

// png_font.cpp's seam: the PNG raster exporter's glyph table, internal
// to libs/circsim/src (like json_helpers.hpp, a src-local header).

namespace ws::circsim {

// One ASCII glyph: 16 rows, 16 bit-columns (bit 15 = column 0), and
// the advance in pixels at scale 1. Pure data; png.cpp draws it.
struct PngGlyph {
    std::uint8_t advance = 0;
    std::array<std::uint16_t, 16> rows{};
};

// ASCII 32..126 -> the glyph; null for every other byte.
[[nodiscard]] const PngGlyph* png_glyph(unsigned char byte);

}  // namespace ws::circsim
