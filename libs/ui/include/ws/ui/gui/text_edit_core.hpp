#pragma once

// The gui's reusable TEXT EDITING ENGINE - the half of a text field
// that is not a widget at all: the caller-owned buffer lens
// (EditBuffer), the persistent caret pair with its undo/redo history
// (TextInputState + TextEditHistory), and the buffer walks every
// field and every app shares (word boundaries for Ctrl+Arrow and
// Ctrl+Backspace, line boundaries for the multi-line editor).
// Split out of widgets.hpp so an app with a CUSTOM field (or no
// widget at all - a buffer plus this engine is a complete editor)
// reaches the machinery without the widget surface; widgets.hpp
// includes this header and keeps every spelling it always had.
//
// The cluster primitives the walks ride are text_cluster.hpp's: the
// engine edits TEXT - UTF-8 codepoints land whole, the caret steps
// grapheme clusters, and no key, click, paste or undo ever splits a
// sequence. An ASCII buffer degenerates to one-byte-per-character
// arithmetic exactly.

#include <ws/ui/gui/text_cluster.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ws::ui::gui {

struct EditBuffer;  // the lens below - the history's verbs speak it

// --- the undo/redo history -------------------------------------------------------
// A bounded, coalescing stack of whole changes. One entry spells one
// step: the span that changed, both its spellings (what stood there,
// what stands there now), and the caret/selection pair on both sides
// of it. Undo re-erases `inserted` and re-lands `erased` at `pos`;
// redo the mirror - the exact arithmetic of every editor the user
// knows, sized for a widget: a few hundred steps, not a document.
//
// Coalescing (one undo step per gesture, the rule the editors set):
// a run of single-cluster typing merges while it stays on word
// characters (undo gives back the word, not the letter), a held
// Backspace merges into the span it ate, a held Delete into the span
// it ate forward. A word delete, a paste, a cut, an Enter - every
// deliberate edit - stands as its own step. Any new edit clears the
// redo stack (the standard: redo answers only an unbroken undo run).
//
// The caller owns the buffer, so the history guards itself: every
// entry remembers the buffer lengths on both sides of the change, and
// an undo/redo that finds a different length (the caller re-spelled
// the buffer between frames - a numeric field's commit, a settings
// sync) drops the whole history rather than splice a stale entry into
// text it never saw. A field that rewrites its own buffer on purpose
// clears the history at the rewrite instead - the numeric field does.

enum class TextEditCoalesce {
    none,            // its own undo step, always
    typing,          // merges with a preceding typing run (word chars only)
    deletion_back,   // merges with a preceding backspace run (eating left)
    deletion_forward // merges with a preceding delete run (eating right)
};

// One recorded step. `pos` is the byte the change landed at; the
// [pos, pos + inserted.size()) span is what stands there now, and
// `erased` is what the change replaced. The caret pair travels both
// ways so undo restores the selection that stood before the change -
// the detail that makes undo feel like undo.
struct TextEditChange {
    int pos = 0;
    std::string erased;    // what stood at the span before the change
    std::string inserted;  // what stands there after it
    int caret_before = 0;
    int anchor_before = 0;
    int caret_after = 0;
    int anchor_after = 0;
    int len_after = 0;   // the WHOLE buffer's length after the change
    int len_before = 0;  // ...and before it (each side's staleness guard)
    TextEditCoalesce kind = TextEditCoalesce::none;
};

class TextEditHistory {
  public:
    // The bounds: the stack never grows past either, and the OLDEST
    // steps drop when a bound is crossed (a field remembers its last
    // few hundred edits, not its birth).
    static constexpr std::size_t kMaxEntries = 128;
    static constexpr std::size_t kMaxBytes = 32u * 1024u;

    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    std::size_t undo_depth() const { return undo_.size(); }
    std::size_t redo_depth() const { return redo_.size(); }

    void clear() {
        undo_.clear();
        redo_.clear();
        bytes_ = 0;
    }

