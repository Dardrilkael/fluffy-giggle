// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the text field - the split pair public for the custom field:
// text_input_behavior (the caret and selection edits against the
// caller's buffer, plus the caret's pixel mapping) and text_input_draw
// (the stock field's look over the answer), with the composed row over
// them. The behavior's keyboard half is the SHARED cluster both text
// fields walk (widgets_detail.hpp's text_field_keys, over the engine
// in text_edit_core.hpp) - the field adds the single-line pointer
// placement and the caret's pixel mapping around it; the multi-line
// editor (widgets_text_editor.cpp) adds its vertical walk. The lens
// (EditBuffer) and the persistent pair (TextInputState, with the
// undo/redo history) live in the public text_edit_core.hpp; the
// cluster primitives both halves walk are the public
// text_cluster.hpp - the field edits TEXT: UTF-8 codepoints land
// whole, the caret steps grapheme clusters, and no key, click or
// truncation ever splits a sequence.

#include "widgets_detail.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace ws::ui::gui {

// --- font-aware filter for the composed text_input ----------------------------
// The composed text_input uses a filter that respects the theme's font:
// when a font is present, only codepoints with glyphs are allowed;
// when no font is present, only the ASCII bake range (32..126) is allowed.
// This is separate from text_input_behavior's default filter (is_insertable)
// which is Unicode-permissive for custom field flexibility.

static thread_local const r2d::Font* g_text_input_font = nullptr;

bool is_insertable_font_aware(std::uint32_t cp) {
    if (g_text_input_font != nullptr) {
        return g_text_input_font->find_glyph(cp) != nullptr;
    }
    // No font: only the ASCII bake range (32..126, excluding DEL)
    return cp >= 32u && cp <= 126u;
}

// --- the composed row --------------------------------------------------------

bool text_input(GuiContext& gui, LayoutCursor& layout,
                const ChromeTheme& theme, const char* label, char* buf,
                int capacity, float width, bool disabled) {
    const RowLayout row = begin_widget_row(gui, layout, theme, label, width);
    TextInputState& edit = gui.persistent_state<TextInputState>(row.id);
    g_text_input_font = theme.font;
    TextInputResult result = text_input_behavior(
        gui, row.id, row.rect, buf, capacity, edit, theme, disabled,
        {}, &is_insertable_font_aware);
    g_text_input_font = nullptr;
    fold_hover_t(gui, row.id, result, theme);  // P4: the face traveler
    paint(theme, &WidgetPainters::text_input, &text_input_draw, row.rect,
          result, theme, label, buf, capacity);
    return result.edited;
}

// --- the behavior half, public for the custom field --------------------------

