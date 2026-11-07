// The P1 font-bake quality acceptance (the ui-skin plan): the bake's
// knobs are coverage-only - every metric stays exact whatever the
// options - and the identity options reproduce the pre-P1 bytes. The
// bundled Inter bakes through the same path, and the two weights
// answer real text. Pure CPU throughout: the suite links the font
// bake + the embedded arrays, nothing else (the same purity class as
// test_ui_gui's font half, no renderer on the link line).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "text/default_font.h"
#include "text/font.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using ws::ui::r2d::FontBakeOptions;
using ws::ui::r2d::font::BakedFont;
using ws::ui::r2d::font::BakedGlyph;

// The ink a glyph carries: its coverage bytes summed (alpha channel).
std::uint64_t ink_of(const BakedGlyph& g) {
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < g.rgba.size() / 4u; ++i) {
        sum += g.rgba[i * 4u + 3u];
    }
    return sum;
}

const BakedGlyph& glyph_of(const BakedFont& baked, std::uint32_t c) {
    return baked.glyphs[static_cast<std::size_t>(c - 32)];
}

}  // namespace

TEST_CASE("the identity options reproduce the pre-P1 bake byte-for-byte") {
    // An independent old-style bake, spelled straight against stb in
    // the test (MakeCodepointBitmap, no oversampling, no LUT) - the
    // escape hatch must be the SAME bytes, not merely similar.
    const BakedFont identity = ws::ui::r2d::font::bake_inter(
        ws::ui::r2d::font::inter_regular_ttf(), 24.0f,
        FontBakeOptions::identity());
    REQUIRE(identity.ok);

    // The reference: stbtt's own single-sample raster, the exact calls
    // the pre-P1 bake made.
    const std::span<const std::uint8_t> ttf =
        ws::ui::r2d::font::inter_regular_ttf();
    const int offset = stbtt_GetFontOffsetForIndex(ttf.data(), 0);
    stbtt_fontinfo info;
    REQUIRE(stbtt_InitFont(&info, ttf.data(), offset));
    const float scale = stbtt_ScaleForPixelHeight(&info, 24.0f);

    for (const char c : {'A', 'g', 'i', 'W', '0', ' '}) {
        const BakedGlyph& g = glyph_of(identity, c);
        int advance = 0;
        int lsb = 0;
        stbtt_GetCodepointHMetrics(&info, static_cast<int>(c), &advance,
                                   &lsb);
        CHECK(g.advance == static_cast<float>(advance) * scale);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBox(&info, static_cast<int>(c), scale,
                                    scale, &x0, &y0, &x1, &y1);
        const int w = x1 - x0;
        const int h = y1 - y0;
        CHECK(g.w == static_cast<std::uint32_t>(w > 0 ? w : 0));
        CHECK(g.h == static_cast<std::uint32_t>(h > 0 ? h : 0));
        CHECK(g.offset_x == static_cast<float>(x0));
        if (w > 0 && h > 0) {
            std::vector<std::uint8_t> coverage(
                static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
            stbtt_MakeCodepointBitmap(
                &info, coverage.data(), w, h, w, scale, scale,
                static_cast<int>(c));
            for (int row = 0; row < h; ++row) {
                for (int col = 0; col < w; ++col) {
                    CHECK(g.rgba[(static_cast<std::size_t>(row) * g.w +
                                  static_cast<std::size_t>(col)) * 4u + 3u] ==
                          coverage[static_cast<std::size_t>(row) * w +
                                   static_cast<std::size_t>(col)]);
                }
            }
        }
    }
}

TEST_CASE("the bake knobs move coverage, never a metric") {
    const auto baked_at = [](const FontBakeOptions& options) {
        return ws::ui::r2d::font::bake_inter(
            ws::ui::r2d::font::inter_regular_ttf(), 18.0f, options);
    };
    const BakedFont plain = baked_at(FontBakeOptions::identity());
    const BakedFont quality = baked_at(FontBakeOptions{});  // the defaults
    const BakedFont heavy = baked_at(FontBakeOptions{.gamma = 2.4f,
                                                     .oversample_x = 2});
    REQUIRE(plain.ok);
    REQUIRE(quality.ok);
    REQUIRE(heavy.ok);

    // The metrics triple is identical across every options set: the
    // line height, every glyph's advance, and every glyph's offset_y
    // (the vertical geometry never supersamples).
    CHECK(plain.line_height == quality.line_height);
    CHECK(plain.line_height == heavy.line_height);
    CHECK(plain.pixel_height == quality.pixel_height);
    CHECK(plain.ascent_px == quality.ascent_px);
    for (std::size_t i = 0; i < plain.glyphs.size(); ++i) {
        CHECK(plain.glyphs[i].advance == quality.glyphs[i].advance);
        CHECK(plain.glyphs[i].advance == heavy.glyphs[i].advance);
        CHECK(plain.glyphs[i].offset_y == quality.glyphs[i].offset_y);
        CHECK(plain.glyphs[i].offset_y == heavy.glyphs[i].offset_y);
        // The horizontal offset stays on the final grid whatever the
        // oversampling - an integer, within a pixel of the plain bake.
        const float dx = quality.glyphs[i].offset_x - plain.glyphs[i].offset_x;
        CHECK(dx >= -1.0f);
        CHECK(dx <= 1.0f);
    }

    // The knobs really turn: the default's gamma LIFTS the mids (the
    // exponent inverts, so gamma > 1 is bolder), so a curvy glyph
    // carries MORE ink than the identity's, and a heavier gamma more
    // again.
    const std::uint64_t plain_ink = ink_of(glyph_of(plain, 'g'));
    const std::uint64_t quality_ink = ink_of(glyph_of(quality, 'g'));
    const std::uint64_t heavy_ink = ink_of(glyph_of(heavy, 'g'));
    CHECK(quality_ink > plain_ink);
    CHECK(heavy_ink > quality_ink);
}

TEST_CASE("boost multiplies after the LUT and clamps at 255") {
    // The third knob: a straight multiplier on the LUT's output. More
    // boost, more ink; a mild boost rides byte-exact through the
    // multiplier (clamping where the product overflows); an absurd
    // boost SATURATES every nonzero byte at 255 - clamped, never a
    // byte wrap - and not a metric moves.
    const auto baked_at = [](const FontBakeOptions& options) {
        return ws::ui::r2d::font::bake_inter(
            ws::ui::r2d::font::inter_regular_ttf(), 18.0f, options);
    };
    const BakedFont plain =
        baked_at(FontBakeOptions{.gamma = 1.6f, .boost = 1.0f,
                                 .oversample_x = 1});
    const BakedFont boosted =
        baked_at(FontBakeOptions{.gamma = 1.6f, .boost = 1.2f,
                                 .oversample_x = 1});
    const BakedFont mild =
        baked_at(FontBakeOptions{.gamma = 1.6f, .boost = 1.5f,
                                 .oversample_x = 1});
    const BakedFont absurd =
        baked_at(FontBakeOptions{.gamma = 1.6f, .boost = 40.0f,
                                 .oversample_x = 1});
    REQUIRE(plain.ok);
    REQUIRE(boosted.ok);
    REQUIRE(mild.ok);
    REQUIRE(absurd.ok);

    const BakedGlyph& base = glyph_of(plain, 'g');
    const BakedGlyph& tuned = glyph_of(boosted, 'g');
    const BakedGlyph& half_again = glyph_of(mild, 'g');
    const BakedGlyph& maxed = glyph_of(absurd, 'g');
    // Coverage-only: the dimensions and metrics are the same bake's.
    CHECK(tuned.w == base.w);
    CHECK(tuned.h == base.h);
    CHECK(tuned.advance == base.advance);
    CHECK(tuned.offset_x == base.offset_x);
    CHECK(tuned.offset_y == base.offset_y);
    CHECK(ink_of(tuned) > ink_of(base));

    // The mild boost, byte-exact: every coverage byte is the
    // multiplier's own answer, clamped at 255. The 18px 'g' exercises
    // both halves of the clamp - the AA fringe scales through below
    // the ceiling, the stem pins against it.
    bool saturated = false;
    bool scaled_through = false;
    for (std::size_t i = 0; i < base.rgba.size() / 4u; ++i) {
        const float low = static_cast<float>(base.rgba[i * 4u + 3u]);
        long expected = std::lround(low * 1.5f);
        if (expected > 255) {
            expected = 255;
            if (low < 255.0f) {
                saturated = true;  // the clamp did the work, not the LUT
            }
        } else if (low > 0.0f) {
            scaled_through = true;
        }
        CHECK(half_again.rgba[i * 4u + 3u] ==
              static_cast<std::uint8_t>(expected));
    }
    CHECK(saturated);
    CHECK(scaled_through);

    // The absurd boost: everything nonzero pins at 255 (the smallest
    // nonzero byte the gamma-1.6 LUT emits is 8, and 8 * 40 overflows),
    // zeros stay zero, and the ink is exactly 255 per lit pixel.
    std::size_t lit = 0;
    for (std::size_t i = 0; i < base.rgba.size() / 4u; ++i) {
        if (base.rgba[i * 4u + 3u] != 0u) {
            ++lit;
            CHECK(maxed.rgba[i * 4u + 3u] == 255u);
        } else {
            CHECK(maxed.rgba[i * 4u + 3u] == 0u);
        }
    }
    CHECK(ink_of(maxed) == 255u * lit);
    CHECK(ink_of(maxed) > ink_of(half_again));
}

TEST_CASE("supersampling preserves the ink area") {
    // The downsample is an area average: the total ink a glyph carries
    // survives within anti-aliasing noise (the boundary pixels are the
    // only ones that can move), and the bitmap stays within a column
    // of the single-sample size.
    const BakedFont plain =
        ws::ui::r2d::font::bake_inter(
            ws::ui::r2d::font::inter_regular_ttf(), 20.0f,
            FontBakeOptions::identity());
    const BakedFont oversampled =
        ws::ui::r2d::font::bake_inter(
            ws::ui::r2d::font::inter_regular_ttf(), 20.0f,
            FontBakeOptions{.gamma = 1.0f, .oversample_x = 3});
    REQUIRE(plain.ok);
    REQUIRE(oversampled.ok);

    for (const char c : {'m', 'e', '8', '?'}) {
        const BakedGlyph& one = glyph_of(plain, c);
        const BakedGlyph& three = glyph_of(oversampled, c);
        CHECK(three.h == one.h);  // vertical untouched
        CHECK(three.w + 1u >= one.w);
        CHECK(one.w + 1u >= three.w);
        const std::uint64_t one_ink = ink_of(one);
        if (one_ink > 0) {
            const std::uint64_t three_ink = ink_of(three);
            // 15% either way: boundary AA is the only moving part.
            CHECK(three_ink + three_ink / 6u > one_ink);
            CHECK(three_ink < one_ink + one_ink / 6u);
        }
    }
}

TEST_CASE("the bundled Inter bakes ASCII and answers real text") {
    const BakedFont regular = ws::ui::r2d::font::bake_inter(
        ws::ui::r2d::font::inter_regular_ttf(), 16.0f);
    REQUIRE(regular.ok);
    REQUIRE(regular.glyphs.size() == 96u);  // ASCII + the fallback box

    // The full ASCII range carries ink or an advance (the space
    // advances; everything printable inks).
    for (std::uint32_t cp = 33; cp <= 126; ++cp) {
        const BakedGlyph& g = regular.glyphs[cp - 32];
        CHECK(g.advance > 0.0f);
        if (cp != ' ') {
            CHECK(ink_of(g) > 0);
        }
    }
    // Shape invariants that hold for any sane face (the suite's own
    // rule: structure, never exact face metrics).
    CHECK(glyph_of(regular, 'W').advance > glyph_of(regular, 'i').advance);
    CHECK(glyph_of(regular, 'm').advance > glyph_of(regular, 'l').advance);
    CHECK(regular.line_height > 12.0f);
    CHECK(regular.line_height < 24.0f);

    // The strong weight is a real, HEAVIER face: the same text's ink
    // grows (the advance table of a semibold is its own, close but not
    // equal), and the fallback box rides both.
    const BakedFont semibold = ws::ui::r2d::font::bake_inter(
        ws::ui::r2d::font::inter_semibold_ttf(), 16.0f);
    REQUIRE(semibold.ok);
    CHECK(ink_of(glyph_of(semibold, 'g')) > ink_of(glyph_of(regular, 'g')));
    CHECK(ink_of(glyph_of(semibold, 'H')) > ink_of(glyph_of(regular, 'H')));
}

TEST_CASE("an invalid size bakes nothing, on any face") {
    const BakedFont zero = ws::ui::r2d::font::bake_inter(
        ws::ui::r2d::font::inter_regular_ttf(), 0.0f);
    CHECK(!zero.ok);
    const BakedFont negative = ws::ui::r2d::font::bake_file(
        "whatever.ttf", -4.0f);
    CHECK(!negative.ok);
    // The empty span is the same refusal.
    const BakedFont empty_span = ws::ui::r2d::font::bake_inter({}, 16.0f);
    CHECK(!empty_span.ok);
}

TEST_CASE("the bundled JetBrains Mono bakes a truly mono table") {
    // The typography pass's data face: the netlist, the problems rows
    // and the measurements read in columns, and a column is an advance
    // every glyph shares - the one property this face must carry that
    // Inter never will.
    const BakedFont mono = ws::ui::r2d::font::bake_inter(
        ws::ui::r2d::font::jetbrains_mono_ttf(), 15.0f);
    REQUIRE(mono.ok);
    REQUIRE(mono.glyphs.size() == 96u);  // ASCII + the fallback box

    // Every printable advances the same (a hair of bake rounding
    // aside): the wide 'W', the thin 'i', the pipe - one column pitch.
    const float pitch = glyph_of(mono, 'M').advance;
    CHECK(pitch > 0.0f);
    float widest = 0.0f;
    float narrowest = pitch;
    for (std::uint32_t cp = 33; cp <= 126; ++cp) {
        const float advance = glyph_of(mono, cp).advance;
        CHECK(advance > 0.0f);
        widest = std::max(widest, advance);
        narrowest = std::min(narrowest, advance);
    }
    CHECK(widest - narrowest < 0.01f);

    // And it is a FACE at the data size: ink on the glyphs, a sane
    // line box, real columns to read. (No cross-face size claim: a
    // 15 px mono column is narrower than a 16 px Inter lowercase run
    // by design - mono's pitch is one column, not an average.)
    CHECK(ink_of(glyph_of(mono, 'r')) > 0);
    CHECK(ink_of(glyph_of(mono, '0')) > 0);
    CHECK(mono.line_height > 12.0f);
    CHECK(mono.line_height < 24.0f);
}

TEST_CASE("the lazy face bake rides the owning font's options") {
    // A stacked face bakes through bake_face_glyph with the options the
    // CALLER hands it (the renderer reads them off FontExtra) - the
    // metrics contract is the same as the primary bake's: options move
    // coverage only.
    const std::span<const std::uint8_t> ttf =
        ws::ui::r2d::font::inter_semibold_ttf();
    const int offset = stbtt_GetFontOffsetForIndex(ttf.data(), 0);
    stbtt_fontinfo info;
    REQUIRE(stbtt_InitFont(&info, ttf.data(), offset));
    const float scale = stbtt_ScaleForPixelHeight(&info, 16.0f);
    int ascent = 0, descent = 0, line_gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
    const float baseline = static_cast<float>(ascent) * scale;

    // e-acute: not in the ASCII bake, carried by the subset - the lazy
    // stack's exact customer.
    const BakedGlyph plain = ws::ui::r2d::font::bake_face_glyph(
        [&] {
            ws::ui::r2d::font::FaceBake face;
            face.bytes.assign(ttf.begin(), ttf.end());
            REQUIRE(stbtt_InitFont(&face.info, face.bytes.data(), offset));
            face.scale = scale;
            face.ascent_px = baseline;
            face.ok = true;
            return face;
        }(),
        0x00E9u, baseline, FontBakeOptions::identity());
    const BakedGlyph tuned = ws::ui::r2d::font::bake_face_glyph(
        [&] {
            ws::ui::r2d::font::FaceBake face;
            face.bytes.assign(ttf.begin(), ttf.end());
            REQUIRE(stbtt_InitFont(&face.info, face.bytes.data(), offset));
            face.scale = scale;
            face.ascent_px = baseline;
            face.ok = true;
            return face;
        }(),
        0x00E9u, baseline, FontBakeOptions{});
    CHECK(plain.codepoint == 0x00E9u);
    CHECK(plain.w > 0u);
    CHECK(plain.advance == tuned.advance);   // metrics exact
    CHECK(plain.offset_y == tuned.offset_y);
    CHECK(ink_of(tuned) > ink_of(plain));    // the gamma really applied
}
