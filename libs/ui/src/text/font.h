#pragma once

// Internal: the pure-CPU bake behind r2d::load_font (stb_truetype does the
// rasterization - PRIVATE, like every third-party) plus the ws-owned UTF-8
// decoder that draw_text and measure_text share. No Vulkan, no renderer
// state, no platform code: bake_file takes a caller-resolved path and
// returns bitmaps and metrics, so tests drive it headless. This header
// never reaches apps.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ws::ui::r2d::font {

// The bake range - must match the public Font's (ASCII 32..126). The
// synthesized fallback box (appended by bake_file) lands at slot
// kBakedCount, mirroring the public Font's kFallbackGlyph.
inline constexpr std::uint32_t kFirstCodepoint = 32;
inline constexpr std::uint32_t kBakedCount = 95;

// Decodes the next UTF-8 sequence at s and advances s past it. Returns the
// codepoint, 0 at the string's end (without consuming the terminator), or
// U+FFFD for malformed input - a forgiving decoder: a bad lead byte or
// broken continuation eats one byte and moves on, so a stray byte never
// stalls the rest of a string. Overlong encodings of NUL are rejected
// (they would collide with the end sentinel); anything that decodes
// cleanly is taken as-is.
std::uint32_t utf8_next(const char*& s);

// One glyph straight out of the rasterizer: its coverage bitmap as RGBA8
// (RGB white, coverage in alpha - the rounded-mask convention, so the
// vertex tint supplies the text color) plus the metrics, in px at the
// baked size. w == 0: no bitmap (the space); advance still applies.
// offset_y is measured from the LINE BOX TOP, not the baseline - draw_text
// positions glyphs from the line top, so the ascent is already folded in.
struct BakedGlyph {
    std::uint32_t codepoint = 0;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    float advance = 0.0f;    // pen advance to the next glyph
    float offset_x = 0.0f;  // bitmap left, relative to the pen
    float offset_y = 0.0f;  // bitmap top, relative to the line top
    std::vector<std::uint8_t> rgba;  // w*h*4, empty when w == 0
};

// One whole bake: the glyphs for 32..126 in order, then the fallback box
// (slot kBakedCount - every codepoint past the range draws it), plus the
// line metrics. ok == false covers every failure (missing file, not a
// font, bad size) - callers degrade to an empty public Font, never
// crash; a failed bake appends nothing, so that font's fallback slot
// stays zeroed and nothing draws.
struct BakedFont {
    bool ok = false;
    float pixel_height = 0.0f;
    float line_height = 0.0f;  // (ascent - descent + lineGap) * scale
    std::vector<BakedGlyph> glyphs;
};

// Reads the ttf/otf at `path`, rasterizes it at `pixel_height` px and
// appends the synthesized fallback box - kBakedCount + 1 glyphs in all.
// Pure CPU; the caller owns the upload (Renderer2D::load_font does).
BakedFont bake_file(const char* path, float pixel_height);

}  // namespace ws::ui::r2d::font
