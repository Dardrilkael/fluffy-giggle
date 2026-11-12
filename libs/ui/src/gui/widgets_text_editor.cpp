// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the multi-line editor - text_input's every rule over LINES. The
// behavior/draw split is the set's own: text_editor_behavior (the
// citizenship fold, the shared keyboard cluster from
// widgets_detail.hpp, the vertical walk with its goal column, the 2D
// pointer placement, the wheel scroll and the caret-follow) and
// text_editor_draw (the box, the visible lines, the per-line selection
// bands and "..." truncation, the caret, the scrollbar thumb), with
// the composed text_editor row over them. The engine it all rides -
// EditBuffer, the history, the word walk, the line walk - is the
// public text_edit_core.hpp, so an app with its own editor surface
// walks the same lines the widget does.

#include "widgets_detail.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace ws::ui::gui {

// One line box's height: the font's line height, or half a control
// row fontless (the v1 bar look's own centering).
static float editor_line_height(const ChromeTheme& theme) {
    return theme.font != nullptr ? theme.font->line_height()
                                 : theme.control_height * 0.5f;
}

// --- the composed row ---------------------------------------------------------

bool text_editor(GuiContext& gui, LayoutCursor& layout,
                 const ChromeTheme& theme, const char* label, char* buf,
                 int capacity, float width, int rows, bool disabled) {
    resolve_width(width, layout);
    const WidgetId id = gui.ids().id(label);
    const float line_h = editor_line_height(theme);
    const float height =
        std::max(theme.control_height,
                 static_cast<float>(rows) * line_h + 2.0f * theme.padding);
    const Rect rect = layout.next(width, height);
    TextEditorState& state = gui.persistent_state<TextEditorState>(id);
    TextEditorResult result =
        text_editor_behavior(gui, id, rect, buf, capacity, state, theme,
                             disabled);
    fold_hover_t(gui, id, result, theme);
    paint(theme, &WidgetPainters::text_editor, &text_editor_draw, rect,
          result, theme, label, buf, capacity);
    return result.edited;
}

// --- the behavior half ----------------------------------------------------------

// The paragraph walk's one question: is line [ls, its end) blank
// (nothing but spaces/tabs, or empty)?
static bool editor_line_blank(const char* buf, int len, int ls) {
    const int le = line_end(buf, len, ls);
    for (int i = ls; i < le; ++i) {
        if (buf[i] != ' ' && buf[i] != '\t') {
            return false;
        }
    }
    return true;
}

// Ctrl+Up's landing: the start of the paragraph the caret stands in -
// or, when the caret already sits at that start, the start of the
// paragraph ABOVE (blank lines skip). The document's top answers 0.
static int editor_paragraph_above(const char* buf, int len, int caret) {
    int ls = line_start(buf, len, caret);
    if (!editor_line_blank(buf, len, ls)) {
        // Walk to this paragraph's first line; a caret already there
        // keeps walking up instead.
        int ps = ls;
        while (ps > 0 && !editor_line_blank(buf, len,
                                            line_start(buf, len, ps - 1))) {
            ps = line_start(buf, len, ps - 1);
        }
        if (caret > ps) {
            return ps;
        }
        ls = ps;
    }
    // From here the walk skips blanks and lands on the paragraph above.
    while (ls > 0) {
        const int prev = line_start(buf, len, ls - 1);
        if (editor_line_blank(buf, len, prev)) {
            ls = prev;
            continue;
        }
        int ps = prev;
        while (ps > 0 && !editor_line_blank(buf, len,
                                            line_start(buf, len, ps - 1))) {
            ps = line_start(buf, len, ps - 1);
        }
        return ps;
    }
    return 0;
}

// Ctrl+Down's landing: the start of the first non-blank line strictly
// below the caret's line, or the document's end.
static int editor_paragraph_below(const char* buf, int len, int caret) {
    int ls = line_start(buf, len, caret);
    while (true) {
        const int le = line_end(buf, len, ls);
        if (le >= len) {
            return len;
        }
        ls = le + 1;
        if (!editor_line_blank(buf, len, ls)) {
            return ls;
        }
    }
}

