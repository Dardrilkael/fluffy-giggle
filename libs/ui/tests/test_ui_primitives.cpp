// The P5 primitives acceptance (the ui-skin plan): the r2d layered pair
// - draw_rounded_outline (exactly 2 quads) and draw_shadow (the
// documented 4-layer stack, gated on the theme's tokens) - plus the
// stock adoption: panels and popups wear the outline at their role
// radii, a themed shadow rides every overlay window, the focus ring
// honors focus_ring_width, and the toggle track / scrollbar thumb read
// the pill role. The stub's counters are the proof: an outline records
// itself AND its quads, a shadow itself and its four layers.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/ui/renderer2d.hpp>

// The internal helpers under test (shadow_bounds, ring_draw) - src/ is
// on this target's include path, the test_ui.cpp precedent for internals.
#include "gui/widgets_detail.hpp"

#include "ui_gui_stub.hpp"

#include <cstdint>

namespace {

using ws::ui::gui::ChromeTheme;
using ws::ui::gui::GuiContext;
using ws::ui::gui::kDefaultChrome;
using ws::ui::gui::LayoutCursor;
using ws::ui::gui::Rect;

ws::input::InputFrame idle() {
    ws::input::InputFrame frame;
    frame.mouse_x = -100.0f;
    frame.mouse_y = -100.0f;
    frame.mouse_present = true;
    return frame;
}

}  // namespace

// --- the primitives themselves ------------------------------------------------

TEST_CASE("draw_rounded_outline records exactly two quads") {
    using namespace ws::ui::gui_test;
    stub.reset();
    ws::ui::r2d::draw_rounded_outline(10.0f, 10.0f, 100.0f, 40.0f, 5.0f, 1.0f,
                                      0x111111FFu, 0x222222FFu);
    CHECK(stub.outlines == 1);
    CHECK(stub.rounded == 2);  // outer color + inner face, the pair
}

TEST_CASE("draw_rounded_outline degenerate gates") {
    using namespace ws::ui::gui_test;
    stub.reset();
    // No stroke, no draw.
    ws::ui::r2d::draw_rounded_outline(10.0f, 10.0f, 100.0f, 40.0f, 5.0f, 0.0f,
                                      0x111111FFu, 0x222222FFu);
    CHECK(stub.outlines == 0);
    CHECK(stub.rounded == 0);
    // A stroke that eats the whole rect: the outer quad alone (the ring
    // IS the face), still an honest single record.
    stub.reset();
    ws::ui::r2d::draw_rounded_outline(10.0f, 10.0f, 8.0f, 8.0f, 4.0f, 4.0f,
                                      0x111111FFu, 0x222222FFu);
    CHECK(stub.outlines == 1);
    CHECK(stub.rounded == 1);
    // Empty rect: nothing.
    stub.reset();
    ws::ui::r2d::draw_rounded_outline(10.0f, 10.0f, 0.0f, 40.0f, 5.0f, 1.0f,
                                      0x111111FFu, 0x222222FFu);
    CHECK(stub.outlines == 0);
    CHECK(stub.rounded == 0);
}

TEST_CASE("draw_shadow records the documented four layers") {
    using namespace ws::ui::gui_test;
    stub.reset();
    ws::ui::r2d::draw_shadow(10.0f, 10.0f, 100.0f, 40.0f, 5.0f, 12.0f,
                             0x0000006Eu);
    CHECK(stub.shadows == 1);
    CHECK(stub.rounded == 4);  // the documented stack
}

TEST_CASE("draw_shadow gates on blur and alpha") {
    using namespace ws::ui::gui_test;
    stub.reset();
    ws::ui::r2d::draw_shadow(10.0f, 10.0f, 100.0f, 40.0f, 5.0f, 0.0f,
                             0x0000006Eu);
    CHECK(stub.shadows == 0);
    CHECK(stub.rounded == 0);
    // A fully transparent shadow color draws nothing too.
    ws::ui::r2d::draw_shadow(10.0f, 10.0f, 100.0f, 40.0f, 5.0f, 12.0f,
                             0x00000000u);
    CHECK(stub.shadows == 0);
    CHECK(stub.rounded == 0);
}

// --- the stock adoption --------------------------------------------------------

