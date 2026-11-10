// The table virtualization suite: the virtualized walk, end to end -
// the visible window over the hit clip (the seam the scroll panels
// already push, read through current_hit_clip), the sentinel rows at
// each end, the chrome path culling identically, and the honest
// limits the contract spells (Tab wraps within the window; a focused
// row outside it pauses its arrow stepping). The scroll-panel case
// drives the REAL panel with a hand-set offset - the reserved
// ScrollState key, spelled the way the panel itself re-derives it -
// so the culling proves out against the genuine scrolled geometry,
// the way test_ui_key_repeat drives its frames. The draws are
// asserted through the r2d stub's counters (texts per cell, the
// deterministic font), the same purity class as test_ui_gui: the gui
// archive, the real font bake, the stub seam; no renderer, no window,
// no platform TU.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/input/input.hpp>

#include "ui_gui_stub.hpp"

#include <array>
#include <string>
#include <vector>

namespace {

using ws::ui::gui::GuiContext;
using ws::input::InputFrame;
using ws::ui::gui::LayoutCursor;
using ws::ui::gui::kDefaultChrome;
namespace keys = ws::input::keys;

InputFrame snap(float x = -100.0f, float y = -100.0f, bool down = false,
                bool pressed = false, bool released = false) {
    InputFrame frame;
    frame.mouse_x = x;
    frame.mouse_y = y;
    frame.mouse_present = true;
    frame.mouse_down[0] = down;
    frame.mouse_pressed[0] = pressed;
    frame.mouse_released[0] = released;
    return frame;
}

// The deterministic font the gui suite builds: 95 ASCII glyphs at a
// flat 10 px advance - every non-null cell draws exactly one text.
std::array<ws::ui::r2d::Font::Glyph, 95> font_glyphs() {
    std::array<ws::ui::r2d::Font::Glyph, 95> glyphs{};
    for (ws::ui::r2d::Font::Glyph& glyph : glyphs) {
        glyph.advance = 10.0f;
    }
    return glyphs;
}

// A table's backing store: N rows of two cells, "rI" and "vI", the
// pointers stable for the suite's lifetime.
struct Grid {
    std::vector<std::string> storage;
    std::vector<const char*> cells;
    int rows = 0;

    explicit Grid(int n) : rows(n) {
        storage.reserve(static_cast<std::size_t>(n) * 2u);
        cells.reserve(static_cast<std::size_t>(n) * 2u);
        for (int i = 0; i < n; ++i) {
            storage.push_back("r" + std::to_string(i));
            storage.push_back("v" + std::to_string(i));
            cells.push_back(storage[static_cast<std::size_t>(i) * 2u].c_str());
            cells.push_back(
                storage[static_cast<std::size_t>(i) * 2u + 1u].c_str());
        }
    }
};

const ws::ui::gui::TableColumn kCols[2] = {{"name", 100.0f}, {"value", 0.0f}};

// The row id, spelled the way the walk derives it.
ws::ui::gui::WidgetId row_id(GuiContext& gui, int index) {
    gui.ids().push("grid");
    gui.ids().push(index);
    const ws::ui::gui::WidgetId id = gui.ids().id("row");
    gui.ids().pop();
    gui.ids().pop();
    return id;
}

}  // namespace

TEST_CASE("a bare table, no clip pushed, walks every row") {
    auto glyphs = font_glyphs();
    const ws::ui::r2d::Font font(16.0f, 20.0f, glyphs.data(), 95);
    ws::ui::gui::ChromeTheme theme = kDefaultChrome;
    theme.font = &font;

    GuiContext gui;
    LayoutCursor layout;
    const Grid grid(50);
    int selected = -1;

    ws::ui::gui_test::stub.reset();
    gui.begin_frame(snap());
    (void)layout.begin_panel({0.0f, 0.0f, 300.0f, 2000.0f}, 0.0f);
    (void)ws::ui::gui::table(gui, layout, theme, "grid", kCols, 2,
                             grid.cells.data(), grid.rows, &selected, 300.0f);
    layout.end_panel();
    gui.end_frame();

    // The header's two labels plus two cells per row - all fifty.
    CHECK(ws::ui::gui_test::stub.texts == 2 + 50 * 2);
}

