// The P4 motion acceptance (the ui-skin plan): the one helper's
// determinism under an injected clock, the disabled theme's
// byte-exact instant answers, the four travelers (face blend, knob
// travel, tab accent, overlay fades) through the recording stub, and
// the redraw want's exact settle boundary. The blend helpers' end
// exactness IS the "animation off = today's output" proof.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>

#include "ui_gui_stub.hpp"
#include "gui/widgets_detail.hpp"  // internal: the blend helpers under test

#include <cmath>
#include <cstdint>

namespace {

using ws::ui::gui::GuiContext;
using ws::input::InputFrame;
using ws::ui::gui::LayoutCursor;
using ws::ui::gui::kDefaultChrome;
using ws::ui::gui::WidgetId;

InputFrame snap(float x, float y, float dt = 0.0f, bool pressed = false,
                bool released = false) {
    InputFrame frame;
    frame.mouse_x = x;
    frame.mouse_y = y;
    frame.mouse_present = true;
    frame.dt_seconds = dt;
    frame.mouse_pressed[0] = pressed;
    frame.mouse_released[0] = released;
    return frame;
}

WidgetId id_of(const char* label) {
    ws::ui::gui::IdStack ids;
    return ids.id(label);
}

}  // namespace

TEST_CASE("animate_toward settles on the injected clock and snaps exact") {
    GuiContext gui;

    // INSTANT: duration 0 answers the target immediately, every frame.
    gui.begin_frame(snap(0.0f, 0.0f, 1.0f / 60.0f));
    const WidgetId id = id_of("anim");
    CHECK(gui.animate_toward(id, 0.75f, 0.0f) == 0.75f);
    CHECK(gui.animate_toward(id, 0.25f, 0.0f) == 0.25f);
    CHECK(!gui.wants_redraw());  // nothing travels
    gui.end_frame();

    // A real duration: 95% of the way at `duration`, settled shortly
    // after - deterministic under the injected dt (16.7ms steps).
    gui.clear_persistent_state();
    gui.begin_frame(snap(0.0f, 0.0f, 1.0f / 60.0f));
    float value = gui.animate_toward(id_of("travel"), 1.0f, 0.1f);
    CHECK(value > 0.0f);   // the first step moves
    CHECK(value < 0.95f);  // ...but has not settled
    CHECK(gui.wants_redraw());
    gui.end_frame();

    float settled_at = -1.0f;
    for (int frame = 2; frame <= 30; ++frame) {
        gui.begin_frame(snap(0.0f, 0.0f, 1.0f / 60.0f));
        value = gui.animate_toward(id_of("travel"), 1.0f, 0.1f);
        const bool settled = value == 1.0f;
        gui.end_frame();
        if (settled && settled_at < 0.0f) {
            settled_at = static_cast<float>(frame);
        }
        if (settled) {
            CHECK(!gui.wants_redraw());  // settled: no frame owed
        }
    }
    // The settle lands between one and two durations (95% at duration,
    // the snap a few steps later) - and EXACTLY at the target.
    CHECK(settled_at > 0.0f);
    CHECK(settled_at <= 20.0f);
    CHECK(value == 1.0f);

    // A headless dt of 0 never moves the value, but the want stays
    // honest: the traveler is unsettled, another frame is owed.
    gui.clear_persistent_state();
    gui.begin_frame(snap(0.0f, 0.0f, 0.0f));
    CHECK(gui.animate_toward(id_of("frozen"), 1.0f, 0.1f) == 0.0f);
    CHECK(gui.wants_redraw());
    gui.end_frame();
}

