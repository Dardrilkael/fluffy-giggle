// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the combo row - the closed row with its right-edge marker and the
// popup list the open flag hangs off it. The row composes its split
// halves in order (the ladder, then combo_behavior, then combo_draw);
// the dropdown is the widget's own half, placed through place_popup.

#include "widgets_detail.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace ws::ui::gui {

bool combo(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
           const char* label, const char* const* items, int count,
           int& selected, bool& open, float width, bool disabled) {
    const RowLayout row = begin_widget_row(gui, layout, theme, label, width);
    const bool empty = count <= 0 || items == nullptr;
    const ComboResult result =
        combo_behavior(gui, row.id, row.rect, open, empty, disabled);
    combo_draw(row.rect, result, theme, label, items, count, selected, open);

    // The dropdown, placed by the widget through the shared placement:
    // below the row while the frame has room, else flipped above,
    // clamped at the frame's edges - the honest v1 clamp (a frame too
    // short shows what fits, and the popup's own clip cuts whatever
    // exceeds the clamped bounds).
    if (empty) {
        return false;  // nothing to lay beneath a bare row
    }
    const float items_h = static_cast<float>(count) * theme.control_height +
                          static_cast<float>(std::max(count - 1, 0)) *
                              theme.spacing;
    const Rect bounds = place_popup(row.rect, row.rect.width,
                                    items_h + 2.0f * theme.padding);

    bool changed = false;
    if (begin_popup(gui, layout, theme, label, row.rect, bounds, open)) {
        for (int i = 0; i < count; ++i) {
            gui.ids().push(i);  // one label, count ids
            const char* item = items[static_cast<std::size_t>(i)];
            const bool picked =
                selectable(gui, layout, theme,
                           item != nullptr ? item : "", i == selected, -1.0f);
            if (picked && i != selected) {
                selected = i;  // the caller's int, written on a real move
                changed = true;
            }
            if (picked) {
                open = false;  // a choice closes: the combo's rule
                // And the focus comes back to the row: the item's id is
                // about to vanish with the dropdown, and the absent-
                // widget rule would leave the next Tab restarting at the
                // screen's first focusable - the row keeps the walk where
                // it was. (The outside-press close needs no help: the
                // press that closed it landed on a focusable beneath,
                // which took the focus by its own rule.)
                gui.set_focus(row.id);
            }
            gui.ids().pop();
        }
        end_popup(gui, layout);
    }
    return changed;
}

ComboResult combo_behavior(GuiContext& gui, WidgetId id, const Rect& rect,
                           bool& open, bool empty, bool disabled,
                           const HitShape& hit_shape) {
    ComboResult result{begin_widget_paint(gui, id, rect, disabled, hit_shape)};

    // The empty combo: nothing to choose, nothing to open - a bare row
    // that never pretends otherwise (and never touches the caller's
    // bool beyond closing it, so a later refill starts closed). A
    // disabled combo locks its dropdown with it: the row claims nothing,
    // so no click or key could reopen one, and a dropdown left open by a
    // lock landing mid-flight closes - the documented whole-widget lock.
    if (empty || disabled) {
        open = false;
    }

    // The row's click and keyboard toggle: Enter/Space on the focused
    // row flips it too, like a header - never on an empty or locked
    // combo (the locked row holds no focus, so the keys never arrive).
    const bool key_toggled = !empty && !disabled && key_activated(gui, id);
    result.toggled = !empty && (result.hit.clicked || key_toggled);
    if (result.toggled) {
        open = !open;  // the caller's bool is the popup's state
    }
    return result;
}

void combo_draw(const Rect& rect, const ComboResult& result,
                const ChromeTheme& theme, const char* label,
                const char* const* items, int count, int selected,
                bool open) {
    // The focus ring: the on-accent as a 1px outline behind the row, so
    // Tab navigation is legible. Skipped for degenerate rows.
    ring_draw(result.focused, rect, theme);

    // The disabled look: the dead face for the row, the marker and the
    // label dimmed through the one helper.
    const float dim_by = dim_factor(result.state, theme);

    // The row: the faces a button wears, rounded by the theme.
    const std::uint32_t face = control_face(result.state, theme);
    r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                           theme.corner_radius, face);

    // The marker at the RIGHT edge - the collapsing_header's marker
    // mirrored: the faint border closed, the accent open. The same
    // square-inset geometry, one gap from the row's right side.
    const float inset = std::max(theme.check_inset, theme.corner_radius);
    const float marker = theme.control_height - 2.0f * inset;
    if (marker > 0.0f) {
        r2d::draw_quad(rect.x + rect.width - inset - marker, rect.y + inset,
                       marker, marker,
                       open ? dim(theme.control_on, dim_by) : theme.border);
    }

    // The "label: item" line, drag_float's shape: the display cut first,
    // then one snprintf - a `selected` out of range (or a label-less
    // combo) shows the item alone, the label alone... a label-less
    // empty combo shows nothing. The marker's band comes off the text
    // budget, a padding each side of what remains.
    char value_buf[64];
    const std::string_view display = label_display(label);
    const bool empty = count <= 0 || items == nullptr;
    const bool has_item = !empty && selected >= 0 && selected < count &&
                          items[static_cast<std::size_t>(selected)] != nullptr;
    if (display.empty()) {
        if (has_item) {
            std::snprintf(value_buf, sizeof(value_buf), "%s",
                          items[static_cast<std::size_t>(selected)]);
        } else {
            value_buf[0] = '\0';
        }
    } else if (has_item) {
        std::snprintf(value_buf, sizeof(value_buf), "%.*s: %s",
                      static_cast<int>(display.size()), display.data(),
                      items[static_cast<std::size_t>(selected)]);
    } else {
        std::snprintf(value_buf, sizeof(value_buf), "%.*s",
                      static_cast<int>(display.size()), display.data());
    }
    char scratch[128];
    const LabelFit fit =
        fit_text(value_buf, std::max(0.0f, rect.width - marker - inset -
                                              2.0f * theme.padding),
                 theme, scratch, sizeof(scratch));
    draw_label(rect.x + theme.padding, rect.y, rect.height, fit,
               dim(theme.text, dim_by), theme);
}

}  // namespace ws::ui::gui
