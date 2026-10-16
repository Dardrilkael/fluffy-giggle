// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the text field - caret, selection and the typed edits through the
// EditBuffer verbs (the lens and the TextInputState live in
// widgets_detail.hpp).

#include "widgets_detail.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace ws::ui::gui {

// --- M20: the text input ----------------------------------------------------

bool text_input(GuiContext& gui, LayoutCursor& layout,
                const ChromeTheme& theme, const char* label, char* buf,
                int capacity, float width) {
    const Row row = begin_row(gui, layout, theme, label, width);
    const WidgetId id = row.id;
    const Rect& rect = row.rect;
    const WidgetResult& result = row.result;
    const HitTest& hit = result.hit;
    TextInputState& edit = gui.persistent_state<TextInputState>(id);

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

    const InputFrame& in = gui.input();
    // state()'s press edge claims the focus the SAME frame, so a quick
    // tap's own press frame already reads focused here.
    const bool focused = gui.is_focused(id);
    const bool dead = result.state == WidgetState::Disabled;
    const bool can_edit = live && focused && !dead;

    // The advance arithmetic the whole field rides: one codepoint's
    // honest glyph advance (0 outside the bake range - inert bytes sit
    // still), or (fontless) the v1 bar's per-char estimate, the same
    // measure fit_text sizes by.
    const auto cp_advance = [&](unsigned char c) -> float {
        if (theme.font != nullptr) {
            const r2d::Font::Glyph* g = theme.font->glyph(c);
            return g != nullptr ? g->advance : 0.0f;
        }
        return theme.label_char_width;
    };
    const auto char_advance = [&](int i) -> float {
        return cp_advance(static_cast<unsigned char>(buf[i]));
    };
    // The pen x at byte `index`, clamped to the buffer's end.
    const auto x_at = [&](int index) -> float {
        float acc = 0.0f;
        for (int i = 0; i < index && i < eb.len; ++i) {
            acc += char_advance(i);
        }
        return acc;
    };
    // The inverse: an x inside the row names the first byte whose
    // half-advance the click crossed - the standard edit-field
    // placement, clamped to the buffer.
    const auto caret_from_x = [&](float dx) -> int {
        float acc = 0.0f;
        for (int i = 0; i < eb.len; ++i) {
            const float adv = char_advance(i);
            if (dx < acc + adv * 0.5f) {
                return i;
            }
            acc += adv;
        }
        return eb.len;
    };

    const float content_x = rect.x + theme.padding;
    const float content_room = std::max(0.0f, rect.width - 2.0f * theme.padding);

    bool changed = false;

    // The frame clock's first consumer: the phase runs every frame (the
    // clock does), and the resets below rewind it solid - every edit,
    // every caret move, every press, and the focus's own arrival.
    edit.blink += in.dt_seconds;
    if (edit.blink >= 1.0f) {
        edit.blink -= 1.0f;
        if (edit.blink >= 1.0f) {
            edit.blink = 0.0f;  // a hitched frame (a pause, a debugger) restarts
        }
    }
    if (focused && !edit.was_focused) {
        edit.blink = 0.0f;  // arrival shows the caret solid
    }

    // --- the pointer: a press places the caret, a sweep selects ---------
    const PointerView* owner = gui.pointer(hit.pointer);
    const bool hold = hit.pressed && owner != nullptr && owner->down;
    const bool shift = in.key_down[static_cast<std::size_t>(keys::kLeftShift)] ||
                       in.key_down[static_cast<std::size_t>(keys::kRightShift)];
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

    // --- the keyboard: caret keys, deleters, typing, the clipboard ------
    if (can_edit) {
        const bool ctrl =
            in.key_down[static_cast<std::size_t>(keys::kLeftControl)] ||
            in.key_down[static_cast<std::size_t>(keys::kRightControl)];

        // The caret keys: Shift extends from the anchor, plain collapses
        // it onto the caret - the rule every editor spells.
        const auto move_caret = [&](int to) {
            eb.caret = to;
            if (!shift) {
                eb.anchor = to;
            }
            edit.blink = 0.0f;
        };
        if (in.key_pressed[static_cast<std::size_t>(keys::kLeft)]) {
            move_caret(std::max(0, eb.caret - 1));
        }
        if (in.key_pressed[static_cast<std::size_t>(keys::kRight)]) {
            move_caret(std::min(eb.len, eb.caret + 1));
        }
        if (in.key_pressed[static_cast<std::size_t>(keys::kHome)]) {
            move_caret(0);
        }
        if (in.key_pressed[static_cast<std::size_t>(keys::kEnd)]) {
            move_caret(eb.len);
        }
        if (ctrl && in.key_pressed[static_cast<std::size_t>(keys::kA)]) {
            eb.anchor = 0;  // select all: the anchor says it
            eb.caret = eb.len;
            edit.blink = 0.0f;
        }
        if (in.key_pressed[static_cast<std::size_t>(keys::kEscape)]) {
            eb.collapse(eb.caret);  // Escape's one job: drop the selection
            edit.blink = 0.0f;
        }

        // Deleters before typing, when one frame carries both (no arrival
        // order exists between key edges and the codepoint list): the
        // old text leaves, then the new lands at the settled caret.
        if (in.key_pressed[static_cast<std::size_t>(keys::kBackspace)] ||
            in.key_pressed[static_cast<std::size_t>(keys::kDelete)]) {
            const bool forward =
                in.key_pressed[static_cast<std::size_t>(keys::kDelete)];
            if (eb.has_selection()) {
                eb.erase_selection();
                changed = true;
            } else if (!forward && eb.caret > 0) {
                eb.erase(eb.caret - 1, eb.caret);
                eb.collapse(eb.caret - 1);
                changed = true;
            } else if (forward && eb.caret < eb.len) {
                eb.erase(eb.caret, eb.caret + 1);
                eb.collapse(eb.caret);
                changed = true;
            }
            edit.blink = 0.0f;
        }

        // Typing: the frame's codepoints through the one filter (v1
        // edits ASCII). A selection dies under what replaces it, and a
        // full buffer drops the rest of the frame's text.
        for (std::uint32_t cp : in.text_input) {
            if (!is_insertable(cp)) {
                continue;
            }
            eb.erase_selection();
            const char c = static_cast<char>(cp);
            const int pen = eb.insert(eb.caret, &c, 1);
            if (pen < 0) {
                break;  // the room refused: the rest of the frame's text drops
            }
            eb.collapse(pen);
            edit.blink = 0.0f;
            changed = true;
        }

        // The clipboard trio - only a selection copies or cuts, and a
        // paste lands at the caret replacing the selection first, like
        // typing. An unwired seam makes all three no-ops (the context's
        // own contract; Android keeps one today).
        if (ctrl &&
            (in.key_pressed[static_cast<std::size_t>(keys::kC)] ||
             in.key_pressed[static_cast<std::size_t>(keys::kX)]) &&
            eb.has_selection()) {
            // The one allocation an explicit copy may make: a mid-buffer
            // range is not NUL-terminated, and the seam speaks C strings.
            gui.set_clipboard_text(
                std::string(eb.buf + eb.sel_lo(), eb.buf + eb.sel_hi()).c_str());
            if (in.key_pressed[static_cast<std::size_t>(keys::kX)]) {
                eb.erase_selection();
                edit.blink = 0.0f;
                changed = true;
            }
        }
        if (ctrl && in.key_pressed[static_cast<std::size_t>(keys::kV)]) {
            const char* pasted = gui.get_clipboard_text();
            if (pasted != nullptr) {
                eb.erase_selection();
                const int pen = eb.insert_filtered(eb.caret, pasted);
                if (pen >= 0) {
                    eb.collapse(pen);
                    edit.blink = 0.0f;
                    changed = true;
                }
            }
        }
    }

    // The buffer may have shrunk under the edits (or under the caller's
    // own hand between frames): the write-back clamps both ends of the
    // working pair into range on their way into the persistent state.
    edit.caret = std::clamp(eb.caret, 0, eb.len);
    edit.anchor = std::clamp(eb.anchor, 0, eb.len);
    edit.was_focused = focused;

    // --- the draw: ring, face, band, text, caret --------------------------
    focus_ring(gui, id, rect, theme);
    const float dim_by = dim_factor(result.state, theme);
    // The field reads held while a press owns it: the plain ladder.
    const std::uint32_t face = control_face(result.state, theme);
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

    // The selection band: the accent under the text, the selected tab's
    // face - only while the field holds the focus (a stale selection
    // never bands an unfocused row).
    const int sel_lo_draw = std::min(edit.caret, edit.anchor);
    const int sel_hi_draw = std::max(edit.caret, edit.anchor);
    if (focused && !dead && sel_hi_draw > sel_lo_draw) {
        const float x0 =
            content_x + std::min(x_at(sel_lo_draw), content_room);
        const float x1 =
            content_x + std::min(x_at(sel_hi_draw), content_room);
        if (x1 > x0) {
            r2d::draw_quad(x0, mark_y, x1 - x0, mark_h, theme.control_on);
        }
    }

    if (eb.len > 0) {
        // The content, left-aligned inside the padding. Content is DATA,
        // so fit_text's "##" cut does not apply - the truncation here is
        // the walk itself: the longest prefix + "..." when the whole
        // line misses the row's room (the v1 filter already guarantees
        // one byte per glyph).
        const float whole = x_at(eb.len);
        int visible = eb.len;
        bool ellipsis = false;
        if (whole > content_room) {
            const float dots = cp_advance('.') * 3.0f;
            const float budget = std::max(0.0f, content_room - dots);
            float acc = 0.0f;
            visible = 0;
            for (int i = 0; i < eb.len; ++i) {
                const float adv = char_advance(i);
                if (acc + adv > budget) {
                    break;
                }
                acc += adv;
                visible = i + 1;
            }
            ellipsis = true;
        }
        if (theme.font != nullptr) {
            if (!ellipsis) {
                r2d::draw_text(content_x, label_top(rect.y, rect.height, theme),
                               buf, *theme.font, dim(theme.text, dim_by));
            } else {
                char scratch[256];
                const int keep =
                    std::min(visible, static_cast<int>(sizeof(scratch)) - 4);
                std::memcpy(scratch, buf, static_cast<std::size_t>(keep));
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
        // The placeholder: the label dimmed, saying what the field
        // wants - only an empty buffer shows it.
        char scratch[128];
        const LabelFit fit =
            fit_text(label, content_room, theme, scratch, sizeof(scratch));
        if (fit.advance > 0.0f) {
            const std::uint32_t ph =
                dim(dim(theme.text, theme.disabled_alpha), dim_by);
            if (theme.font != nullptr) {
                r2d::draw_text(content_x, label_top(rect.y, rect.height, theme),
                               fit.text, *theme.font, ph);
            } else {
                r2d::draw_quad(content_x, mark_y, fit.advance, mark_h, ph);
            }
        }
    }

    // The caret: a 2px bar in the text color - solid on arrival and
    // through work, half on/half off at rest, clamped to the row's room
    // when the line runs past it (v1 has no horizontal scroll; the
    // caret rides the edge while the keys still edit the hidden tail).
    if (focused && !dead && live && edit.blink < 0.5f) {
        const float cx =
            content_x + std::min(x_at(edit.caret), content_room);
        r2d::draw_quad(cx, mark_y, 2.0f, mark_h, theme.text);
    }
    return changed;
}

}  // namespace ws::ui::gui