    // The one recording verb: replace [lo, hi) with `n` whole bytes at
    // `lo` (both spans already filtered - typing and the deleters),
    // move the caret to the pen, and record the step. Returns the pen
    // (>= 0) when the buffer changed, -1 when nothing happened (an
    // empty span the room refused - the verbs' no-op, unrecorded).
    int replace(EditBuffer& eb, int lo, int hi, const char* bytes, int n,
                TextEditCoalesce kind);

    // The paste spelling: the landing is whatever `utf8` admits
    // through `filter` and the room (EditBuffer::insert_filtered's own
    // count), one recorded step either way - undo gives back the
    // selection the paste replaced in one move. Returns the pen, or
    // -1 when nothing changed.
    int replace_filtered(EditBuffer& eb, int lo, int hi, const char* utf8,
                         bool (*filter)(std::uint32_t), TextEditCoalesce kind);

    // Walk back/forward one step. True when a step applied; the
    // EditBuffer's text and caret pair stand at the step's far side,
    // ready for the frame's write-back into TextInputState. A stale
    // buffer (the caller's own rewrite) answers false and clears the
    // history - see the class doc.
    bool undo(EditBuffer& eb);
    bool redo(EditBuffer& eb);

  private:
    void record(TextEditChange change);
    void trim();

    std::vector<TextEditChange> undo_;
    std::vector<TextEditChange> redo_;
    std::size_t bytes_ = 0;  // the undo stack's erased+inserted bytes
};

// The text field's persistent half: the caret and its selection anchor
// are byte offsets into the CALLER's buffer, always on grapheme-cluster
// boundaries - a user-perceived character is base + combining marks, an
// emoji sequence; an ASCII buffer sees the old one-byte-per-character
// arithmetic exactly; equal means no selection. The blink phase is the
// frame clock's first consumer - dt_seconds accumulates here, half on/half
// off at rest, and every edit, caret move or press rewinds it so the
// caret reads solid while the user works. `was_focused` lights it the
// frame the field GAINS the focus, so a Tab-arrival caret shows
// immediately instead of mid-blink. The undo/redo history rides along:
// every text field edits through it, so Ctrl+Z/Y (and the engine's
// own verbs for a custom field) work everywhere for free.
struct TextInputState {
    int caret = 0;
    int anchor = 0;      // the selection origin; caret == anchor: none
    float blink = 0.0f;  // [0, 1): under 0.5 draws the caret
    bool was_focused = false;
    TextEditHistory history;  // the field's own undo/redo, bounded
};

// The typing filter: the field edits TEXT - the cut is every scalar
// value UTF-8 can spell, not v1's ASCII. Control codes stay out (a
// single line has no use for them; DEL rides with them), and the
// surrogate halves and anything past U+10FFFF drop rather than land a
// truncated byte-cast alias - the test takes the CODEPOINT, never a
// byte: a surrogate half whose low byte would alias into 'A' still
// refuses whole. The insert lands the codepoint's whole 1..4-byte
// UTF-8 span (utf8_encode beside it), one cluster at a time.
inline bool is_insertable(std::uint32_t cp) {
    return cp >= 32u && cp != 127u && cp <= 0x10FFFFu &&
           !(cp >= 0xD800u && cp <= 0xDFFFu);
}

// The multi-line editor's filter: the single-line cut plus LINE BREAKS
// - the editor's Enter lands '\n' through the same whole-span path
// typing walks (the newline is one cluster, one entry, one undo step).
// '\r' still refuses: a pasted CRLF pair normalizes to LF, the way
// every modern editor spells it.
inline bool is_insertable_multiline(std::uint32_t cp) {
    return cp == '\n' || is_insertable(cp);
}

// The text field's per-frame lens: the persistent caret pair from
// TextInputState plus the caller's buffer geometry as one object, so
// every edit speaks a verb instead of spelling memmove. Built once per
// frame with the length measured fresh; the frame-end clamp writes the
// pair back into the persistent state. A dead field carries room 0,
// and the verbs' own clamps turn that into a no-op.
struct EditBuffer {
    char* buf = nullptr;  // the caller's buffer (the widget never owns it)
    int room = 0;   // bytes the buffer may hold: capacity minus the NUL
    int len = 0;    // bytes before the terminator, measured this frame
    int caret = 0;   // the editing position; the selection's live end
    int anchor = 0;  // the selection origin; caret == anchor: none