TextEditorResult text_editor_behavior(GuiContext& gui, WidgetId id,
                                      const Rect& rect, char* buf,
                                      int capacity, TextEditorState& state,
                                      const ChromeTheme& theme,
                                      bool disabled, const HitShape& hit_shape,
                                      bool (*filter)(std::uint32_t)) {
    TextEditorResult out{begin_widget_paint(gui, id, rect, disabled, hit_shape)};
    const HitTest& hit = out.hit;

    // The caller's buffer contract is text_input's own: NUL-terminated
    // within `capacity`, a null or under-2 buffer lays the box inert.
    const bool live = buf != nullptr && capacity >= 2;
    const int room = live ? capacity - 1 : 0;
    int len = 0;
    if (live) {
        while (len < room && buf[len] != '\0') {
            ++len;
        }
    }
    EditBuffer eb{buf, room, len, state.field.caret, state.field.anchor};

    const ws::input::InputFrame& in = gui.input();
    const bool focused = out.focused;
    const bool dead = out.state == WidgetState::Disabled;
    const bool can_edit = live && focused && !dead;

    const ws::input::Preedit& ime = in.preedit;
    const bool composing = can_edit && ime.active;

    // The geometry: the line grid the whole behavior maps through.
    const float line_h = editor_line_height(theme);
    const float content_x = rect.x + theme.padding;
    const float content_top = rect.y + theme.padding;
    const float content_room = std::max(0.0f, rect.width - 2.0f * theme.padding);
    const float view_h = std::max(0.0f, rect.height - 2.0f * theme.padding);
    const int lines = line_count(buf, len);
    const float content_h = static_cast<float>(lines) * line_h;

    // The advance arithmetic, the single-line field's own walk
    // parameterized by a SPAN (a line) instead of the whole buffer:
    // one cluster's advance is its codepoints' glyph advances summed
    // (or the fontless per-codepoint estimate), and the caret lands on
    // the cluster boundary a click's x crossed.
    const auto cp_advance = [&](std::uint32_t cp) -> float {
        if (theme.font != nullptr) {
            const r2d::Font::Glyph* g = theme.font->glyph(cp);
            return g != nullptr ? g->advance : 0.0f;
        }
        return theme.label_char_width;
    };
    const auto cluster_advance_at = [&](const char* p, const char* stop) -> float {
        float acc = 0.0f;
        const int span = grapheme_length(p, stop);
        for (int taken = 0; taken < span;) {
            std::uint32_t cp = 0;
            const int used = utf8_decode(p + taken, stop, cp);
            acc += cp_advance(cp);
            taken += used > 0 ? used : 1;
        }
        return acc;
    };
    // The pen x at `into` bytes inside the span (a boundary): the
    // whole clusters before it, summed.
    const auto x_at = [&](const char* p, const char* stop, int into) -> float {
        float acc = 0.0f;
        const char* q = p;
        while (q < stop && q - p < into) {
            const int span = grapheme_length(q, stop);
            if ((q - p) + span > into) {
                break;  // the index sits inside this cluster
            }
            acc += cluster_advance_at(q, stop);
            q += span;
        }
        return acc;
    };
    // The inverse: an x inside the span names the first cluster whose
    // half-advance the click crossed - always a boundary, never a
    // split sequence; past the end answers the span's end.
    const auto caret_from_x = [&](const char* p, const char* stop, float dx) -> int {
        float acc = 0.0f;
        const char* q = p;
        while (q < stop) {
            const float adv = cluster_advance_at(q, stop);
            if (dx < acc + adv * 0.5f) {
                return static_cast<int>(q - p);
            }
            acc += adv;
            q += grapheme_length(q, stop);
        }
        return static_cast<int>(stop - p);
    };
    // The caret's x within its own line, content px (the goal column's
    // measure and the Up/Down walk's landing arithmetic).
    const auto caret_x_in_line = [&](int byte) -> float {
        const int ls = line_start(buf, len, byte);
        const int le = line_end(buf, len, byte);
        return x_at(buf + ls, buf + le, byte - ls);
    };
    // A point in frame px -> a buffer byte: the line the y names, the
    // cluster the x names inside it. Clamped on every edge - a click
    // past the bottom names the last line's end, a click above the
    // first line the top, a click past a line's width its end.
    const auto caret_from_point = [&](float x, float y) -> int {
        const float dy = y - content_top + state.scroll_y;
        int line_idx = static_cast<int>(std::floor(dy / line_h));
        line_idx = std::max(0, std::min(lines - 1, line_idx));
        const int ls = line_start_of(buf, len, line_idx);
        const int le = line_end(buf, len, ls);
        const int placed = caret_from_x(buf + ls, buf + le, x - content_x);
        return ls + placed;
    };

    bool changed = false;
    // The focus's arrival frame (the clock's own read, captured before
    // the write-back spends was_focused).
    const bool arrived = focused && !state.field.was_focused;

    // The frame clock's shared half (the single-line field's own).
    text_edit_clock(state.field, in.dt_seconds, focused);

    // The wheel: the box registers as a claimant every frame (the
    // scroll panel's own one-frame settle, its whole-bounds claim), and
    // the settled winner scrolls - one notch = the theme's
    // scroll_speed, clamped below against the content's height once
    // the caret-follow has spoken.
    gui.register_wheel_claim(rect, &state);
    if (gui.input().mouse_present && in.scroll_dy != 0.0f &&
        gui.wheel_won(&state)) {
        state.scroll_y -= in.scroll_dy * theme.scroll_speed;
    }

    // --- the pointer: a press places the caret by (line, x), a sweep
    // selects - the single-line field's rule, one dimension up.
    const ws::input::PointerView* owner = gui.pointer(hit.pointer);
    const bool hold = hit.pressed && owner != nullptr && owner->down;
    const bool shift = text_edit_shift(in);
    if (can_edit && hold) {
        const int placed = caret_from_point(owner->x, owner->y);
        if (owner->pressed || placed != eb.caret) {
            if (owner->pressed && !shift) {
                eb.anchor = placed;
            }
            eb.caret = placed;
            state.field.blink = 0.0f;
            state.goal_x = -1.0f;  // a placed caret re-derives the goal
        }
    }

    // --- the keyboard: the editor's vertical walk, then the shared cluster.
    // The IME's commit edge lands first, the composition deferring the
    // rest (the single-line field's own order).
    if (can_edit && text_field_ime_commit(gui, eb, state.field, ime, filter)) {
        changed = true;
    }
    const bool ctrl = text_edit_ctrl(in);
    if (can_edit && !composing) {
        // The vertical walk owns Up/Down (Ctrl: paragraphs). The goal
        // column stands from the last vertical step and dies on any
        // other move or edit - the editors' rule - so a walk through
        // short lines keeps the column the user meant.
        const bool up = gui.key_pressed_or_repeated(ws::input::keys::kUp);
        const bool down = gui.key_pressed_or_repeated(ws::input::keys::kDown);
        int vertical_to = -1;
        if (up || down) {
            if (ctrl) {
                vertical_to = up ? editor_paragraph_above(buf, len, eb.caret)
                                 : editor_paragraph_below(buf, len, eb.caret);
                state.goal_x = -1.0f;  // a paragraph jump re-derives the goal
            } else {
                if (state.goal_x < 0.0f) {
                    state.goal_x = caret_x_in_line(eb.caret);
                }
                const int ls = line_start(buf, len, eb.caret);
                if (up) {
                    if (ls > 0) {
                        const int pls = line_start(buf, len, ls - 1);
                        const int ple = line_end(buf, len, pls);
                        vertical_to =
                            pls + caret_from_x(buf + pls, buf + ple,
                                               state.goal_x);
                    } else {
                        vertical_to = 0;  // the top line's up is its home
                    }
                } else {
                    const int le = line_end(buf, len, eb.caret);
                    if (le < len) {
                        const int nls = le + 1;
                        const int nle = line_end(buf, len, nls);
                        vertical_to =
                            nls + caret_from_x(buf + nls, buf + nle,
                                               state.goal_x);
                    } else {
                        vertical_to = len;  // the tail line's down is its end
                    }
                }
            }
            if (vertical_to >= 0) {
                text_edit_move(eb, state.field, vertical_to, shift);
            }
        }
        // The shared cluster - every single-line shortcut rides here,
        // Enter included (the multi-line flag lands '\n' through the
        // filter); the vertical_home_end opt-out is the editor's own
        // (its Up/Down just walked above).
        const TextFieldKeysResult keys =
            text_field_keys(gui, eb, state.field, {true, false, filter});
        changed = changed || keys.changed;
        if (!up && !down && (keys.changed || keys.caret_moved)) {
            state.goal_x = -1.0f;  // a horizontal move or edit re-derives
        }
    }

    // The app's submit chord: Ctrl+Enter passes through untouched (the
    // editor's Enter breaks lines; an app that wants send-on-a-chord
    // reads this edge).
    out.ctrl_enter = can_edit && !composing && ctrl &&
                     in.key_pressed[static_cast<std::size_t>(
                         ws::input::keys::kEnter)];

    // The write-back (the single-line field's own clamps).
    state.field.caret =
        eb.clamp_to_cluster(std::clamp(eb.caret, 0, eb.len));
    state.field.anchor =
        eb.clamp_to_cluster(std::clamp(eb.anchor, 0, eb.len));
    state.field.was_focused = focused;

    // The caret-follow and the scroll's clamp: the view pulls the
    // caret's line on screen when the CARET moves lines (or the focus
    // arrives) - and only then, so a wheel-away from a parked caret
    // stays where the user scrolled (the follow_line memory is the
    // whole trick: a same-line frame never fights the wheel). The
    // scroll then settles into the content's range (a shrunk buffer
    // never scrolls past its own end).
    const int caret_line = line_index(buf, len, state.field.caret);
    const float caret_top = static_cast<float>(caret_line) * line_h;
    const float caret_bottom = caret_top + line_h;
    if (focused && !dead &&
        (arrived || caret_line != state.follow_line)) {
        if (caret_top < state.scroll_y) {
            state.scroll_y = caret_top;
        } else if (caret_bottom > state.scroll_y + view_h) {
            state.scroll_y = caret_bottom - view_h;
        }
        state.follow_line = caret_line;
    }
    state.scroll_y =
        std::clamp(state.scroll_y, 0.0f, std::max(0.0f, content_h - view_h));

    // The mapped answer, paint-ready.
    out.caret_byte = state.field.caret;
    out.sel_lo = std::min(state.field.caret, state.field.anchor);
    out.sel_hi = std::max(state.field.caret, state.field.anchor);
    out.line_h = line_h;
    out.caret_line = caret_line;
    out.caret_x = content_x +
                  std::min(caret_x_in_line(state.field.caret), content_room);
    out.caret_y = content_top + caret_top - state.scroll_y;
    out.blink = state.field.blink;
    out.edited = changed;
    out.scroll_y = state.scroll_y;
    out.content_h = content_h;
    out.view_h = view_h;

    // The composition's own mapping: the preedit run rides the caret's
    // LINE (the single-line field's arithmetic, one line up), its x
    // mapped inside that line, its caret solid - composing is work.
    if (composing) {
        const int ls = line_start(buf, len, state.field.caret);
        const int le = line_end(buf, len, state.field.caret);
        const float caret_in_line = x_at(buf + ls, buf + le,
                                         state.field.caret - ls);
        const char* run = ime.text.c_str();
        const char* run_end = run + ime.text.size();
        float run_before = 0.0f;  // the run's prefix up to the caret span
        const char* q = run;
        while (q < run_end && q - run < ime.caret_lo) {
            const int span = grapheme_length(q, run_end);
            if ((q - run) + span > ime.caret_lo) {
                break;
            }
            run_before += cluster_advance_at(q, run_end);
            q += span;
        }
        out.preedit_text = run;  // borrowed: the context's frame snapshot
        out.preedit_len = static_cast<int>(ime.text.size());
        out.preedit_caret_lo = ime.caret_lo;
        out.preedit_caret_hi = ime.caret_hi;
        out.preedit_x = content_x + std::min(caret_in_line, content_room);
        out.preedit_y = out.caret_y;
        out.preedit_advance = x_at(run, run_end, out.preedit_len);
        out.preedit_caret_x =
            content_x +
            std::min(caret_in_line + run_before, content_room);
        out.blink = 0.0f;
    }
    return out;
}

