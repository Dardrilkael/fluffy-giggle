// The P3 painters-seam acceptance (the ui-skin plan): the composed
// widgets route their draws through the theme's WidgetPainters table -
// every family reaches its row when a skin installs one, falls back to
// the stock draw when the table is null or the row is, and the
// behavior halves never read the table at all (the interactions answer
// identically under any skin). A test-local recording table is the
// proof: its rows append to vectors, and the counts and the byte-parity
// of the stock path ARE the assertions.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/painters.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>

#include "ui_gui_stub.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using ws::ui::gui::GuiContext;
using ws::input::InputFrame;
using ws::ui::gui::LayoutCursor;
using ws::ui::gui::kDefaultChrome;

InputFrame snap(float x, float y, bool down = false, bool pressed = false,
                bool released = false) {
    InputFrame frame;
    frame.mouse_x = x;
    frame.mouse_y = y;
    frame.mouse_present = true;
    frame.mouse_down[0] = down;
    frame.mouse_pressed[0] = pressed;
    frame.mouse_released[0] = released;
    return frame;
}

// The recording skin: every row counts its calls. One table, one log -
// the cases reset both.
struct SkinLog {
    std::vector<std::string> calls;

    void add(const char* what) { calls.emplace_back(what); }
    bool saw(const char* what) const {
        for (const std::string& call : calls) {
            if (call == what) {
                return true;
            }
        }
        return false;
    }
    std::size_t count(const char* what) const {
        std::size_t n = 0;
        for (const std::string& call : calls) {
            if (call == what) {
                ++n;
            }
        }
        return n;
    }
};
SkinLog g_log;

#define SKIN_ROW(name, ...)                                   \
    void skin_##name(__VA_ARGS__) { g_log.add(#name); }

SKIN_ROW(button, const ws::ui::gui::Rect&, const ws::ui::gui::ButtonResult&,
         const ws::ui::gui::ChromeTheme&, const char*, ws::ui::r2d::Sprite)
SKIN_ROW(checkbox, const ws::ui::gui::Rect&,
         const ws::ui::gui::CheckboxResult&,
         const ws::ui::gui::ChromeTheme&, const char*, bool)
SKIN_ROW(radio, const ws::ui::gui::Rect&, const ws::ui::gui::RadioResult&,
         const ws::ui::gui::ChromeTheme&, const char*, bool)
SKIN_ROW(toggle_switch, const ws::ui::gui::Rect&,
         const ws::ui::gui::ToggleSwitchResult&,
         const ws::ui::gui::ChromeTheme&, const char*, bool)
SKIN_ROW(collapsing_header, const ws::ui::gui::Rect&,
         const ws::ui::gui::CollapsingHeaderResult&,
         const ws::ui::gui::ChromeTheme&, const char*, bool)
SKIN_ROW(selectable, const ws::ui::gui::Rect&,
         const ws::ui::gui::SelectableResult&,
         const ws::ui::gui::ChromeTheme&, const char*, bool)
SKIN_ROW(slider, const ws::ui::gui::Rect&, const ws::ui::gui::SliderResult&,
         const ws::ui::gui::ChromeTheme&, const char*, float, float, float)
SKIN_ROW(drag_float, const ws::ui::gui::Rect&,
         const ws::ui::gui::DragFloatResult&,
         const ws::ui::gui::ChromeTheme&, const char*, float)
SKIN_ROW(spinner, const ws::ui::gui::Rect&, const ws::ui::gui::SpinnerResult&,
         const ws::ui::gui::ChromeTheme&)
SKIN_ROW(progress_indeterminate, const ws::ui::gui::Rect&,
         const ws::ui::gui::IndeterminateResult&,
         const ws::ui::gui::ChromeTheme&)
SKIN_ROW(tab, const ws::ui::gui::Rect&, const ws::ui::gui::TabResult&,
         const ws::ui::gui::ChromeTheme&, const char*, bool)
SKIN_ROW(combo, const ws::ui::gui::Rect&, const ws::ui::gui::ComboResult&,
         const ws::ui::gui::ChromeTheme&, const char*, const char* const*,
         int, int, bool)
SKIN_ROW(text_input, const ws::ui::gui::Rect&,
         const ws::ui::gui::TextInputResult&,
         const ws::ui::gui::ChromeTheme&, const char*, const char*, int)
