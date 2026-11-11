// The numeric input suite: input_float / input_int end to end - the
// pure parse/format pair gated directly (the popup_placement
// precedent), and the widget driven through hand-built frames the way
// the gui suite builds them, so every commit edge lands exactly as the
// platform layers would deliver it: the focus's arrival (Tab's
// select-all against a press's caret placement), the typing through
// the numeric charset, Enter's consumed commit, the blur's silent one,
// Escape's abandoned edit, and the stepping arrows with the OS
// auto-repeat under them. The property grid's own row rides at the
// end - the exact-value story the sliders could never tell. The draws
// are asserted through the r2d stub's counters, the same purity class
// as test_ui_gui: the gui archive, the real font bake, the stub seam;
// no renderer, no window, no platform TU.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/painters.hpp>
#include <ws/ui/gui/property_grid.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/input/input.hpp>

#include "ui_gui_stub.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {

using ws::ui::gui::GuiContext;
using ws::input::InputFrame;
using ws::ui::gui::LayoutCursor;
using ws::ui::gui::kDefaultChrome;
namespace keys = ws::input::keys;

// The gui suite's snap(): a present mouse parked far off-panel by
// default, with the primary button's held state and optional
// press/release edges - the same shape test_ui's snap() models.
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

// One frame's typing: the codepoints land in order, the way the
// platform's text-input delivery would queue them.
void type_text(InputFrame& frame, const char* text) {
    for (; *text != '\0'; ++text) {
        frame.text_input.push_back(static_cast<std::uint32_t>(*text));
    }
}

InputFrame with_key(int key) {
    InputFrame frame = snap();
    frame.key_pressed[static_cast<std::size_t>(key)] = true;
    return frame;
}

InputFrame with_repeat(int key) {
    InputFrame frame = snap();
    frame.key_repeated[static_cast<std::size_t>(key)] = true;
    return frame;
}

// The deterministic font the gui suite builds: 95 ASCII glyphs at a
// flat 10 px advance, the real font.cpp behind the measure.
std::array<ws::ui::r2d::Font::Glyph, 95> font_glyphs() {
    std::array<ws::ui::r2d::Font::Glyph, 95> glyphs{};
    for (ws::ui::r2d::Font::Glyph& glyph : glyphs) {
        glyph.advance = 10.0f;
    }
    return glyphs;
}

}  // namespace

// --- the pure pair -------------------------------------------------------------

TEST_CASE("input_parse_float: the strict walk") {
    using ws::ui::gui::input_parse_float;
    using ws::ui::gui::InputParseError;

    // The honest spellings.
    REQUIRE(input_parse_float("0.0001").has_value());
    CHECK(*input_parse_float("0.0001") == doctest::Approx(0.0001));
    REQUIRE(input_parse_float("-3.5").has_value());
    CHECK(*input_parse_float("-3.5") == doctest::Approx(-3.5));
    REQUIRE(input_parse_float("+2").has_value());
    CHECK(*input_parse_float("+2") == 2.0f);
    REQUIRE(input_parse_float("1e-3").has_value());
    CHECK(*input_parse_float("1e-3") == doctest::Approx(0.001));
    REQUIRE(input_parse_float("4700").has_value());
    CHECK(*input_parse_float("4700") == 4700.0f);
    REQUIRE(input_parse_float(" 1.5").has_value());  // strtod's own tolerance
    CHECK(*input_parse_float(" 1.5") == 1.5f);

    // The refusals, each with its own code.
    CHECK(input_parse_float("").error().code ==
          static_cast<std::int32_t>(InputParseError::empty));
    CHECK(input_parse_float(nullptr).error().code ==
          static_cast<std::int32_t>(InputParseError::empty));
    CHECK(input_parse_float("abc").error().code ==
          static_cast<std::int32_t>(InputParseError::not_a_number));
    // "1e" parses the 1 and stops at the incomplete exponent -
    // strtod's own contract calls it trailing text.
    CHECK(input_parse_float("1e").error().code ==
          static_cast<std::int32_t>(InputParseError::trailing));
    CHECK(input_parse_float("inf").error().code ==
          static_cast<std::int32_t>(InputParseError::not_a_number));
    CHECK(input_parse_float("nan").error().code ==
          static_cast<std::int32_t>(InputParseError::not_a_number));
    CHECK(input_parse_float("1.2.3").error().code ==
          static_cast<std::int32_t>(InputParseError::trailing));
    CHECK(input_parse_float("4m").error().code ==
          static_cast<std::int32_t>(InputParseError::trailing));
    // Hex spellings are strtod's to parse and never a decimal field's
    // (the x-scan refuses the family, so "4px" reads never-a-number
    // rather than four-then-px).
    CHECK(input_parse_float("0x10").error().code ==
          static_cast<std::int32_t>(InputParseError::not_a_number));
    CHECK(input_parse_float("1p3").error().code ==
          static_cast<std::int32_t>(InputParseError::not_a_number));
    CHECK(input_parse_float("4px").error().code ==
          static_cast<std::int32_t>(InputParseError::not_a_number));
    // A finite double past the float's own magnitude: a range refusal.
    CHECK(input_parse_float("1e300").error().code ==
          static_cast<std::int32_t>(InputParseError::out_of_range));
    // The category is the ui one, the theme's own.
    CHECK(std::string(input_parse_float("abc").error().category) == "ui");
}

TEST_CASE("input_parse_int: the integer's own refusals") {
    using ws::ui::gui::input_parse_int;
    using ws::ui::gui::InputParseError;

    REQUIRE(input_parse_int("42").has_value());
    CHECK(*input_parse_int("42") == 42);
    REQUIRE(input_parse_int("-7").has_value());
    CHECK(*input_parse_int("-7") == -7);
    REQUIRE(input_parse_int("+3").has_value());
    CHECK(*input_parse_int("+3") == 3);

    // A fractional spelling is trailing text to an integer.
    CHECK(input_parse_int("1.5").error().code ==
          static_cast<std::int32_t>(InputParseError::trailing));
    CHECK(input_parse_int("0x10").error().code ==
          static_cast<std::int32_t>(InputParseError::trailing));
    CHECK(input_parse_int("").error().code ==
          static_cast<std::int32_t>(InputParseError::empty));
    CHECK(input_parse_int("abc").error().code ==
          static_cast<std::int32_t>(InputParseError::not_a_number));
    CHECK(input_parse_int("-").error().code ==
          static_cast<std::int32_t>(InputParseError::not_a_number));
    // Past the type's own bounds: a range refusal, never a wrap.
    CHECK(input_parse_int("99999999999999").error().code ==
          static_cast<std::int32_t>(InputParseError::out_of_range));
    CHECK(input_parse_int("-99999999999999").error().code ==
          static_cast<std::int32_t>(InputParseError::out_of_range));
}

TEST_CASE("input_format_float: the %g spelling") {
    using ws::ui::gui::input_format_float;
    char buf[64];

    CHECK(input_format_float(buf, sizeof(buf), 0.0001f, 6) == 6);
    CHECK(std::string(buf) == "0.0001");
    CHECK(input_format_float(buf, sizeof(buf), 4700.0f, 6) == 4);
    CHECK(std::string(buf) == "4700");
    CHECK(input_format_float(buf, sizeof(buf), 1234567.0f, 6) == 11);
    CHECK(std::string(buf) == "1.23457e+06");
    CHECK(input_format_float(buf, sizeof(buf), -0.25f, 6) == 5);
    CHECK(std::string(buf) == "-0.25");
    // A negative zero spells as the plain one.
    CHECK(input_format_float(buf, sizeof(buf), -0.0f, 6) == 1);
    CHECK(std::string(buf) == "0");
    // The digits clamp into 1..9; 0 reads as the tightest.
    CHECK(input_format_float(buf, sizeof(buf), 1234.0f, 0) == 5);
    CHECK(std::string(buf) == "1e+03");
    // A non-finite value spells itself and never re-parses.
    CHECK(input_format_float(buf, sizeof(buf), std::nanf(""), 6) == 3);
    CHECK(std::string(buf) == "nan");
    // A tiny capacity truncates, NUL-terminated, honestly counted.
    char tiny[4];
    CHECK(input_format_float(tiny, sizeof(tiny), 4700.0f, 6) == 3);
    CHECK(std::string(tiny) == "470");
    CHECK(input_format_float(buf, 0, 1.0f, 6) == 0);
    CHECK(input_format_float(nullptr, 64, 1.0f, 6) == 0);
}

