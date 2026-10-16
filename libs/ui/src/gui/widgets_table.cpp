// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the table - the column grid, its header chrome and the selectable
// rows under the loop-children id discipline.

#include "widgets_detail.hpp"

#include <algorithm>
#include <cstdint>

namespace ws::ui::gui {

// --- M21: the table --------------------------------------------------------

bool table(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
           const char* label, const TableColumn* columns, int column_count,
           const char* const* cells, int row_count,
           int* selected, float width) {
    // Fill-width, like button: a table wants its container's line.
    resolve_width(width, layout);

    // No columns, no grid: a bare slot that pretends nothing, the empty
    // combo's rule - a table with nothing to lay never latches looks.
    if (column_count <= 0 || columns == nullptr) {
        (void)layout.next(width, theme.control_height);
        return false;
    }
    // Null cells = an empty body: the header still shows (the caller's
    // "no results" state is the grid's own chrome), the rows do not.
    if (cells == nullptr) {
        row_count = 0;
    }

    // The geometry: one header row, the 2-px rule (the separator's own
    // look, a hardcoded pixel like the focus ring's 1), then CONTIGUOUS
    // rows - a grid, not a spaced list. The whole table is one slot in
    // the parent's flow; the rows never touch the cursor.
    const float row_h = theme.control_height;
    const float body_h = static_cast<float>(std::max(row_count, 0)) * row_h;
    const Rect rect = layout.next(width, row_h + 2.0f + body_h);

    // The width policy, resolved once: the fixed columns come off the
    // table's width first, the stretch columns (width <= 0) share what
    // remains equally, clamped at zero - never a backwards cell.
    int stretch_count = 0;
    float fixed_sum = 0.0f;
    for (int j = 0; j < column_count; ++j) {
        if (columns[static_cast<std::size_t>(j)].width > 0.0f) {
            fixed_sum += columns[static_cast<std::size_t>(j)].width;
        } else {
            ++stretch_count;
        }
    }
    const float stretch_w =
        stretch_count > 0
            ? std::max(0.0f, (rect.width - fixed_sum) /
                                 static_cast<float>(stretch_count))
            : 0.0f;

    // One cell-draw shared by the header and every row: the fitted text
    // at the column's x, one label_inset clear of each side - the same
    // draw every widget's label takes, so a cell wider than its column
    // truncates with "..." instead of spilling into the next.
    const auto cell_text = [&](const char* cell, float x, float cell_w,
                               float y, std::uint32_t color) {
        if (cell == nullptr || cell_w <= 0.0f) {
            return;
        }
        char scratch[128];
        const LabelFit fit =
            fit_text(cell, std::max(0.0f, cell_w - 2.0f * theme.label_inset),
                     theme, scratch, sizeof(scratch));
        draw_label(x + theme.label_inset, y, row_h, fit, color, theme);
    };
    // The column walk's one step: the policy width clamped to what the
    // table has left of itself, so a fixed set that sums past the width
    // clamps the CELLS to the table's own right edge - the walk reads
    // the clamp, so every column past the edge lays at it and draws
    // nothing, never outside the rect.
    const auto column_width = [&](float x, float policy) {
        return std::min(policy, std::max(0.0f, rect.x + rect.width - x));
    };

    // The disabled scope dims the whole table as one - the header is
    // chrome, but a locked grid reads dead from its top. One factor for
    // every non-face color; the rows' faces come from their own state().
    const float dim_by = gui.disabled() ? theme.disabled_alpha : 1.0f;

    // The header: a control face bar, its labels in the cell geometry,
    // then the rule that anchors the grid. Chrome - no id, no
    // hit-test, nothing focusable (a header click claims nothing;
    // sorting is a future seam, not a hidden one).
    r2d::draw_rounded_quad(rect.x, rect.y, rect.width, row_h,
                           theme.corner_radius,
                           gui.disabled() ? theme.control_disabled
                                          : theme.control_bg);
    {
        float x = rect.x;
        for (int j = 0; j < column_count; ++j) {
            const TableColumn& col = columns[static_cast<std::size_t>(j)];
            const float cw =
                column_width(x, col.width > 0.0f ? col.width : stretch_w);
            cell_text(col.label, x, cw, rect.y, dim(theme.text, dim_by));
            x += cw;
        }
    }
    r2d::draw_quad(rect.x, rect.y + row_h, rect.width, 2.0f,
                   dim(theme.border, dim_by));

    // One row's cells in the column walk - the same draw the header's
    // labels take, at the row's own y.
    const auto draw_cells = [&](const char* const* row_cells, float y) {
        float x = rect.x;
        for (int j = 0; j < column_count; ++j) {
            const TableColumn& col = columns[static_cast<std::size_t>(j)];
            const float cw =
                column_width(x, col.width > 0.0f ? col.width : stretch_w);
            cell_text(row_cells[static_cast<std::size_t>(j)], x, cw, y,
                      dim(theme.text, dim_by));
            x += cw;
        }
    };

    // The rows. Interactive ones (a non-null selected) are citizens -
    // selectable's exact look and answer; a null selected lays the same
    // grid as pure chrome, no ids at all (the statics' "cannot interact"
    // spelled by the pointer).
    bool changed = false;
    const float body_top = rect.y + row_h + 2.0f;
    {
        // The rows id under the table's label, then by index - the
        // combo's item discipline, so the same grid works anywhere and
        // two tables differ by label like any widget.
        ScopedId table_scope(gui.ids(), label);
        const auto row_id = [&](int index) {
            ScopedId row_scope(gui.ids(), index);
            return gui.ids().id("row");
        };
        for (int i = 0; i < row_count; ++i) {
            const Rect row_rect{rect.x,
                                body_top + static_cast<float>(i) * row_h,
                                rect.width, row_h};
            const char* const* row_cells =
                cells + static_cast<std::size_t>(i) *
                            static_cast<std::size_t>(column_count);
            if (selected == nullptr) {
                draw_cells(row_cells, row_rect.y);
                continue;
            }

            const WidgetId rid = row_id(i);
            gui.register_focusable(rid);
            const WidgetResult result = gui.state(rid, row_rect);
            const HitTest& hit = result.hit;

            // Up/Down step the focused row, clamped at the grid's ends -
            // the slider's arrow rule (Tab's wrap is the screen's walk,
            // not the grid's). set_focus is immediate, combo's handback
            // idiom: the neighbor's state() still runs this frame, so its
            // ring draws now, and the scroll panel's focus-into-view
            // follows the move for free.
            if (gui.is_focused(rid)) {
                if (gui.input().key_pressed[keys::kUp] && i > 0) {
                    gui.set_focus(row_id(i - 1));
                } else if (gui.input().key_pressed[keys::kDown] &&
                           i + 1 < row_count) {
                    gui.set_focus(row_id(i + 1));
                }
            }

            // Keyboard pick: Enter/Space on the focused row, edge-only -
            // selectable's rule; the selection itself stays the caller's
            // to move.
            const bool key_clicked = key_activated(gui, rid);

            // The focus ring, like every row's.
            focus_ring(gui, rid, row_rect, theme);

            // The bare-list face - selectable's look, no idle rung: an
            // unhovered, unselected row is text on the parent's chrome.
            if (const auto face = list_face(result.state, theme)) {
                r2d::draw_rounded_quad(row_rect.x, row_rect.y, row_rect.width,
                                       row_rect.height, theme.corner_radius,
                                       *face);
            }

            // The selection marker: selectable's 3-px accent bar down the
            // row's left edge, riding every face, dimmed under the lock.
            // An out-of-range *selected (the caller's own) marks nothing.
            if (*selected == i && row_rect.width > 0.0f &&
                row_rect.height > 0.0f) {
                r2d::draw_quad(row_rect.x, row_rect.y, 3.0f, row_rect.height,
                               dim(theme.control_on, dim_by));
            }

            // The pick: a click or the focused row's key writes the
            // caller's int and answers ONLY on a real move - the radio's
            // idempotence (a re-pick on the current row is a no-op).
            if ((hit.clicked || key_clicked) && *selected != i) {
                *selected = i;
                changed = true;
            }
            draw_cells(row_cells, row_rect.y);
        }
    }
    return changed;
}

}  // namespace ws::ui::gui