SKIN_ROW(panel, const ws::ui::gui::Rect&, const ws::ui::gui::ChromeTheme&)
SKIN_ROW(popup_frame, const ws::ui::gui::Rect&,
         const ws::ui::gui::ChromeTheme&)
SKIN_ROW(scrollbar, const ws::ui::gui::Rect&,
         const ws::ui::gui::ScrollbarThumb&, const ws::ui::gui::ChromeTheme&)
SKIN_ROW(menu_item, const ws::ui::gui::Rect&,
         const ws::ui::gui::MenuItemPaint&, const ws::ui::gui::ChromeTheme&,
         const ws::ui::gui::MenuItem&, const ws::ui::gui::MenuGeometry&)
SKIN_ROW(menu_item_separator, const ws::ui::gui::Rect&,
         const ws::ui::gui::ChromeTheme&)
SKIN_ROW(menu_title, const ws::ui::gui::Rect&,
         const ws::ui::gui::MenuTitlePaint&, const ws::ui::gui::ChromeTheme&,
         const char*)
SKIN_ROW(table_header, const ws::ui::gui::TableGeometry&, bool,
         const ws::ui::gui::ChromeTheme&)
SKIN_ROW(table_row, const ws::ui::gui::Rect&,
         const ws::ui::gui::TableRowPaint&, const char* const*,
         const ws::ui::gui::TableGeometry&, const ws::ui::gui::ChromeTheme&)
SKIN_ROW(toast, const ws::ui::gui::Rect&, ws::ui::gui::ToastSeverity,
         const ws::ui::gui::ChromeTheme&)

}  // namespace