TEST_CASE("a clipped table walks only the visible window plus the "
          "sentinels - the interactive path") {
    auto glyphs = font_glyphs();
    const ws::ui::r2d::Font font(16.0f, 20.0f, glyphs.data(), 95);
    ws::ui::gui::ChromeTheme theme = kDefaultChrome;
    theme.font = &font;

    GuiContext gui;
    LayoutCursor layout;
    const Grid grid(1000);
    int selected = -1;

    // The geometry: the table at (0,0) lays header 28 + rule 2, the
    // body from y=30, rows 28 px tall. A clip covering y 170..282
    // shows rows 5..8 exactly; the sentinels are 4 and 9.
    const auto run = [&](const ws::ui::gui::Rect& clip) {
        ws::ui::gui_test::stub.reset();
        gui.begin_frame(snap());
        (void)layout.begin_panel({0.0f, 0.0f, 300.0f, 30000.0f}, 0.0f);
        gui.push_hit_clip(clip);
        (void)ws::ui::gui::table(gui, layout, theme, "grid", kCols, 2,
                                 grid.cells.data(), grid.rows, &selected,
                                 300.0f);
        gui.pop_hit_clip();
        layout.end_panel();
        gui.end_frame();
    };

    run({0.0f, 170.0f, 300.0f, 112.0f});
    // Header (2) + six walked rows (4..9) x two cells.
    CHECK(ws::ui::gui_test::stub.texts == 2 + 6 * 2);

    // The same clip one band lower - rows 8..11 visible, 7..12 walked.
    run({0.0f, 254.0f, 300.0f, 112.0f});
    CHECK(ws::ui::gui_test::stub.texts == 2 + 6 * 2);

    // A clip that misses the body entirely: the header alone (the
    // header is chrome, never culled; the stub counts the calls the
    // real renderer's clip would have hidden anyway).
    run({0.0f, 30000.0f, 300.0f, 50.0f});
    CHECK(ws::ui::gui_test::stub.texts == 2);
}

TEST_CASE("the chrome path culls identically - a display-only table") {
    auto glyphs = font_glyphs();
    const ws::ui::r2d::Font font(16.0f, 20.0f, glyphs.data(), 95);
    ws::ui::gui::ChromeTheme theme = kDefaultChrome;
    theme.font = &font;

    GuiContext gui;
    LayoutCursor layout;
    const Grid grid(1000);

    ws::ui::gui_test::stub.reset();
    gui.begin_frame(snap());
    (void)layout.begin_panel({0.0f, 0.0f, 300.0f, 30000.0f}, 0.0f);
    gui.push_hit_clip({0.0f, 170.0f, 300.0f, 112.0f});
    (void)ws::ui::gui::table(gui, layout, theme, "grid", kCols, 2,
                             grid.cells.data(), grid.rows, nullptr, 300.0f);
    gui.pop_hit_clip();
    layout.end_panel();
    gui.end_frame();

    CHECK(ws::ui::gui_test::stub.texts == 2 + 6 * 2);
}

TEST_CASE("the sentinels register; the rows beyond them do not") {
    auto glyphs = font_glyphs();
    const ws::ui::r2d::Font font(16.0f, 20.0f, glyphs.data(), 95);
    ws::ui::gui::ChromeTheme theme = kDefaultChrome;
    theme.font = &font;

    GuiContext gui;
    LayoutCursor layout;
    const Grid grid(1000);
    int selected = -1;

    const auto run = [&]() {
        gui.begin_frame(snap());
        (void)layout.begin_panel({0.0f, 0.0f, 300.0f, 30000.0f}, 0.0f);
        gui.push_hit_clip({0.0f, 170.0f, 300.0f, 112.0f});
        (void)ws::ui::gui::table(gui, layout, theme, "grid", kCols, 2,
                                 grid.cells.data(), grid.rows, &selected,
                                 300.0f);
        gui.pop_hit_clip();
        layout.end_panel();
        gui.end_frame();
    };

    // A row's state() latches the focused bounds the frame it runs;
    // a culled row never runs, so the latch stays null. The sentinels
    // (4 and 9) run; the rows beyond them (3 and 10) do not.
    gui.set_focus(row_id(gui, 4));
    run();
    CHECK(gui.focused_bounds() != nullptr);
    gui.set_focus(row_id(gui, 9));
    run();
    CHECK(gui.focused_bounds() != nullptr);
    gui.set_focus(row_id(gui, 3));
    run();
    CHECK(gui.focused_bounds() == nullptr);
    gui.set_focus(row_id(gui, 10));
    run();
    CHECK(gui.focused_bounds() == nullptr);
}