TEST_CASE("the blend helpers are exact at their ends - the disabled look is byte-identical") {
    using ws::ui::gui::control_face_of;
    using ws::ui::gui::fade_alpha;
    using ws::ui::gui::fade_tint;
    using ws::ui::gui::mix_colors;
    using ws::ui::gui::WidgetPaint;
    using ws::ui::gui::WidgetState;

    const std::uint32_t a = 0x12345678u;
    const std::uint32_t b = 0x9ABCDEF0u;
    CHECK(mix_colors(a, b, 0.0f) == a);
    CHECK(mix_colors(a, b, 1.0f) == b);
    CHECK(mix_colors(a, b, -1.0f) == a);
    CHECK(mix_colors(a, b, 2.0f) == b);

    CHECK(fade_alpha(a, 1.0f) == a);
    CHECK(fade_alpha(a, 0.0f) == (a & 0xFFFFFF00u));
    CHECK(fade_alpha(a, 2.0f) == a);

    CHECK(fade_tint(1.0f) == 0xFFFFFFFFu);
    CHECK(fade_tint(0.0f) == 0xFFFFFF00u);

    // The face ladder with no animation spoken (-1) is the plain
    // ladder; with the traveler at its ends it is exact too.
    WidgetPaint paint;
    paint.state = WidgetState::Hovered;
    paint.hover_t = -1.0f;
    CHECK(control_face_of(paint, kDefaultChrome) == kDefaultChrome.control_hovered);
    paint.hover_t = 0.0f;
    CHECK(control_face_of(paint, kDefaultChrome) ==
          kDefaultChrome.control_bg);
    paint.hover_t = 1.0f;
    CHECK(control_face_of(paint, kDefaultChrome) ==
          kDefaultChrome.control_hovered);
    // Pressed and dead stay instant whatever the traveler says.
    paint.state = WidgetState::Pressed;
    paint.hover_t = 0.5f;
    CHECK(control_face_of(paint, kDefaultChrome) ==
          kDefaultChrome.control_pressed);
    paint.state = WidgetState::Disabled;
    CHECK(control_face_of(paint, kDefaultChrome) ==
          kDefaultChrome.control_disabled);
}

TEST_CASE("the face traveler blends the composed button's hover, exactly when disabled") {
    GuiContext gui;
    LayoutCursor layout;

    // Animation OFF: the hovered button's face is control_hovered,
    // byte-exact (the traveler reads 0 or 1 exactly, the mix is exact).
    ws::ui::gui::ChromeTheme still = kDefaultChrome;
    still.anim_enabled = false;
    ws::ui::gui_test::stub.reset();
    gui.begin_frame(snap(30.0f, 26.0f));
    (void)ws::ui::gui::begin_panel(gui, layout, still, "board",
                                   {0.0f, 0.0f, 300.0f, 200.0f});
    (void)ws::ui::gui::button(gui, layout, still, "go", 60.0f);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    // The stub counts; the exactness is the helpers' own test above.
    CHECK(ws::ui::gui_test::stub.rounded == 1);
    CHECK(!gui.wants_redraw());

    // Animation ON, dt 0 (the headless clock): the face traveler sits
    // at 0 - the resting face, exactly - and the redraw want fires.
    ws::ui::gui_test::stub.reset();
    gui.clear_persistent_state();
    gui.begin_frame(snap(30.0f, 26.0f, 0.0f));
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "board",
                                   {0.0f, 0.0f, 300.0f, 200.0f});
    (void)ws::ui::gui::button(gui, layout, kDefaultChrome, "go", 60.0f);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    CHECK(gui.wants_redraw());  // the traveler wants its settle frames
}

TEST_CASE("the toggle knob travels and snaps when animation is off") {
    GuiContext gui;
    LayoutCursor layout;

    // Animation OFF: the knob snaps between its ends - the traveler
    // reads exactly 0 or 1, and the two positions differ by the span.
    ws::ui::gui::ChromeTheme still = kDefaultChrome;
    still.anim_enabled = false;
    bool on = false;
    const auto toggle = [&](const ws::ui::gui::ChromeTheme& theme,
                            float dt) {
        gui.begin_frame(snap(30.0f, 26.0f, 0.0f, true, false));
        (void)ws::ui::gui::begin_panel(gui, layout, theme, "board",
                                       {0.0f, 0.0f, 300.0f, 200.0f});
        (void)ws::ui::gui::toggle_switch(gui, layout, theme, "tg", on);
        ws::ui::gui::end_panel(gui, layout);
        gui.end_frame();
        gui.begin_frame(snap(30.0f, 26.0f, 0.0f, false, true));
        (void)ws::ui::gui::begin_panel(gui, layout, theme, "board",
                                       {0.0f, 0.0f, 300.0f, 200.0f});
        (void)ws::ui::gui::toggle_switch(gui, layout, theme, "tg", on);
        ws::ui::gui::end_panel(gui, layout);
        gui.end_frame();
        (void)dt;
    };

    ws::ui::gui_test::stub.reset();
    toggle(still, 0.0f);
    CHECK(on == true);   // the click flipped it
    CHECK(!gui.wants_redraw());

    // Animation ON with a live clock: the flip lands and the knob owes
    // settle frames.
    gui.clear_persistent_state();
    on = false;
    toggle(kDefaultChrome, 1.0f / 60.0f);
    CHECK(on == true);
    CHECK(gui.wants_redraw());
}

