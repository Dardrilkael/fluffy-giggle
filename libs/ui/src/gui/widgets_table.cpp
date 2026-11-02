// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the table - the column grid, its header chrome and the selectable
// rows under the loop-children id discipline. The rows fold through the
// shared begin but keep their own late key edge under the grid walk.
// The look halves are public (table_header_draw / table_row_draw over
// TableGeometry) - the composed widget owns the geometry resolution
// and the walk, the draws own the pixels.

#include "widgets_detail.hpp"

#include <algorithm>
#include <cstdint>

namespace ws::ui::gui {

// --- M21: the table --------------------------------------------------------

// The column walk's one step, shared by both draws: the policy width
// (fixed, or the stretch width the composed table resolved) clamped to
// what the table has left of itself, so a fixed set that sums past the
// width clamps the CELLS to the table's own right edge - the walk reads
// the clamp, so every column past the edge lays at it and draws
// nothing, never outside the rect.
namespace {
float column_width(const TableGeometry& geom, float x, float policy) {
    return std::min(policy,
                    std::max(0.0f, geom.rect.x + geom.rect.width - x));
}
}  // namespace

void table_header_draw(const TableGeometry& geom, bool disabled,
                       const ChromeTheme& theme) {
    // The header: a control face bar, its labels in the cell geometry,
    // then the rule that anchors the grid. Chrome - no id, no
    // hit-test, nothing focusable (a header click claims nothing;
    // sorting is a future seam, not a hidden one). A locked grid's
    // chrome reads dead from its top.
    const float row_h = geom.row_height;
    const float dim_by = disabled ? theme.disabled_alpha : 1.0f;
    r2d::draw_rounded_quad(geom.rect.x, geom.rect.y, geom.rect.width, row_h,
                           theme.corner_radius,
                           disabled ? theme.control_disabled : theme.control_bg);
    {
        float x = geom.rect.x;
        for (int j = 0; j < geom.column_count; ++j) {
            const TableColumn& col =
                geom.columns[static_cast<std::size_t>(j)];
            const float cw = column_width(
                geom, x, col.width > 0.0f ? col.width : geom.stretch_width);
            if (col.label != nullptr && cw > 0.0f) {
                char scratch[128];
                const LabelFit fit = fit_text(
                    col.label, std::max(0.0f, cw - 2.0f * theme.label_inset),
                    theme, scratch, sizeof(scratch));
                draw_label(x + theme.label_inset, geom.rect.y, row_h, fit,
                           dim(theme.text, dim_by), theme);
            }
            x += cw;
        }
    }
    r2d::draw_quad(geom.rect.x, geom.rect.y + row_h, geom.rect.width, 2.0f,
                   dim(theme.border, dim_by));
}

void table_row_draw(const Rect& row_rect, const TableRowPaint& paint,
                    const char* const* cells, const TableGeometry& geom,
                    const ChromeTheme& theme) {
    const float dim_by = paint.dimmed ? theme.disabled_alpha : 1.0f;

    // The ring: reads the paint's captured focus (the composed walk
    // reads it after its neighbor stepping - the capture timing is the
    // walk's, not the draw's).
    ring_draw(paint.focused, row_rect, theme);

    // The bare-list face - selectable's look, no idle rung: an
    // unhovered, unselected row is text on the parent's chrome.
    if (const auto face = list_face_of(paint, theme)) {
        r2d::draw_rounded_quad(row_rect.x, row_rect.y, row_rect.width,
                               row_rect.height, theme.corner_radius, *face);
    }

    // The selection marker: selectable's 3-px accent bar down the
    // row's left edge, riding every face, dimmed under the lock.
    if (paint.selected && row_rect.width > 0.0f && row_rect.height > 0.0f) {
        r2d::draw_quad(row_rect.x, row_rect.y, 3.0f, row_rect.height,
                       dim(theme.control_on, dim_by));
    }

    // The cells in the column walk - the same draw the header's labels
    // take, at the row's own y.
    float x = geom.rect.x;
    for (int j = 0; j < geom.column_count; ++j) {
        const TableColumn& col = geom.columns[static_cast<std::size_t>(j)];
        const float cw = column_width(
            geom, x, col.width > 0.0f ? col.width : geom.stretch_width);
        const char* cell = cells != nullptr ? cells[static_cast<std::size_t>(j)]
                                            : nullptr;
        if (cell != nullptr && cw > 0.0f) {
            char scratch[128];
            const LabelFit fit = fit_text(
                cell, std::max(0.0f, cw - 2.0f * theme.label_inset), theme,
                scratch, sizeof(scratch));
            draw_label(x + theme.label_inset, row_rect.y, geom.row_height, fit,
                       dim(theme.text, dim_by), theme);
        }
        x += cw;
    }
}

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