TextInputResult text_input_behavior(GuiContext& gui, WidgetId id,
                                    const Rect& rect, char* buf, int capacity,
                                    TextInputState& edit,
                                    const ChromeTheme& theme, bool disabled,
                                    const HitShape& hit_shape,
                                    bool (*filter)(std::uint32_t),
                                    bool vertical_keys) {
    TextInputResult out{begin_widget_paint(gui, id, rect, disabled, hit_shape)};
    const HitTest& hit = out.hit;

    // The caller owns the buffer; the widget owns the editing. `live` is
    // the honest capacity read - a null buf or no room for a byte plus
    // its NUL lays the same row inert (the empty-combo idiom: an
    // uneditable field does not pretend otherwise).
    const bool live = buf != nullptr && capacity >= 2;
    const int room = live ? capacity - 1 : 0;  // bytes the buffer may hold

    // The caller's contract: NUL-terminated within `capacity`. The walk
    // bounds itself at the room anyway, so a buffer past its stated
    // capacity sees its own prefix edited, and every write the widget
    // makes still lands inside the capacity.
    int len = 0;
    if (live) {
        while (len < room && buf[len] != '\0') {
            ++len;
        }
    }

    // The frame's working lens: the edits below speak its verbs, and the
    // write-back at the frame's end re-clamps the pair into the
    // persistent state.
    EditBuffer eb{buf, room, len, edit.caret, edit.anchor};

    const ws::input::InputFrame& in = gui.input();
    // state()'s press edge claims the focus the SAME frame, so a quick
    // tap's own press frame already reads focused in the fold above.
    const bool focused = out.focused;
    const bool dead = out.state == WidgetState::Disabled;
    const bool can_edit = live && focused && !dead;

    // The IME composition's frame half (G1.2's channel): while a
    // composition stands in a field that can edit, the preedit rides
    // the line at the caret and the KEYBOARD is the IME's - its arrows
    // walk the candidate list, its backspace re-edits the preedit, and
    // any glyphs the platform still reports this frame belong to the
    // composition, not the buffer. The pointer path above stands: a
    // press elsewhere still moves the focus away, and the platform
    // ends the composition when the field loses it.
    const ws::input::Preedit& ime = in.preedit;
    const bool composing = can_edit && ime.active;

    // The advance arithmetic the whole field rides: one CLUSTER's
    // honest advance - its codepoints' glyph advances summed, the draw
    // path's own per-codepoint walk (a multi-byte character is ONE
    // cell to the caret, never one-per-byte: the renderer's loop is
    // the model), or (fontless) the v1 bar's per-char estimate once
    // per CODEPOINT (the East-Asian-width weighting a fontless
    // non-Latin measure wants is the fit_text gap's own work, not
    // the caret's).
    const auto cp_advance = [&](std::uint32_t cp) -> float {
        if (theme.font != nullptr) {
            const r2d::Font::Glyph* g = theme.font->glyph(cp);
            return g != nullptr ? g->advance : 0.0f;
        }
        return theme.label_char_width;
    };
    // The cluster at byte i: its byte span and its advance - the
    // members' advances summed (a ZWJ family's members each advance;
    // the cluster is their SUM). The buffer END is read live from
    // eb.len on every call: the edits below grow the buffer inside the
    // frame, and an end cached at the frame's start would answer
    // zero-span clusters for the new bytes - the walk would never
    // advance.
    const auto cluster_span = [&](int i) -> int {
        return grapheme_length(buf + i, buf + eb.len);
    };
    const auto cluster_advance = [&](int i) -> float {
        float acc = 0.0f;
        const int span = cluster_span(i);
        for (int taken = 0; taken < span;) {
            std::uint32_t cp = 0;
            const int used = utf8_decode(buf + i + taken, buf + eb.len, cp);
            acc += cp_advance(cp);
            taken += used > 0 ? used : 1;
        }
        return acc;
    };
    // The pen x at byte `index` (a cluster boundary): the whole
    // clusters strictly before it, summed - a mid-cluster index
    // (only an externally re-spelled buffer leaves one) parks at
    // the boundary below, deterministic, never a split draw.
    const auto x_at = [&](int index) -> float {
        float acc = 0.0f;
        int i = 0;
        while (i < index && i < eb.len) {
            const int span = cluster_span(i);
            if (i + span > index) {
                break;  // the index sits inside this cluster
            }
            acc += cluster_advance(i);
            i += span;
        }
        return acc;
    };
    // The inverse: an x inside the row names the first CLUSTER whose
    // half-advance the click crossed - the standard edit-field
    // placement, clamped to the buffer, always on a boundary: a
    // click never lands the caret inside a sequence.
    const auto caret_from_x = [&](float dx) -> int {
        float acc = 0.0f;
        int i = 0;
        while (i < eb.len) {
            const float adv = cluster_advance(i);
            if (dx < acc + adv * 0.5f) {
                return i;
            }
            acc += adv;
            i += cluster_span(i);
        }
        return eb.len;
    };

    const float content_x = rect.x + theme.padding;
    const float content_room = std::max(0.0f, rect.width - 2.0f * theme.padding);

    bool changed = false;

    // The frame clock's first consumer (the shared spelling): the
    // phase runs every frame (the clock does), and the resets below
    // rewind it solid - every edit, every caret move, every press,
    // and the focus's own arrival.
    text_edit_clock(edit, in.dt_seconds, focused);

    // --- the pointer: a press places the caret, a sweep selects ---------
    const ws::input::PointerView* owner = gui.pointer(hit.pointer);
    const bool hold = hit.pressed && owner != nullptr && owner->down;
    const bool shift = text_edit_shift(in);
    if (can_edit && hold) {
        const int placed = caret_from_x(owner->x - content_x);
        if (owner->pressed || placed != eb.caret) {
            // Shift at the press extends the standing selection; a plain
            // press collapses the anchor onto the caret, and the sweep
            // that follows re-opens a selection the moment the pointer
            // travels.
            if (owner->pressed && !shift) {
                eb.anchor = placed;
            }
            eb.caret = placed;
            edit.blink = 0.0f;
        }
    }

    // --- the keyboard: the shared cluster -------------------------------
    // The whole editing keyboard is the one spelling both fields walk
    // (widgets_detail.hpp's text_field_keys): the caret keys with
    // their Ctrl word steps, the deleters with their word eats, the
    // undo/redo pair, typing, the clipboard trio, Ctrl+A and Escape -
    // all through the history riding in `edit`, so the single-line
    // field and the multi-line editor can never disagree about an
    // edit. The IME's commit edge lands first (a composition that
    // ended this frame is OLDER than any glyphs typed after it), then
    // the cluster inside the composition's deferral gate.
    if (can_edit && text_field_ime_commit(gui, eb, edit, ime, filter)) {
        changed = true;
    }
    if (can_edit && !composing) {
        const TextFieldKeysResult keys = text_field_keys(
            gui, eb, edit,
            {false, vertical_keys, filter});
        changed = changed || keys.changed;
    }

    // The buffer may have shrunk under the edits (or under the caller's
    // own hand between frames): the write-back clamps both ends of the
    // working pair into range on their way into the persistent state,
    // and snaps them onto cluster boundaries - a caller who re-spelled
    // the content between frames never leaves the caret mid-character.
    edit.caret = eb.clamp_to_cluster(std::clamp(eb.caret, 0, eb.len));
    edit.anchor = eb.clamp_to_cluster(std::clamp(eb.anchor, 0, eb.len));
    edit.was_focused = focused;

    // The mapped answer, paint-ready: the clamped pair's places against
    // the row's room (a line running past it parks the marks at the
    // edge - v1 has no horizontal scroll, the keys still edit the
    // hidden tail), the blink phase as it stands, the frame's edit
    // word.
    out.caret_x = content_x + std::min(x_at(edit.caret), content_room);
    const int sel_lo = std::min(edit.caret, edit.anchor);
    const int sel_hi = std::max(edit.caret, edit.anchor);
    out.sel_x0 = content_x + std::min(x_at(sel_lo), content_room);
    out.sel_x1 = content_x + std::min(x_at(sel_hi), content_room);
    out.blink = edit.blink;
    out.edited = changed;

    // The composition's own mapping: the preedit run rides the line AT
    // the caret, its own cluster walk over the frame's borrowed string
    // (the same arithmetic as the buffer's, second spelling - the
    // preedit is not in the buffer until it commits). The run's whole
    // advance is the honest unclamped number (the draw clips its own
    // fits); the mapped x's follow the caret_x convention and clamp to
    // the content edge. The composition caret stands at the span's LOW
    // end's advance into the run; caret_x answers it (the honest
    // caret while composing), and blink reads solid - composing is
    // work, the same rule every edit's rewind spells.
    if (composing) {
        const char* run = ime.text.c_str();
        const char* run_end = run + ime.text.size();
        // One cluster's advance inside the run - cp_advance over the
        // members, the buffer walk's own arithmetic.
        const auto run_cluster_advance = [&](const char* p) -> float {
            float acc = 0.0f;
            int taken = 0;
            const int span = grapheme_length(p, run_end);
            while (taken < span) {
                std::uint32_t cp = 0;
                const int used = utf8_decode(p + taken, run_end, cp);
                acc += cp_advance(cp);
                taken += used > 0 ? used : 1;
            }
            return acc;
        };
        // The advance of the run's prefix up to byte `into` (the
        // composition caret's own position when into == caret_lo).
        const auto run_x_at = [&](int into) -> float {
            float acc = 0.0f;
            const char* p = run;
            while (p < run_end && p - run < into) {
                const int span = grapheme_length(p, run_end);
                if ((p - run) + span > into) {
                    break;  // the byte sits inside this cluster
                }
                acc += run_cluster_advance(p);
                p += span;
            }
            return acc;
        };
        out.preedit_text = run;  // borrowed: the context's frame snapshot
        out.preedit_len = static_cast<int>(ime.text.size());
        out.preedit_at = edit.caret;
        out.preedit_caret_lo = ime.caret_lo;
        out.preedit_caret_hi = ime.caret_hi;
        out.preedit_x = content_x + std::min(x_at(edit.caret), content_room);
        out.preedit_advance = run_x_at(out.preedit_len);
        out.preedit_caret_x =
            content_x +
            std::min(x_at(edit.caret) + run_x_at(ime.caret_lo), content_room);
        out.caret_x = out.preedit_caret_x;
        out.blink = 0.0f;
    }
    return out;
}