TEST_CASE("panel chrome draws the rounded outline") {
    using namespace ws::ui::gui;
    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    LayoutCursor layout;
    ChromeTheme theme = kDefaultChrome;
    gui.begin_frame(idle());
    (void)begin_panel(gui, layout, theme, "board", {0.0f, 0.0f, 200.0f, 100.0f});
    end_panel(gui, layout);
    gui.end_frame();
    // The chrome is one outline (2 quads) at the panel radius; no
    // straight quads remain from the old pair.
    CHECK(ws::ui::gui_test::stub.outlines == 1);
    CHECK(ws::ui::gui_test::stub.quads == 0);
}

TEST_CASE("popup chrome carries the shadow exactly when the theme says so") {
    using namespace ws::ui::gui;
    // The stock default is still shadowless (blur 0): the popup frame is
    // outline-only.
    {
        ws::ui::gui_test::stub.reset();
        GuiContext gui;
        LayoutCursor layout;
        ChromeTheme theme = kDefaultChrome;
        bool open = true;
        gui.begin_frame(idle());
        if (begin_popup(gui, layout, theme, "pp",
                        {0.0f, 0.0f, 10.0f, 10.0f},
                        {0.0f, 0.0f, 80.0f, 60.0f}, open)) {
            end_popup(gui, layout);
        }
        gui.end_frame();
        CHECK(ws::ui::gui_test::stub.shadows == 0);
        CHECK(ws::ui::gui_test::stub.outlines == 1);
    }
    // A theme with shadow tokens draws one 4-layer shadow per open popup.
    {
        ws::ui::gui_test::stub.reset();
        GuiContext gui;
        LayoutCursor layout;
        ChromeTheme theme = kDefaultChrome;
        theme.shadow_blur = 12.0f;
        theme.shadow_offset_y = 3.0f;
        bool open = true;
        gui.begin_frame(idle());
        if (begin_popup(gui, layout, theme, "pp",
                        {0.0f, 0.0f, 10.0f, 10.0f},
                        {0.0f, 0.0f, 80.0f, 60.0f}, open)) {
            end_popup(gui, layout);
        }
        gui.end_frame();
        CHECK(ws::ui::gui_test::stub.shadows == 1);
        CHECK(ws::ui::gui_test::stub.outlines == 1);
    }
}

TEST_CASE("popup_frame_draw answers both the shadow and the outline alone") {
    using namespace ws::ui::gui;
    ws::ui::gui_test::stub.reset();
    ChromeTheme theme = kDefaultChrome;
    theme.shadow_blur = 10.0f;
    popup_frame_draw({0.0f, 0.0f, 60.0f, 30.0f}, theme);
    CHECK(ws::ui::gui_test::stub.shadows == 1);
    CHECK(ws::ui::gui_test::stub.outlines == 1);
    // The look seam: blur 0 - a skin's flat spelling - loses only the
    // shadow, never the frame.
    ws::ui::gui_test::stub.reset();
    theme.shadow_blur = 0.0f;
    popup_frame_draw({0.0f, 0.0f, 60.0f, 30.0f}, theme);
    CHECK(ws::ui::gui_test::stub.shadows == 0);
    CHECK(ws::ui::gui_test::stub.outlines == 1);
}

TEST_CASE("shadow_bounds grows by the blur and the drop") {
    using namespace ws::ui::gui;
    const Rect bounds{10.0f, 10.0f, 100.0f, 50.0f};
    ChromeTheme theme = kDefaultChrome;
    theme.shadow_blur = 0.0f;
    const Rect flat = shadow_bounds(bounds, theme);
    CHECK(flat.x == bounds.x);
    CHECK(flat.width == bounds.width);
    theme.shadow_blur = 8.0f;
    theme.shadow_offset_y = 2.0f;
    const Rect grown = shadow_bounds(bounds, theme);
    CHECK(grown.x == doctest::Approx(bounds.x - 8.0f));
    CHECK(grown.y == doctest::Approx(bounds.y - 8.0f));
    CHECK(grown.width == doctest::Approx(bounds.width + 16.0f));
    CHECK(grown.height == doctest::Approx(bounds.height + 16.0f + 2.0f));
}