    // The selection as a range, low end first (equal ends: none).
    int sel_lo() const { return std::min(caret, anchor); }
    int sel_hi() const { return std::max(caret, anchor); }
    bool has_selection() const { return caret != anchor; }

    // --- the cluster navigation ------------------------------------------
    // Byte offsets snapped onto grapheme-cluster boundaries (see
    // text_cluster.hpp): the invariant every editing verb leaves
    // behind. The containing-cluster read is asymmetric on purpose,
    // the two halves that need it: cluster_start(byte) names the START
    // of the cluster containing byte-1 - Backspace's target, and the
    // Left key's step (byte 0 -> 0); cluster_end(byte) the END of the
    // cluster containing byte - Delete's target, and the Right key's
    // step (the length clamps). clamp_to_cluster snaps DOWN - the
    // boundary at or below - so an externally re-spelled buffer (the
    // caller's own hand between frames) never finds the caret mid-
    // character. An ASCII buffer degenerates to the old arithmetic
    // exactly: start = byte-1, end = byte+1, the clamp the identity.
    int cluster_start(int byte) const {
        byte = std::max(0, std::min(len, byte));
        int start = 0;
        while (start < byte) {
            const int span = grapheme_length(buf + start, buf + len);
            if (start + span >= byte) {
                return start;  // the cluster [start, start+span) holds byte-1
            }
            start += span;
        }
        return 0;
    }
    int cluster_end(int byte) const {
        byte = std::max(0, std::min(len, byte));
        int start = 0;
        while (start < byte) {
            const int span = grapheme_length(buf + start, buf + len);
            if (start + span > byte) {
                return start + span;  // byte sits inside [start, start+span)
            }
            start += span;
        }
        // byte is a boundary at/below the scan's stop: the cluster
        // STARTING there owns the answer (or the buffer's end).
        return byte < len ? byte + grapheme_length(buf + byte, buf + len)
                          : len;
    }
    int clamp_to_cluster(int byte) const {
        byte = std::max(0, std::min(len, byte));
        int start = 0;
        while (start < byte) {
            const int span = grapheme_length(buf + start, buf + len);
            if (start + span >= byte) {
                return byte == start + span ? byte : start;
            }
            start += span;
        }
        return byte;
    }

    // Both ends onto `at`: the selection dies, the caret stays.
    void collapse(int at) {
        caret = anchor = at;
    }

    // The one eraser every deleter funnels through: [lo, hi) leaves,
    // the tail slides, the terminator follows the new end.
    void erase(int lo, int hi) {
        lo = std::max(0, lo);
        hi = std::min(len, hi);
        if (lo >= hi) {
            return;
        }
        std::memmove(buf + lo, buf + hi, static_cast<std::size_t>(len - hi));
        len -= hi - lo;
        buf[len] = '\0';
    }

    // A selection dies under what replaces it - typing and pasting both
    // spell it; the caret lands where the selection was.
    void erase_selection() {
        if (has_selection()) {
            const int lo = sel_lo();
            erase(lo, sel_hi());
            collapse(lo);
        }
    }

    // n bytes at `at` (clamped into [0, len], never past the room): the
    // tail slides once, the bytes land, the terminator follows. The
    // return is the pen just past the insertion, or -1 when the room
    // refused everything. Callers pass WHOLE UTF-8 codepoint spans (the
    // typing loop encodes through utf8_encode and checks the span fits
    // first - the room never cuts a sequence mid-byte under the
    // widget's own hand).
    int insert(int at, const char* bytes, int n) {
        at = std::max(0, std::min(len, at));
        n = std::min(n, room - len);
        if (n <= 0) {
            return -1;
        }
        std::memmove(buf + at + n, buf + at,
                     static_cast<std::size_t>(len - at));
        std::memcpy(buf + at, bytes, static_cast<std::size_t>(n));
        len += n;
        buf[len] = '\0';
        return at + n;
    }