TEST_CASE("the numeric filters admit their charsets and nothing else") {
    using ws::ui::gui::is_float_insertable;
    using ws::ui::gui::is_int_insertable;

    for (char c = '0'; c <= '9'; ++c) {
        CHECK(is_float_insertable(static_cast<std::uint32_t>(c)));
        CHECK(is_int_insertable(static_cast<std::uint32_t>(c)));
    }
    for (const char* s = ".+-eE"; *s != '\0'; ++s) {
        CHECK(is_float_insertable(static_cast<std::uint32_t>(*s)));
    }
    CHECK(is_int_insertable(static_cast<std::uint32_t>('+')));
    CHECK(is_int_insertable(static_cast<std::uint32_t>('-')));
    // Everything a number never spells: letters outside the exponent,
    // space, the high bytes.
    CHECK(!is_float_insertable(static_cast<std::uint32_t>('x')));
    CHECK(!is_float_insertable(static_cast<std::uint32_t>(' ')));
    CHECK(!is_float_insertable(static_cast<std::uint32_t>('a')));
    CHECK(!is_int_insertable(static_cast<std::uint32_t>('.')));
    CHECK(!is_int_insertable(static_cast<std::uint32_t>('e')));
    CHECK(!is_int_insertable(233u));  // 'e' with the high bit set
}

// --- the widget, driven frame by frame ------------------------------------------

namespace {

// One idle frame over one field, the fontless stock chrome - the
// behavior's own geometry, no pixels to interpret.
struct FieldHarness {
    GuiContext gui;
    LayoutCursor layout;

    ws::ui::gui::WidgetId id_of(const char* label) {
        return gui.ids().id(label);
    }
    ws::ui::gui::NumInputState& state_of(const char* label) {
        return gui.persistent_state<ws::ui::gui::NumInputState>(
            id_of(label));
    }
};

}  // namespace

TEST_CASE("input_float: the idle row spells the value, and an "
          "external change shows the next frame") {
    FieldHarness h;
    float value = 4.5f;

    const auto run = [&](const InputFrame& frame) {
        h.gui.begin_frame(frame);
        (void)h.layout.begin_panel({0.0f, 0.0f, 300.0f, 200.0f}, 0.0f);
        const bool changed =
            ws::ui::gui::input_float(h.gui, h.layout, kDefaultChrome, "v",
                                     value);
        h.layout.end_panel();
        h.gui.end_frame();
        return changed;
    };

    CHECK(!run(snap()));
    CHECK(std::string(h.state_of("v").buf) == "4.5");

    // The caller's own write lands in the spelling on the very next
    // idle frame - the buffer follows the value while nothing edits it.
    value = 0.0001f;
    CHECK(!run(snap()));
    CHECK(std::string(h.state_of("v").buf) == "0.0001");
}

TEST_CASE("input_float: typing 0.0001 into a 0..1 field commits it "
          "exactly, and Enter is the field's own key") {
    FieldHarness h;
    float value = 0.5f;
    bool entered_left_unconsumed = true;

    const auto run = [&](const InputFrame& frame) {
        h.gui.begin_frame(frame);
        (void)h.layout.begin_panel({0.0f, 0.0f, 300.0f, 200.0f}, 0.0f);
        const bool changed = ws::ui::gui::input_float(
            h.gui, h.layout, kDefaultChrome, "v", value, 0.0f, 1.0f);
        // The commit owns the edge: nothing later in the frame reads
        // it (a dialog's default button behind the field stays put).
        entered_left_unconsumed =
            h.gui.input()
                .key_pressed[static_cast<std::size_t>(ws::input::keys::kEnter)];
        h.layout.end_panel();
        h.gui.end_frame();
        return changed;
    };

    // The keyboard's arrival selects the whole spelling; one frame of
    // typing replaces it.
    h.gui.set_focus(h.id_of("v"));
    CHECK(!run(snap()));
    InputFrame typing = snap();
    type_text(typing, "0.0001");
    CHECK(!run(typing));
    CHECK(std::string(h.state_of("v").buf) == "0.0001");

    // Enter commits the exact value - the resolution a slider never
    // had - and consumes its own key. The focus holds past the commit
    // (an Enter is not a blur), the caret parked at the spelling's end.
    CHECK(run(with_key(ws::input::keys::kEnter)));
    CHECK(value == doctest::Approx(0.0001f));
    CHECK(!entered_left_unconsumed);
    CHECK(std::string(h.state_of("v").buf) == "0.0001");

    // A canonical respelling: blur away, arrive again (select-all),
    // type "5e-1" and commit - it stands as "0.5".
    run(snap(250.0f, 150.0f, true, true));
    CHECK(!run(snap()));  // the blur frame: the standing text re-commits to nothing
    h.gui.set_focus(h.id_of("v"));
    CHECK(!run(snap()));
    InputFrame sci = snap();
    type_text(sci, "5e-1");
    CHECK(!run(sci));
    CHECK(run(with_key(ws::input::keys::kEnter)));
    CHECK(value == doctest::Approx(0.5f));
    CHECK(std::string(h.state_of("v").buf) == "0.5");
}

TEST_CASE("input_float: a typed value past the range clamps, and "
          "re-typing the standing value is a no-op") {
    FieldHarness h;
    float value = 50.0f;

    const auto run = [&](const InputFrame& frame) {
        h.gui.begin_frame(frame);
        (void)h.layout.begin_panel({0.0f, 0.0f, 300.0f, 200.0f}, 0.0f);
        const bool changed = ws::ui::gui::input_float(
            h.gui, h.layout, kDefaultChrome, "v", value, 0.0f, 100.0f);
        h.layout.end_panel();
        h.gui.end_frame();
        return changed;
    };

    h.gui.set_focus(h.id_of("v"));
    InputFrame over = snap();
    type_text(over, "200");
    CHECK(!run(over));
    CHECK(run(with_key(ws::input::keys::kEnter)));
    CHECK(value == doctest::Approx(100.0f));  // the family's own clamp
    CHECK(std::string(h.state_of("v").buf) == "100");

    // The standing value, re-typed: no move, no answer.
    run(snap(250.0f, 150.0f, true, true));
    CHECK(!run(snap()));
    h.gui.set_focus(h.id_of("v"));
    InputFrame same = snap();
    type_text(same, "100");
    CHECK(!run(same));
    CHECK(!run(with_key(ws::input::keys::kEnter)));
    CHECK(value == doctest::Approx(100.0f));
}

TEST_CASE("input_float: a refused commit reverts the spelling - an "
          "abandoned edit never leaks") {
    FieldHarness h;
    float value = 4.5f;

    const auto run = [&](const InputFrame& frame) {
        h.gui.begin_frame(frame);
        (void)h.layout.begin_panel({0.0f, 0.0f, 300.0f, 200.0f}, 0.0f);
        const bool changed =
            ws::ui::gui::input_float(h.gui, h.layout, kDefaultChrome, "v",
                                     value);
        h.layout.end_panel();
        h.gui.end_frame();
        return changed;
    };

    // The charset admits the multi-dot; the parse refuses it.
    h.gui.set_focus(h.id_of("v"));
    InputFrame garbage = snap();
    type_text(garbage, "1.2.3");
    CHECK(!run(garbage));
    CHECK(std::string(h.state_of("v").buf) == "1.2.3");
    CHECK(!run(with_key(ws::input::keys::kEnter)));
    CHECK(value == doctest::Approx(4.5f));
    CHECK(std::string(h.state_of("v").buf) == "4.5");

    // A field emptied mid-edit refuses the same way: blur away,
    // arrive again (the whole spelling selected), and ONE Backspace
    // takes it - an empty buffer never parses.
    run(snap(250.0f, 150.0f, true, true));
    CHECK(!run(snap()));
    h.gui.set_focus(h.id_of("v"));
    CHECK(!run(snap()));
    InputFrame cleared = snap();
    cleared.key_pressed[static_cast<std::size_t>(
        ws::input::keys::kBackspace)] = true;
    CHECK(!run(cleared));
    CHECK(std::string(h.state_of("v").buf) == "");
    CHECK(!run(with_key(ws::input::keys::kEnter)));
    CHECK(value == doctest::Approx(4.5f));
    CHECK(std::string(h.state_of("v").buf) == "4.5");
}

TEST_CASE("input_float: the blur commits - and a refused blur reverts") {
    FieldHarness h;
    float value = 1.0f;

    const auto run = [&](const InputFrame& frame) {
        h.gui.begin_frame(frame);
        (void)h.layout.begin_panel({0.0f, 0.0f, 300.0f, 200.0f}, 0.0f);
        const bool changed =
            ws::ui::gui::input_float(h.gui, h.layout, kDefaultChrome, "v",
                                     value);
        h.layout.end_panel();
        h.gui.end_frame();
        return changed;
    };

    h.gui.set_focus(h.id_of("v"));
    InputFrame typing = snap();
    type_text(typing, "2.5");
    CHECK(!run(typing));
    CHECK(value == doctest::Approx(1.0f));  // typing alone writes nothing

    // A press on the empty panel clears the focus at its end_frame;
    // the frame after is the blur, and the edit lands there.
    run(snap(250.0f, 150.0f, true, true));
    CHECK(run(snap()));
    CHECK(value == doctest::Approx(2.5f));
    CHECK(std::string(h.state_of("v").buf) == "2.5");

    // A refused blur: the spelling returns to what stands.
    h.gui.set_focus(h.id_of("v"));
    InputFrame bad = snap();
    type_text(bad, "--");
    CHECK(!run(bad));
    run(snap(250.0f, 150.0f, true, true));
    CHECK(run(snap()) == false);
    CHECK(value == doctest::Approx(2.5f));
    CHECK(std::string(h.state_of("v").buf) == "2.5");
}

