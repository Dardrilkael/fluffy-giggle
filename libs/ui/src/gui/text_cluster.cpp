// The cluster primitives' implementation (text_cluster.hpp's contract).
// Three layers, cheapest first: the UTF-8 codec (pure arithmetic, no
// table), the curated Extend table (a sorted static range array,
// binary-searched, never allocated - the no-mid-frame-alloc rule),
// and the boundary rules over both (CRLF, the ZWJ glue,
// regional-indicator pairs, the Hangul jamo grammar). Nothing here
// allocates, reads past `end`, or depends on anything but <cstdint>.

#include <ws/ui/gui/text_cluster.hpp>

#include <algorithm>
#include <cstdint>

namespace ws::ui::gui {
namespace {

// The curated Extend table: sorted, non-overlapping codepoint ranges
// whose members CONTINUE a cluster after the base (Unicode's
// Grapheme_Extend class, approximated for the scripts text fields
// actually carry - and for the Mc spacing vowel-signs of the Indic
// heads, which the 15.1 SpacingMark rule now joins the same way).
// Growth is a data edit in this array alone; see the header's scope
// note for what an uncovered script costs.
struct Range {
    std::uint32_t lo;
    std::uint32_t hi;
};
constexpr Range kExtend[] = {
    // The Latin/Greek/Cyrillic diacritics and their supplements.
    {0x0300, 0x036F},  // combining diacritical marks
    {0x0483, 0x0489},  // Cyrillic combining
    // Hebrew points (the exact mark subranges: 05BE/05C0/05C3 are
    // punctuation and keep their own clusters).
    {0x0591, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2},
    {0x05C4, 0x05C5}, {0x05C7, 0x05C7},
    // Arabic marks.
    {0x0610, 0x061A}, {0x064B, 0x065F}, {0x0670, 0x0670},
    {0x06D6, 0x06DC}, {0x06DF, 0x06E4}, {0x06E7, 0x06E8},
    {0x06EA, 0x06ED},
    // Syriac, Thaana, Nko, the Arabic extensions, Mandaic.
    {0x0711, 0x0711}, {0x0730, 0x074A}, {0x07A6, 0x07B0},
    {0x07EB, 0x07F3}, {0x0816, 0x0819}, {0x081B, 0x0823},
    {0x0825, 0x0827}, {0x0829, 0x082D}, {0x0859, 0x085B},
    {0x08E3, 0x0903},
    // The Indic heads' vowel signs and viramas: Devanagari,
    // Bengali, Gurmukhi, Gujarati, Oriya, Tamil, Telugu, Kannada,
    // Malayalam (093D/0D3D avagraha and friends stay OUT - they are
    // letters, and letters never extend).
    {0x093A, 0x093C}, {0x093E, 0x094D}, {0x0951, 0x0957},
    {0x0962, 0x0963}, {0x0981, 0x0983}, {0x09BC, 0x09CD},
    {0x09D7, 0x09D7}, {0x09E2, 0x09E3}, {0x0A01, 0x0A03},
    {0x0A3C, 0x0A3C}, {0x0A41, 0x0A51}, {0x0A70, 0x0A71},
    {0x0A75, 0x0A75}, {0x0A81, 0x0A83}, {0x0ABC, 0x0ACD},
    {0x0AE2, 0x0AE3}, {0x0B01, 0x0B03}, {0x0B3C, 0x0B4D},
    {0x0B56, 0x0B57}, {0x0B62, 0x0B63}, {0x0B82, 0x0B82},
    {0x0BBE, 0x0BCC}, {0x0BD7, 0x0BD7}, {0x0C00, 0x0C03},
    {0x0C3E, 0x0C4D}, {0x0C55, 0x0C56}, {0x0C62, 0x0C63},
    {0x0C81, 0x0C83}, {0x0CBC, 0x0CCC}, {0x0CD5, 0x0CD6},
    {0x0CE2, 0x0CE3}, {0x0D00, 0x0D03}, {0x0D3B, 0x0D3C},
    {0x0D3E, 0x0D4D}, {0x0D57, 0x0D57}, {0x0D62, 0x0D63},
    // Thai, Lao, Myanmar, Khmer.
    {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E},
    {0x0EB1, 0x0EB1}, {0x0EB4, 0x0EBC}, {0x0EC8, 0x0ECD},
    {0x102D, 0x1030}, {0x1032, 0x1037}, {0x1039, 0x103A},
    {0x17B4, 0x17D3},
    // The Mongolian variation selectors, then the combining
    // supplements, the symbol marks, kana voicing, the half marks and
    // the emoji text presentation selectors - numeric order, the sort
    // guard under the table holds it.
    {0x180B, 0x180D},
    {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200C, 0x200C},
    {0x20D0, 0x20F0}, {0x3099, 0x309A}, {0xFE00, 0xFE0F},
    {0xFE20, 0xFE2F},
    // The emoji modifiers (skin tones).
    {0x1F3FB, 0x1F3FF},
};

// The table's own guard: sorted and non-overlapping, or the binary
// search below is a lie. A data edit that breaks the invariant fails
// the build here, not a user's cluster at runtime.
constexpr bool ranges_sorted() {
    for (std::size_t i = 1;
         i < sizeof(kExtend) / sizeof(kExtend[0]); ++i) {
        if (kExtend[i - 1].hi >= kExtend[i].lo) {
            return false;
        }
    }
    return true;
}
static_assert(ranges_sorted(),
              "kExtend must stay sorted and non-overlapping");

// Does `cp` continue the cluster before it: the table's ranges
// (binary search over the sorted spans) - ZWNJ rides the table like
// every other mark (never heads a cluster, never glues what follows).
bool extends_cluster(std::uint32_t cp) {
    const Range* const begin = kExtend;
    const Range* const end = kExtend + sizeof(kExtend) / sizeof(kExtend[0]);
    const auto it = std::lower_bound(begin, end, cp,
                                     [](const Range& r, std::uint32_t v) {
                                         return r.hi < v;
                                     });
    return it != end && cp >= it->lo;
}

bool is_zwj(std::uint32_t cp) {
    return cp == 0x200Du;
}

bool is_regional_indicator(std::uint32_t cp) {
    return cp >= 0x1F1E6u && cp <= 0x1F1FFu;
}

// The Hangul classes (GB6/7/8's grammar): the leading L jamo, the
// medial V, the trailing T, and the precomposed syllables - one
// 28-syllable block per AC00 block start, LV exactly on the boundary.
bool is_hangul_l(std::uint32_t cp) {
    return (cp >= 0x1100u && cp <= 0x115Fu) ||
           (cp >= 0xA960u && cp <= 0xA97Cu);
}
bool is_hangul_v(std::uint32_t cp) {
    return (cp >= 0x1160u && cp <= 0x11A7u) ||
           (cp >= 0xD7B0u && cp <= 0xD7C6u);
}
bool is_hangul_t(std::uint32_t cp) {
    return (cp >= 0x11A8u && cp <= 0x11FFu) ||
           (cp >= 0xD7CBu && cp <= 0xD7FBu);
}
bool is_hangul_lv(std::uint32_t cp) {
    return cp >= 0xAC00u && cp <= 0xD7A3u && ((cp - 0xAC00u) % 28u) == 0u;
}
bool is_hangul_lvt(std::uint32_t cp) {
    return cp >= 0xAC00u && cp <= 0xD7A3u && ((cp - 0xAC00u) % 28u) != 0u;
}

// GB6/7/8: whether `next` continues the cluster whose last codepoint
// was `last` under the jamo grammar.
bool hangul_continues(std::uint32_t last, std::uint32_t next) {
    if (is_hangul_l(last)) {
        return is_hangul_l(next) || is_hangul_v(next) ||
               is_hangul_lv(next) || is_hangul_lvt(next);
    }
    if (is_hangul_v(last) || is_hangul_lv(last)) {
        return is_hangul_v(next) || is_hangul_t(next);
    }
    if (is_hangul_t(last) || is_hangul_lvt(last)) {
        return is_hangul_t(next);
    }
    return false;
}

}  // namespace

int utf8_decode(const char* p, const char* end, std::uint32_t& cp) {
    if (p >= end) {
        cp = 0;
        return 0;
    }
    const std::uint32_t b0 = static_cast<std::uint32_t>(
        static_cast<unsigned char>(*p));
    if (b0 < 0x80u) {
        cp = b0;
        return 1;  // ASCII: the one-byte span
    }
    // The lead byte names its span: C2..DF two bytes, E0..EF three,
    // F0..F4 four. Everything else - a continuation read as a lead, the
    // C0/C1 and F5+ never-valid leads - degrades the same honest way.
    const int span =
        (b0 >= 0xC2u && b0 <= 0xDFu) ? 2 :
        (b0 >= 0xE0u && b0 <= 0xEFu) ? 3 :
        (b0 >= 0xF0u && b0 <= 0xF4u) ? 4 : 0;
    if (span == 0 || p + span > end) {
        cp = b0;  // invalid lead, or the span is truncated at `end`
        return 1;
    }
    std::uint32_t value = b0 & (span == 2 ? 0x1Fu : (span == 3 ? 0x0Fu : 0x07u));
    for (int i = 1; i < span; ++i) {
        const std::uint32_t b = static_cast<std::uint32_t>(
            static_cast<unsigned char>(p[i]));
        if ((b & 0xC0u) != 0x80u) {
            cp = b0;  // a broken continuation: stop at the lead, no further read
            return 1;
        }
        value = (value << 6) | (b & 0x3Fu);
    }
    // The overlong floors (2-byte under U+0080, 3-byte under U+0800,
    // 4-byte under U+10000) and the surrogate halves are ill-formed
    // by the standard: the byte alone again, the walk stays total.
    const std::uint32_t floor =
        span == 2 ? 0x80u : (span == 3 ? 0x800u : 0x10000u);
    if (value < floor || (value >= 0xD800u && value <= 0xDFFFu) ||
        value > 0x10FFFFu) {
        cp = b0;
        return 1;
    }
    cp = value;
    return span;
}

int utf8_encode(std::uint32_t cp, char* out) {
    if (cp < 0x80u) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = static_cast<char>(0xC0u | (cp >> 6));
        out[1] = static_cast<char>(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        if (cp >= 0xD800u && cp <= 0xDFFFu) {
            return 0;  // a surrogate half: no UTF-8 spells it
        }
        out[0] = static_cast<char>(0xE0u | (cp >> 12));
        out[1] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = static_cast<char>(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        out[0] = static_cast<char>(0xF0u | (cp >> 18));
        out[1] = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = static_cast<char>(0x80u | (cp & 0x3Fu));
        return 4;
    }
    return 0;  // past the top: not a scalar value
}

int grapheme_length(const char* p, const char* end) {
    if (p >= end) {
        return 0;
    }
    std::uint32_t cp = 0;
    int total = utf8_decode(p, end, cp);
    if (total <= 0) {
        return 0;
    }
    // CRLF is one cluster (the platform's two-byte newline, kept
    // un-split even where a field admits it through).
    if (cp == '\r' && p + total < end && *(p + total) == '\n') {
        return total + 1;
    }
    std::uint32_t last = cp;
    int regional = is_regional_indicator(cp) ? 1 : 0;
    bool glued = false;  // a ZWJ just joined: whatever follows rides with it
    const char* q = p + total;
    while (q < end) {
        std::uint32_t next = 0;
        const int span = utf8_decode(q, end, next);
        if (span <= 0) {
            break;
        }
        bool keep = false;
        if (glued) {
            keep = true;  // GB11's glue: the ZWJ's follower joins the cluster
            glued = false;
        } else if (is_zwj(next)) {
            keep = true;
            glued = true;  // ...and the cluster stays open past it
        } else if (extends_cluster(next)) {
            keep = true;  // GB9: the marks, selectors and modifiers extend
        } else if (regional == 1 && is_regional_indicator(next)) {
            keep = true;
            regional = 2;  // GB12/13: the pair closes; a third RI is its own
        } else if (hangul_continues(last, next)) {
            keep = true;  // GB6/7/8: the jamo grammar
        }
        if (!keep) {
            break;
        }
        total += span;
        last = next;
        q += span;
    }
    return total;
}

}  // namespace ws::ui::gui
