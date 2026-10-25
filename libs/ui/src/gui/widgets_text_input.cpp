// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the text field - the split pair public for the custom field:
// text_input_behavior (the caret and selection edits against the
// caller's buffer, plus the caret's pixel mapping) and text_input_draw
// (the stock field's look over the answer), with the composed row over
// them. The lens (EditBuffer) and the persistent pair (TextInputState)
// live in the public widgets.hpp.

#include "widgets_detail.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace ws::ui::gui {

// --- the composed row --------------------------------------------------------

bool text_input(GuiContext& gui, LayoutCursor& layout,
                const ChromeTheme& theme, const char* label, char* buf,
                int capacity, float width, bool disabled) {
    const RowLayout row = begin_widget_row(gui, layout, theme, label, width);
    TextInputState& edit = gui.persistent_state<TextInputState>(row.id);
    const TextInputResult result = text_input_behavior(
        gui, row.id, row.rect, buf, capacity, edit, theme, disabled);
    text_input_draw(row.rect, result, theme, label, buf, capacity);
    return result.edited;
}

// --- the behavior half, public for the custom field --------------------------

TextInputResult text_input_behavior(GuiContext& gui, WidgetId id,
                                    const Rect& rect, char* buf, int capacity,
                                    TextInputState& edit,
                                    const ChromeTheme& theme, bool disabled,
                                    const HitShape& hit_shape) {
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
    const ws::input::PointerView* owner = gui.pointer(hit.pointer);
    const bool hold = hit.pressed && owner != nullptr && owner->down;
    const bool shift = in.key_down[static_cast<std::size_t>(ws::input::keys::kLeftShift)] ||
                       in.key_down[static_cast<std::size_t>(ws::input::keys::kRightShift)];
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
            in.key_down[static_cast<std::size_t>(ws::input::keys::kLeftControl)] ||
            in.key_down[static_cast<std::size_t>(ws::input::keys::kRightControl)];

        // The caret keys: Shift extends from the anchor, plain collapses
        // it onto the caret - the rule every editor spells.
        const auto move_caret = [&](int to) {
            eb.caret = to;
            if (!shift) {
                eb.anchor = to;
            }
            edit.blink = 0.0f;
        };
        if (in.key_pressed[static_cast<std::size_t>(ws::input::keys::kLeft)]) {
            move_caret(std::max(0, eb.caret - 1));
        }
        if (in.key_pressed[static_cast<std::size_t>(ws::input::keys::kRight)]) {
            move_caret(std::min(eb.len, eb.caret + 1));
        }
        if (in.key_pressed[static_cast<std::size_t>(ws::input::keys::kHome)]) {
            move_caret(0);
        }
        if (in.key_pressed[static_cast<std::size_t>(ws::input::keys::kEnd)]) {
            move_caret(eb.len);
        }
        if (ctrl && in.key_pressed[static_cast<std::size_t>(ws::input::keys::kA)]) {
            eb.anchor = 0;  // select all: the anchor says it
            eb.caret = eb.len;
            edit.blink = 0.0f;
        }
        if (in.key_pressed[static_cast<std::size_t>(ws::input::keys::kEscape)]) {
            // Escape's one job: drop the selection. The drop SPENDS the
            // key - consume_escape, only when a selection actually
            // stood - so the popup or dialog behind the field keeps
            // its claim that frame: one Escape abandons the edit, the
            // next one closes the overlay. A bare-caret Escape (or a
            // field inside no overlay) leaves the key to the
            // arbitration untouched.
            if (eb.has_selection()) {
                eb.collapse(eb.caret);
                edit.blink = 0.0f;
                gui.consume_escape();
            }
        }

        // Deleters before typing, when one frame carries both (no arrival
        // order exists between key edges and the codepoint list): the
        // old text leaves, then the new lands at the settled caret.
        if (in.key_pressed[static_cast<std::size_t>(ws::input::keys::kBackspace)] ||
            in.key_pressed[static_cast<std::size_t>(ws::input::keys::kDelete)]) {
            const bool forward =
                in.key_pressed[static_cast<std::size_t>(ws::input::keys::kDelete)];
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
            (in.key_pressed[static_cast<std::size_t>(ws::input::keys::kC)] ||
             in.key_pressed[static_cast<std::size_t>(ws::input::keys::kX)]) &&
            eb.has_selection()) {
            // The one allocation an explicit copy may make: a mid-buffer
            // range is not NUL-terminated, and the seam speaks C strings.
            gui.set_clipboard_text(
                std::string(eb.buf + eb.sel_lo(), eb.buf + eb.sel_hi()).c_str());
            if (in.key_pressed[static_cast<std::size_t>(ws::input::keys::kX)]) {
                eb.erase_selection();
                edit.blink = 0.0f;
                changed = true;
            }
        }
        if (ctrl && in.key_pressed[static_cast<std::size_t>(ws::input::keys::kV)]) {
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
    return out;
}

// --- the draw half ------------------------------------------------------------

void text_input_draw(const Rect& rect, const TextInputResult& result,
                     const ChromeTheme& theme, const char* label,
                     const char* buf, int capacity) {
    ring_draw(result.focused, rect, theme);
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
    const auto x_at = [&](int index) -> float {
        float acc = 0.0f;
        for (int i = 0; i < index && i < len; ++i) {
            acc += char_advance(i);
        }
        return acc;
    };

    if (len > 0) {
        // The content, left-aligned inside the padding. Content is DATA,
        // so fit_text's "##" cut does not apply - the truncation here is
        // the walk itself: the longest prefix + "..." when the whole
        // line misses the row's room (the v1 filter already guarantees
        // one byte per glyph).
        const float whole = x_at(len);
        int visible = len;
        bool ellipsis = false;
        if (whole > content_room) {
            const float dots = cp_advance('.') * 3.0f;
            const float budget = std::max(0.0f, content_room - dots);
            float acc = 0.0f;
            visible = 0;
            for (int i = 0; i < len; ++i) {
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

    // The caret: a 2px bar in the text color at the mapped x - solid
    // on arrival and through work, half on/half off at rest (the phase
    // the behavior reports; the stock look's own under-half rule),
    // hidden while unfocused, locked or inert.
    if (result.focused && result.state != WidgetState::Disabled && live &&
        result.blink < 0.5f) {
        r2d::draw_quad(result.caret_x, mark_y, 2.0f, mark_h, theme.text);
    }
}

}  // namespace ws::ui::gui