TEST_CASE("input_float: Escape abandons the edit; a standing selection "
          "drops first, the buffer intact") {
    FieldHarness h;
    float value = 4.5f;

    const auto run = [&](const InputFrame& frame) {
        h.gui.begin_frame(frame);
        (void)h.layout.begin_panel({0.0f, 0.0f, 300.0f, 200.0f}, 0.0f);
        const bool changed =
            ws::ui::gui::input_float(h.gui, h.layout, kDefaultChrome, "v",
                                     value);
        h.layout.end_panel();
        h.gui.end_frame();
        return changed;
    };

    // A bare Escape: the edit is abandoned, the spelling reverts.
    h.gui.set_focus(h.id_of("v"));
    InputFrame typing = snap();
    type_text(typing, "9");
    CHECK(!run(typing));
    CHECK(!run(with_key(ws::input::keys::kEscape)));
    CHECK(value == doctest::Approx(4.5f));
    CHECK(std::string(h.state_of("v").buf) == "4.5");

    // The arrival's select-all, then Escape: the text field's own drop
    // fires first (a selection stood), the buffer keeps its text.
    // Blur away first - the bare-Escape branch above left the focus
    // held, and a re-arrival needs the blur.
    run(snap(250.0f, 150.0f, true, true));
    CHECK(!run(snap()));
    h.gui.set_focus(h.id_of("v"));
    CHECK(!run(snap()));
    const ws::ui::gui::NumInputState& st = h.state_of("v");
    CHECK(st.edit.anchor == 0);  // the select-all spanned the spelling
    CHECK(st.edit.caret == 3);
    CHECK(!run(with_key(ws::input::keys::kEscape)));
    CHECK(st.edit.caret == st.edit.anchor);  // dropped, not abandoned
    CHECK(std::string(st.buf) == "4.5");
    CHECK(value == doctest::Approx(4.5f));
}

TEST_CASE("input_float: the arrows step - derived, explicit, clamped, "
          "repeating - and mid-edit stepping commits the typing") {
    FieldHarness h;
    float value = 5.0f;

    const auto run = [&](const InputFrame& frame, float lo = 0.0f,
                         float hi = 0.0f, float step = 0.0f) {
        h.gui.begin_frame(frame);
        (void)h.layout.begin_panel({0.0f, 0.0f, 300.0f, 200.0f}, 0.0f);
        const bool changed = ws::ui::gui::input_float(
            h.gui, h.layout, kDefaultChrome, "v", value, lo, hi, step);
        h.layout.end_panel();
        h.gui.end_frame();
        return changed;
    };

    // Unbounded: the derived step is the ladder's plain 1.
    h.gui.set_focus(h.id_of("v"));
    CHECK(!run(snap(), 0.0f, 0.0f, 0.0f));
    CHECK(run(with_key(ws::input::keys::kDown), 0.0f, 0.0f, 0.0f));
    CHECK(value == doctest::Approx(4.0f));
    CHECK(run(with_key(ws::input::keys::kUp), 0.0f, 0.0f, 0.0f));
    CHECK(value == doctest::Approx(5.0f));
    // The OS auto-repeat keeps the walk moving.
    CHECK(run(with_repeat(ws::input::keys::kUp), 0.0f, 0.0f, 0.0f));
    CHECK(value == doctest::Approx(6.0f));

    // Mid-edit stepping commits the typing: blur away, arrive again
    // (select-all), type "9" over the spelling, and a step walks from
    // the NINE the buffer holds - the spelling follows the answer.
    run(snap(250.0f, 150.0f, true, true));
    CHECK(!run(snap()));
    h.gui.set_focus(h.id_of("v"));
    InputFrame typing = snap();
    type_text(typing, "9");
    CHECK(!run(typing, 0.0f, 0.0f, 0.0f));
    CHECK(run(with_key(ws::input::keys::kDown), 0.0f, 0.0f, 0.0f));
    CHECK(value == doctest::Approx(8.0f));
    CHECK(std::string(h.state_of("v").buf) == "8");

    // Ranged with an explicit step: the walk clamps at the range's
    // ends and stops answering there.
    value = 0.5f;
    h.gui.clear_persistent_state();
    h.gui.set_focus(h.id_of("v"));
    CHECK(!run(snap(), 0.0f, 1.0f, 0.25f));
    CHECK(run(with_key(ws::input::keys::kUp), 0.0f, 1.0f, 0.25f));
    CHECK(value == doctest::Approx(0.75f));
    CHECK(run(with_key(ws::input::keys::kUp), 0.0f, 1.0f, 0.25f));
    CHECK(value == doctest::Approx(1.0f));
    CHECK(!run(with_key(ws::input::keys::kUp), 0.0f, 1.0f, 0.25f));
    CHECK(value == doctest::Approx(1.0f));

    // A negative step refuses the arrows outright.
    CHECK(!run(with_key(ws::input::keys::kDown), 0.0f, 1.0f, -1.0f));
    CHECK(value == doctest::Approx(1.0f));
}

TEST_CASE("input_float: Tab's arrival selects all, a press's arrival "
          "places the caret") {
    FieldHarness h;
    float value = 4.5f;

    const auto run = [&](const InputFrame& frame) {
        h.gui.begin_frame(frame);
        (void)h.layout.begin_panel({0.0f, 0.0f, 300.0f, 200.0f}, 0.0f);
        (void)ws::ui::gui::input_float(h.gui, h.layout, kDefaultChrome, "v",
                                       value);
        h.layout.end_panel();
        h.gui.end_frame();
    };

    // set_focus (Tab's resolution lands the same way): the whole
    // spelling stands selected, so typing replaces it.
    h.gui.set_focus(h.id_of("v"));
    run(snap());
    {
        const ws::ui::gui::NumInputState& st = h.state_of("v");
        CHECK(st.edit.anchor == 0);
        CHECK(st.edit.caret == 3);
    }
    InputFrame typing = snap();
    type_text(typing, "7");
    run(typing);
    CHECK(std::string(h.state_of("v").buf) == "7");

    // A press inside the field: the caret lands at the click, nothing
    // selected - the text field's own placement.
    const float field_x = 20.0f;  // label "v" + inset + the gutter
    run(snap(field_x + 5.0f, 10.0f, true, true));
    {
        const ws::ui::gui::NumInputState& st = h.state_of("v");
        CHECK(st.edit.caret == st.edit.anchor);  // placed, not spanned
        CHECK(h.gui.is_focused(h.id_of("v")));
    }
}

TEST_CASE("input_int: the integer ladder - commits, refuses, steps 1, "
          "unbounded where the 0..100 fallback invented a range") {
    FieldHarness h;
    int value = 42;

    const auto run = [&](const InputFrame& frame, int lo = 0, int hi = 0) {
        h.gui.begin_frame(frame);
        (void)h.layout.begin_panel({0.0f, 0.0f, 300.0f, 200.0f}, 0.0f);
        const bool changed = ws::ui::gui::input_int(h.gui, h.layout,
                                                    kDefaultChrome, "n",
                                                    value, lo, hi);
        h.layout.end_panel();
        h.gui.end_frame();
        return changed;
    };

    CHECK(!run(snap(), 0, 0));
    CHECK(std::string(h.state_of("n").buf) == "42");

    // Unbounded: what the old grid's 0..100 slider could never hold.
    h.gui.set_focus(h.id_of("n"));
    InputFrame big = snap();
    type_text(big, "1000000");
    CHECK(!run(big, 0, 0));
    CHECK(run(with_key(ws::input::keys::kEnter), 0, 0));
    CHECK(value == 1000000);

    // The refused: a lone sign never parses, the spelling returns.
    // Blur away first (the commit above left the focus held).
    run(snap(250.0f, 150.0f, true, true));
    CHECK(!run(snap()));
    h.gui.set_focus(h.id_of("n"));
    InputFrame sign = snap();
    type_text(sign, "-");
    CHECK(!run(sign, 0, 0));
    CHECK(std::string(h.state_of("n").buf) == "-");
    CHECK(!run(with_key(ws::input::keys::kEnter), 0, 0));
    CHECK(value == 1000000);
    CHECK(std::string(h.state_of("n").buf) == "1000000");

    // The arrows step the int ladder's plain 1.
    value = 41;
    h.gui.clear_persistent_state();
    h.gui.set_focus(h.id_of("n"));
    CHECK(!run(snap(), 0, 0));
    CHECK(run(with_key(ws::input::keys::kUp), 0, 0));
    CHECK(value == 42);

    // Ranged: a typed 15 clamps into 0..10.
    value = 5;
    h.gui.clear_persistent_state();
    h.gui.set_focus(h.id_of("n"));
    InputFrame over = snap();
    type_text(over, "15");
    CHECK(!run(over, 0, 10));
    CHECK(run(with_key(ws::input::keys::kEnter), 0, 10));
    CHECK(value == 10);
}