TEST_CASE("a culled row cannot be picked; a visible one still can") {
    auto glyphs = font_glyphs();
    const ws::ui::r2d::Font font(16.0f, 20.0f, glyphs.data(), 95);
    ws::ui::gui::ChromeTheme theme = kDefaultChrome;
    theme.font = &font;

    GuiContext gui;
    LayoutCursor layout;
    const Grid grid(1000);
    int selected = -1;
    bool changed = false;

    const auto run = [&](const InputFrame& frame) {
        gui.begin_frame(frame);
        (void)layout.begin_panel({0.0f, 0.0f, 300.0f, 30000.0f}, 0.0f);
        gui.push_hit_clip({0.0f, 170.0f, 300.0f, 112.0f});
        changed = ws::ui::gui::table(gui, layout, theme, "grid", kCols, 2,
                                     grid.cells.data(), grid.rows, &selected,
                                     300.0f);
        gui.pop_hit_clip();
        layout.end_panel();
        gui.end_frame();
    };

    // Row 500's band (y 14030..14058) is inside the table, outside
    // the clip: the press claims nothing.
    run(snap(150.0f, 14040.0f, false, true, true));
    CHECK(selected == -1);
    CHECK(!changed);

    // Row 6's band (y 198..226) is inside the clip: the tap picks it.
    run(snap(150.0f, 212.0f, false, true, true));
    CHECK(selected == 6);
    CHECK(changed);
}

TEST_CASE("the arrow walk steps within the window and pauses past the "
          "sentinel - the contract's own limit") {
    auto glyphs = font_glyphs();
    const ws::ui::r2d::Font font(16.0f, 20.0f, glyphs.data(), 95);
    ws::ui::gui::ChromeTheme theme = kDefaultChrome;
    theme.font = &font;

    GuiContext gui;
    LayoutCursor layout;
    const Grid grid(1000);
    int selected = -1;

    const auto run = [&](const InputFrame& frame) {
        gui.begin_frame(frame);
        (void)layout.begin_panel({0.0f, 0.0f, 300.0f, 30000.0f}, 0.0f);
        gui.push_hit_clip({0.0f, 170.0f, 300.0f, 112.0f});
        (void)ws::ui::gui::table(gui, layout, theme, "grid", kCols, 2,
                                 grid.cells.data(), grid.rows, &selected,
                                 300.0f);
        gui.pop_hit_clip();
        layout.end_panel();
        gui.end_frame();
    };

    // Into the window, then a step inside it.
    gui.set_focus(row_id(gui, 6));
    run(snap());
    InputFrame down = snap();
    down.key_pressed[static_cast<std::size_t>(ws::input::keys::kDown)] = true;
    run(down);
    CHECK(gui.is_focused(row_id(gui, 7)));

    // A step off the bottom sentinel still FIRES - set_focus needs no
    // registration - and lands on a row the walk never touched. The
    // step's own frame still latches row 9 (its fold ran before the
    // gate moved the focus); the frame AFTER is the honest read:
    // nothing latches, because row 10 never walks (in a scroll panel
    // the into-view shift pulls the landing row back the same frame;
    // this fixed clip cannot move, so the arrows rest there until
    // something else moves the window).
    gui.set_focus(row_id(gui, 9));
    run(snap());
    run(down);
    CHECK(gui.is_focused(row_id(gui, 10)));
    run(snap());
    CHECK(gui.focused_bounds() == nullptr);
    run(down);
    CHECK(gui.is_focused(row_id(gui, 10)));  // paused: row 10 never walks
    // A step off the TOP sentinel lands off-window the same way.
    gui.set_focus(row_id(gui, 4));
    run(snap());
    InputFrame up = snap();
    up.key_pressed[static_cast<std::size_t>(ws::input::keys::kUp)] = true;
    run(up);
    CHECK(gui.is_focused(row_id(gui, 3)));
    run(snap());
    CHECK(gui.focused_bounds() == nullptr);
}