// --- the draw half --------------------------------------------------------------

void text_editor_draw(const Rect& rect, const TextEditorResult& result,
                      const ChromeTheme& theme, const char* label,
                      const char* buf, int capacity) {
    ring_draw(result.focused, rect, theme);
    const float dim_by = dim_factor(result.state, theme);
    const std::uint32_t face = control_face_of(result, theme);
    r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                           theme.corner_radius, face);

    const bool live = buf != nullptr && capacity >= 2;
    const int room = live ? capacity - 1 : 0;
    int len = 0;
    if (live) {
        while (len < room && buf[len] != '\0') {
            ++len;
        }
    }
    const float line_h = result.line_h;
    const float content_x = rect.x + theme.padding;
    const float content_top = rect.y + theme.padding;
    const float content_room =
        std::max(0.0f, rect.width - 2.0f * theme.padding);
    const float view_h =
        std::max(0.0f, rect.height - 2.0f * theme.padding);
    const bool composing_draw =
        result.preedit_text != nullptr && live && result.focused &&
        result.state != WidgetState::Disabled;

    // The look owns its own fits (the split convention): the same
    // advance arithmetic the behavior maps the caret by, recomputed
    // here over each line's span.
    const auto cp_advance = [&](std::uint32_t cp) -> float {
        if (theme.font != nullptr) {
            const r2d::Font::Glyph* g = theme.font->glyph(cp);
            return g != nullptr ? g->advance : 0.0f;
        }
        return theme.label_char_width;
    };
    const auto cluster_advance_at = [&](const char* p, const char* stop) -> float {
        float acc = 0.0f;
        const int span = grapheme_length(p, stop);
        for (int taken = 0; taken < span;) {
            std::uint32_t cp = 0;
            const int used = utf8_decode(p + taken, stop, cp);
            acc += cp_advance(cp);
            taken += used > 0 ? used : 1;
        }
        return acc;
    };
    const auto x_at = [&](const char* p, const char* stop, int into) -> float {
        float acc = 0.0f;
        const char* q = p;
        while (q < stop && q - p < into) {
            const int span = grapheme_length(q, stop);
            if ((q - p) + span > into) {
                break;
            }
            acc += cluster_advance_at(q, stop);
            q += span;
        }
        return acc;
    };

    // The visible line window: the lines the scrolled view overlaps,
    // one on each side for a partially cut line box.
    const int total_lines = len > 0 ? line_count(buf, len) : 1;
    int first = static_cast<int>(std::floor(result.scroll_y / line_h));
    first = std::max(0, std::min(total_lines - 1, first));
    int last = first + static_cast<int>(std::ceil(view_h / line_h)) + 1;
    last = std::max(first + 1, std::min(total_lines, last));

    // The placeholder: an empty buffer shows the label on the first
    // line, the single-line field's own muted hint.
    if (len == 0 && !composing_draw) {
        char scratch[128];
        const LabelFit fit =
            fit_text(label, content_room, theme, scratch, sizeof scratch);
        if (fit.advance > 0.0f) {
            const std::uint32_t ph = dim(theme.text_muted, dim_by);
            const float y = content_top - result.scroll_y;
            if (theme.font != nullptr) {
                r2d::draw_text(content_x, y, fit.text, *theme.font, ph);
            } else {
                r2d::draw_quad(content_x,
                               y + (line_h - line_h * 0.5f) * 0.5f,
                               fit.advance, line_h * 0.5f, ph);
            }
        }
    }

    for (int i = first; i < last; ++i) {
        const int ls = line_start_of(buf, len, i);
        const int le = line_end(buf, len, ls);
        const float y = content_top +
                        static_cast<float>(i) * line_h - result.scroll_y;

        // The selection band: this line's share of the range, the
        // accent under the text (only while the box holds the focus -
        // a stale selection never bands an unfocused box).
        if (result.focused && result.state != WidgetState::Disabled &&
            result.sel_hi > result.sel_lo) {
            const int lo = std::max(result.sel_lo, ls);
            const int hi = std::min(result.sel_hi, le);
            if (hi > lo) {
                const float x0 = content_x + x_at(buf + ls, buf + le, lo - ls);
                const float x1 = content_x + x_at(buf + ls, buf + le, hi - ls);
                r2d::draw_quad(x0, y, x1 - x0, line_h, theme.control_on);
            }
        }

        if (ls >= le) {
            continue;  // an empty line draws no run (the band above may)
        }

        // The line's content: the spliced run while a composition
        // rides the caret's line (prefix + preedit + suffix, the
        // single-line field's own splice), the line itself otherwise.
        // Either way ONE text run, truncated with "..." when the walk
        // runs past the room - the cluster-aligned cut that never
        // splits a sequence.
        char splice[512];
        const char* content = buf + ls;
        int content_len = le - ls;
        if (composing_draw && result.caret_byte >= ls &&
            result.caret_byte <= le) {
            const int at = std::clamp(result.caret_byte - ls, 0, le - ls);
            const int pre = std::max(0, result.preedit_len);
            const int cap = static_cast<int>(sizeof splice) - 1;
            int keep_pre = pre;
            int keep_at = at;
            int keep_suf = (le - ls) - at;
            if (keep_pre + keep_at + keep_suf > cap) {
                keep_suf = 0;  // the suffix is the least news
                if (keep_pre + keep_at > cap) {
                    keep_at = 0;  // then a cluster-aligned prefix cut
                    int i2 = 0;
                    while (i2 < at) {
                        const int span =
                            grapheme_length(buf + ls + i2, buf + le);
                        if (span <= 0 || keep_pre + i2 + span > cap) {
                            break;
                        }
                        i2 += span;
                    }
                    keep_at = i2;
                    keep_pre = std::min(keep_pre, cap);
                }
            }
            std::memcpy(splice, buf + ls, static_cast<std::size_t>(keep_at));
            std::memcpy(splice + keep_at, result.preedit_text,
                        static_cast<std::size_t>(keep_pre));
            std::memcpy(splice + keep_at + keep_pre, buf + ls + at,
                        static_cast<std::size_t>(keep_suf));
            content_len = keep_at + keep_pre + keep_suf;
            splice[content_len] = '\0';
            content = splice;
        }
        const char* const stop = content + content_len;
        const float whole = x_at(content, stop, content_len);
        int visible = content_len;
        bool ellipsis = false;
        if (whole > content_room) {
            const float dots = cp_advance('.') * 3.0f;
            const float budget = std::max(0.0f, content_room - dots);
            float acc = 0.0f;
            visible = 0;
            const char* q = content;
            while (q < stop) {
                const float adv = cluster_advance_at(q, stop);
                if (acc + adv > budget) {
                    break;
                }
                acc += adv;
                q += grapheme_length(q, stop);
            }
            visible = static_cast<int>(q - content);
            ellipsis = true;
        }
        if (theme.font != nullptr) {
            if (!ellipsis) {
                r2d::draw_text(content_x, y, content, *theme.font,
                               dim(theme.text, dim_by));
            } else {
                char scratch[256];
                int keep = visible;
                const int keep_cap = static_cast<int>(sizeof scratch) - 4;
                if (keep > keep_cap) {
                    keep = 0;
                    const char* q = content;
                    while (q < stop) {
                        const int span = grapheme_length(q, stop);
                        if ((q - content) + span > keep_cap) {
                            break;
                        }
                        q += span;
                    }
                    keep = static_cast<int>(q - content);
                }
                std::memcpy(scratch, content, static_cast<std::size_t>(keep));
                scratch[keep] = '.';
                scratch[keep + 1] = '.';
                scratch[keep + 2] = '.';
                scratch[keep + 3] = '\0';
                r2d::draw_text(content_x, y, scratch, *theme.font,
                               dim(theme.text, dim_by));
            }
        } else {
            const float w = ellipsis ? std::min(whole, content_room) : whole;
            r2d::draw_quad(content_x, y + (line_h - line_h * 0.5f) * 0.5f, w,
                           line_h * 0.5f, dim(theme.text, dim_by));
        }

        // The composition's marks on the caret's line: the underline
        // at the run's foot (the selection band's machinery, the TEXT
        // color - an in-flight edit, not a selection) and the SOLID
        // composition caret inside the run - the single-line field's
        // own pair, one line up.
        if (composing_draw && result.caret_byte >= ls &&
            result.caret_byte <= le) {
            const float edge = content_x + content_room;
            const float x0 = std::min(result.preedit_x, edge);
            const float x1 =
                std::min(result.preedit_x + result.preedit_advance, edge);
            if (x1 > x0) {
                r2d::draw_quad(x0, y + line_h - 2.0f, x1 - x0, 2.0f,
                               dim(theme.text, dim_by));
            }
            r2d::draw_quad(std::min(result.preedit_caret_x, edge), y, 2.0f,
                           line_h, theme.text);
        }
    }

    // The caret: a 2px bar in the text color at the mapped box -
    // solid on arrival and through work, half on/half off at rest,
    // hidden while unfocused or locked, and hidden while a composition
    // stands (its own caret drew above).
    if (!composing_draw && result.focused &&
        result.state != WidgetState::Disabled && live &&
        result.blink < 0.5f && result.caret_y >= content_top - 1.0f &&
        result.caret_y + result.line_h <= content_top + view_h + 1.0f) {
        r2d::draw_quad(result.caret_x, result.caret_y, 2.0f, result.line_h,
                       theme.text);
    }

    // The scrollbar: the scroll panel's own thumb, proportioned over
    // the box's inner height (a fitted box wears no thumb at all).
    if (result.content_h > result.view_h && result.view_h > 0.0f) {
        ScrollbarThumb thumb;
        thumb.height = std::max(theme.scrollbar_min_thumb,
                                result.view_h * result.view_h /
                                    result.content_h);
        const float track = result.view_h - thumb.height;
        const float max_scroll = std::max(
            1.0f, result.content_h - result.view_h);
        thumb.top = content_top + track * (result.scroll_y / max_scroll);
        thumb.shows = true;
        const Rect viewport{content_x, content_top, content_room, view_h};
        scrollbar_draw(viewport, thumb, theme);
    }
}

}  // namespace ws::ui::gui
