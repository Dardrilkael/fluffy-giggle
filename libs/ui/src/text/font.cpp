// The public Font's out-of-line half (declared in <ws/ui/renderer2d.hpp>)
// plus the stb_truetype bake behind r2d::load_font. Pure CPU throughout:
// no Vulkan, no renderer state, no platform headers - bake_file takes a
// caller-resolved path, so this TU builds and runs anywhere.

#include "text/font.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <stb_truetype.h>  // declarations only; the implementation TU is stb_impl.cpp
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <ws/ui/renderer2d.hpp>  // the public Font whose out-of-line half lives here

#include <algorithm>
#include <cmath>
#include <fstream>
#include <utility>

namespace ws::ui::r2d {

// --- public Font ------------------------------------------------------------

Font::Font(float pixel_height, float line_height, const Glyph* glyphs,
           std::uint32_t count)
    : pixel_height_(pixel_height), line_height_(line_height) {
    if (glyphs == nullptr) {
        return;
    }
    const std::uint32_t n =
        count > kFallbackGlyph + 1 ? kFallbackGlyph + 1 : count;
    for (std::uint32_t i = 0; i < n; ++i) {
        glyphs_[i] = glyphs[i];
    }
}

const Font::Glyph* Font::glyph(std::uint32_t codepoint) const {
    if (codepoint < kFirstBaked) {
        return nullptr;  // the controls: no draw, no advance
    }
    if (codepoint >= kFirstBaked + kBakedCount) {
        // Past the bake: the fallback box. A zeroed slot (a font built
        // with fewer glyphs, a failed load) keeps the old skip - no
        // draw, no advance - by data.
        return &glyphs_[kFallbackGlyph];
    }
    return &glyphs_[codepoint - kFirstBaked];
}

float Font::measure_text(const char* utf8_text) const {
    if (utf8_text == nullptr) {
        return 0.0f;
    }
    float width = 0.0f;
    const char* s = utf8_text;
    for (std::uint32_t cp = font::utf8_next(s); cp != 0u;
         cp = font::utf8_next(s)) {
        const Glyph* g = glyph(cp);
        if (g != nullptr) {
            width += g->advance;
        }
    }
    return width;
}

namespace font {

namespace {

// The whole file as bytes; a missing or unreadable font bakes nothing.
std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const std::streamoff size = file.tellg();
    std::vector<std::uint8_t> data;
    if (size > 0) {
        data.resize(static_cast<std::size_t>(size));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(data.data()),
                  static_cast<std::streamsize>(size));
    }
    return data;
}

}  // namespace