TEST_CASE("Tab wraps within the visible window") {
    auto glyphs = font_glyphs();
    const ws::ui::r2d::Font font(16.0f, 20.0f, glyphs.data(), 95);
    ws::ui::gui::ChromeTheme theme = kDefaultChrome;
    theme.font = &font;

    GuiContext gui;
    LayoutCursor layout;
    const Grid grid(1000);
    int selected = -1;

    const auto run = [&](const InputFrame& frame) {
        gui.begin_frame(frame);
        (void)layout.begin_panel({0.0f, 0.0f, 300.0f, 30000.0f}, 0.0f);
        gui.push_hit_clip({0.0f, 170.0f, 300.0f, 112.0f});
        (void)ws::ui::gui::table(gui, layout, theme, "grid", kCols, 2,
                                 grid.cells.data(), grid.rows, &selected,
                                 300.0f);
        gui.pop_hit_clip();
        layout.end_panel();
        gui.end_frame();
    };

    // The bottom sentinel is the LAST row that registered; Tab off it
    // wraps to the FIRST - row 4, the top sentinel - never to the
    // thousand rows the clip culled.
    gui.set_focus(row_id(gui, 9));
    run(snap());
    InputFrame tab = snap();
    tab.key_pressed[static_cast<std::size_t>(ws::input::keys::kTab)] = true;
    run(tab);
    CHECK(gui.is_focused(row_id(gui, 4)));
}

TEST_CASE("the scroll panel's own viewport drives the window - a real "
          "scrolled table") {
    auto glyphs = font_glyphs();
    const ws::ui::r2d::Font font(16.0f, 20.0f, glyphs.data(), 95);
    ws::ui::gui::ChromeTheme theme = kDefaultChrome;
    theme.font = &font;

    GuiContext gui;
    LayoutCursor layout;
    const Grid grid(100);
    int selected = -1;

    const auto run = [&]() {
        gui.begin_frame(snap());
        (void)ws::ui::gui::begin_scroll_panel(gui, layout, theme, "scroller",
                                              {0.0f, 0.0f, 300.0f, 200.0f});
        (void)ws::ui::gui::table(gui, layout, theme, "grid", kCols, 2,
                                 grid.cells.data(), grid.rows, &selected,
                                 -1.0f);
        ws::ui::gui::end_scroll_panel(gui, layout, theme);
        gui.end_frame();
    };

    // Frame one, offset 0: the viewport is the panel inset by the
    // padding ({12,12,276,176}); the body runs from y=42, so the
    // window shows rows 0..5 and the sentinels walk 0..6.
    ws::ui::gui_test::stub.reset();
    run();
    CHECK(ws::ui::gui_test::stub.texts == 2 + 7 * 2);

    // The scroll state, keyed the way the panel re-derives it (the
    // reserved internal id under its own scope), scrolled down five
    // rows: the window moves to 3..10, the sentinels walk 2..11.
    gui.ids().push("scroller");
    const ws::ui::gui::WidgetId scroll_id = gui.ids().internal_id("scroll");
    gui.ids().pop();
    gui.persistent_state<ws::ui::gui::ScrollState>(scroll_id).offset = 140.0f;
    ws::ui::gui_test::stub.reset();
    run();
    CHECK(ws::ui::gui_test::stub.texts == 2 + 10 * 2);

    // Scrolled to the very bottom: the last row must still walk (the
    // clamp holds the window inside the grid's ends).
    gui.persistent_state<ws::ui::gui::ScrollState>(scroll_id).offset = 100000.0f;
    ws::ui::gui_test::stub.reset();
    run();
    // offset clamps to max (content 2830 - viewport 176 = 2654): the
    // body's visible band is its final 176 px - rows 93..99 plus the
    // sentinel at 92.
    CHECK(ws::ui::gui_test::stub.texts == 2 + 8 * 2);
    const ws::ui::gui::ScrollState& scroll =
        gui.persistent_state<ws::ui::gui::ScrollState>(scroll_id);
    CHECK(scroll.offset == doctest::Approx(2654.0f));
}
