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
#include <cstring>
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