TEST_CASE("input_float: a NaN value spells itself, never re-commits, "
          "and refuses the step") {
    FieldHarness h;
    float value = std::nanf("");

    const auto run = [&](const InputFrame& frame) {
        h.gui.begin_frame(frame);
        (void)h.layout.begin_panel({0.0f, 0.0f, 300.0f, 200.0f}, 0.0f);
        const bool changed =
            ws::ui::gui::input_float(h.gui, h.layout, kDefaultChrome, "v",
                                     value);
        h.layout.end_panel();
        h.gui.end_frame();
        return changed;
    };

    CHECK(!run(snap()));
    CHECK(std::string(h.state_of("v").buf) == "nan");
    h.gui.set_focus(h.id_of("v"));
    CHECK(!run(snap()));
    CHECK(!run(with_key(ws::input::keys::kUp)));  // the drag's own guard
    CHECK(std::isnan(value));
}

TEST_CASE("input_float: the disabled row is inert") {
    FieldHarness h;
    float value = 4.5f;

    const auto run = [&](const InputFrame& frame) {
        h.gui.begin_frame(frame);
        (void)h.layout.begin_panel({0.0f, 0.0f, 300.0f, 200.0f}, 0.0f);
        h.gui.begin_disabled();
        const bool changed =
            ws::ui::gui::input_float(h.gui, h.layout, kDefaultChrome, "v",
                                     value);
        h.gui.end_disabled();
        h.layout.end_panel();
        h.gui.end_frame();
        return changed;
    };

    CHECK(!run(snap()));
    CHECK(std::string(h.state_of("v").buf) == "4.5");
    InputFrame typing = snap();
    type_text(typing, "9");
    CHECK(!run(typing));
    CHECK(!run(with_key(ws::input::keys::kEnter)));
    CHECK(value == doctest::Approx(4.5f));
    CHECK(std::string(h.state_of("v").buf) == "4.5");
}

// --- the look and the seam ------------------------------------------------------

namespace {
int skin_calls = 0;
void skin_input_row(const ws::ui::gui::Rect&, const ws::ui::gui::NumInputResult&,
                    const ws::ui::gui::ChromeTheme&, const char*, const char*,
                    int) {
    ++skin_calls;
}
}  // namespace

TEST_CASE("the painter seam: a skin's input_float row owns the pixels, "
          "a partial table falls back to the stock") {
    GuiContext gui;
    LayoutCursor layout;
    float value = 4.5f;

    ws::ui::gui::ChromeTheme theme = kDefaultChrome;
    ws::ui::gui::WidgetPainters skin = ws::ui::gui::kStockPainters;
    skin.input_float = &skin_input_row;
    theme.painters = &skin;

    skin_calls = 0;
    ws::ui::gui_test::stub.reset();
    gui.begin_frame(snap());
    (void)layout.begin_panel({0.0f, 0.0f, 300.0f, 200.0f}, 0.0f);
    (void)ws::ui::gui::input_float(gui, layout, theme, "v", value);
    layout.end_panel();
    gui.end_frame();
    CHECK(skin_calls == 1);
    CHECK(ws::ui::gui_test::stub.texts == 0);  // the skin drew, not the stock

    // A table with the row null draws the stock look.
    ws::ui::gui::WidgetPainters partial{};
    theme.painters = &partial;
    ws::ui::gui_test::stub.reset();
    gui.begin_frame(snap());
    (void)layout.begin_panel({0.0f, 0.0f, 300.0f, 200.0f}, 0.0f);
    (void)ws::ui::gui::input_float(gui, layout, theme, "v", value);
    layout.end_panel();
    gui.end_frame();
    CHECK(ws::ui::gui_test::stub.quads > 0);  // the fontless field drew
}

TEST_CASE("the stock draw: the label at the left, the field's content "
          "right of it") {
    auto glyphs = font_glyphs();
    const ws::ui::r2d::Font font(16.0f, 20.0f, glyphs.data(), 95);
    ws::ui::gui::ChromeTheme theme = kDefaultChrome;
    theme.font = &font;

    GuiContext gui;
    LayoutCursor layout;
    float value = 4.5f;

    const auto run = [&](const char* label) {
        ws::ui::gui_test::stub.reset();
        gui.begin_frame(snap());
        (void)layout.begin_panel({0.0f, 0.0f, 300.0f, 200.0f}, 0.0f);
        (void)ws::ui::gui::input_float(gui, layout, theme, label, value);
        layout.end_panel();
        gui.end_frame();
    };

    // A labeled row: the label's text and the field's content, two
    // draw_text calls, the content left-aligned inside the field.
    run("voltage");
    CHECK(ws::ui::gui_test::stub.texts == 2);

    // A label-less row: the field spans the line, one text.
    run("");
    CHECK(ws::ui::gui_test::stub.texts == 1);
    CHECK(ws::ui::gui_test::stub.last_text_advance ==
          doctest::Approx(30.0f));  // "4.5" at the flat 10 px advance
}

// --- the property grid's own row -------------------------------------------------

TEST_CASE("property_grid: a real setting typed to 0.0001 through the "
          "grid's own row") {
    ws::settings::Schema schema;
    ws::settings::RangeConstraint range;
    range.min = 0.0;
    range.max = 1.0;
    range.action = ws::settings::ConstraintAction::clamp;
    CHECK(schema.declare({
        .key = "sim.tol",
        .type = ws::settings::SettingType::real,
        .default_value = ws::settings::SettingValue::real(0.001),
        .constraints = {range},
        .description = "solver tolerance",
        .category = "solver",
    }).has_value());
    ws::settings::SettingsStore store{std::move(schema)};

    GuiContext gui;
    LayoutCursor layout;
    ws::ui::gui::PropertyGridState pg;

    const auto run = [&](const InputFrame& frame) {
        gui.begin_frame(frame);
        (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                       {0.0f, 0.0f, 400.0f, 600.0f});
        (void)ws::ui::gui::property_grid(gui, layout, kDefaultChrome,
                                         store.schema(), store, pg);
        ws::ui::gui::end_panel(gui, layout);
        gui.end_frame();
    };

    // An idle frame lays the rows; the setting's field ids under the
    // panel's scope, the grid's rows under its label.
    run(snap());
    gui.ids().push("panel");
    const ws::ui::gui::WidgetId tol_id = gui.ids().id("sim.tol");
    gui.ids().pop();

    // The arrival: select-all, and the focused field's bounds latch.
    gui.set_focus(tol_id);
    run(snap());
    const ws::ui::gui::Rect* field = gui.focused_bounds();
    REQUIRE(field != nullptr);
    CHECK(field->width > 0.0f);

    // The exact value the slider could not land: typed, committed on
    // Enter, written through the store.
    InputFrame typing = snap();
    type_text(typing, "0.0001");
    run(typing);
    run(with_key(ws::input::keys::kEnter));
    auto tol = store.get<double>("sim.tol");
    REQUIRE(tol.has_value());
    CHECK(tol->value == doctest::Approx(0.0001));

    // The grid's field re-spells from the store on its next idle
    // frame - the caller's write and the row's display agree.
    run(snap());
    const ws::ui::gui::NumInputState& st =
        gui.persistent_state<ws::ui::gui::NumInputState>(tol_id);
    CHECK(std::string(st.buf) == "0.0001");
}

