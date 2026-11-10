// The key-repeat suite: the OS's auto-repeat of held keys, end to end.
// The input frame carries the repeat as its OWN edge (key_repeated,
// beside the fresh-press key_pressed - the platform layers never fold
// one into the other), and GuiContext::key_pressed_or_repeated is the
// policy read. The cases pin both halves of the contract:
//
//   * the consumers that STEP on held keys honor the repeat edge - the
//     text field's deleters and caret keys, the slider's arrows, the
//     table's row walk;
//   * the consumers of DISTINCT presses stay edge-only - a held Enter
//     never re-fires a button, a held Enter never re-picks a table
//     row, and the text field's Home/End ignore repeats;
//   * the gui never SYNTHESIZES a repeat: a plain held key with no OS
//     repeat edge moves nothing (a soft keyboard, a test frame - the
//     platforms that report no repeats simply step once per press).
//
// Same purity class as test_ui_gui: the gui archive, the real font
// bake and the r2d stub - no renderer, no window, no platform TU. The
// frames are hand-built the way the gui suite builds them, so the
// repeat edge arrives exactly as the platform layers would write it.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/input/input.hpp>

#include "ui_gui_stub.hpp"

#include <cstring>

namespace {

using ws::ui::gui::GuiContext;
using ws::input::InputFrame;
using ws::ui::gui::LayoutCursor;
using ws::ui::gui::kDefaultChrome;
namespace keys = ws::input::keys;

// The gui suite's snap(): a present mouse parked far off-panel by
// default, so no widget hover or click can interfere with the
// keyboard paths - or a tap inside the field, with the button's held
// state and edges, when the focus path needs one.
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

// One held key's frame the way the platform layer writes it: down, and
// (only when the OS says so) the repeat edge. `fresh` adds the
// pressed edge - the press frame of the same hold.
InputFrame held(int key, bool repeat, bool fresh = false) {
    InputFrame frame = snap();
    frame.key_down[static_cast<std::size_t>(key)] = true;
    if (repeat) {
        frame.key_repeated[static_cast<std::size_t>(key)] = true;
    }
    if (fresh) {
        frame.key_pressed[static_cast<std::size_t>(key)] = true;
    }
    return frame;
}

}  // namespace

TEST_CASE("key_pressed_or_repeated: the repeat edge is a first-class read") {
    GuiContext gui;

    // A repeat-only frame: the helper answers, the fresh edge does not.
    gui.begin_frame(held(keys::kRight, true));
    CHECK(gui.input().key_pressed[static_cast<std::size_t>(keys::kRight)] ==
          false);
    CHECK(gui.input().key_repeated[static_cast<std::size_t>(keys::kRight)]);
    CHECK(gui.key_pressed_or_repeated(keys::kRight));
    gui.end_frame();

    // A fresh press alone answers too - the stepping consumers read one
    // spelling for both halves of the hold.
    gui.begin_frame(held(keys::kRight, false, true));
    CHECK(gui.key_pressed_or_repeated(keys::kRight));
    gui.end_frame();

    // A plain hold with no OS repeat: neither edge, no answer - the
    // gui never invents a repeat.
    gui.begin_frame(held(keys::kRight, false));
    CHECK(!gui.key_pressed_or_repeated(keys::kRight));
    gui.end_frame();

    // consume_key owns the hold whole: the pressed edge and the repeat
    // edge clear together, so a later reader this frame sees neither.
    gui.begin_frame(held(keys::kRight, true, true));
    gui.consume_key(keys::kRight);
    CHECK(!gui.key_pressed_or_repeated(keys::kRight));
    CHECK(gui.input().key_pressed[static_cast<std::size_t>(keys::kRight)] ==
          false);
    CHECK(gui.input().key_repeated[static_cast<std::size_t>(keys::kRight)] ==
          false);
    gui.end_frame();

    // Out-of-range codes answer false - the drop-safe shape every key
    // read in the context already rides.
    CHECK(!gui.key_pressed_or_repeated(-1));
    CHECK(!gui.key_pressed_or_repeated(ws::input::InputFrame::kMaxKeys));
}