    // A paste through the filter: only CODEPOINTS the filter admits
    // land, and each lands its WHOLE 1..4-byte UTF-8 span (the
    // clipboard seam speaks UTF-8 - the same bytes draw_text walks) -
    // a span the room cannot take whole stops the count there, an
    // ill-formed span (a truncated or overlong sequence, a lone
    // continuation byte) drops rather than lands, and the buffer keeps
    // its UTF-8 shape. Still counted in one pass over the text, then
    // landed in one move, so the buffer never walks per byte and the
    // capacity never splits mid-paste. The return is insert's pen. The
    // filter rides as a parameter so a constrained field (a numeric
    // one) admits only its own charset; the default is the field's
    // plain cut, so every existing caller keeps its edit - with
    // non-ASCII text now LANDING instead of dropping per byte.
    int insert_filtered(int at, const char* text,
                        bool (*filter)(std::uint32_t) = &is_insertable) {
        const char* const stop = text + std::strlen(text);
        // The shared admission test: decode, refuse the ill-formed, ask
        // the filter - identical in the counting and the landing pass.
        const auto admits = [&](const char* p, std::uint32_t& cp,
                                 int& span) -> bool {
            span = utf8_decode(p, stop, cp);
            if (span <= 0) {
                return false;
            }
            if (span == 1 && cp >= 0x80u) {
                return false;  // not well-formed UTF-8: the paste drops it
            }
            return filter(cp);
        };
        int take = 0;  // bytes that will land
        for (const char* p = text; *p != '\0' && take < room - len;) {
            std::uint32_t cp = 0;
            int span = 0;
            if (admits(p, cp, span)) {
                if (take + span > room - len) {
                    break;  // the room would split the span: nothing more lands
                }
                take += span;
            }
            p += span;
        }
        at = std::max(0, std::min(len, at));
        if (take <= 0) {
            return -1;
        }
        std::memmove(buf + at + take, buf + at,
                     static_cast<std::size_t>(len - at));
        int pen = at;
        for (const char* p = text; *p != '\0';) {
            if (pen - at >= take) {
                break;  // the room's count is landed; the tail never walks
            }
            std::uint32_t cp = 0;
            int span = 0;
            if (admits(p, cp, span)) {
                std::memcpy(buf + pen, p, static_cast<std::size_t>(span));
                pen += span;
            }
            p += span;
        }
        len += take;
        buf[len] = '\0';
        return pen;
    }
};

// --- the word walk (Ctrl+Arrow, Ctrl+Backspace/Delete) -------------------------
// The buffer's characters fall in three classes - word (letters,
// digits, underscore, and every non-ASCII codepoint: a CJK run, an
// emoji family), space (whitespace, newlines included) and punct
// (everything else, each run its own stop). Ctrl+Left lands on the
// start of the run before the caret (spaces skipped), Ctrl+Right on
// the end of the run after it - the VS Code school, where a
// punctuation run is a stop of its own rather than transparent: in
// "foo, bar" the stops read 0, 3, 4, 5, 8. Cluster-aligned: a
// combining mark or a ZWJ family walks with its base, never splits.
enum class TextWordClass { word, space, punct };

inline TextWordClass word_class(std::uint32_t cp) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\v' ||
        cp == '\f') {
        return TextWordClass::space;
    }
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
        (cp >= '0' && cp <= '9') || cp == '_' || cp >= 0x80u) {
        return TextWordClass::word;  // non-ASCII edits as word text
    }
    return TextWordClass::punct;
}

// The cluster boundary at or below `byte` (the snap every walk enters
// through, so a mid-cluster byte - only an externally re-spelled
// buffer leaves one - never splits a sequence).
inline int cluster_floor(const char* buf, int len, int byte) {
    byte = std::max(0, std::min(len, byte));
    int at = 0;
    while (at < byte) {
        const int span = grapheme_length(buf + at, buf + len);
        if (at + span >= byte) {
            return at + span == byte ? byte : at;
        }
        at += span;
    }
    return byte;
}