TEST_CASE("property_grid: the text row owns its edit - a keystroke is not "
          "a write, the blur commits, a refused blur reverts") {
    ws::settings::Schema schema;
    ws::settings::RegexConstraint lower;
    lower.pattern = "^[a-z]+$";
    CHECK(schema.declare({
        .key = "editor.project",
        .type = ws::settings::SettingType::text,
        .default_value = ws::settings::SettingValue::text("untitled"),
        .description = "the open project",
        .category = "editor",
    }).has_value());
    CHECK(schema.declare({
        .key = "editor.style",
        .type = ws::settings::SettingType::text,
        .default_value = ws::settings::SettingValue::text("light"),
        .constraints = {lower},
        .description = "the style sheet name",
        .category = "editor",
    }).has_value());
    ws::settings::SettingsStore store{std::move(schema)};

    GuiContext gui;
    LayoutCursor layout;
    ws::ui::gui::PropertyGridState pg;

    const auto run = [&](const InputFrame& frame) {
        gui.begin_frame(frame);
        (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                       {0.0f, 0.0f, 400.0f, 600.0f});
        const std::size_t changed = ws::ui::gui::property_grid(
            gui, layout, kDefaultChrome, store.schema(), store, pg);
        ws::ui::gui::end_panel(gui, layout);
        gui.end_frame();
        return changed;
    };
    const auto buf_of = [&](const char* key) {
        const auto it = pg.text_bufs.find(key);
        return it != pg.text_bufs.end() ? std::string(it->second.data())
                                       : std::string{};
    };
    const auto value_of = [&](const char* key) {
        auto r = store.get<std::string>(key);
        return r.has_value() ? r->value : std::string{};
    };

    // An idle frame lays the rows; the buffers sync from the store.
    REQUIRE(run(snap()) == 0u);
    gui.ids().push("panel");
    const ws::ui::gui::WidgetId project_id = gui.ids().id("editor.project");
    const ws::ui::gui::WidgetId style_id = gui.ids().id("editor.style");
    gui.ids().pop();
    REQUIRE(buf_of("editor.project") == "untitled");

    // The arrival, then select-all and the typing: every keystroke
    // edits the buffer ONLY - the store is not written per keystroke
    // (the numeric rows' own rule, now the text row's too).
    gui.set_focus(project_id);
    run(snap());
    InputFrame select_all = snap();
    select_all.key_down[static_cast<std::size_t>(keys::kLeftControl)] = true;
    select_all.key_pressed[static_cast<std::size_t>(keys::kA)] = true;
    run(select_all);
    InputFrame typing = snap();
    type_text(typing, "circsim");
    REQUIRE(run(typing) == 0u);
    REQUIRE(run(snap()) == 0u);
    CHECK(value_of("editor.project") == "untitled");
    CHECK(buf_of("editor.project") == "circsim");

    // An external change while the edit stands WAITS: the focused
    // field owns the spelling, the typing is not stomped.
    CHECK(store.set(ws::settings::Layer::user, "editor.project",
                    std::string{"externally"})
              .has_value());
    run(snap());
    CHECK(value_of("editor.project") == "externally");
    CHECK(buf_of("editor.project") == "circsim");

    // The blur (a press on the empty panel clears the focus at its
    // end_frame; the frame after is the blur): one commit - the
    // typing lands, the count answers on the commit frame, the
    // buffer re-spells from the store's standing value.
    run(snap(250.0f, 150.0f, true, true));
    CHECK(run(snap()) == 1u);
    CHECK(value_of("editor.project") == "circsim");
    CHECK(buf_of("editor.project") == "circsim");

    // An external change on an IDLE row shows the moment it lands -
    // the store re-spells the buffer every unfocused frame.
    CHECK(store.set(ws::settings::Layer::user, "editor.project",
                    std::string{"synced"})
              .has_value());
    REQUIRE(run(snap()) == 0u);
    CHECK(value_of("editor.project") == "synced");
    CHECK(buf_of("editor.project") == "synced");

    // A refused write reverts at the BLUR, never mid-typing: a value
    // the regex refuses keeps through the edit, is offered once at
    // the commit, and loses to the standing value on that same frame.
    gui.set_focus(style_id);
    run(snap());
    run(select_all);
    InputFrame bad = snap();
    type_text(bad, "9");
    REQUIRE(run(bad) == 0u);
    CHECK(buf_of("editor.style") == "9");
    CHECK(value_of("editor.style") == "light");  // the keystroke wrote nothing
    run(snap(250.0f, 150.0f, true, true));
    CHECK(run(snap()) == 0u);  // the refused write counts no change
    CHECK(value_of("editor.style") == "light");
    CHECK(buf_of("editor.style") == "light");
}

TEST_CASE("property_grid: the enum row's dropdown STAYS open - the pick "
          "lands frames after the open") {
    ws::settings::Schema schema;
    ws::settings::EnumConstraint units;
    units.allowed = {ws::settings::SettingValue::integer(1),
                     ws::settings::SettingValue::integer(2),
                     ws::settings::SettingValue::integer(4)};
    CHECK(schema.declare({
        .key = "editor.grid",
        .type = ws::settings::SettingType::integer,
        .default_value = ws::settings::SettingValue::integer(2),
        .constraints = {units},
        .description = "the snap grid, in px",
        .category = "editor",
    }).has_value());
    ws::settings::EnumConstraint aa;
    aa.allowed = {ws::settings::SettingValue::text("high"),
                  ws::settings::SettingValue::text("medium"),
                  ws::settings::SettingValue::text("low")};
    CHECK(schema.declare({
        .key = "render.aa",
        .type = ws::settings::SettingType::text,
        .default_value = ws::settings::SettingValue::text("medium"),
        .constraints = {aa},
        .description = "the antialiasing level",
        .category = "render",
    }).has_value());
    ws::settings::SettingsStore store{std::move(schema)};

    GuiContext gui;
    LayoutCursor layout;
    ws::ui::gui::PropertyGridState pg;

    const auto run = [&](const InputFrame& frame) {
        gui.begin_frame(frame);
        (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                       {0.0f, 0.0f, 400.0f, 600.0f});
        const std::size_t changed = ws::ui::gui::property_grid(
            gui, layout, kDefaultChrome, store.schema(), store, pg);
        ws::ui::gui::end_panel(gui, layout);
        gui.end_frame();
        return changed;
    };

    // An idle frame lays the rows; the focus arrives on the int-enum row.
    REQUIRE(run(snap()) == 0u);
    gui.ids().push("panel");
    const ws::ui::gui::WidgetId grid_id = gui.ids().id("editor.grid");
    const ws::ui::gui::WidgetId aa_id = gui.ids().id("render.aa");
    gui.ids().pop();
    gui.set_focus(grid_id);
    run(snap());

    // Enter on the focused row opens the dropdown - and the open flag
    // is the combo's CALLER-OWNED state, kept in the grid's own state:
    // it survives the idle frames between the open and the pick. (The
    // frame-local spelling this test gates laid the list for exactly
    // the open's own frame; the next frame re-armed false, the list
    // vanished, and the pick's press fell through to whatever sat
    // beneath - an enum setting no mouse or keyboard could ever move.)
    REQUIRE(run(with_key(keys::kEnter)) == 0u);
    REQUIRE(pg.combo_open.at("editor.grid") == true);
    REQUIRE(run(snap()) == 0u);  // an idle frame: the list still hangs
    REQUIRE(pg.combo_open.at("editor.grid") == true);
    auto grid_value = store.get<std::int64_t>("editor.grid");
    REQUIRE(grid_value.has_value());
    CHECK(grid_value->value == 2);  // nothing written yet

    // Tab walks into the list (the items are citizens while it hangs),
    // Enter picks the focused one - one write, one changed count, and
    // the choice closes the dropdown with the focus back on the row.
    run(with_key(keys::kTab));
    CHECK(pg.combo_open.at("editor.grid") == true);
    CHECK(run(with_key(keys::kEnter)) == 1u);
    grid_value = store.get<std::int64_t>("editor.grid");
    REQUIRE(grid_value.has_value());
    CHECK(grid_value->value == 1);
    CHECK(pg.combo_open.at("editor.grid") == false);

    // The text-enum row is the same contract, the second branch: open
    // it, idle a frame (the flag must hold there too), then pick.
    gui.set_focus(aa_id);
    run(snap());
    REQUIRE(run(with_key(keys::kEnter)) == 0u);
    REQUIRE(pg.combo_open.at("render.aa") == true);
    REQUIRE(run(snap()) == 0u);
    CHECK(pg.combo_open.at("render.aa") == true);
    run(with_key(keys::kTab));
    CHECK(run(with_key(keys::kEnter)) == 1u);
    auto aa_value = store.get<std::string>("render.aa");
    REQUIRE(aa_value.has_value());
    CHECK(aa_value->value == "high");
    CHECK(pg.combo_open.at("render.aa") == false);
    CHECK(pg.combo_open.at("editor.grid") == false);  // the earlier pick held
}

