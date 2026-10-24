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
#include <string>
#include <utility>

namespace ws::ui::r2d {

// --- public Font ------------------------------------------------------------

// Every constructor allocates the out-of-line half, never lazily: a
// copy made before a lazy allocation would own a different half than its
// original, and the shared-copy contract (renderer2d.hpp) is worth one
// small allocation per font.
Font::Font() : extras_(std::make_shared<font::FontExtra>()) {}

Font::Font(float pixel_height, float line_height, const Glyph* glyphs,
           std::uint32_t count)
    : pixel_height_(pixel_height), line_height_(line_height),
      extras_(std::make_shared<font::FontExtra>()) {
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
        // Past the inline bake: the lazily-baked half first. References
        // into an unordered_map are stable, so the pointer outlives later
        // bakes (only the font's death ends it).
        const auto at = extras_->glyphs.find(codepoint);
        if (at != extras_->glyphs.end()) {
            return &at->second;
        }
        // Nothing baked (or nothing ever carried): the fallback box. A
        // zeroed slot (a font built with fewer glyphs, a failed load)
        // keeps the old skip - no draw, no advance - by data.
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
    out.ascent_px = ascent_px;  // the lazy half's shared-baseline number

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

// --- the face stack (the growing Font's fallback chain) --------------------

bool FaceBake::carries(std::uint32_t codepoint) const {
    if (!ok) {
        return false;
    }
    return stbtt_FindGlyphIndex(&info, static_cast<int>(codepoint)) != 0;
}

FaceBake load_face(const char* path, float pixel_height) {
    FaceBake face;
    if (path == nullptr || pixel_height <= 0.0f) {
        return face;
    }
    face.bytes = read_file(path);
    if (face.bytes.empty()) {
        return face;
    }
    // Face 0 of a collection (.ttc) is the pick, same as bake_file.
    const int offset = stbtt_GetFontOffsetForIndex(face.bytes.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&face.info, face.bytes.data(), offset)) {
        // A half-initialized info must never leak out as a usable face.
        face.info = stbtt_fontinfo{};
        face.bytes.clear();
        face.bytes.shrink_to_fit();
        return face;
    }
    face.scale = stbtt_ScaleForPixelHeight(&face.info, pixel_height);
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(&face.info, &ascent, &descent, &line_gap);
    face.ascent_px = static_cast<float>(ascent) * face.scale;
    face.ok = true;
    return face;
}

BakedGlyph bake_face_glyph(const FaceBake& face, std::uint32_t codepoint,
                           float baseline_y) {
    // The lazy path's single-glyph step: bake_file's per-glyph math, one
    // codepoint at a time, against the font's shared baseline.
    BakedGlyph g;
    g.codepoint = codepoint;
    if (!face.ok || !face.carries(codepoint)) {
        return g;  // blank: nothing to draw, nothing to advance
    }
    int advance = 0;
    int lsb = 0;  // unused: offset_x comes from the bitmap box below
    stbtt_GetCodepointHMetrics(&face.info, static_cast<int>(codepoint),
                               &advance, &lsb);
    g.advance = static_cast<float>(advance) * face.scale;
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    stbtt_GetCodepointBitmapBox(&face.info, static_cast<int>(codepoint),
                                face.scale, face.scale, &x0, &y0, &x1, &y1);
    g.offset_x = static_cast<float>(x0);
    // The one typographic fold of the lazy path: the stacked face's box is
    // baseline-relative, and the font's line top sits baseline_y above the
    // SHARED baseline - so the glyph lands where the primary bake's inline
    // glyphs sit, whatever this face's own ascent.
    g.offset_y = baseline_y + static_cast<float>(y0);
    const int w = x1 - x0;
    const int h = y1 - y0;
    if (w > 0 && h > 0) {
        g.w = static_cast<std::uint32_t>(w);
        g.h = static_cast<std::uint32_t>(h);
        std::vector<std::uint8_t> coverage(static_cast<std::size_t>(w) *
                                           static_cast<std::size_t>(h));
        stbtt_MakeCodepointBitmap(&face.info, coverage.data(), w, h,
                                  w,  // stride: rows are tight
                                  face.scale, face.scale,
                                  static_cast<int>(codepoint));
        // The tint convention, same as bake_file: RGB white, coverage in
        // alpha - the vertex tint at draw time supplies the text color.
        constexpr std::uint8_t white = 255;
        g.rgba.assign(coverage.size() * 4u, white);
        for (std::size_t i = 0; i < coverage.size(); ++i) {
            g.rgba[i * 4u + 3u] = coverage[i];
        }
    }
    return g;
}

const FaceBake* FontExtra::resolve(std::uint32_t codepoint) const {
    for (const Face& face : faces) {
        if (face.bake.carries(codepoint)) {
            return &face.bake;
        }
    }
    return nullptr;
}

}  // namespace font

// --- the face stack free functions: the pure-CPU halves --------------------
// (font_bake_pending - the upload half - lives in renderer2d.cpp, where
// the active renderer is.)

ws::core::Status font_add_face(Font& font, const char* path, int priority) {
    if (font.empty() || font.pixel_height() <= 0.0f) {
        return fail(FontError::font_empty,
                    "font_add_face: faces stack on a baked font - this one "
                    "was never baked");
    }
    if (path == nullptr) {
        return fail(FontError::face_load_failed, "font_add_face: no path");
    }
    // Deliberately non-const: this is the one FaceBake that MOVES (into the
    // stack), and std::move on a const object silently copies - the copy's
    // info.data would still reach THIS local's buffer, which dies with the
    // call. The struct deletes its copy operations to make that a compile
    // error instead.
    font::FaceBake loaded = font::load_face(path, font.pixel_height());
    if (!loaded.ok) {
        return fail(FontError::face_load_failed,
                    std::string{"font_add_face: '"} + path +
                        "' is missing, unreadable or not a font");
    }
    font::FontExtra* extra = font.extras_state();
    if (!extra->baseline_known) {
        // A font built by hand (the fill ctor) has no ascent of its own:
        // the first face to arrive lends its baseline, and every face
        // added after bakes against the same one.
        extra->baseline_y = loaded.ascent_px;
        extra->baseline_known = true;
    }
    // Sorted stable insert: a lower priority value answers first; find_if
    // lands before the first strictly-later face, so equal priorities
    // keep addition order.
    const auto at = std::find_if(
        extra->faces.begin(), extra->faces.end(),
        [priority](const font::FontExtra::Face& face) {
            return face.priority > priority;
        });
    extra->faces.insert(
        at, font::FontExtra::Face{std::move(loaded), priority});
    return {};
}

std::uint32_t font_request_text(Font& font, const char* utf8_text) {
    if (utf8_text == nullptr) {
        return 0u;
    }
    font::FontExtra* extra = font.extras_state();
    std::uint32_t marked = 0;
    const char* s = utf8_text;
    for (std::uint32_t cp = font::utf8_next(s); cp != 0u;
         cp = font::utf8_next(s)) {
        // The controls and the inline bake (32..126) are not this half's
        // business; 127 on can be.
        if (cp < font::kFirstCodepoint + font::kBakedCount) {
            continue;
        }
        if (extra->glyphs.find(cp) != extra->glyphs.end()) {
            continue;  // baked already
        }
        if (extra->pending_set.count(cp) != 0u) {
            continue;  // queued already
        }
        if (extra->resolve(cp) == nullptr) {
            continue;  // nobody carries it - the fallback box keeps answering
        }
        extra->pending.push_back(cp);
        extra->pending_set.insert(cp);
        ++marked;
    }
    return marked;
}

}  // namespace ws::ui::r2d