// The class of the cluster starting at `at` (its first codepoint);
// the buffer's end reads as a stop.
inline TextWordClass cluster_class(const char* buf, int len, int at) {
    if (at >= len) {
        return TextWordClass::space;
    }
    std::uint32_t cp = 0;
    (void)utf8_decode(buf + at, buf + len, cp);
    return word_class(cp);
}

// The start of the cluster ENDING at boundary `at` (the cluster that
// owns byte at-1).
inline int cluster_before(const char* buf, int len, int at) {
    int start = 0;
    while (start < at) {
        const int span = grapheme_length(buf + start, buf + len);
        if (start + span >= at) {
            return start;
        }
        start += span;
    }
    return at;
}

// Ctrl+Left's landing: the start of the run before `byte` - spaces to
// the left skip, then the whole run of the class that stands there.
// The buffer's top answers 0, always.
inline int word_start(const char* buf, int len, int byte) {
    int at = cluster_floor(buf, len, byte);
    while (at > 0 && cluster_class(buf, len, cluster_before(buf, len, at)) ==
                         TextWordClass::space) {
        at = cluster_before(buf, len, at);
    }
    if (at > 0) {
        const TextWordClass cls =
            cluster_class(buf, len, cluster_before(buf, len, at));
        while (at > 0 && cluster_class(buf, len, cluster_before(buf, len, at)) ==
                             cls) {
            at = cluster_before(buf, len, at);
        }
    }
    return at;
}

// Ctrl+Right's landing: the end of the run after `byte` - spaces skip
// forward, then the run of the class that lands. The buffer's end
// answers len, always.
inline int word_end(const char* buf, int len, int byte) {
    int at = cluster_floor(buf, len, byte);
    while (at < len && cluster_class(buf, len, at) == TextWordClass::space) {
        at += grapheme_length(buf + at, buf + len);
    }
    if (at < len) {
        const TextWordClass cls = cluster_class(buf, len, at);
        while (at < len && cluster_class(buf, len, at) == cls) {
            at += grapheme_length(buf + at, buf + len);
        }
    }
    return at;
}

// --- the line walk (the multi-line editor's vertical half) ----------------------
// A line is the span between '\n' bytes - hard breaks only; the editor
// does not soft-wrap (a long line truncates with "..." like the
// single-line field always has, and the keys still edit the hidden
// tail). Pure byte arithmetic over the caller's buffer, so the
// editor's navigation, its draw and any app walk the same lines.

// The line containing `byte` starts here (just past the previous '\n').
inline int line_start(const char* buf, int len, int byte) {
    byte = std::max(0, std::min(len, byte));
    while (byte > 0 && buf[byte - 1] != '\n') {
        --byte;
    }
    return byte;
}

// The line containing `byte` ends here (at its own '\n', which is the
// terminator and NOT part of the line; len at the buffer's tail).
inline int line_end(const char* buf, int len, int byte) {
    byte = std::max(0, std::min(len, byte));
    while (byte < len && buf[byte] != '\n') {
        ++byte;
    }
    return byte;
}

// How many lines the buffer holds (1 + the '\n' count).
inline int line_count(const char* buf, int len) {
    int n = 1;
    for (int i = 0; i < len; ++i) {
        if (buf[i] == '\n') {
            ++n;
        }
    }
    return n;
}

// The 0-based index of the line containing `byte`.
inline int line_index(const char* buf, int len, int byte) {
    byte = std::max(0, std::min(len, byte));
    int n = 0;
    for (int i = 0; i < byte; ++i) {
        if (buf[i] == '\n') {
            ++n;
        }
    }
    return n;
}

// The start of line `index` (0-based; an index past the end clamps to
// the last line's start). O(n), like every walk here - editing
// buffers are small.
inline int line_start_of(const char* buf, int len, int index) {
    int at = 0;
    int n = 0;
    while (at < len && n < index) {
        if (buf[at] == '\n') {
            ++n;
        }
        ++at;
    }
    return line_start(buf, len, at);
}

}  // namespace ws::ui::gui