TEST_CASE("a held Enter never re-fires: activation stays edge-only") {
    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 120.0f, 28.0f};

    // The focused button fires on the fresh Enter press...
    gui.begin_frame(held(keys::kEnter, false, true));
    gui.set_focus(gui.ids().id("fire"));
    const ws::ui::gui::ButtonResult fired = ws::ui::gui::button_behavior(
        gui, gui.ids().id("fire"), rect);
    gui.end_frame();
    CHECK(fired.key_clicked);

    // ...and not on the hold's repeats: one activation per press, the
    // way every activation key in the gui reads.
    gui.begin_frame(held(keys::kEnter, true));
    gui.set_focus(gui.ids().id("fire"));
    const ws::ui::gui::ButtonResult repeat = ws::ui::gui::button_behavior(
        gui, gui.ids().id("fire"), rect);
    gui.end_frame();
    CHECK(!repeat.key_clicked);
    CHECK(!repeat.activated());

    // Space repeats the same rule: edge-only, never a held re-fire.
    gui.begin_frame(held(keys::kSpace, true));
    gui.set_focus(gui.ids().id("fire"));
    const ws::ui::gui::ButtonResult space = ws::ui::gui::button_behavior(
        gui, gui.ids().id("fire"), rect);
    gui.end_frame();
    CHECK(!space.key_clicked);
}

TEST_CASE("slider: held arrows keep stepping on the OS repeat") {
    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 120.0f, 28.0f};
    const auto pass = [&](const InputFrame& frame, float& value) {
        gui.begin_frame(frame);
        gui.set_focus(gui.ids().id("step"));
        const ws::ui::gui::SliderResult result = ws::ui::gui::slider_behavior(
            gui, gui.ids().id("step"), rect, value, 0.0f, 100.0f, 2.5f);
        gui.end_frame();
        return result.changed;
    };

    // The press frame steps once...
    float value = 10.0f;
    CHECK(pass(held(keys::kRight, false, true), value));
    CHECK(value == 12.5f);

    // ...the hold's repeat frames keep stepping, one key_step each...
    CHECK(pass(held(keys::kRight, true), value));
    CHECK(value == 15.0f);
    CHECK(pass(held(keys::kRight, true), value));
    CHECK(value == 17.5f);

    // ...a plain hold with no OS repeat steps nothing, and the
    // repeat of the OTHER direction steps its own way.
    CHECK(!pass(held(keys::kRight, false), value));
    CHECK(value == 17.5f);
    CHECK(pass(held(keys::kLeft, true), value));
    CHECK(value == 15.0f);
}

TEST_CASE("table: held arrows walk rows on repeats; a held Enter never re-picks") {
    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    LayoutCursor layout;
    static const ws::ui::gui::TableColumn cols[] = {{"name", 100.0f},
                                                    {"value", 0.0f}};
    static const char* const cells[] = {"alpha", "1", "beta", "2",
                                       "gamma", "3"};
    int selected = 0;
    bool changed = false;

    const auto row_w = [&](int i) {
        gui.ids().push("grid");
        gui.ids().push(i);
        const ws::ui::gui::WidgetId w = gui.ids().id("row");
        gui.ids().pop();
        gui.ids().pop();
        return w;
    };
    const auto pass = [&](const InputFrame& frame) {
        ws::ui::gui_test::stub.reset();
        gui.begin_frame(frame);
        (void)layout.begin_panel({0.0f, 0.0f, 300.0f, 100.0f}, 0.0f);
        changed = ws::ui::gui::table(gui, layout, kDefaultChrome, "grid",
                                     cols, 2, cells, 3, &selected, 300.0f);
        layout.end_panel();
        gui.end_frame();
    };

    // A tap on the last row picks it and latches the focus.
    pass(snap(150.0f, 86.0f, false, true, true));
    CHECK(selected == 2);
    CHECK(gui.is_focused(row_w(2)));

    // The hold's repeats walk the focus row by row - the press frame
    // stepped already (the tap's), these are pure repeat frames.
    pass(held(keys::kUp, true));
    CHECK(gui.is_focused(row_w(1)));
    pass(held(keys::kUp, true));
    CHECK(gui.is_focused(row_w(0)));

    // A FRESH Down press steps exactly one row (0 -> 1, not the grid's
    // end): the walk's one-step gate - the landing row's own fold
    // re-captures the live focus, so without the gate a single Down
    // would cascade to the bottom in one frame. Fresh presses and OS
    // repeats ride the same gate.
    pass(held(keys::kDown, false, true));
    CHECK(gui.is_focused(row_w(1)));

    // A repeated Down steps one more, the same single row.
    pass(held(keys::kDown, true));
    CHECK(gui.is_focused(row_w(2)));

    // A plain hold with no OS repeat moves nothing.
    pass(held(keys::kDown, false));
    CHECK(gui.is_focused(row_w(2)));

    // Walk back up so the focused row differs from the selection, then
    // a held Enter never re-picks: the selection holds, no change - a
    // wrongly honored repeat would pick row 1.
    pass(held(keys::kUp, true));
    CHECK(gui.is_focused(row_w(1)));
    CHECK(selected == 2);
    pass(held(keys::kEnter, true));
    CHECK(selected == 2);
    CHECK(!changed);
}

