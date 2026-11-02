// The P0 split-draw acceptance: every look half the ui-skin plan lifted
// out of the composed widgets (menu item / title / separator, table
// header / row, toast chrome, popup frame) paints over a hand-built
// paint exactly the stock draws it replaced - driven standalone (rect +
// paint + theme, no context) AND through the composed widgets, whose
// stub recordings prove the rewiring changed nothing. The stock parity
// the plan gates P0 on: same draw counts, same call kinds.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>

#include <ws/ui/renderer2d.hpp>

#include "ui_gui_stub.hpp"

#include <cstdint>

namespace {

using ws::ui::gui::GuiContext;
using ws::input::InputFrame;
using ws::ui::gui::LayoutCursor;
using ws::ui::gui::kDefaultChrome;

// The suite's own snap(): a present pointer, nothing else.
InputFrame snap(float x, float y) {
    InputFrame frame;
    frame.mouse_x = x;
    frame.mouse_y = y;
    frame.mouse_present = true;
    return frame;
}

}  // namespace

TEST_CASE("popup_frame_draw paints the panel chrome, the seam every overlay wears") {
    ws::ui::gui_test::stub.reset();
    const ws::ui::gui::Rect bounds{10.0f, 20.0f, 200.0f, 100.0f};

    ws::ui::gui::popup_frame_draw(bounds, kDefaultChrome);

    // The stock spelling (P5): the border as a rounded OUTLINE at the
    // popup radius - exactly panel_draw's pair, no straight quads, and
    // no shadow while the stock tokens stay zero.
    CHECK(ws::ui::gui_test::stub.outlines == 1);
    CHECK(ws::ui::gui_test::stub.quads == 0);
    CHECK(ws::ui::gui_test::stub.shadows == 0);
    CHECK(ws::ui::gui_test::stub.clip_pushes == 0);

    // The parity with the panel's own public chrome: the same bounds
    // through panel_draw is the same outline (the counts pin the
    // bytes; the values are the pair's own).
    ws::ui::gui_test::stub.reset();
    ws::ui::gui::panel_draw(bounds, kDefaultChrome);
    CHECK(ws::ui::gui_test::stub.outlines == 1);
    CHECK(ws::ui::gui_test::stub.quads == 0);
}

TEST_CASE("toast_draw paints the floating frame plus the severity band") {
    ws::ui::gui_test::stub.reset();
    const ws::ui::gui::Rect bounds{0.0f, 0.0f, 160.0f, 40.0f};

    ws::ui::gui::toast_draw(bounds, ws::ui::gui::ToastSeverity::info,
                            kDefaultChrome);

    // popup_frame_draw's outline (the frame pair) + the 3-px severity
    // band = one outline call and one straight quad, nothing clipped.
    CHECK(ws::ui::gui_test::stub.outlines == 1);
    CHECK(ws::ui::gui_test::stub.quads == 1);

    // Every severity answers the same shape - the accent is a color
    // choice, never a geometry change.
    for (const ws::ui::gui::ToastSeverity severity :
         {ws::ui::gui::ToastSeverity::warning,
          ws::ui::gui::ToastSeverity::error}) {
        ws::ui::gui_test::stub.reset();
        ws::ui::gui::toast_draw(bounds, severity, kDefaultChrome);
        CHECK(ws::ui::gui_test::stub.outlines == 1);
        CHECK(ws::ui::gui_test::stub.quads == 1);
    }
}

