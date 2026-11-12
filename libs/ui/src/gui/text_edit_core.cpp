// The undo/redo history's out-of-line half (text_edit_core.hpp's own
// doc block spells the design): replace/replace_filtered perform AND
// record in one verb, coalescing merges a gesture's small steps into
// one undo step, and undo/redo splice the recorded span back guarded
// by the buffer lengths the entries remember - a caller who re-spelled
// the buffer between frames voids the history instead of corrupting
// it. Pure std + the engine's own types: no context, no drawing, no
// platform, so the same code links into every app and test.

#include <ws/ui/gui/text_edit_core.hpp>

#include <utility>

namespace ws::ui::gui {

namespace {

// An entry's byte weight for the budget: both spellings, both stacks.
std::size_t change_bytes(const TextEditChange& c) {
    return c.erased.size() + c.inserted.size();
}

}  // namespace

int TextEditHistory::replace(EditBuffer& eb, int lo, int hi,
                             const char* bytes, int n,
                             TextEditCoalesce kind) {
    lo = std::max(0, std::min(eb.len, lo));
    hi = std::max(0, std::min(eb.len, hi));
    if (lo > hi) {
        std::swap(lo, hi);
    }
    const bool erased_any = lo < hi;
    if (!erased_any && (bytes == nullptr || n <= 0)) {
        return -1;  // nothing asked, nothing happened, nothing records
    }

    TextEditChange c;
    c.pos = lo;
    c.len_before = eb.len;
    c.caret_before = eb.caret;
    c.anchor_before = eb.anchor;
    if (erased_any) {
        c.erased.assign(eb.buf + lo, eb.buf + hi);
    }

    eb.erase(lo, hi);
    int pen = lo;
    if (bytes != nullptr && n > 0) {
        const int landed = eb.insert(lo, bytes, n);
        if (landed >= 0) {
            pen = landed;
        }
    }
    if (!erased_any && pen == lo) {
        return -1;  // the room refused the insert and nothing stood there
    }
    c.inserted.assign(eb.buf + lo, eb.buf + pen);
    c.len_after = eb.len;
    c.caret_after = pen;
    c.anchor_after = pen;
    c.kind = kind;

    eb.caret = pen;
    eb.anchor = pen;
    record(std::move(c));
    return pen;
}

int TextEditHistory::replace_filtered(EditBuffer& eb, int lo, int hi,
                                      const char* utf8,
                                      bool (*filter)(std::uint32_t),
                                      TextEditCoalesce kind) {
    lo = std::max(0, std::min(eb.len, lo));
    hi = std::max(0, std::min(eb.len, hi));
    if (lo > hi) {
        std::swap(lo, hi);
    }
    if (utf8 == nullptr || *utf8 == '\0') {
        // An empty paste is still an erase when a selection stood.
        return replace(eb, lo, hi, nullptr, 0, kind);
    }

    TextEditChange c;
    c.pos = lo;
    c.len_before = eb.len;
    c.caret_before = eb.caret;
    c.anchor_before = eb.anchor;
    if (lo < hi) {
        c.erased.assign(eb.buf + lo, eb.buf + hi);
    }

    eb.erase(lo, hi);
    int pen = eb.insert_filtered(lo, utf8, filter);
    if (pen < 0) {
        pen = lo;
    }
    if (lo >= hi && pen == lo) {
        return -1;  // the filter refused everything and nothing stood there
    }
    c.inserted.assign(eb.buf + lo, eb.buf + pen);
    c.len_after = eb.len;
    c.caret_after = pen;
    c.anchor_after = pen;
    c.kind = kind;

    eb.caret = pen;
    eb.anchor = pen;
    record(std::move(c));
    return pen;
}

void TextEditHistory::record(TextEditChange change) {
    redo_.clear();  // a new edit answers the undo run's question

    // Coalescing: the small steps of ONE gesture merge into the step
    // below them - a typing run on word characters, a held backspace
    // eating left, a held delete eating right. Contiguity is the whole
    // test: the new change must land exactly where the run stands.
    if (change.kind != TextEditCoalesce::none && !undo_.empty()) {
        TextEditChange& last = undo_.back();
        const bool contiguous =
            (change.kind == TextEditCoalesce::typing &&
             change.pos ==
                 last.pos + static_cast<int>(last.inserted.size()) &&
             change.erased.empty()) ||
            (change.kind == TextEditCoalesce::deletion_back &&
             change.pos + static_cast<int>(change.erased.size()) ==
                 last.pos) ||
            (change.kind == TextEditCoalesce::deletion_forward &&
             change.pos == last.pos && change.inserted.empty());
        if (contiguous && last.kind == change.kind) {
            if (change.kind == TextEditCoalesce::typing) {
                last.inserted += change.inserted;
                bytes_ += change.inserted.size();
            } else if (change.kind == TextEditCoalesce::deletion_back) {
                last.pos = change.pos;
                last.erased.insert(0, change.erased);
                bytes_ += change.erased.size();
            } else {
                last.erased += change.erased;
                bytes_ += change.erased.size();
            }
            last.caret_after = change.caret_after;
            last.anchor_after = change.anchor_after;
            last.len_after = change.len_after;
            return;
        }
    }

    undo_.push_back(std::move(change));
    bytes_ += change_bytes(undo_.back());
    trim();
}

bool TextEditHistory::undo(EditBuffer& eb) {
    while (!undo_.empty()) {
        const TextEditChange e = undo_.back();
        // The staleness guard: the entry left the buffer at len_after;
        // anything else means the caller re-spelled it - the history
        // never splices into text it did not see.
        if (eb.len != e.len_after ||
            e.pos + static_cast<int>(e.inserted.size()) > eb.len) {
            clear();
            return false;
        }
        eb.erase(e.pos, e.pos + static_cast<int>(e.inserted.size()));
        if (!e.erased.empty()) {
            (void)eb.insert(e.pos, e.erased.data(),
                            static_cast<int>(e.erased.size()));
        }
        eb.caret = eb.clamp_to_cluster(
            std::max(0, std::min(eb.len, e.caret_before)));
        eb.anchor = eb.clamp_to_cluster(
            std::max(0, std::min(eb.len, e.anchor_before)));

        bytes_ -= change_bytes(e);
        undo_.pop_back();
        redo_.push_back(e);
        return true;
    }
    return false;
}

bool TextEditHistory::redo(EditBuffer& eb) {
    while (!redo_.empty()) {
        const TextEditChange e = redo_.back();
        // The mirror guard: redo applies onto the state the undo left
        // (len_before, with the erased spelling standing at pos).
        if (eb.len != e.len_before ||
            e.pos + static_cast<int>(e.erased.size()) > eb.len) {
            clear();
            return false;
        }
        eb.erase(e.pos, e.pos + static_cast<int>(e.erased.size()));
        if (!e.inserted.empty()) {
            (void)eb.insert(e.pos, e.inserted.data(),
                            static_cast<int>(e.inserted.size()));
        }
        eb.caret = eb.clamp_to_cluster(
            std::max(0, std::min(eb.len, e.caret_after)));
        eb.anchor = eb.clamp_to_cluster(
            std::max(0, std::min(eb.len, e.anchor_after)));

        undo_.push_back(e);
        bytes_ += change_bytes(e);
        redo_.pop_back();
        return true;
    }
    return false;
}

void TextEditHistory::trim() {
    while (undo_.size() > kMaxEntries || bytes_ > kMaxBytes) {
        if (undo_.empty()) {
            bytes_ = 0;
            return;
        }
        bytes_ -= change_bytes(undo_.front());
        undo_.erase(undo_.begin());
        if (bytes_ > kMaxBytes * 4u) {
            bytes_ = 0;  // defensive: a corrupted budget self-heals
        }
    }
}

}  // namespace ws::ui::gui
