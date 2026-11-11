#pragma once

// The gui's UTF-8 + grapheme-cluster primitives: the editing side of
// the library's text handling (G1.1 of the gap guide). Rendering has
// walked multi-byte UTF-8 by codepoint since the face stack (the
// renderer's own utf8_next loop); the EDITING half still spoke bytes
// AS characters - the v1 ASCII cut - so a non-ASCII codepoint could
// not even be inserted (the typing path narrowed every codepoint to
// one byte before the buffer), and the caret walked each byte as a
// standalone glyph, splitting any multi-byte character mid-sequence.
// These free functions are the shared fix: every text-editing widget
// (text_input today; the multi-line area the guide plans rides the
// same primitives) decodes through them, and the caret/anchor pair
// stays in BYTES (the EditBuffer's own contract) but always lands on
// CLUSTER boundaries - a user-perceived character: a base plus its
// combining marks, an emoji ZWJ sequence, a flag's regional-indicator
// pair, a Hangul syllable's jamo run. ASCII buffers degenerate to the
// exact v1 arithmetic (one byte, one cluster), so every existing
// caller keeps its behavior byte-for-byte.
//
// The boundary data is a CURATED static range set - the combining
// blocks text fields actually see (Latin/Greek/Cyrillic diacritics,
// Hebrew and Arabic marks, the Indic vowel signs, Thai/Lao/Myanmar/
// Khmer, the symbol combining marks, the variation selectors, the
// emoji modifiers) plus the rule-based joins (ZWJ glue,
// regional-indicator pairs, the Hangul jamo grammar, CRLF). It is not
// the full UCD GraphemeBreakProperty: growing it is a data edit in
// text_cluster.cpp alone, never an API change, and an uncovered
// script simply edits one-cluster-per-codepoint - the v1 behavior -
// instead of merging its marks.

#include <cstdint>

namespace ws::ui::gui {

// One codepoint at p: `cp` receives the decoded scalar value, the
// return is the bytes consumed (1..4; 0 only at p == end). A byte
// that does not head a well-formed sequence - an invalid lead, a
// span truncated at end, an overlong encoding, a surrogate half -
// decodes AS ONE BYTE with cp = the byte itself: the orphan byte
// becomes its own cluster and every walk stays total, never stalling
// on a malformed buffer (the same never-crash discipline the
// renderer's fallback box gives the draw side; the paste path
// refuses such spans outright, so the widget's own writes never
// produce one - only a caller's hand can).
int utf8_decode(const char* p, const char* end, std::uint32_t& cp);

// One codepoint into out (>= 4 bytes of room): 1..4 bytes written, or
// 0 when cp is not a scalar value (a surrogate half, anything past
// U+10FFFF) - UTF-8 cannot spell those, and the typing path drops
// them instead of aliasing into a truncated sequence.
int utf8_encode(std::uint32_t cp, char* out);

// The byte length of the grapheme cluster starting at p: at least 1,
// never reading past end (0 only at p == end). ASCII fast-path: the
// cluster IS the byte.
int grapheme_length(const char* p, const char* end);

}  // namespace ws::ui::gui