TEST_CASE("menu_item_draw ladders the faces over a hand-built paint") {
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 180.0f, 26.0f};
    const ws::ui::gui::MenuItem item{.label = "open"};

    // The geometry a level would resolve for a plain row: the label
    // column is the whole window.
    ws::ui::gui::MenuGeometry geometry;
    geometry.content_width = rect.width;
    geometry.label_budget = rect.width;

    // A resting, unhighlighted row: the bare-list look - no face at
    // all, just the label draw (fontless: the label bar's quad).
    ws::ui::gui_test::stub.reset();
    ws::ui::gui::MenuItemPaint rest;
    rest.state = ws::ui::gui::WidgetState::Normal;
    ws::ui::gui::menu_item_draw(rect, rest, kDefaultChrome, item, geometry);
    CHECK(ws::ui::gui_test::stub.quads == 1);  // the label bar alone
    CHECK(ws::ui::gui_test::stub.rounded == 0);

    // The highlight: the accent face lands under the label.
    ws::ui::gui_test::stub.reset();
    ws::ui::gui::MenuItemPaint aimed = rest;
    aimed.highlighted = true;
    ws::ui::gui::menu_item_draw(rect, aimed, kDefaultChrome, item, geometry);
    CHECK(ws::ui::gui_test::stub.quads == 2);  // the face + the label bar

    // A dead row: the one dead face wherever it sits (the MenuItem's
    // own flag drives it, like the chain's state() fold does).
    ws::ui::gui_test::stub.reset();
    const ws::ui::gui::MenuItem dead_item{.label = "open", .disabled = true};
    ws::ui::gui::menu_item_draw(rect, rest, kDefaultChrome, dead_item,
                                geometry);
    CHECK(ws::ui::gui_test::stub.quads == 2);  // the dead face + the bar

    // A check row in a gutter: the box (rounded) + the fill while
    // checked + the label bar - the same shapes checkbox_draw wears.
    ws::ui::gui_test::stub.reset();
    const ws::ui::gui::MenuItem check{.label = "bold",
                                      .kind = ws::ui::gui::MenuItemKind::check,
                                      .checked = true};
    ws::ui::gui::MenuGeometry with_gutter = geometry;
    with_gutter.gutter = kDefaultChrome.control_height;
    with_gutter.label_budget = rect.width - with_gutter.gutter;
    ws::ui::gui::menu_item_draw(rect, rest, kDefaultChrome, check,
                                with_gutter);
    CHECK(ws::ui::gui_test::stub.rounded == 2);  // the box + the fill
    CHECK(ws::ui::gui_test::stub.quads == 1);    // the label bar

    // A parent row: the submenu diamond, a rotated quad in the marker
    // column - the accent while its child is open, the border while not.
    ws::ui::gui_test::stub.reset();
    // A live parent: a submenu pointer with a count above zero - the
    // marker's own test, exactly the chain's is_parent rule.
    static const ws::ui::gui::MenuItem submenu[1] = {
        {.label = "pdf"},
    };
    const ws::ui::gui::MenuItem parent{.label = "export",
                                       .submenu = submenu,
                                       .submenu_count = 1};
    ws::ui::gui::MenuGeometry with_marker = geometry;
    with_marker.marker = kDefaultChrome.control_height * 0.5f;
    with_marker.marker_room = kDefaultChrome.padding + with_marker.marker;
    with_marker.label_budget = rect.width - with_marker.marker_room;
    ws::ui::gui::menu_item_draw(rect, rest, kDefaultChrome, parent,
                                with_marker);
    // label bar + diamond, and the diamond only (the rest draws no face)
    CHECK(ws::ui::gui_test::stub.quads == 2);
}

TEST_CASE("menu_item_separator_draw is exactly one rule") {
    ws::ui::gui_test::stub.reset();
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 180.0f, 26.0f};
    ws::ui::gui::menu_item_separator_draw(rect, kDefaultChrome);
    CHECK(ws::ui::gui_test::stub.quads == 1);
    CHECK(ws::ui::gui_test::stub.rounded == 0);
}

TEST_CASE("menu_title_draw keys its face ladder on the open flag") {
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 64.0f, 26.0f};
    const char* title = "&file";

    // A resting title: nothing but the label (fontless bar) - text on
    // the bar strip, the VS Code menu look.
    ws::ui::gui_test::stub.reset();
    ws::ui::gui::MenuTitlePaint rest;
    rest.state = ws::ui::gui::WidgetState::Normal;
    ws::ui::gui::menu_title_draw(rect, rest, kDefaultChrome, title);
    // the label bar + the mnemonic underline (F is the first char)
    CHECK(ws::ui::gui_test::stub.quads == 2);
    CHECK(ws::ui::gui_test::stub.rounded == 0);

    // The open title: the popup window's own face (rounded panel_bg).
    ws::ui::gui_test::stub.reset();
    ws::ui::gui::MenuTitlePaint open_title = rest;
    open_title.open = true;
    ws::ui::gui::menu_title_draw(rect, open_title, kDefaultChrome, title);
    CHECK(ws::ui::gui_test::stub.rounded == 1);
    CHECK(ws::ui::gui_test::stub.quads == 2);  // bar + underline

    // The focused title: the ring (rounded) behind no face.
    ws::ui::gui_test::stub.reset();
    ws::ui::gui::MenuTitlePaint focused = rest;
    focused.focused = true;
    ws::ui::gui::menu_title_draw(rect, focused, kDefaultChrome, title);
    CHECK(ws::ui::gui_test::stub.rounded == 1);
}

TEST_CASE("table_header_draw paints the bar, the labels and the rule") {
    ws::ui::gui_test::stub.reset();
    const ws::ui::gui::TableColumn columns[] = {
        {"name", 0.0f}, {"value", 60.0f}};
    ws::ui::gui::TableGeometry geom;
    geom.rect = {0.0f, 0.0f, 200.0f, 26.0f};
    geom.row_height = 26.0f;
    geom.columns = columns;
    geom.column_count = 2;
    geom.stretch_width = 140.0f;

    ws::ui::gui::table_header_draw(geom, false, kDefaultChrome);

    // The face bar (rounded) + the rule under it (flat) + two label
    // bars (fontless) = 1 rounded + 3 quads.
    CHECK(ws::ui::gui_test::stub.rounded == 1);
    CHECK(ws::ui::gui_test::stub.quads == 3);

    // The locked grid's chrome reads dead from its top: the same
    // geometry, the dead face, everything dimmed but the shape intact.
    ws::ui::gui_test::stub.reset();
    ws::ui::gui::table_header_draw(geom, true, kDefaultChrome);
    CHECK(ws::ui::gui_test::stub.rounded == 1);
    CHECK(ws::ui::gui_test::stub.quads == 3);
}

