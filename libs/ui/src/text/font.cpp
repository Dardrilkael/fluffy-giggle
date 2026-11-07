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

const Font::Glyph* Font::find_glyph(std::uint32_t codepoint) const {
    // glyph() without the fallback: the carried-only answer the icon
    // mapping wants (a missing icon must degrade to "no sprite", never
    // to the tofu box's sprite - the icon consumers draw their pre-icon
    // square instead). Same resolution order, the fallback branch cut.
    if (codepoint < kFirstBaked) {
        return nullptr;
    }
    if (codepoint >= kFirstBaked + kBakedCount) {
        const auto at = extras_->glyphs.find(codepoint);
        return at != extras_->glyphs.end() ? &at->second : nullptr;
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

// The coverage LUT: a' = 255 * (a/255)^(1/gamma), built once per bake.
// The exponent INVERTS the knob, so a LARGER gamma is BOLDER - the mids
// lift toward full coverage (the P1b redefinition; the original spelled
// a^gamma, which compounded the thinning a gamma-space blend already
// gives light-on-dark text while its comment claimed the opposite).
// gamma 1 (and any degenerate <= 0) is the identity table - the
// pre-P1 bake's bytes, exactly, the escape hatch FontBakeOptions::
// identity() names. The table is monotonic by construction (x^(1/g) is
// monotone for any fixed g > 0), so it can never reorder coverage (a
// darker sample cannot out-ink a brighter one) and the tofu's binary
// 0/255 ends map to themselves.
struct CoverageLut {
    std::uint8_t v[256];
    explicit CoverageLut(float gamma) {
        if (gamma <= 0.0f || gamma == 1.0f) {
            for (int i = 0; i < 256; ++i) {
                v[static_cast<std::size_t>(i)] =
                    static_cast<std::uint8_t>(i);
            }
            return;
        }
        const double exponent = 1.0 / static_cast<double>(gamma);
        for (int i = 0; i < 256; ++i) {
            const double shaded =
                std::pow(static_cast<double>(i) / 255.0, exponent);
            // x in [0,1] to a positive power stays in [0,1], so the
            // rounding can never leave the byte range - the clamp is
            // belt and braces, not arithmetic the LUT needs.
            const long scaled = std::lround(shaded * 255.0);
            v[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(
                scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled));
        }
    }
};

// One glyph's rasterization, the P1 quality pass's single step (shared
// by the primary bake and every lazy face bake): render at
// `oversample_x` samples per final column, box-average the groups down,
// then run the coverage through the LUT. Metrics are EXACT whatever
// the options: the advance rides the face's h-metrics, the vertical
// geometry the single-sampled box, and offset_x the final column grid
// (an integer by the glyph-snap contract, its sub-pixel residue folded
// into the first column group's width - the same rounding the ox = 1
// bake always did, one final pixel of phase at worst).
BakedGlyph rasterize_glyph(const stbtt_fontinfo& info,
                           std::uint32_t codepoint, float scale,
                           float baseline, const FontBakeOptions& options,
                           const CoverageLut& lut) {
    BakedGlyph g;
    g.codepoint = codepoint;
    int advance = 0;
    int lsb = 0;  // unused: offset_x comes from the bitmap box below
    stbtt_GetCodepointHMetrics(&info, static_cast<int>(codepoint), &advance,
                               &lsb);
    g.advance = static_cast<float>(advance) * scale;

    // The oversampling factor, clamped once (a bogus 0 or a huge value
    // are configuration noise, not bake failures).
    const int ox = options.oversample_x < 1
                       ? 1
                       : (options.oversample_x > 4 ? 4
                                                   : options.oversample_x);

    // The box at the oversampled horizontal scale: x lands on the
    // oversampled grid, y on the final one (vertical never supersamples
    // - the snap quantizes x, and a vertical box change would move
    // offset_y, a metric).
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    stbtt_GetCodepointBitmapBoxSubpixel(&info, static_cast<int>(codepoint),
                                        scale * static_cast<float>(ox),
                                        scale, 0.0f, 0.0f, &x0, &y0, &x1,
                                        &y1);
    const int w_over = x1 - x0;  // oversampled columns
    const int h = y1 - y0;       // final rows
    if (w_over <= 0 || h <= 0) {
        return g;  // the blanks: an advance to honor, no bitmap
    }

    // The oversampled buffer, rendered at whole-sample shift 0. The
    // final column j covers oversampled coordinates [j*ox, (j+1)*ox)
    // ABSOLUTE (not relative to x0), so the grouping keys on x0's
    // phase: the first group may be short, the ink lands in the
    // column the screen pixel it occupies owns, and offset_x is the
    // final pixel containing x0 (floor division - negative x0
    // included).
    const int phase = ((x0 % ox) + ox) % ox;
    const int w = (w_over + phase + ox - 1) / ox;  // final columns
    std::vector<std::uint8_t> over(
        static_cast<std::size_t>(w_over) * static_cast<std::size_t>(h), 0);
    stbtt_MakeCodepointBitmapSubpixel(
        &info, over.data(), w_over, h,
        w_over,  // stride: rows are tight
        scale * static_cast<float>(ox), scale, 0.0f, 0.0f,
        static_cast<int>(codepoint));

    g.w = static_cast<std::uint32_t>(w);
    g.h = static_cast<std::uint32_t>(h);
    // Floor division, negatives included: the final pixel x0 sits in.
    g.offset_x = static_cast<float>((x0 - phase) / ox);
    // stbtt's box is baseline-relative; the glyph contract measures
    // from the LINE TOP, which sits `baseline` px above it (the
    // primary bake passes the ascent, a stacked face the shared
    // baseline).
    g.offset_y = baseline + static_cast<float>(y0);

    // The downsample + tint: RGB white, coverage in alpha - the vertex
    // tint at draw time supplies the text color. Column group j spans
    // oversampled [j*ox - phase, (j + 1)*ox - phase), clamped to the
    // rendered width; a partial group averages what it has (the ink
    // area carries, the exact sample count does not).
    g.rgba.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) *
                      4u,
                  255);
    for (int row = 0; row < h; ++row) {
        const std::uint8_t* src =
            over.data() + static_cast<std::size_t>(row) *
                              static_cast<std::size_t>(w_over);
        std::uint8_t* dst =
            g.rgba.data() +
            (static_cast<std::size_t>(row) * static_cast<std::size_t>(w)) *
                4u;
        for (int col = 0; col < w; ++col) {
            const int lo = col * ox - phase;
            const int hi = lo + ox;
            const int begin = lo < 0 ? 0 : lo;
            const int end = hi > w_over ? w_over : hi;
            int acc = 0;
            int count = 0;
            for (int k = begin; k < end; ++k) {
                acc += src[k];
                ++count;
            }
            const int averaged =
                count > 0 ? (acc + count / 2) / count : 0;
            // The boost: a straight multiplier AFTER the LUT - the last
            // word on weight, clamped at 255 (never a wrap). 1.0 is the
            // identity; a positive multiplier keeps the LUT's monotone
            // order, so coverage still cannot reorder.
            const int boosted = static_cast<int>(std::lround(
                static_cast<float>(lut.v[static_cast<std::size_t>(averaged)]) *
                options.boost));
            dst[static_cast<std::size_t>(col) * 4u + 3u] =
                static_cast<std::uint8_t>(boosted < 0
                                              ? 0
                                              : (boosted > 255 ? 255
                                                               : boosted));
        }
    }
    return g;
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

BakedFont bake_file(const char* path, float pixel_height,
                    const FontBakeOptions& options) {
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
    return bake_info(info, pixel_height, options);
}

BakedFont bake_info(const stbtt_fontinfo& info, float pixel_height,
                    const FontBakeOptions& options) {
    BakedFont out;
    if (pixel_height <= 0.0f) {
        return out;
    }
    const CoverageLut lut{options.gamma};

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
        out.glyphs.push_back(
            rasterize_glyph(info, cp, scale, ascent_px, options, lut));
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

// --- the built-in icon set (P6) ----------------------------------------------
// Seventeen interface icons as analytic shapes: each is a small list of
// line segments (round-capped) plus up to two filled dots, rasterized by
// Euclidean distance to the nearest stroke edge with a one-pixel
// antialiasing ramp. The geometry is spelled in a 0..1 unit square (y
// down, like the gui) and scaled to the font's pixel height at bake
// time, so every size bakes the same icon at the same weight. The
// stroke is proportional (a tenth of the box, floored at 1.25 px) -
// chunky enough to read at 14 px, fine enough not to blob at 28.
// The first seven are the chrome's own markers (chevrons, check,
// close, search, plus, minus); the toolbar ten after them (document,
// folder, save, undo, redo, trash, rotate, mirror, wire,
// clipboard_check) are the app toolbars' standard glyphs - the wire is
// the one that needs TWO dots (its junction endpoints), hence the
// second circle in IconShape.
namespace {

struct IconSegment {
    float x0, y0, x1, y1;
};
struct IconShape {
    const IconSegment* segments;
    std::uint32_t segment_count;
    float circle_x, circle_y, circle_r;   // r <= 0: no first dot
    float circle2_x, circle2_y, circle2_r;  // r <= 0: no second dot
};

// Distance from (px, py) to a segment, round caps included (the plain
// point-to-segment distance's clamp already reads as a cap).
float segment_distance(float px, float py, const IconSegment& s) {
    const float dx = s.x1 - s.x0;
    const float dy = s.y1 - s.y0;
    const float len2 = dx * dx + dy * dy;
    float t = 0.0f;
    if (len2 > 0.0f) {
        t = ((px - s.x0) * dx + (py - s.y0) * dy) / len2;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }
    const float cx = s.x0 + t * dx;
    const float cy = s.y0 + t * dy;
    const float ex = px - cx;
    const float ey = py - cy;
    return std::sqrt(ex * ex + ey * ey);
}

}  // namespace

bool bake_icon_glyph(std::uint32_t codepoint, float pixel_height,
                     BakedGlyph& out) {
    if (codepoint < kIconFirst || codepoint >= kIconFirst + kIconCount ||
        pixel_height <= 0.0f) {
        return false;
    }

    // The shapes, in enum order (chevron down, chevron right, check,
    // close, search, plus, minus, document, folder, save, undo, redo,
    // trash, rotate, mirror, wire, clipboard_check) - a static table,
    // the whole "look" of the set in one place. The toolbar ten's
    // arcs (undo, redo, rotate) are approximated by short polyline
    // runs; their arrowheads are two-segment barbs off the arc's end.
    static const IconSegment kChevronDown[] = {{0.28f, 0.36f, 0.50f, 0.62f},
                                               {0.50f, 0.62f, 0.72f, 0.36f}};
    static const IconSegment kChevronRight[] = {{0.36f, 0.28f, 0.62f, 0.50f},
                                                {0.62f, 0.50f, 0.36f, 0.72f}};
    static const IconSegment kCheck[] = {{0.20f, 0.52f, 0.42f, 0.74f},
                                         {0.42f, 0.74f, 0.80f, 0.30f}};
    static const IconSegment kClose[] = {{0.26f, 0.26f, 0.74f, 0.74f},
                                         {0.74f, 0.26f, 0.26f, 0.74f}};
    static const IconSegment kSearchTail[] = {{0.62f, 0.62f, 0.82f, 0.82f}};
    static const IconSegment kPlus[] = {{0.50f, 0.22f, 0.50f, 0.78f},
                                        {0.22f, 0.50f, 0.78f, 0.50f}};
    static const IconSegment kMinus[] = {{0.22f, 0.50f, 0.78f, 0.50f}};
    // document: a page with a folded corner (the dog-ear reads at
    // toolbar sizes where a bare rectangle would not).
    static const IconSegment kDocument[] = {
        {0.30f, 0.14f, 0.62f, 0.14f},   // the top edge to the fold
        {0.62f, 0.14f, 0.78f, 0.30f},  // the diagonal tear
        {0.78f, 0.30f, 0.78f, 0.86f},  // the right edge
        {0.78f, 0.86f, 0.30f, 0.86f},  // the bottom edge
        {0.30f, 0.86f, 0.30f, 0.14f},  // the left edge
        {0.62f, 0.14f, 0.62f, 0.30f},  // the fold's vertical
        {0.62f, 0.30f, 0.78f, 0.30f},  // the fold's horizontal
    };
    // folder: tab, then the body under it.
    static const IconSegment kFolder[] = {
        {0.16f, 0.28f, 0.40f, 0.28f},   // the tab's top
        {0.40f, 0.28f, 0.48f, 0.36f},  // the tab's shoulder
        {0.48f, 0.36f, 0.84f, 0.36f},  // the body's top
        {0.84f, 0.36f, 0.84f, 0.78f},  // the body's right
        {0.84f, 0.78f, 0.16f, 0.78f},  // the body's bottom
        {0.16f, 0.78f, 0.16f, 0.28f},  // the body's left + the tab's
    };
    // save: the floppy - body, shutter slot at the top, one label
    // bar near the bottom (the classic three-part read).
    static const IconSegment kSave[] = {
        {0.22f, 0.14f, 0.78f, 0.14f},   // body top
        {0.78f, 0.14f, 0.78f, 0.86f},  // body right
        {0.78f, 0.86f, 0.22f, 0.86f},  // body bottom
        {0.22f, 0.86f, 0.22f, 0.14f},  // body left
        {0.42f, 0.14f, 0.42f, 0.42f},  // shutter left
        {0.42f, 0.42f, 0.62f, 0.42f},  // shutter bottom
        {0.62f, 0.42f, 0.62f, 0.14f},  // shutter right
        {0.38f, 0.68f, 0.62f, 0.68f},  // the label bar
    };
    // undo: the arc over the top, the head pointing left at its end.
    static const IconSegment kUndo[] = {
        {0.74f, 0.36f, 0.66f, 0.26f}, {0.66f, 0.26f, 0.50f, 0.22f},
        {0.50f, 0.22f, 0.34f, 0.26f}, {0.34f, 0.26f, 0.26f, 0.40f},
        {0.26f, 0.40f, 0.37f, 0.34f},  // the head's upper barb
        {0.26f, 0.40f, 0.37f, 0.46f},  // the head's lower barb
    };
    // redo: undo's mirror (the arc over the top, head pointing right).
    static const IconSegment kRedo[] = {
        {0.26f, 0.36f, 0.34f, 0.26f}, {0.34f, 0.26f, 0.50f, 0.22f},
        {0.50f, 0.22f, 0.66f, 0.26f}, {0.66f, 0.26f, 0.74f, 0.40f},
        {0.74f, 0.40f, 0.63f, 0.34f},  // the head's upper barb
        {0.74f, 0.40f, 0.63f, 0.46f},  // the head's lower barb
    };
    // trash: the lid + handle, the tapered body, the base.
    static const IconSegment kTrash[] = {
        {0.24f, 0.28f, 0.76f, 0.28f},   // the lid
        {0.42f, 0.20f, 0.58f, 0.20f},   // the handle's top
        {0.42f, 0.20f, 0.42f, 0.28f},  // the handle's left
        {0.58f, 0.20f, 0.58f, 0.28f},  // the handle's right
        {0.32f, 0.28f, 0.35f, 0.82f},  // the body's left, tapering in
        {0.68f, 0.28f, 0.65f, 0.82f},  // the body's right
        {0.35f, 0.82f, 0.65f, 0.82f},  // the base
    };
    // rotate: a near-full clockwise circle whose head points down at
    // the lower left (the standard "rotate CW" read).
    static const IconSegment kRotate[] = {
        {0.74f, 0.42f, 0.70f, 0.28f}, {0.70f, 0.28f, 0.58f, 0.20f},
        {0.58f, 0.20f, 0.42f, 0.20f}, {0.42f, 0.20f, 0.30f, 0.28f},
        {0.30f, 0.28f, 0.26f, 0.42f}, {0.26f, 0.42f, 0.28f, 0.56f},
        {0.28f, 0.56f, 0.21f, 0.47f},  // the head's left barb
        {0.28f, 0.56f, 0.37f, 0.47f},  // the head's right barb
    };
    // mirror: two horizontal arrows meeting at the center (the
    // "flip horizontally" read).
    static const IconSegment kMirror[] = {
        {0.16f, 0.50f, 0.44f, 0.50f},   // the left arrow's shaft
        {0.44f, 0.50f, 0.35f, 0.41f},  // its head, upper barb
        {0.44f, 0.50f, 0.35f, 0.59f},  // its head, lower barb
        {0.84f, 0.50f, 0.56f, 0.50f},  // the right arrow's shaft
        {0.56f, 0.50f, 0.65f, 0.41f},  // its head, upper barb
        {0.56f, 0.50f, 0.65f, 0.59f},  // its head, lower barb
    };
    // wire: an ortho route between two junction dots - the dots are
    // the IconShape's two circles (filled disks under the SDF, like
    // the search lens).
    static const IconSegment kWire[] = {
        {0.22f, 0.74f, 0.22f, 0.44f}, {0.22f, 0.44f, 0.50f, 0.44f},
        {0.50f, 0.44f, 0.50f, 0.30f}, {0.50f, 0.30f, 0.78f, 0.30f},
    };
    // clipboard_check: the ERC's list - a clipboard body, the clip
    // tab riding its top edge, a check inside.
    static const IconSegment kClipboardCheck[] = {
        {0.28f, 0.18f, 0.72f, 0.18f},   // body top
        {0.72f, 0.18f, 0.72f, 0.86f},  // body right
        {0.72f, 0.86f, 0.28f, 0.86f},  // body bottom
        {0.28f, 0.86f, 0.28f, 0.18f},  // body left
        {0.42f, 0.14f, 0.42f, 0.24f},  // the clip's left
        {0.42f, 0.24f, 0.58f, 0.24f},  // the clip's bottom
        {0.58f, 0.24f, 0.58f, 0.14f},  // the clip's right
        {0.40f, 0.54f, 0.47f, 0.62f},  // the check's short leg
        {0.47f, 0.62f, 0.62f, 0.42f},  // the check's long leg
    };
    static const IconShape kShapes[] = {
        {kChevronDown, 2, 0, 0, -1.0f, 0, 0, -1.0f},
        {kChevronRight, 2, 0, 0, -1.0f, 0, 0, -1.0f},
        {kCheck, 2, 0, 0, -1.0f, 0, 0, -1.0f},
        {kClose, 2, 0, 0, -1.0f, 0, 0, -1.0f},
        {kSearchTail, 1, 0.42f, 0.42f, 0.22f, 0, 0, -1.0f},
        {kPlus, 2, 0, 0, -1.0f, 0, 0, -1.0f},
        {kMinus, 1, 0, 0, -1.0f, 0, 0, -1.0f},
        {kDocument, 7, 0, 0, -1.0f, 0, 0, -1.0f},
        {kFolder, 6, 0, 0, -1.0f, 0, 0, -1.0f},
        {kSave, 8, 0, 0, -1.0f, 0, 0, -1.0f},
        {kUndo, 6, 0, 0, -1.0f, 0, 0, -1.0f},
        {kRedo, 6, 0, 0, -1.0f, 0, 0, -1.0f},
        {kTrash, 7, 0, 0, -1.0f, 0, 0, -1.0f},
        {kRotate, 8, 0, 0, -1.0f, 0, 0, -1.0f},
        {kMirror, 6, 0, 0, -1.0f, 0, 0, -1.0f},
        {kWire, 4, 0.22f, 0.74f, 0.11f, 0.78f, 0.30f, 0.11f},
        {kClipboardCheck, 9, 0, 0, -1.0f, 0, 0, -1.0f},
    };
    const IconShape& shape = kShapes[codepoint - kIconFirst];

    // The bitmap: the box plus a 1 px apron so the AA ramp never clips.
    const float stroke = std::max(1.25f, 0.10f * pixel_height);
    const std::uint32_t side =
        static_cast<std::uint32_t>(pixel_height) + 2u;
    out = BakedGlyph{};
    out.codepoint = codepoint;
    out.w = side;
    out.h = side;
    out.advance = 0.0f;   // draw_icon places the quad; this never flows
    out.offset_x = 0.0f;  // text
    out.offset_y = 0.0f;
    out.rgba.assign(static_cast<std::size_t>(side) * side * 4u, 255);
    for (std::uint32_t py = 0; py < side; ++py) {
        for (std::uint32_t px = 0; px < side; ++px) {
            // Pixel center in the unit square, against the apron: the
            // box's (0,0) sits at (-1, -1) px in the bitmap.
            const float ux =
                (static_cast<float>(px) - 1.0f + 0.5f) / pixel_height;
            const float uy =
                (static_cast<float>(py) - 1.0f + 0.5f) / pixel_height;
            float d = 1e9f;
            for (std::uint32_t i = 0; i < shape.segment_count; ++i) {
                d = std::min(d, segment_distance(ux, uy, shape.segments[i]));
            }
            // The dots (a filled disk each, the search lens's own SDF):
            // |distance from center| minus radius, min'd into the walk.
            if (shape.circle_r > 0.0f) {
                const float ex = ux - shape.circle_x;
                const float ey = uy - shape.circle_y;
                d = std::min(
                    d, std::sqrt(ex * ex + ey * ey) - shape.circle_r);
            }
            if (shape.circle2_r > 0.0f) {
                const float ex = ux - shape.circle2_x;
                const float ey = uy - shape.circle2_y;
                d = std::min(
                    d, std::sqrt(ex * ex + ey * ey) - shape.circle2_r);
            }
            // Half the stroke on each side of the ideal line, the usual
            // 1 px ramp over the edge. The distance converts to PIXELS
            // first (it is unit-square; the stroke and the ramp are px)
            // - mixed units would scale the stroke with the bake and
            // blob every icon at small sizes. Colors stay white,
            // coverage in alpha (the baked-mask convention).
            const float d_px = d * pixel_height;
            const float coverage =
                std::clamp(stroke * 0.5f + 0.5f - d_px, 0.0f, 1.0f);
            out.rgba[(static_cast<std::size_t>(py) * side + px) * 4u + 3u] =
                static_cast<std::uint8_t>(coverage * 255.0f + 0.5f);
        }
    }
    return true;
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
                           float baseline_y, const FontBakeOptions& options) {
    // The lazy path's single-glyph step: bake_info's per-glyph math, one
    // codepoint at a time, against the font's shared baseline. Blank
    // when the face does not carry the codepoint (nothing to draw,
    // nothing to advance - the fallback box keeps answering it).
    if (!face.ok || !face.carries(codepoint)) {
        return {};
    }
    const CoverageLut lut{options.gamma};
    return rasterize_glyph(face.info, codepoint, face.scale, baseline_y,
                           options, lut);
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