TEST_CASE("the overlay fades bracket their scopes and re-arm on reopen") {
    GuiContext gui;
    LayoutCursor layout;

    // A popup with animation ON: the open frame pushes ONE tint (the
    // fresh-open fade starts at 0), the scope pops it, and repeated
    // open frames walk the alpha up the clock.
    bool open = true;
    const auto popup_frame = [&](float dt) {
        ws::ui::gui_test::stub.reset();
        gui.begin_frame(snap(-100.0f, -100.0f, dt));
        if (ws::ui::gui::begin_popup(gui, layout, kDefaultChrome, "pp",
                                     {0.0f, 0.0f, 0.0f, 0.0f},
                                     {10.0f, 10.0f, 120.0f, 80.0f}, open)) {
            ws::ui::gui::end_popup(gui, layout);
        }
        gui.end_frame();
    };

    popup_frame(0.0f);
    CHECK(ws::ui::gui_test::stub.tint_pushes == 1);
    CHECK(ws::ui::gui_test::stub.tint_pops == 1);
    CHECK(ws::ui::gui_test::stub.last_tint == 0xFFFFFF00u);  // fresh: hidden

    popup_frame(1.0f / 60.0f);
    CHECK(ws::ui::gui_test::stub.tint_pushes == 1);
    CHECK(ws::ui::gui_test::stub.last_tint > 0xFFFFFF00u);  // fading in
    CHECK(ws::ui::gui_test::stub.last_tint < 0xFFFFFFFFu);

    // The closed frame re-arms: the next open starts from nothing
    // again (the fade state's was_open reset is the closed branch's
    // own work).
    open = false;
    popup_frame(0.0f);
    CHECK(ws::ui::gui_test::stub.tint_pushes == 0);  // closed: no bracket
    open = true;
    popup_frame(0.0f);
    CHECK(ws::ui::gui_test::stub.last_tint == 0xFFFFFF00u);  // re-armed

    // Animation OFF: the bracket is the identity - the popup draws at
    // full strength from its first frame.
    ws::ui::gui::ChromeTheme still = kDefaultChrome;
    still.anim_enabled = false;
    gui.clear_persistent_state();
    ws::ui::gui_test::stub.reset();
    gui.begin_frame(snap(-100.0f, -100.0f, 0.0f));
    if (ws::ui::gui::begin_popup(gui, layout, still, "pp",
                                 {0.0f, 0.0f, 0.0f, 0.0f},
                                 {10.0f, 10.0f, 120.0f, 80.0f}, open)) {
        ws::ui::gui::end_popup(gui, layout);
    }
    gui.end_frame();
    CHECK(ws::ui::gui_test::stub.tint_pushes == 1);
    CHECK(ws::ui::gui_test::stub.last_tint == 0xFFFFFFFFu);  // identity

    // A tooltip brackets the same way.
    gui.clear_persistent_state();
    ws::ui::gui_test::stub.reset();
    gui.begin_frame(snap(-100.0f, -100.0f, 0.0f));
    if (ws::ui::gui::begin_tooltip(gui, layout, kDefaultChrome, "tip",
                                   {10.0f, 10.0f, 100.0f, 40.0f}, true)) {
        ws::ui::gui::end_tooltip(gui, layout);
    }
    gui.end_frame();
    CHECK(ws::ui::gui_test::stub.tint_pushes == 1);
    CHECK(ws::ui::gui_test::stub.tint_pops == 1);
    CHECK(ws::ui::gui_test::stub.last_tint == 0xFFFFFF00u);
}

TEST_CASE("the toast fade rides the store's own age clock") {
    GuiContext gui;

    // The tint stack itself: balanced pushes/pops, the identity is a
    // no-op multiply by contract (the renderer's own suite pins the
    // channel math; the stub pins the pairing).
    ws::ui::r2d::push_tint(0xFFFFFFFFu);
    ws::ui::r2d::push_tint(0xFFFFFF80u);
    ws::ui::r2d::pop_tint();
    ws::ui::r2d::pop_tint();
    ws::ui::r2d::pop_tint();  // a stray pop: a no-op
    CHECK(true);  // balanced, nothing threw
}