// --- the draw half ------------------------------------------------------------

void text_input_draw(const Rect& rect, const TextInputResult& result,
                     const ChromeTheme& theme, const char* label,
                     const char* buf, int capacity) {
    ring_draw(result.focused, rect, theme);
    const float dim_by = dim_factor(result.state, theme);
    // The field reads held while a press owns it: the plain ladder.
    const std::uint32_t face = control_face_of(result, theme);
    r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                           theme.corner_radius, face);

    // The caret and the selection band ride the line box with a font,
    // the half-height bar without - the same two shapes every widget's
    // label wears.
    float mark_y = rect.y + rect.height * 0.25f;
    float mark_h = rect.height * 0.5f;
    if (theme.font != nullptr) {
        mark_y = label_top(rect.y, rect.height, theme);
        mark_h = theme.font->line_height();
    }

    const float content_x = rect.x + theme.padding;
    const float content_room = std::max(0.0f, rect.width - 2.0f * theme.padding);

    // The selection band: the accent under the text, the selected tab's
    // face - only while the field holds the focus (a stale selection
    // never bands an unfocused row). The behavior already clamped the
    // span to the room; equal edges mean nothing stands.
    if (result.focused && result.state != WidgetState::Disabled &&
        result.sel_x1 > result.sel_x0) {
        r2d::draw_quad(result.sel_x0, mark_y, result.sel_x1 - result.sel_x0,
                       mark_h, theme.control_on);
    }

    // The content walk: the same advance arithmetic the behavior maps
    // the caret by, recomputed here - the look owns its own fits.
    const bool live = buf != nullptr && capacity >= 2;
    const int room = live ? capacity - 1 : 0;
    int len = 0;
    if (live) {
        while (len < room && buf[len] != '\0') {
            ++len;
        }
    }

    // The composed line while a composition stands (G1.2): the buffer's
    // prefix, the preedit run at the caret, the buffer's suffix -
    // spliced into one string the walk below treats exactly like plain
    // content, so the "..." cut and the cluster walks keep their rules
    // over it. The run's own marks (the underline + the solid
    // composition caret) paint after the content, from the behavior's
    // mapped x's. An overlong splice degrades in tiers - the suffix
    // first (the least news), then a cluster-aligned prefix cut - so a
    // pathological report past half a K of preedit stays sound, just
    // short. The draw re-reads `buf` for the pieces, never the frame:
    // the behavior's mapping is the only thing borrowed from it.
    char compose_scratch[512];
    const char* content = buf;
    int content_len = len;
    const bool composing_draw = result.preedit_text != nullptr && live &&
                                result.focused &&
                                result.state != WidgetState::Disabled;
    if (composing_draw) {
        const int at = std::clamp(result.preedit_at, 0, len);
        const int pre = std::max(0, result.preedit_len);
        const int cap = static_cast<int>(sizeof(compose_scratch)) - 1;
        int keep_pre = pre;
        int keep_at = at;
        int keep_suf = len - at;
        if (keep_pre + keep_at + keep_suf > cap) {
            keep_suf = 0;  // tier 1: the tail is the least news
            if (keep_pre + keep_at > cap) {
                // tier 2: the longest cluster-aligned prefix that fits
                // beside the whole run - never a split sequence.
                keep_at = 0;
                int i = 0;
                while (i < at) {
                    const int span = grapheme_length(buf + i, buf + len);
                    if (span <= 0 || keep_pre + i + span > cap) {
                        break;
                    }
                    i += span;
                }
                keep_at = i;
                if (keep_pre > cap) {
                    // tier 3, the absurd platform: the run itself cut
                    // on codepoint boundaries - sound, just short.
                    keep_pre = cap;
                }
            }
        }
        std::memcpy(compose_scratch, buf, static_cast<std::size_t>(keep_at));
        std::memcpy(compose_scratch + keep_at, result.preedit_text,
                    static_cast<std::size_t>(keep_pre));
        std::memcpy(compose_scratch + keep_at + keep_pre, buf + at,
                    static_cast<std::size_t>(keep_suf));
        content_len = keep_at + keep_pre + keep_suf;
        compose_scratch[content_len] = '\0';
        content = compose_scratch;
    }
    const auto cp_advance = [&](std::uint32_t cp) -> float {
        if (theme.font != nullptr) {
            const r2d::Font::Glyph* g = theme.font->glyph(cp);
            return g != nullptr ? g->advance : 0.0f;
        }
        return theme.label_char_width;
    };
    // The cluster at byte i: span and advance, the behavior's own
    // walk recomputed (the look owns its fits) - over the CONTENT,
    // which is the spliced line while a composition stands. The end
    // caches fine HERE: the draw never mutates, `content_len` stands
    // the whole call.
    const char* const content_end = content + content_len;
    const auto cluster_span = [&](int i) -> int {
        return grapheme_length(content + i, content_end);
    };
    const auto cluster_advance = [&](int i) -> float {
        float acc = 0.0f;
        const int span = cluster_span(i);
        for (int taken = 0; taken < span;) {
            std::uint32_t cp = 0;
            const int used = utf8_decode(content + i + taken, content_end, cp);
            acc += cp_advance(cp);
            taken += used > 0 ? used : 1;
        }
        return acc;
    };
    // The pen x at a boundary: the whole clusters before it - the
    // behavior's own walk, recomputed here (the look owns its fits).
    const auto x_at = [&](int index) -> float {
        float acc = 0.0f;
        int i = 0;
        while (i < index && i < content_len) {
            const int span = cluster_span(i);
            if (i + span > index) {
                break;
            }
            acc += cluster_advance(i);
            i += span;
        }
        return acc;
    };

    if (content_len > 0) {
        // The content, left-aligned inside the padding. Content is DATA,
        // so fit_text's "##" cut does not apply - the truncation here is
        // the walk itself: the longest cluster-aligned prefix + "..."
        // when the whole line misses the row's room.
        const float whole = x_at(content_len);
        int visible = content_len;
        bool ellipsis = false;
        if (whole > content_room) {
            const float dots = cp_advance('.') * 3.0f;
            const float budget = std::max(0.0f, content_room - dots);
            float acc = 0.0f;
            visible = 0;
            int i = 0;
            while (i < content_len) {
                // The walk steps CLUSTERS: the "..." cut never lands
                // mid-sequence - no dangling combining mark, no half
                // an emoji, at the edge.
                const float adv = cluster_advance(i);
                if (acc + adv > budget) {
                    break;
                }
                acc += adv;
                i += cluster_span(i);
            }
            visible = i;
            ellipsis = true;
        }
        if (theme.font != nullptr) {
            if (!ellipsis) {
                r2d::draw_text(content_x, label_top(rect.y, rect.height, theme),
                               content, *theme.font, dim(theme.text, dim_by));
            } else {
                char scratch[256];
                // The scratch clamp walks clusters too: a line wider
                // than the scratch never leaves a cut sequence inside
                // the "..." spelling.
                int keep = visible;
                const int keep_cap =
                    static_cast<int>(sizeof(scratch)) - 4;
                if (keep > keep_cap) {
                    keep = 0;
                    int i = 0;
                    while (i < content_len) {
                        const int span = cluster_span(i);
                        if (i + span > keep_cap) {
                            break;
                        }
                        i += span;
                    }
                    keep = i;
                }
                std::memcpy(scratch, content,
                            static_cast<std::size_t>(keep));
                scratch[keep] = '.';
                scratch[keep + 1] = '.';
                scratch[keep + 2] = '.';
                scratch[keep + 3] = '\0';
                r2d::draw_text(content_x, label_top(rect.y, rect.height, theme),
                               scratch, *theme.font, dim(theme.text, dim_by));
            }
        } else {
            const float w = ellipsis ? std::min(whole, content_room) : whole;
            r2d::draw_quad(content_x, mark_y, w, mark_h,
                           dim(theme.text, dim_by));
        }
    } else {
        // The placeholder: the muted rung, saying what the field wants -
        // only an empty buffer shows it. The typography brief's own
        // class: a hint that must not compete with real content, its
        // own token (#858585 on the dark stock) rather than an alpha
        // dim of body text, so a theme tunes it once for every field.
        char scratch[128];
        const LabelFit fit =
            fit_text(label, content_room, theme, scratch, sizeof(scratch));
        if (fit.advance > 0.0f) {
            const std::uint32_t ph = dim(theme.text_muted, dim_by);
            if (theme.font != nullptr) {
                r2d::draw_text(content_x, label_top(rect.y, rect.height, theme),
                               fit.text, *theme.font, ph);
            } else {
                r2d::draw_quad(content_x, mark_y, fit.advance, mark_h, ph);
            }
        }
    }

    // The composition's own marks (G1.2): the underline at the run's
    // foot - the selection band's machinery, the TEXT color (an
    // in-flight edit, not a selection) - and the SOLID composition
    // caret inside the run at the behavior's mapped x. Both clip to
    // the content edge: a run past the room parks its marks at the
    // edge like any line. These replace the blink caret below for the
    // duration - the composition caret never blinks (composing is
    // work, the behavior reports blink 0 for the same reason).
    if (composing_draw) {
        const float edge = content_x + content_room;
        const float x0 = std::min(result.preedit_x, edge);
        const float x1 =
            std::min(result.preedit_x + result.preedit_advance, edge);
        if (x1 > x0) {
            r2d::draw_quad(x0, mark_y + mark_h - 2.0f, x1 - x0, 2.0f,
                           dim(theme.text, dim_by));
        }
        r2d::draw_quad(std::min(result.preedit_caret_x, edge), mark_y, 2.0f,
                       mark_h, theme.text);
    }

    // The caret: a 2px bar in the text color at the mapped x - solid
    // on arrival and through work, half on/half off at rest (the phase
    // the behavior reports; the stock look's own under-half rule),
    // hidden while unfocused, locked or inert - and hidden while a
    // composition stands (its own caret drew above).
    if (!composing_draw && result.focused &&
        result.state != WidgetState::Disabled && live &&
        result.blink < 0.5f) {
        r2d::draw_quad(result.caret_x, mark_y, 2.0f, mark_h, theme.text);
    }
}

}  // namespace ws::ui::gui