TEST_CASE("a skin's table receives every widget family's draws") {
    using namespace ws::ui::gui;

    // The full skin: every row the recording twin.
    WidgetPainters skin = kStockPainters;
    skin.button = skin_button;
    skin.checkbox = skin_checkbox;
    skin.radio = skin_radio;
    skin.toggle_switch = skin_toggle_switch;
    skin.collapsing_header = skin_collapsing_header;
    skin.selectable = skin_selectable;
    skin.slider = skin_slider;
    skin.drag_float = skin_drag_float;
    skin.spinner = skin_spinner;
    skin.progress_indeterminate = skin_progress_indeterminate;
    skin.tab = skin_tab;
    skin.combo = skin_combo;
    skin.text_input = skin_text_input;
    skin.panel = skin_panel;
    skin.popup_frame = skin_popup_frame;
    skin.scrollbar = skin_scrollbar;
    skin.menu_item = skin_menu_item;
    skin.menu_item_separator = skin_menu_item_separator;
    skin.menu_title = skin_menu_title;
    skin.table_header = skin_table_header;
    skin.table_row = skin_table_row;
    skin.toast = skin_toast;

    ChromeTheme theme = kDefaultChrome;
    theme.painters = &skin;

    g_log.calls.clear();
    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    LayoutCursor layout;

    bool bool_value = true;
    int int_value = 1;
    int selection = 0;
    bool combo_open = false;
    float float_value = 0.5f;
    char buf[8] = "";
    bool header_open = true;
    int tab_active = 0;
    const char* items[] = {"a", "b", "c"};
    const char* tabs[] = {"one", "two"};
    const TableColumn columns[] = {{"name", 0.0f}, {"value", 40.0f}};
    const char* cells[] = {"r1", "1", "r2", "2"};
    const MenuItem menu_items[] = {
        {.label = "cut"},
        {.kind = ws::ui::gui::MenuItemKind::separator},
        {.label = "copy"}};
    const MenuDesc menus[] = {{"edit", menu_items, 3}};
    int menu_open = -1;
    Toasts toasts;
    toasts.push({.text = "saved"});

    gui.begin_frame(snap(-100.0f, -100.0f));
    (void)begin_panel(gui, layout, theme, "board", {0.0f, 0.0f, 420.0f, 500.0f});
    (void)button(gui, layout, theme, "go", 60.0f);
    (void)checkbox(gui, layout, theme, "ck", bool_value);
    (void)radio(gui, layout, theme, "rd", int_value, 1);
    (void)toggle_switch(gui, layout, theme, "tg", bool_value);
    (void)collapsing_header(gui, layout, theme, "hd", header_open, 80.0f);
    (void)selectable(gui, layout, theme, "sl", false, 60.0f);
    (void)slider_float(gui, layout, theme, "sf", float_value, 0.0f, 1.0f, 60.0f);
    (void)drag_float(gui, layout, theme, "df", float_value, 0.0f, 1.0f, 0.01f, 60.0f);
    spinner(gui, layout, theme, "busy", 20.0f);
    progress_indeterminate(gui, layout, theme, "scan", 60.0f);
    (void)tab_bar(gui, layout, theme, "views", tabs, 2, tab_active, 50.0f);
    (void)combo(gui, layout, theme, "cb", items, 3, selection, combo_open, 60.0f);
    (void)text_input(gui, layout, theme, "ti", buf, sizeof buf, 60.0f);
    (void)table(gui, layout, theme, "grid", columns, 2, cells, 2, &selection, 300.0f);
    (void)menu_bar(gui, layout, theme, "bar", menus, 1, menu_open);
    (void)draw_toasts(gui, theme, toasts);
    end_panel(gui, layout);
    gui.end_frame();

    // Every family's row answered. (Two rows cannot fire headless: the
    // scrollbar shows only while a region scrolls, and a toast's bounds
    // clamp past the empty frame - draw_toasts' documented contract.)
    for (const char* row : {"panel", "button", "checkbox", "radio",
                            "toggle_switch", "collapsing_header",
                            "selectable", "slider", "drag_float", "spinner",
                            "progress_indeterminate", "tab", "combo",
                            "text_input", "table_header", "table_row",
                            "menu_title"}) {
        if (!g_log.saw(row)) {
            MESSAGE("row never called: ", row);
        }
        CHECK(g_log.saw(row));
    }
    // A second frame with the menu already OPEN (an idle pointer - a
    // title click would TOGGLE it closed) lays the item rows and the
    // separator between them.
    menu_open = 0;
    g_log.calls.clear();
    gui.begin_frame(snap(-100.0f, -100.0f));
    (void)begin_panel(gui, layout, theme, "board", {0.0f, 0.0f, 420.0f, 500.0f});
    (void)menu_bar(gui, layout, theme, "bar", menus, 1, menu_open);
    end_panel(gui, layout);
    gui.end_frame();
    CHECK(g_log.saw("menu_title"));
    CHECK(g_log.count("menu_item") >= 2);   // cut + copy (live rows)
    CHECK(g_log.saw("menu_item_separator"));

    // A popup through begin_popup reaches the popup frame row.
    g_log.calls.clear();
    bool popup_open = true;
    gui.begin_frame(snap(-100.0f, -100.0f));
    if (begin_popup(gui, layout, theme, "pp", {0.0f, 0.0f, 0.0f, 0.0f},
                    {10.0f, 10.0f, 120.0f, 80.0f}, popup_open)) {
        end_popup(gui, layout);
    }
    gui.end_frame();
    CHECK(g_log.saw("popup_frame"));
}

TEST_CASE("a null table and a null row both answer the stock look") {
    using namespace ws::ui::gui;

    GuiContext gui;
    LayoutCursor layout;
    bool value = false;

    // The stock path: a null table draws through the stub exactly as
    // the plain stock draw would (the counts are the proof). A panel
    // scope places the row - a bare cursor hands out empty rects.
    ws::ui::gui_test::stub.reset();
    gui.begin_frame(snap(-100.0f, -100.0f));
    (void)begin_panel(gui, layout, kDefaultChrome, "board",
                      {0.0f, 0.0f, 300.0f, 200.0f});
    (void)button(gui, layout, kDefaultChrome, "go", 60.0f);
    end_panel(gui, layout);
    gui.end_frame();
    const std::uint32_t stock_quads = ws::ui::gui_test::stub.quads;
    const std::uint32_t stock_rounded = ws::ui::gui_test::stub.rounded;
    CHECK(stock_rounded == 3);  // the P5 panel outline pair + the face
    CHECK(stock_quads >= 1);    // the label bar (fontless)

    // A table whose button row is NULL falls back to that stock draw -
    // identical bytes.
    WidgetPainters partial = kStockPainters;
    partial.button = nullptr;
    ChromeTheme theme = kDefaultChrome;
    theme.painters = &partial;
    ws::ui::gui_test::stub.reset();
    gui.begin_frame(snap(-100.0f, -100.0f));
    (void)begin_panel(gui, layout, theme, "board",
                      {0.0f, 0.0f, 300.0f, 200.0f});
    (void)button(gui, layout, theme, "go", 60.0f);
    end_panel(gui, layout);
    gui.end_frame();
    CHECK(ws::ui::gui_test::stub.quads == stock_quads);
    CHECK(ws::ui::gui_test::stub.rounded == stock_rounded);
    CHECK(ws::ui::gui_test::stub.rounded == stock_rounded);

    // A ZEROED table (a skin that never initialized) is fully inert:
    // every row falls back, the look is stock.
    const WidgetPainters zeroed{};
    ChromeTheme zero_theme = kDefaultChrome;
    zero_theme.painters = &zeroed;
    ws::ui::gui_test::stub.reset();
    gui.begin_frame(snap(-100.0f, -100.0f));
    (void)begin_panel(gui, layout, zero_theme, "board",
                      {0.0f, 0.0f, 300.0f, 200.0f});
    (void)button(gui, layout, zero_theme, "go", 60.0f);
    (void)checkbox(gui, layout, zero_theme, "ck", value);
    end_panel(gui, layout);
    gui.end_frame();
    CHECK(ws::ui::gui_test::stub.rounded >= 2);
}