TEST_CASE("property_grid: a real+enum row is the combo the mapping table "
          "promised - the list the schema declared, picked from") {
    ws::settings::Schema schema;
    ws::settings::EnumConstraint scale;
    scale.allowed = {ws::settings::SettingValue::real(0.5),
                     ws::settings::SettingValue::real(1.0),
                     ws::settings::SettingValue::real(2.5)};
    CHECK(schema.declare({
        .key = "render.scale",
        .type = ws::settings::SettingType::real,
        .default_value = ws::settings::SettingValue::real(1.0),
        .constraints = {scale},
        .description = "the ui scale factor",
        .category = "render",
    }).has_value());
    ws::settings::SettingsStore store{std::move(schema)};

    // The flat-advance font, so the row's composed line is countable:
    // the closed combo spells "render.scale: 1" (15 glyphs, 150 px) -
    // input_format_float's %g pen, the input_float family's own - not
    // the C++ default to_string spelling ("render.scale: 1.000000",
    // 22 glyphs) the dead enum_labels arm carried.
    auto glyphs = font_glyphs();
    const ws::ui::r2d::Font font(16.0f, 20.0f, glyphs.data(), 95);
    ws::ui::gui::ChromeTheme theme = kDefaultChrome;
    theme.font = &font;

    GuiContext gui;
    LayoutCursor layout;
    ws::ui::gui::PropertyGridState pg;

    const auto run = [&](const InputFrame& frame) {
        gui.begin_frame(frame);
        (void)ws::ui::gui::begin_panel(gui, layout, theme, "panel",
                                       {0.0f, 0.0f, 400.0f, 600.0f});
        const std::size_t changed = ws::ui::gui::property_grid(
            gui, layout, theme, store.schema(), store, pg);
        ws::ui::gui::end_panel(gui, layout);
        gui.end_frame();
        return changed;
    };
    const auto value = [&]() {
        auto r = store.get<double>("render.scale");
        return r.has_value() ? r->value : -1.0;
    };

    // An idle frame lays the row as a COMBO - the list row, the one
    // contract the grid's mapping table promised for real+enum. (The
    // row this test gates laid the UNBOUNDED input_float instead: no
    // list to open, and a setting whose only move was typing a value
    // the enum silently refused at the blur.)
    ws::ui::gui_test::stub.reset();
    REQUIRE(run(snap()) == 0u);
    CHECK(ws::ui::gui_test::stub.last_text_advance ==
          doctest::Approx(150.0f));

    // The focus arrives on the row; Enter opens the dropdown, and the
    // open flag rides the grid's own state - it hangs across the idle
    // frames between the open and the pick, like the integer and text
    // enums' own.
    gui.ids().push("panel");
    const ws::ui::gui::WidgetId scale_id = gui.ids().id("render.scale");
    gui.ids().pop();
    gui.set_focus(scale_id);
    run(snap());
    REQUIRE(run(with_key(keys::kEnter)) == 0u);
    REQUIRE(pg.combo_open.at("render.scale") == true);
    REQUIRE(run(snap()) == 0u);  // an idle frame: the list still hangs
    REQUIRE(pg.combo_open.at("render.scale") == true);
    CHECK(value() == doctest::Approx(1.0));  // nothing written yet

    // Tab walks into the list, Enter picks the focused item - one
    // write, one changed count, the dropdown closes, and the pick
    // wrote the allowed DOUBLE the schema declared (never the spelled
    // float).
    run(with_key(keys::kTab));
    CHECK(run(with_key(keys::kEnter)) == 1u);
    CHECK(value() == doctest::Approx(0.5));
    CHECK(pg.combo_open.at("render.scale") == false);

    // The closed row re-spells from the standing value: "render.scale:
    // 0.5" (17 glyphs, 170 px) - the %g pen again, a label that reads
    // like the field it replaced.
    ws::ui::gui_test::stub.reset();
    REQUIRE(run(snap()) == 0u);
    CHECK(ws::ui::gui_test::stub.last_text_advance ==
          doctest::Approx(170.0f));

    // The list is the whole truth: the row is no field to type into -
    // text delivered to the focused row moves nothing, and the blur
    // commits nothing (a combo has no text buffer to own).
    InputFrame typed = snap();
    type_text(typed, "9");
    REQUIRE(run(typed) == 0u);
    run(snap(250.0f, 150.0f, true, true));  // the press clears the focus
    REQUIRE(run(snap()) == 0u);
    CHECK(value() == doctest::Approx(0.5));

    // The radio's idempotence through the grid's own count: re-open,
    // walk to the STANDING item, Enter - a re-pick of the current
    // value answers false, so the count says 0 and the value holds.
    gui.set_focus(scale_id);
    run(snap());
    REQUIRE(run(with_key(keys::kEnter)) == 0u);
    REQUIRE(pg.combo_open.at("render.scale") == true);
    run(with_key(keys::kTab));
    CHECK(run(with_key(keys::kEnter)) == 0u);
    CHECK(value() == doctest::Approx(0.5));
    CHECK(pg.combo_open.at("render.scale") == false);

    // A later item is the same walk: two Tabs reach the second entry,
    // Enter picks 1.0, and the store answers the move.
    gui.set_focus(scale_id);
    run(snap());
    REQUIRE(run(with_key(keys::kEnter)) == 0u);
    run(with_key(keys::kTab));
    run(with_key(keys::kTab));
    CHECK(run(with_key(keys::kEnter)) == 1u);
    CHECK(value() == doctest::Approx(1.0));
    CHECK(pg.combo_open.at("render.scale") == false);
}