std::uint32_t utf8_next(const char*& s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    const std::uint32_t lead = p[0];
    if (lead == 0u) {
        return 0u;  // the terminator: end of string, s not advanced
    }
    std::uint32_t cp = 0;
    std::uint32_t len = 0;
    if (lead < 0x80u) {
        cp = lead;
        len = 1;
    } else if ((lead & 0xE0u) == 0xC0u) {
        cp = lead & 0x1Fu;
        len = 2;
    } else if ((lead & 0xF0u) == 0xE0u) {
        cp = lead & 0x0Fu;
        len = 3;
    } else if ((lead & 0xF8u) == 0xF0u) {
        cp = lead & 0x07u;
        len = 4;
    } else {
        s += 1;  // not a UTF-8 lead byte: one replacement, one byte eaten
        return 0xFFFDu;
    }
    for (std::uint32_t i = 1; i < len; ++i) {
        if ((p[i] & 0xC0u) != 0x80u) {
            s += 1;  // broken continuation: same one-byte policy
            return 0xFFFDu;
        }
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    s += len;
    if (cp == 0u || cp > 0x10FFFFu) {
        // Overlong NUL would collide with the end sentinel, and past
        // 0x10FFFF is not Unicode - the well-formed bytes are consumed,
        // the decoded value is not trusted.
        return 0xFFFDu;
    }
    return cp;
}

BakedFont bake_file(const char* path, float pixel_height) {
    BakedFont out;
    if (path == nullptr || pixel_height <= 0.0f) {
        return out;
    }
    // stbtt keeps pointers into this buffer for every call below, hence
    // one named local that outlives the bake - not a chained expression.
    const std::vector<std::uint8_t> data = read_file(path);
    if (data.empty()) {
        return out;
    }
    const int offset = stbtt_GetFontOffsetForIndex(data.data(), 0);
    stbtt_fontinfo info;
    if (offset < 0 || !stbtt_InitFont(&info, data.data(), offset)) {
        return out;
    }

    const float scale = stbtt_ScaleForPixelHeight(&info, pixel_height);
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
    out.pixel_height = pixel_height;
    out.line_height = (static_cast<float>(ascent) -
                       static_cast<float>(descent) +
                       static_cast<float>(line_gap)) * scale;
    const float ascent_px = static_cast<float>(ascent) * scale;

    out.glyphs.reserve(kBakedCount + 1);
    for (std::uint32_t cp = kFirstCodepoint;
         cp < kFirstCodepoint + kBakedCount; ++cp) {
        BakedGlyph g;
        g.codepoint = cp;
        int advance = 0;
        int lsb = 0;  // unused: offset_x comes from the bitmap box below
        stbtt_GetCodepointHMetrics(&info, static_cast<int>(cp), &advance,
                                  &lsb);
        g.advance = static_cast<float>(advance) * scale;
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        stbtt_GetCodepointBitmapBox(&info, static_cast<int>(cp), scale,
                                   scale, &x0, &y0, &x1, &y1);
        g.offset_x = static_cast<float>(x0);
        // stbtt's box is baseline-relative; the glyph contract measures
        // from the LINE TOP, which sits ascent px above the baseline.
        g.offset_y = ascent_px + static_cast<float>(y0);
        const int w = x1 - x0;
        const int h = y1 - y0;
        if (w > 0 && h > 0) {
            g.w = static_cast<std::uint32_t>(w);
            g.h = static_cast<std::uint32_t>(h);
            std::vector<std::uint8_t> coverage(static_cast<std::size_t>(w) *
                                               static_cast<std::size_t>(h));
            stbtt_MakeCodepointBitmap(&info, coverage.data(), w, h,
                                      w,  // stride: rows are tight
                                      scale, scale, static_cast<int>(cp));
            // The tint convention: RGB white, coverage in alpha - the
            // vertex tint at draw time supplies the text color.
            constexpr std::uint8_t white = 255;
            g.rgba.assign(coverage.size() * 4u, white);
            for (std::size_t i = 0; i < coverage.size(); ++i) {
                g.rgba[i * 4u + 3u] = coverage[i];
            }
        }
        out.glyphs.push_back(std::move(g));
    }

    // The fallback box - the bake's one glyph past the ASCII range, the
    // "tofu" every codepoint outside 32..126 draws. Synthesized, never
    // rasterized: cap-height tall, half an em wide, a 1 px stroke, RGB
    // white with the coverage in alpha like every baked bitmap, and an
    // advance wide enough that consecutive boxes read as boxes, not a
    // merged strip. The box's bottom sits on the baseline (clamped at
    // the line top for a degenerate ascent). Only a successful bake
    // reaches here - a failed one appends nothing, so an empty public
    // Font keeps its fallback zeroed and draws nothing.
    BakedGlyph tofu;
    tofu.codepoint = 0xFFFDu;  // the replacement character's own face
    tofu.w = std::max(
        2u, static_cast<std::uint32_t>(std::lround(0.5f * pixel_height)));
    tofu.h = std::max(
        2u, static_cast<std::uint32_t>(std::lround(0.7f * pixel_height)));
    tofu.advance = 0.6f * pixel_height;
    tofu.offset_x = 0.0f;
    tofu.offset_y = std::max(0.0f, ascent_px - static_cast<float>(tofu.h));
    // All white first, then the interior's coverage zeroes out: the 1 px
    // border ring is the stroke, the hollow center draws nothing.
    tofu.rgba.assign(static_cast<std::size_t>(tofu.w) * tofu.h * 4u, 255);
    for (std::uint32_t ty = 0; ty < tofu.h; ++ty) {
        for (std::uint32_t tx = 0; tx < tofu.w; ++tx) {
            const bool edge =
                tx == 0u || ty == 0u || tx + 1u == tofu.w || ty + 1u == tofu.h;
            tofu.rgba[(static_cast<std::size_t>(ty) * tofu.w + tx) * 4u + 3u] =
                edge ? 255 : 0;
        }
    }
    out.glyphs.push_back(std::move(tofu));
    out.ok = true;
    return out;
}

}  // namespace font
}  // namespace ws::ui::r2d