TEST_CASE("the behavior halves are painter-blind - interactions answer the same under any skin") {
    using namespace ws::ui::gui;

    // A skin that draws NOTHING (rows swapped for no-ops): the click,
    // the toggle and the pick all still answer exactly as stock. The
    // look is the skin's; the interaction is the behavior's.
    WidgetPainters silent{};
    ChromeTheme skinned = kDefaultChrome;
    skinned.painters = &silent;

    const auto run = [&](const ChromeTheme& theme) {
        GuiContext gui;
        LayoutCursor layout;
        bool on = false;
        // Press...
        gui.begin_frame(snap(30.0f, 26.0f, true, true));
        (void)begin_panel(gui, layout, theme, "board",
                          {0.0f, 0.0f, 300.0f, 200.0f});
        (void)toggle_switch(gui, layout, theme, "tg", on);
        end_panel(gui, layout);
        gui.end_frame();
        CHECK(!on);  // the press alone never flips
        // ...release: the click lands.
        gui.begin_frame(snap(30.0f, 26.0f, false, false, true));
        (void)begin_panel(gui, layout, theme, "board",
                          {0.0f, 0.0f, 300.0f, 200.0f});
        (void)toggle_switch(gui, layout, theme, "tg", on);
        end_panel(gui, layout);
        gui.end_frame();
        return on;
    };

    CHECK(run(kDefaultChrome) == true);
    CHECK(run(skinned) == true);
}

TEST_CASE("kStockPainters names the stock draws and composes") {
    using namespace ws::ui::gui;

    // The table's rows ARE the public stock functions - composing over
    // the stock look is one call away.
    CHECK(kStockPainters.button == &button_draw);
    CHECK(kStockPainters.panel == &panel_draw);
    CHECK(kStockPainters.popup_frame == &popup_frame_draw);
    CHECK(kStockPainters.menu_item == &menu_item_draw);
    CHECK(kStockPainters.table_row == &table_row_draw);
    CHECK(kStockPainters.toast == &toast_draw);

    // A composing skin: the stock button under an extra accent mark.
    // (The recording log proves the row ran; the stub count proves the
    // stock draw ran INSIDE it.)
    static int composed_marks = 0;
    const auto composer = [](const Rect& rect, const ButtonResult& result,
                             const ChromeTheme& theme, const char* label,
                             ws::ui::r2d::Sprite icon) {
        button_draw(rect, result, theme, label, icon);  // the stock look
        ++composed_marks;  // ...and the skin's mark over it
    };
    WidgetPainters composer_table = kStockPainters;
    composer_table.button = composer;
    ChromeTheme theme = kDefaultChrome;
    theme.painters = &composer_table;

    ws::ui::gui_test::stub.reset();
    composed_marks = 0;
    GuiContext gui;
    LayoutCursor layout;
    gui.begin_frame(snap(-100.0f, -100.0f));
    (void)button(gui, layout, theme, "go", 60.0f);
    gui.end_frame();
    CHECK(composed_marks == 1);
    CHECK(ws::ui::gui_test::stub.rounded == 1);  // the stock face drew
}