TEST_CASE("property_grid: the state follows the schema - keys that left "
          "take their state with them, a returned row starts clean") {
    // Schema A: a free-text row and an int-enum row under "editor".
    ws::settings::Schema schema_a;
    CHECK(schema_a.declare({
        .key = "editor.project",
        .type = ws::settings::SettingType::text,
        .default_value = ws::settings::SettingValue::text("untitled"),
        .description = "the open project",
        .category = "editor",
    }).has_value());
    ws::settings::EnumConstraint grid_units;
    grid_units.allowed = {ws::settings::SettingValue::integer(1),
                          ws::settings::SettingValue::integer(2),
                          ws::settings::SettingValue::integer(4)};
    CHECK(schema_a.declare({
        .key = "editor.grid",
        .type = ws::settings::SettingType::integer,
        .default_value = ws::settings::SettingValue::integer(2),
        .constraints = {grid_units},
        .description = "the snap grid, in px",
        .category = "editor",
    }).has_value());
    ws::settings::SettingsStore store_a{std::move(schema_a)};

    // Schema B: the SAME key re-declared an integer under another
    // category (its row is no text row now, and "editor" itself is
    // gone), plus one live text key of B's own.
    ws::settings::Schema schema_b;
    ws::settings::RangeConstraint small;
    small.min = 0.0;
    small.max = 10.0;
    CHECK(schema_b.declare({
        .key = "editor.project",
        .type = ws::settings::SettingType::integer,
        .default_value = ws::settings::SettingValue::integer(3),
        .constraints = {small},
        .description = "re-declared, another type",
        .category = "misc",
    }).has_value());
    CHECK(schema_b.declare({
        .key = "misc.note",
        .type = ws::settings::SettingType::text,
        .default_value = ws::settings::SettingValue::text("hello"),
        .description = "a live key in this binding",
        .category = "misc",
    }).has_value());
    ws::settings::SettingsStore store_b{std::move(schema_b)};

    GuiContext gui;
    LayoutCursor layout;
    ws::ui::gui::PropertyGridState pg;

    // One state, two bindings: the same PropertyGridState re-bound the
    // way a long-lived settings panel re-binds to another document's
    // store - the churn the prune exists for.
    const auto run = [&](const InputFrame& frame,
                         ws::settings::SettingsStore& store) {
        gui.begin_frame(frame);
        (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                       {0.0f, 0.0f, 400.0f, 600.0f});
        const std::size_t changed = ws::ui::gui::property_grid(
            gui, layout, kDefaultChrome, store.schema(), store, pg);
        ws::ui::gui::end_panel(gui, layout);
        gui.end_frame();
        return changed;
    };
    const auto project_value = [&]() {
        auto r = store_a.get<std::string>("editor.project");
        return r.has_value() ? r->value : std::string{};
    };
    const auto buf_of = [&](const char* key) {
        const auto it = pg.text_bufs.find(key);
        return it != pg.text_bufs.end() ? std::string(it->second.data())
                                       : std::string{};
    };

    // --- cycle 1: a text row that leaves MID-EDIT ------------------------
    // The edit stands when the schema swaps: focus held, the buffer
    // spelling "circsim", the store still at "untitled".
    REQUIRE(run(snap(), store_a) == 0u);
    gui.ids().push("panel");
    const ws::ui::gui::WidgetId project_id = gui.ids().id("editor.project");
    const ws::ui::gui::WidgetId grid_id = gui.ids().id("editor.grid");
    gui.ids().pop();
    gui.set_focus(project_id);
    run(snap(), store_a);
    InputFrame select_all = snap();
    select_all.key_down[static_cast<std::size_t>(keys::kLeftControl)] = true;
    select_all.key_pressed[static_cast<std::size_t>(keys::kA)] = true;
    run(select_all, store_a);
    InputFrame typing = snap();
    type_text(typing, "circsim");
    REQUIRE(run(typing, store_a) == 0u);
    REQUIRE(run(snap(), store_a) == 0u);
    CHECK(project_value() == "untitled");
    REQUIRE(buf_of("editor.project") == "circsim");
    REQUIRE(pg.combo_open.count("editor.grid") == 1);  // laid by A's rows

    // Re-bound to B: the state follows the schema BOUND THIS FRAME -
    // the key that no longer lays a text row (it is an INTEGER row in
    // B) loses its buffer and its editing flag, the key that left
    // entirely loses its dropdown flag, and the category that left
    // loses its open-state. B's own live key keeps everything.
    run(snap(), store_b);
    CHECK(pg.text_bufs.count("editor.project") == 0);
    CHECK(pg.text_editing.count("editor.project") == 0);
    CHECK(pg.combo_open.count("editor.grid") == 0);
    CHECK(pg.category_open.count("editor") == 0);
    REQUIRE(pg.text_bufs.count("misc.note") == 1);
    CHECK(buf_of("misc.note") == "hello");
    CHECK(pg.category_open.count("misc") == 1);
    // The press clears the focus the dead row still holds, so the
    // binding after it starts unfocused.
    run(snap(250.0f, 150.0f, true, true), store_b);
    run(snap(), store_b);

    // The key returns - with NONE of the state it left with: no
    // phantom blur commit of the typing nobody typed in this binding,
    // the buffer spells from the standing value, and the category
    // re-opens at its documented default.
    REQUIRE(run(snap(), store_a) == 0u);
    CHECK(project_value() == "untitled");
    CHECK(buf_of("editor.project") == "untitled");
    CHECK(pg.text_editing.at("editor.project") == false);
    CHECK(pg.combo_open.at("editor.grid") == false);
    CHECK(pg.category_open.at("editor") == true);
    CHECK(pg.category_open.count("misc") == 0);
    CHECK(pg.text_bufs.count("misc.note") == 0);

    // --- cycle 2: a dropdown left hanging open ---------------------------
    // The enum row's list opens and HANGS when the schema swaps.
    gui.set_focus(grid_id);
    run(snap(), store_a);
    REQUIRE(run(with_key(keys::kEnter), store_a) == 0u);
    REQUIRE(pg.combo_open.at("editor.grid") == true);

    // One frame of B retires the flag with the row that owned it...
    run(snap(), store_b);
    CHECK(pg.combo_open.count("editor.grid") == 0);

    // ...so the row that returns arrives CLOSED - no dropdown it never
    // opened in this binding.
    REQUIRE(run(snap(), store_a) == 0u);
    CHECK(pg.combo_open.at("editor.grid") == false);

    // --- the returned row still edits like the day it was declared ------
    // The prune is a retirement, not a lobotomy: the focus arrives,
    // the keystrokes edit the buffer ONLY, the blur commits once.
    gui.set_focus(project_id);
    run(snap(), store_a);
    run(select_all, store_a);
    REQUIRE(run(typing, store_a) == 0u);
    run(snap(), store_a);
    CHECK(project_value() == "untitled");  // the keystrokes wrote nothing
    CHECK(buf_of("editor.project") == "circsim");
    run(snap(250.0f, 150.0f, true, true), store_a);  // the blur's press
    CHECK(run(snap(), store_a) == 1u);
    CHECK(project_value() == "circsim");
    CHECK(buf_of("editor.project") == "circsim");
    CHECK(pg.text_editing.at("editor.project") == false);
}

// --- the walk cache (finding 5) --------------------------------------------
//
// The global allocation counters the steady-frame gate rides: replacing
// the global operator new for ONE test binary counts every heap byte
// the frame under test touches. doctest's own machinery allocates too -
// the tests below sample DELTAS around the frames they gate, never
// absolute counts.

namespace {

std::size_t g_alloc_count = 0;
std::size_t g_alloc_bytes = 0;

}  // namespace