TEST_CASE("the toggle track and scrollbar thumb read the pill role") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;
    ChromeTheme theme = kDefaultChrome;
    bool on = true;

    // The stock: the track (the toggle draw's FIRST quad - its knob
    // paints after) is the full pill, half its height.
    ws::ui::gui_test::stub.reset();
    gui.begin_frame(idle());
    (void)toggle_switch(gui, layout, theme, "tg", on);
    gui.end_frame();
    CHECK(ws::ui::gui_test::stub.first_rounded_radius ==
          doctest::Approx(theme.control_height * 0.5f));

    // A skin's radius_pill overrides - 0 means the square look.
    theme.radius_pill = 0.0f;
    ws::ui::gui_test::stub.reset();
    gui.begin_frame(idle());
    (void)toggle_switch(gui, layout, theme, "tg", on);
    gui.end_frame();
    CHECK(ws::ui::gui_test::stub.first_rounded_radius ==
          doctest::Approx(0.0f));

    // The scrollbar thumb, driven standalone: a themed pill radius
    // rides, and the stock corner radius degrades to the same full pill
    // (draw_rounded_quad clamps to the 4 px thumb's half-width).
    const ScrollbarThumb thumb{.shows = true, .top = 10.0f, .height = 40.0f};
    const Rect viewport{0.0f, 0.0f, 200.0f, 100.0f};
    theme.radius_pill = 2.0f;
    ws::ui::gui_test::stub.reset();
    scrollbar_draw(viewport, thumb, theme);
    CHECK(ws::ui::gui_test::stub.last_rounded_radius ==
          doctest::Approx(2.0f));
    theme.radius_pill = -1.0f;
    ws::ui::gui_test::stub.reset();
    scrollbar_draw(viewport, thumb, theme);
    CHECK(ws::ui::gui_test::stub.last_rounded_radius ==
          doctest::Approx(theme.corner_radius));
}

TEST_CASE("the focus ring rides focus_ring_width without changing its quad count") {
    using namespace ws::ui::gui;
    ChromeTheme theme = kDefaultChrome;
    ws::ui::gui_test::stub.reset();
    ring_draw(true, {10.0f, 10.0f, 60.0f, 24.0f}, theme);
    const std::uint32_t stock_ring = ws::ui::gui_test::stub.rounded;
    CHECK(stock_ring == 1);
    theme.focus_ring_width = 3.0f;
    ws::ui::gui_test::stub.reset();
    ring_draw(true, {10.0f, 10.0f, 60.0f, 24.0f}, theme);
    CHECK(ws::ui::gui_test::stub.rounded == stock_ring);
    // Unfocused: no ring at either width.
    ws::ui::gui_test::stub.reset();
    ring_draw(false, {10.0f, 10.0f, 60.0f, 24.0f}, theme);
    CHECK(ws::ui::gui_test::stub.rounded == 0);
}

TEST_CASE("a tooltip with a themed shadow still fades in through the escape") {
    using namespace ws::ui::gui;
    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    LayoutCursor layout;
    ChromeTheme theme = kDefaultChrome;
    theme.shadow_blur = 8.0f;
    // begin_tooltip driven directly with explicit bounds - tooltip_text's
    // placement clamps to the headless frame's zero size, and the bounds
    // are the point here, not the placement.
    gui.begin_frame(idle());
    if (begin_tooltip(gui, layout, theme, "tt", {5.0f, 5.0f, 80.0f, 40.0f},
                      true)) {
        text(layout, theme, "the hint", -1.0f);
        end_tooltip(gui, layout);
    }
    gui.end_frame();
    CHECK(ws::ui::gui_test::stub.shadows == 1);
    CHECK(ws::ui::gui_test::stub.outlines == 1);
    // The flat theme: the shadow goes, the frame stays.
    ws::ui::gui_test::stub.reset();
    theme.shadow_blur = 0.0f;
    gui.begin_frame(idle());
    if (begin_tooltip(gui, layout, theme, "tt2", {5.0f, 5.0f, 80.0f, 40.0f},
                      true)) {
        text(layout, theme, "the hint", -1.0f);
        end_tooltip(gui, layout);
    }
    gui.end_frame();
    CHECK(ws::ui::gui_test::stub.shadows == 0);
    CHECK(ws::ui::gui_test::stub.outlines == 1);
}