TEST_CASE("table_row_draw is the bare list until marked") {
    const ws::ui::gui::TableColumn columns[] = {
        {"name", 0.0f}, {"value", 60.0f}};
    ws::ui::gui::TableGeometry geom;
    geom.rect = {0.0f, 0.0f, 200.0f, 26.0f};
    geom.row_height = 26.0f;
    geom.columns = columns;
    geom.column_count = 2;
    geom.stretch_width = 140.0f;
    const char* cells[] = {"rc", "1k"};

    // The resting row: no face, no ring, no bar - just the two cells
    // (fontless label bars).
    ws::ui::gui_test::stub.reset();
    ws::ui::gui::TableRowPaint rest;
    rest.state = ws::ui::gui::WidgetState::Normal;
    ws::ui::gui::table_row_draw({0.0f, 28.0f, 200.0f, 26.0f}, rest, cells,
                                geom, kDefaultChrome);
    CHECK(ws::ui::gui_test::stub.quads == 2);
    CHECK(ws::ui::gui_test::stub.rounded == 0);

    // Hover: the list face (rounded) under the same cells.
    ws::ui::gui_test::stub.reset();
    ws::ui::gui::TableRowPaint hovered = rest;
    hovered.state = ws::ui::gui::WidgetState::Hovered;
    ws::ui::gui::table_row_draw({0.0f, 28.0f, 200.0f, 26.0f}, hovered, cells,
                                geom, kDefaultChrome);
    CHECK(ws::ui::gui_test::stub.quads == 2);
    CHECK(ws::ui::gui_test::stub.rounded == 1);

    // The selected row: the 3-px accent bar joins the cells.
    ws::ui::gui_test::stub.reset();
    ws::ui::gui::TableRowPaint picked = rest;
    picked.selected = true;
    ws::ui::gui::table_row_draw({0.0f, 28.0f, 200.0f, 26.0f}, picked, cells,
                                geom, kDefaultChrome);
    CHECK(ws::ui::gui_test::stub.quads == 3);

    // The focused row: the ring (rounded) behind nothing.
    ws::ui::gui_test::stub.reset();
    ws::ui::gui::TableRowPaint keyed = rest;
    keyed.focused = true;
    ws::ui::gui::table_row_draw({0.0f, 28.0f, 200.0f, 26.0f}, keyed, cells,
                                geom, kDefaultChrome);
    CHECK(ws::ui::gui_test::stub.rounded == 1);
}

TEST_CASE("the composed menu bar and table route through the split draws") {
    // The composed widgets' own stub recordings: the rewiring onto the
    // split halves must not have changed what they draw. A bar with one
    // menu (two rows, one a check) and a table beside it paint through
    // the seam, balanced clips, no stray layers.
    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    LayoutCursor layout;

    const ws::ui::gui::MenuItem items[] = {
        {.label = "cut", .shortcut = "Ctrl+X"},
        {.label = "bold", .kind = ws::ui::gui::MenuItemKind::check,
         .checked = true},
    };
    const ws::ui::gui::MenuDesc menus[] = {
        {"edit", items, 2},
    };
    int open = -1;

    // The strip lays through a panel scope - a bare cursor hands out
    // empty rects (the drop-safe rule), and the bar is chrome a host
    // places like any panel content.
    gui.begin_frame(snap(-100.0f, -100.0f));
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "board",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    ws::ui::gui::menu_bar(gui, layout, kDefaultChrome, "bar", menus, 1, open);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();

    // The strip drew (the title's label) and drew nothing it should
    // not have: no open menu means no popup chrome at all.
    CHECK(ws::ui::gui_test::stub.quads >= 1);
    CHECK(ws::ui::gui_test::stub.layer_sets == 0);

    // A table with two rows, interactive: header + rows paint, and the
    // grid stays balanced.
    const ws::ui::gui::TableColumn columns[] = {
        {"name", 0.0f}, {"value", 60.0f}};
    const char* cells[] = {"r1", "10", "r2", "20"};
    int selected = 0;
    ws::ui::gui_test::stub.reset();
    gui.begin_frame(snap(-100.0f, -100.0f));
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "board",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    CHECK(!ws::ui::gui::table(gui, layout, kDefaultChrome, "grid", columns,
                              2, cells, 2, &selected));
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    CHECK(ws::ui::gui_test::stub.rounded >= 1);  // the header face
    CHECK(ws::ui::gui_test::stub.quads >= 4);    // labels + the bar + the rule

    // A toast through the composed draw: the split chrome lands inside
    // the scope, balanced as ever.
    ws::ui::gui::Toast toast{.text = "saved"};
    ws::ui::gui::Toasts toasts;
    toasts.push(toast);
    ws::ui::gui_test::stub.reset();
    gui.begin_frame(snap(-100.0f, -100.0f));
    (void)ws::ui::gui::draw_toasts(gui, kDefaultChrome, toasts);
    gui.end_frame();
    // Headless the frame is empty - the toast's bounds clamp to nothing
    // and nothing paints. The composed call still balances its clips.
    CHECK(ws::ui::gui_test::stub.clip_pushes ==
          ws::ui::gui_test::stub.clip_pops);
}