TEST_CASE("text_input: held deleters and caret keys honor the repeat edge") {
    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    LayoutCursor layout;

    char buf[16] = "";
    const auto run_field = [&](const InputFrame& frame) -> bool {
        gui.begin_frame(frame);
        (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                       {0.0f, 0.0f, 400.0f, 300.0f});
        const bool edited = ws::ui::gui::text_input(
            gui, layout, kDefaultChrome, "field", buf, sizeof buf);
        ws::ui::gui::end_panel(gui, layout);
        gui.end_frame();
        return edited;
    };

    // Focus the field with a press inside it, then type three bytes:
    // the caret lands at the end, "abc".
    CHECK(!run_field(snap(60.0f, 20.0f, true, true)));
    CHECK(!run_field(snap(60.0f, 20.0f, false, false, true)));
    InputFrame typing = snap();
    typing.text_input = {'a', 'b', 'c'};
    CHECK(run_field(typing));
    CHECK(std::strcmp(buf, "abc") == 0);

    // The hold's repeats keep deleting: two pure repeat frames of
    // Backspace eat two bytes.
    CHECK(run_field(held(keys::kBackspace, true)));
    CHECK(std::strcmp(buf, "ab") == 0);
    CHECK(run_field(held(keys::kBackspace, true)));
    CHECK(std::strcmp(buf, "a") == 0);

    // A plain hold with no OS repeat deletes nothing.
    CHECK(!run_field(held(keys::kBackspace, false)));
    CHECK(std::strcmp(buf, "a") == 0);

    // Rebuild "ab" (caret at the end), then a repeated Left walks the
    // caret back one - the next typed byte lands at the walked
    // position, not the end.
    InputFrame more = snap();
    more.text_input = {'b'};
    CHECK(run_field(more));
    CHECK(std::strcmp(buf, "ab") == 0);
    CHECK(!run_field(held(keys::kLeft, true)));  // a caret move is not an edit
    InputFrame land = snap();
    land.text_input = {'z'};
    CHECK(run_field(land));
    CHECK(std::strcmp(buf, "azb") == 0);  // between a and b, not appended

    // Home/End stay edge-only: a repeated End does not jump the caret,
    // so the next byte still lands at the caret's walked spot. (The
    // caret sits at 2 after the insert; a honored End repeat would
    // append at 3 instead.)
    CHECK(!run_field(held(keys::kEnd, true)));
    InputFrame tail = snap();
    tail.text_input = {'q'};
    CHECK(run_field(tail));
    CHECK(std::strcmp(buf, "azqb") == 0);

    // And the deleters' repeat works mid-string too: the caret is at 3
    // ("azq|b"), a repeated Delete eats forward.
    CHECK(run_field(held(keys::kDelete, true)));
    CHECK(std::strcmp(buf, "azq") == 0);
}