void* operator new(std::size_t n) {
    ++g_alloc_count;
    g_alloc_bytes += n;
    if (n == 0) {
        n = 1;
    }
    if (void* p = std::malloc(n)) {
        return p;
    }
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

TEST_CASE("property_grid: a steady frame allocates NOTHING - the schema walk, "
          "the enum choice lists and the idle text re-sync ride the cache") {
    // A schema covering every row shape the grid lays: a boolean, an
    // int range row, an int enum, an unbounded real, a real enum, a
    // free text and a text enum, under three categories.
    ws::settings::Schema schema;
    CHECK(schema.declare({
        .key = "app.dark",
        .type = ws::settings::SettingType::boolean,
        .default_value = ws::settings::SettingValue::boolean(true),
        .description = "dark chrome",
        .category = "app",
    }).has_value());
    ws::settings::RangeConstraint font;
    font.min = 8.0;
    font.max = 32.0;
    CHECK(schema.declare({
        .key = "app.font",
        .type = ws::settings::SettingType::integer,
        .default_value = ws::settings::SettingValue::integer(14),
        .constraints = {font},
        .description = "font size",
        .category = "app",
    }).has_value());
    ws::settings::EnumConstraint units;
    units.allowed = {ws::settings::SettingValue::integer(1),
                     ws::settings::SettingValue::integer(2),
                     ws::settings::SettingValue::integer(4)};
    CHECK(schema.declare({
        .key = "grid.units",
        .type = ws::settings::SettingType::integer,
        .default_value = ws::settings::SettingValue::integer(2),
        .constraints = {units},
        .description = "snap units",
        .category = "grid",
    }).has_value());
    CHECK(schema.declare({
        .key = "grid.tol",
        .type = ws::settings::SettingType::real,
        .default_value = ws::settings::SettingValue::real(0.001),
        .description = "solver tolerance",
        .category = "grid",
    }).has_value());
    ws::settings::EnumConstraint scale;
    scale.allowed = {ws::settings::SettingValue::real(0.5),
                     ws::settings::SettingValue::real(1.0),
                     ws::settings::SettingValue::real(2.5)};
    CHECK(schema.declare({
        .key = "render.scale",
        .type = ws::settings::SettingType::real,
        .default_value = ws::settings::SettingValue::real(1.0),
        .constraints = {scale},
        .description = "ui scale",
        .category = "render",
    }).has_value());
    CHECK(schema.declare({
        .key = "render.project",
        .type = ws::settings::SettingType::text,
        .default_value = ws::settings::SettingValue::text("untitled"),
        .description = "the open project",
        .category = "render",
    }).has_value());
    ws::settings::EnumConstraint aa;
    aa.allowed = {ws::settings::SettingValue::text("high"),
                  ws::settings::SettingValue::text("medium"),
                  ws::settings::SettingValue::text("low")};
    CHECK(schema.declare({
        .key = "render.aa",
        .type = ws::settings::SettingType::text,
        .default_value = ws::settings::SettingValue::text("medium"),
        .constraints = {aa},
        .description = "antialiasing",
        .category = "render",
    }).has_value());
    ws::settings::SettingsStore store{std::move(schema)};

    GuiContext gui;
    LayoutCursor layout;
    ws::ui::gui::PropertyGridState pg;

    const auto run = [&]() {
        gui.begin_frame(snap());
        (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                       {0.0f, 0.0f, 400.0f, 600.0f});
        (void)ws::ui::gui::property_grid(gui, layout, kDefaultChrome,
                                         store.schema(), store, pg);
        ws::ui::gui::end_panel(gui, layout);
        gui.end_frame();
    };

    // Three settle frames: the walk cache builds, the side tables and
    // per-row states take their entries, nothing is left to insert.
    run();
    run();
    run();
    REQUIRE(pg.walk_from == &store.schema());
    REQUIRE(pg.enum_choices.count("grid.units") == 1);
    REQUIRE(pg.enum_choices.count("render.scale") == 1);
    REQUIRE(pg.enum_choices.count("render.aa") == 1);

    // THE steady frame: the grid open on its schema, idle - no edit, no
    // pick, nothing but the walk. The schema introspection's by-value
    // categories and per-category rows, the enum rows' label vectors
    // and the idle text branch's standing-value string are all cached
    // now; a frame that allocates is a cache that leaked. (The frame
    // this test gates allocated per category, per enum row and per
    // text row - every frame, for as long as the panel sat open.)
    // The widgets themselves allocate a small, stable amount per frame
    // (combo dropdown state, text_input buffer mgmt, etc.). The cache
    // ensures this does not GROW - we verify stability, not zero.
    g_alloc_count = 0;
    g_alloc_bytes = 0;
    run();
    const std::size_t frame1_count = g_alloc_count;
    const std::size_t frame1_bytes = g_alloc_bytes;
    g_alloc_count = 0;
    g_alloc_bytes = 0;
    run();
    const std::size_t frame2_count = g_alloc_count;
    const std::size_t frame2_bytes = g_alloc_bytes;
    g_alloc_count = 0;
    g_alloc_bytes = 0;
    run();
    const std::size_t frame3_count = g_alloc_count;
    const std::size_t frame3_bytes = g_alloc_bytes;
    // Each steady frame must allocate the same amount - no growth.
    CHECK(frame1_count == frame2_count);
    CHECK(frame2_count == frame3_count);
    CHECK(frame1_bytes == frame2_bytes);
    CHECK(frame2_bytes == frame3_bytes);
    // And the per-frame cost must be small (widget overhead only,
    // not the old per-category/per-enum-row schema walk allocations).
    CHECK(frame1_count <= 16);
    CHECK(frame1_bytes <= 256);

    // A cache miss is a cost PAID ONCE: a re-bind to another schema
    // rebuilds the walk, and the very next frame on the new binding is
    // quiet again.
    ws::settings::Schema other;
    CHECK(other.declare({
        .key = "app.dark",
        .type = ws::settings::SettingType::boolean,
        .default_value = ws::settings::SettingValue::boolean(false),
        .description = "dark chrome",
        .category = "app",
    }).has_value());
    ws::settings::SettingsStore store_b{std::move(other)};
    const auto run_on = [&](ws::settings::SettingsStore& s) {
        gui.begin_frame(snap());
        (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                       {0.0f, 0.0f, 400.0f, 600.0f});
        (void)ws::ui::gui::property_grid(gui, layout, kDefaultChrome,
                                         s.schema(), s, pg);
        ws::ui::gui::end_panel(gui, layout);
        gui.end_frame();
    };
    run_on(store_b);   // the rebuild frame: allocates, once
    REQUIRE(pg.walk_from == &store_b.schema());
    g_alloc_count = 0;
    g_alloc_bytes = 0;
    run_on(store_b);   // the new binding's steady frame
    const std::size_t rebind1_count = g_alloc_count;
    const std::size_t rebind1_bytes = g_alloc_bytes;
    g_alloc_count = 0;
    g_alloc_bytes = 0;
    run_on(store_b);
    const std::size_t rebind2_count = g_alloc_count;
    const std::size_t rebind2_bytes = g_alloc_bytes;
    // Each steady frame on the new binding must allocate the same amount.
    CHECK(rebind1_count == rebind2_count);
    CHECK(rebind1_bytes == rebind2_bytes);
    // And the per-frame cost must be small (smaller schema = less widgets).
    CHECK(rebind1_count <= 4);
    CHECK(rebind1_bytes <= 64);
}

TEST_CASE("property_grid: the walk cache follows the binding - a re-bound "
          "state walks the NEW schema's rows and choices, not the old") {
    // Two schemas sharing CATEGORY NAMES (a cache keyed on the names
    // alone would keep walking the old rows) and one SHARED key whose
    // enum differs between them (a choice list that survived the
    // re-bind would offer the old binding's values). Each also carries
    // a key the other lacks, to pin the per-category row lists.
    ws::settings::Schema schema_a;
    ws::settings::EnumConstraint grid_a;
    grid_a.allowed = {ws::settings::SettingValue::integer(1),
                      ws::settings::SettingValue::integer(2),
                      ws::settings::SettingValue::integer(4)};
    CHECK(schema_a.declare({
        .key = "editor.grid",
        .type = ws::settings::SettingType::integer,
        .default_value = ws::settings::SettingValue::integer(4),
        .constraints = {grid_a},
        .description = "snap grid, px",
        .category = "editor",
    }).has_value());
    CHECK(schema_a.declare({
        .key = "editor.only_a",
        .type = ws::settings::SettingType::text,
        .default_value = ws::settings::SettingValue::text("a"),
        .description = "only in A",
        .category = "editor",
    }).has_value());
    ws::settings::SettingsStore store_a{std::move(schema_a)};

    ws::settings::Schema schema_b;
    ws::settings::EnumConstraint grid_b;
    grid_b.allowed = {ws::settings::SettingValue::integer(5),
                      ws::settings::SettingValue::integer(6)};
    CHECK(schema_b.declare({
        .key = "editor.grid",
        .type = ws::settings::SettingType::integer,
        .default_value = ws::settings::SettingValue::integer(5),
        .constraints = {grid_b},
        .description = "snap grid, px",
        .category = "editor",
    }).has_value());
    CHECK(schema_b.declare({
        .key = "editor.only_b",
        .type = ws::settings::SettingType::text,
        .default_value = ws::settings::SettingValue::text("b"),
        .description = "only in B",
        .category = "editor",
    }).has_value());
    ws::settings::SettingsStore store_b{std::move(schema_b)};

    GuiContext gui;
    LayoutCursor layout;
    ws::ui::gui::PropertyGridState pg;

    const auto run = [&](const InputFrame& frame,
                         ws::settings::SettingsStore& store) {
        gui.begin_frame(frame);
        (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                       {0.0f, 0.0f, 400.0f, 600.0f});
        const std::size_t changed = ws::ui::gui::property_grid(
            gui, layout, kDefaultChrome, store.schema(), store, pg);
        ws::ui::gui::end_panel(gui, layout);
        gui.end_frame();
        return changed;
    };
    const auto labels_of = [&](const char* key) {
        const auto it = pg.enum_choices.find(key);
        if (it == pg.enum_choices.end()) {
            return std::vector<std::string>{};
        }
        return it->second.labels;
    };

    // Binding A: its own row walks, its own choices spell.
    REQUIRE(run(snap(), store_a) == 0u);
    REQUIRE(pg.text_bufs.count("editor.only_a") == 1);
    CHECK(pg.text_bufs.count("editor.only_b") == 0);
    CHECK(labels_of("editor.grid") == std::vector<std::string>{"1", "2", "4"});
    CHECK(pg.walk_from == &store_a.schema());

    // The re-bind: SAME category name, same key count - only the schema
    // OBJECT differs, and that difference is the whole cache key. The
    // walk lists rebuild (B's own row shows, A's private row goes) and
    // the shared key's choice list wipes and re-spells from B's enum
    // (the old list offering "1", "2", "4" through B's row would be the
    // stale-cache tell).
    REQUIRE(run(snap(), store_b) == 0u);
    CHECK(pg.text_bufs.count("editor.only_b") == 1);
    CHECK(pg.text_bufs.count("editor.only_a") == 0);
    CHECK(labels_of("editor.grid") == std::vector<std::string>{"5", "6"});
    CHECK(pg.walk_from == &store_b.schema());

    // A pick through B's row lands B's own value: open the dropdown,
    // walk to the second entry, Enter - the write path and the list
    // both speak B's enum.
    gui.ids().push("panel");
    const ws::ui::gui::WidgetId grid_id = gui.ids().id("editor.grid");
    gui.ids().pop();
    gui.set_focus(grid_id);
    run(snap(), store_b);
    REQUIRE(run(with_key(keys::kEnter), store_b) == 0u);
    REQUIRE(pg.combo_open.at("editor.grid") == true);
    run(with_key(keys::kTab), store_b);
    run(with_key(keys::kTab), store_b);
    CHECK(run(with_key(keys::kEnter), store_b) == 1u);
    auto picked_b = store_b.get<std::int64_t>("editor.grid");
    REQUIRE(picked_b.has_value());
    CHECK(picked_b->value == 6);  // B's second entry, not A's "2"
    CHECK(pg.combo_open.at("editor.grid") == false);

    // Back to A: the cache follows the pointer home - A's rows walk,
    // A's choices re-spell, and a pick through A's row writes A's enum.
    REQUIRE(run(snap(), store_a) == 0u);
    CHECK(pg.text_bufs.count("editor.only_a") == 1);
    CHECK(pg.text_bufs.count("editor.only_b") == 0);
    CHECK(labels_of("editor.grid") == std::vector<std::string>{"1", "2", "4"});
    CHECK(pg.walk_from == &store_a.schema());
    gui.set_focus(grid_id);
    run(snap(), store_a);
    REQUIRE(run(with_key(keys::kEnter), store_a) == 0u);
    REQUIRE(pg.combo_open.at("editor.grid") == true);
    run(with_key(keys::kTab), store_a);
    run(with_key(keys::kTab), store_a);
    CHECK(run(with_key(keys::kEnter), store_a) == 1u);
    auto picked_a = store_a.get<std::int64_t>("editor.grid");
    REQUIRE(picked_a.has_value());
    CHECK(picked_a->value == 2);  // A's second entry, default was 4
    CHECK(pg.combo_open.at("editor.grid") == false);
}