    // The resolved grid the split draws walk - the same numbers the
    // geometry above computed, handed to the look halves whole.
    const TableGeometry geom{rect, row_h, columns, column_count, stretch_w};

    // The header: chrome, the split draw's half.
    paint(theme, &WidgetPainters::table_header, &table_header_draw, geom,
          gui.disabled(), theme);

    // The rows. Interactive ones (a non-null selected) are citizens -
    // selectable's exact look and answer; a null selected lays the same
    // grid as pure chrome, no ids at all (the statics' "cannot interact"
    // spelled by the pointer). The row look is the split draw's half;
    // the walk (focus stepping, the pick) stays here.
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
                // The chrome path: the resting paint - no face, no ring,
                // no selection bar (a null selected lays text on the
                // parent's chrome), the scope's dim the only mark.
                TableRowPaint rest;
                rest.state = WidgetState::Normal;
                rest.dimmed = gui.disabled();
                paint(theme, &WidgetPainters::table_row, &table_row_draw,
                      row_rect, rest, row_cells, geom, theme);
                continue;
            }

            const WidgetId rid = row_id(i);
            // The row's fold: registration, the one hit-test with its
            // focus latch, the focused capture - the same begin every
            // behavior takes. Not selectable_behavior itself: the walk
            // below hands focus to a neighbor mid-row, and this row's
            // Enter edge and ring must read after it, where the
            // behavior's fold reads before.
            const WidgetPaint row =
                begin_widget_paint(gui, rid, row_rect, false, {});

            // Up/Down step the focused row, clamped at the grid's ends -
            // the slider's arrow rule (Tab's wrap is the screen's walk,
            // not the grid's). set_focus is immediate, combo's handback
            // idiom: the neighbor's state() still runs this frame, so its
            // ring draws now, and the scroll panel's focus-into-view
            // follows the move for free. The gate rides the fold's
            // capture - nothing between the fold and here moves focus.
            if (row.focused) {
                if (gui.input().key_pressed[ws::input::keys::kUp] && i > 0) {
                    gui.set_focus(row_id(i - 1));
                } else if (gui.input().key_pressed[ws::input::keys::kDown] &&
                           i + 1 < row_count) {
                    gui.set_focus(row_id(i + 1));
                }
            }

            // Keyboard pick: Enter/Space on the focused row, edge-only -
            // selectable's rule; the selection itself stays the caller's
            // to move. Read after the walk, not folded into the
            // behavior's begin: the edge belongs to the row that holds
            // focus once the walk has moved it.
            const bool key_clicked = key_activated(gui, rid);

            // The row's paint over the walk's answer: the focus reads
            // FRESH (the walk above may have moved it since the fold
            // took it), the selection is the caller's display state,
            // and the disabled scope's whole-table dim rides the paint.
            TableRowPaint row_paint;
            row_paint.state = row.state;
            row_paint.focused = gui.is_focused(rid);
            row_paint.hit = row.hit;
            row_paint.selected = *selected == i;  // out-of-range marks none
            row_paint.dimmed = gui.disabled();
            paint(theme, &WidgetPainters::table_row, &table_row_draw,
                  row_rect, row_paint, row_cells, geom, theme);

            // The pick: a click or the focused row's key writes the
            // caller's int and answers ONLY on a real move - the radio's
            // idempotence (a re-pick on the current row is a no-op).
            if ((row.hit.clicked || key_clicked) && *selected != i) {
                *selected = i;
                changed = true;
            }
        }
    }
    return changed;
}

}  // namespace ws::ui::gui
