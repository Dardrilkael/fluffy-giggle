#pragma once

// Internal: the pure-CPU bake behind r2d::load_font (stb_truetype does the
// rasterization - PRIVATE, like every third-party) plus the ws-owned UTF-8
// decoder that draw_text and measure_text share. No Vulkan, no renderer
// state, no platform code: bake_file takes a caller-resolved path and
// returns bitmaps and metrics, so tests drive it headless. This header
// never reaches apps.
//
// It also owns the growing Font's out-of-line half (FontExtra): the face
// stack, the lazily-baked glyph table and the pending queue. That is why
// it keeps an stbtt_fontinfo - the volk-in-renderer2d.h precedent for a
// third-party type in an internal header - and includes the public
// <ws/ui/renderer2d.hpp> for Font::Glyph, the table's value type.

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

#include <ws/ui/renderer2d.hpp>  // Font::Glyph (FontExtra's table value)

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
    // The baseline's distance below the line box top. The lazy half needs
    // it: a stacked face's glyphs must sit on the primary bake's shared
    // baseline, and this is the one number that places it.
    float ascent_px = 0.0f;
    std::vector<BakedGlyph> glyphs;
};

// Reads the ttf/otf at `path`, rasterizes it at `pixel_height` px and
// appends the synthesized fallback box - kBakedCount + 1 glyphs in all.
// Pure CPU; the caller owns the upload (Renderer2D::load_font does).
BakedFont bake_file(const char* path, float pixel_height);

// --- the face stack (the growing Font's fallback chain) ---------------------
// A Font is one typeface at one size, but one font can STACK more typeface
// files under its primary bake: every codepoint past the inline bake
// resolves through the stack before the fallback box. The public Font
// holds this half through a shared_ptr to FontExtra - stbtt_fontinfo can
// never reach a public header, and sharing is the copy contract (a Font
// copy is the same font; see renderer2d.hpp).

// One loaded face, rasterization-ready for ONE pixel height (the owning
// font's own - faces never resize; a new size is a new font). The whole
// file as bytes: stbtt keeps pointers into it, so the bytes own the
// info's lifetime - the vector never reallocates after init, and moving
// the struct is safe (the info's pointers reach the bytes' heap buffer,
// never into the struct itself). A COPY is not safe - the copy's info
// would still point into the donor's buffer, which outlives it only by
// luck - so the copy operations are deleted and the mistake is a
// compile error (std::move on a const FaceBake would otherwise silently
// take that path, and /W4 says nothing about it).
struct FaceBake {
    bool ok = false;  // false covers missing file / not a font / bad size
    std::vector<std::uint8_t> bytes;
    stbtt_fontinfo info{};
    float scale = 0.0f;       // ScaleForPixelHeight at the owning font's size
    float ascent_px = 0.0f;  // the face's own baseline distance (the borrow rule below)
    FaceBake() = default;
    FaceBake(const FaceBake&) = delete;
    FaceBake& operator=(const FaceBake&) = delete;
    FaceBake(FaceBake&&) = default;
    FaceBake& operator=(FaceBake&&) = default;
    // True when the face's cmap resolves the codepoint (a nonzero glyph
    // index) - the stack's "does this face answer it" test.
    [[nodiscard]] bool carries(std::uint32_t codepoint) const;
};

// Reads and inits a face at pixel_height. Pure CPU; ok=false leaves the
// bytes empty and the info zeroed, so a refused face never half-loads.
FaceBake load_face(const char* path, float pixel_height);

// Bakes ONE codepoint from an already-loaded face, at its bound size and
// positioned against `baseline_y` (px from the line box top - the font's
// primary ascent, so a stacked face's glyphs sit on the font's shared
// baseline). Blank (w == 0, advance 0) when the face does not carry the
// codepoint - never the fallback box, which stays the Font's own answer.
BakedGlyph bake_face_glyph(const FaceBake& face, std::uint32_t codepoint,
                           float baseline_y);

// The public Font's out-of-line half (the growing font): the face stack,
// the lazily-baked table and the pending queue. Eagerly allocated in
// every Font constructor, so a copy shares it and IS the same font: it
// draws what the original bakes, and faces added to either show in both.
// Independent fonts come from load_font/bake_file, never from a copy.
struct FontExtra {
    // One stacked face: the loaded file plus its caller-assigned priority.
    struct Face {
        FaceBake bake;
        int priority = 0;
    };
    // Kept sorted by (priority, addition order): a lower priority value
    // answers first, ties stay in the order they were added.
    std::vector<Face> faces;
    // The lazily-baked glyphs: codepoints past the inline bake (>= 127).
    // unordered_map never invalidates references, so Font::glyph() can
    // hand out pointers that live until the font dies.
    std::unordered_map<std::uint32_t, Font::Glyph> glyphs;
    // Requested, not yet baked: the queue keeps request order (the bake
    // drains from the front); the set is the dedup.
    std::vector<std::uint32_t> pending;
    std::unordered_set<std::uint32_t> pending_set;
    // The baseline (px below the line box top) stacked-face glyphs bake
    // against: load_font sets it from the primary face's ascent; a font
    // built by hand (the fill ctor) borrows its first added face's.
    float baseline_y = 0.0f;
    bool baseline_known = false;
    // The first face in stack order that carries the codepoint - null
    // when none does (the fallback box keeps answering). Owner-thread
    // only: resolve and bake with no font_add_face in between, or the
    // returned pointer can dangle (faces insert can reallocate).
    [[nodiscard]] const FaceBake* resolve(std::uint32_t codepoint) const;
};

}  // namespace ws::ui::r2d::font
