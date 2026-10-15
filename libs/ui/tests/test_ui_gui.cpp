// The stage-2 acceptance test, in executable form: the gui library and
// its tests build and run with NO Vulkan, GLFW, volk or vma anywhere in
// the link. The target's link line IS the contract (see the
// ws_add_test(ui_gui ...) in CMakeLists.txt): ws::ui_gui (the gui
// archive), the real pure-CPU font bake (src/text/font.cpp + its stb
// TU) and a stub for the r2d free-function seam the gui draws through -
// nothing else. The cases below drive the real gui headless, so "runs"
// means more than "linked": widgets lay out, hit-test and draw through
// the stubbed seam, fit_text measures through the real font code, the
// layer scopes and the InputFrame touch logic run - and if the gui ever
// grows a renderer- or platform-side dependency, the link fails loudly.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/ui/input.hpp>
#include <ws/ui/renderer2d.hpp>

#include "ui_gui_stub.hpp"

#include <array>
#include <cstring>

namespace {

using ws::ui::gui::GuiContext;
using ws::ui::gui::InputFrame;
using ws::ui::gui::LayoutCursor;
using ws::ui::gui::kDefaultChrome;

// A pointer snapshot at (x, y) with the primary button's held state and
// optional press/release edges set - the same shape test_ui's snap()
// models: the mouse is PRESENT because the platform said so.
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

}  // namespace

TEST_CASE("widgets lay out, hit-test and draw through the stubbed r2d seam") {
    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    LayoutCursor layout;

    gui.begin_frame(snap(60.0f, 20.0f));  // hover inside the button's row
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                  {0.0f, 0.0f, 400.0f, 300.0f});
    CHECK(!ws::ui::gui::button(gui, layout, kDefaultChrome, "fire"));
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();

    // The button really drew: its chrome went through the r2d seam the
    // stub records, and the themed panel scope pushed its content clip
    // and closed it as balanced as it opened. A hover is not a click -
    // button() stays false.
    CHECK(ws::ui::gui_test::stub.quads > 0);
    CHECK(ws::ui::gui_test::stub.clip_pushes > 0);
    CHECK(ws::ui::gui_test::stub.clip_pushes ==
          ws::ui::gui_test::stub.clip_pops);
}

TEST_CASE("a full click fires exactly once through the seam") {
    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    LayoutCursor layout;

    gui.begin_frame(snap(60.0f, 20.0f, true, true));  // press lands
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                  {0.0f, 0.0f, 400.0f, 300.0f});
    CHECK(!ws::ui::gui::button(gui, layout, kDefaultChrome, "fire"));
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();

    gui.begin_frame(snap(60.0f, 20.0f, false, false, true));  // release
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                  {0.0f, 0.0f, 400.0f, 300.0f});
    CHECK(ws::ui::gui::button(gui, layout, kDefaultChrome, "fire"));
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();

    gui.begin_frame(snap(60.0f, 20.0f));  // held-off idle: no repeat
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                  {0.0f, 0.0f, 400.0f, 300.0f});
    CHECK(!ws::ui::gui::button(gui, layout, kDefaultChrome, "fire"));
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
}

TEST_CASE("fit_text truncates through the real font bake") {
    using ws::ui::r2d::Font;
    // 95 glyphs (ASCII 32..126, in order), every one advancing 10 px - a
    // deterministic font built through the public fill ctor, with the
    // real font.cpp behind it.
    std::array<Font::Glyph, 95> glyphs{};
    for (Font::Glyph& glyph : glyphs) {
        glyph.advance = 10.0f;
    }
    const Font font(16.0f, 20.0f, glyphs.data(), 95);
    REQUIRE(!font.empty());
    CHECK(font.measure_text("abcdef") == 60.0f);

    ws::ui::gui::ChromeTheme theme = kDefaultChrome;
    theme.font = &font;
    char scratch[64];
    const auto fit = ws::ui::gui::fit_text("abcdef", 45.0f, theme, scratch,
                                           sizeof scratch);
    // "abcdef" measures 60 > 45, so the longest prefix + "..." that fits
    // wins: "a..." (10 + 3 dots = 40 <= 45; "ab..." is 50 > 45).
    CHECK(std::strcmp(fit.text, "a...") == 0);
    CHECK(fit.advance == 40.0f);

    // The fontless theme sizes by the per-byte estimate (6 px/char), so
    // all six characters fit whole at 36 <= 45 - the exact pre-font look.
    const auto whole = ws::ui::gui::fit_text("abcdef", 45.0f, kDefaultChrome,
                                             scratch, sizeof scratch);
    CHECK(std::strcmp(whole.text, "abcdef") == 0);
    CHECK(whole.advance == 36.0f);
}

TEST_CASE("layer scopes drive r2d::set_layer through the seam") {
    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    LayoutCursor layout;

    gui.begin_frame(snap(-100.0f, -100.0f));  // present, over nothing
    gui.begin_layer(ws::ui::gui::Layer::popup);
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "pop",
                                  {0.0f, 0.0f, 120.0f, 60.0f});
    (void)ws::ui::gui::button(gui, layout, kDefaultChrome, "x");
    ws::ui::gui::end_panel(gui, layout);
    gui.end_layer();
    gui.end_frame();

    // The scope drew at the popup stratum and the close restored the
    // enclosing base - the r2d half of begin_layer/end_layer, proven
    // through the recording stub.
    CHECK(ws::ui::gui_test::stub.layer_sets >= 2);
    CHECK(ws::ui::gui_test::stub.max_layer == 20u);  // Layer::popup
    CHECK(ws::ui::gui_test::stub.last_layer == 10u);  // Layer::base
}

TEST_CASE("the InputFrame touch bridge runs without the platform layer") {
    using ws::ui::gui::PointerId;
    using ws::ui::gui::PointerView;

    InputFrame frame;
    const PointerId finger{7u};
    CHECK(frame.touch_down(finger, 40.0f, 30.0f) == 0);

    // The first contact of the gesture bridges to the mouse: position
    // and button 0 mirror with their edges, while mouse_present stays
    // the platform's own call (still false here).
    CHECK(frame.bridged_touch() == finger);
    CHECK(frame.mouse_x == 40.0f);
    CHECK(frame.mouse_y == 30.0f);
    CHECK(frame.mouse_down[0]);
    CHECK(frame.mouse_pressed[0]);
    CHECK(!frame.mouse_present);

    std::array<PointerView, InputFrame::kMaxPointers> pointers{};
    const int count = frame.gather_pointers(pointers);
    CHECK(count == 1);  // no mouse listed: the bridge never fabricates one
    CHECK(pointers[0].id == finger);
    CHECK(pointers[0].x == 40.0f);
    CHECK(pointers[0].pressed);
}
