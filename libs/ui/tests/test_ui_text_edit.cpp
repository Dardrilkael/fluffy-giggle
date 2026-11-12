// The text-editing engine suite: the pieces text_input and the new
// multi-line text_editor SHARE, gated directly and end to end. First
// the engine's own arithmetic (the word walk, the line walk), then
// the history (coalescing, the roundtrip, the staleness guard, the
// bounds), then the single-line field's new keys through
// text_input_behavior (Ctrl+arrows, Ctrl+Backspace/Delete, undo/redo,
// Up/Down as Home/End), and the editor's vertical half through
// text_editor_behavior (Enter, the goal column, the line/documented
// Home/End pair, the click placement, the wheel and the
// caret-follow), with the draws asserted through the r2d stub's
// counters - the same purity class as test_ui_gui: the gui archive,
// the real font bake, the stub seam; no renderer, no window, no
// platform TU.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/painters.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/text_edit_core.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/input/input.hpp>

#include "ui_gui_stub.hpp"

#include <array>
#include <cstring>
#include <string>

namespace {

using ws::ui::gui::GuiContext;
using ws::input::InputFrame;
using ws::ui::gui::LayoutCursor;
using ws::ui::gui::kDefaultChrome;
namespace keys = ws::input::keys;

// The gui suite's snap(): a present mouse parked far off-panel by
// default, with the primary button's held state and optional
// press/release edges.
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
InputFrame type(std::string_view text) {
    InputFrame frame = snap();
    for (char c : text) {
        frame.text_input.push_back(static_cast<std::uint32_t>(c));
    }
    return frame;
}

// A frame with Ctrl held and the named key freshly pressed.
InputFrame ctrl_press(int key) {
    InputFrame frame = snap();
    frame.key_down[static_cast<std::size_t>(keys::kLeftControl)] = true;
    frame.key_pressed[static_cast<std::size_t>(key)] = true;
    return frame;
}

}  // namespace

// --- the word walk -------------------------------------------------------------

TEST_CASE("the word walk names the run boundaries: words, punct, spaces") {
    using ws::ui::gui::word_start;
    using ws::ui::gui::word_end;

    // "foo, bar": f0 o1 o2 ','3 ' '4 b5 a6 r7. The stops (where a
    // Ctrl+walk can land) read 0, 3, 4, 5, 8 - the VS Code school,
    // where the comma run is a stop of its own and spaces skip.
    const char* const text = "foo, bar";
    const int len = 8;
    CHECK(word_start(text, len, 8) == 5);  // the end -> "bar"'s start
    CHECK(word_start(text, len, 5) == 3);  // "bar"'s start -> the comma's
                                           // (the space skipped on the way)
    CHECK(word_start(text, len, 4) == 3);  // the space's start -> the comma's
    CHECK(word_start(text, len, 3) == 0);  // the comma's start -> "foo"'s
    CHECK(word_start(text, len, 0) == 0);  // the top stays the top
    CHECK(word_end(text, len, 0) == 3);    // "foo"'s start -> its end
    CHECK(word_end(text, len, 3) == 4);    // the comma's start -> its end
    CHECK(word_end(text, len, 4) == 8);    // the space's start -> "bar"'s end
    CHECK(word_end(text, len, 5) == 8);    // "bar"'s start -> its end
    CHECK(word_end(text, len, 8) == 8);    // the end stays the end
    // Mid-word bytes land on the run's ends, not their own cluster.
    CHECK(word_start(text, len, 2) == 0);
    CHECK(word_end(text, len, 6) == 8);
}

TEST_CASE("the word walk rides multi-byte clusters whole") {
    using ws::ui::gui::word_end;

    // "aé b": 'a' (1 byte) + é (2 bytes, word class) + space + 'b'.
    const char* const text = "a\xc3\xa9 b";
    const int len = 5;
    CHECK(word_end(text, len, 0) == 3);  // "aé" walks as ONE word
    CHECK(word_end(text, len, 3) == 5);  // past the space: "b"'s end
    // The newline is a space-class stop: across a break the walk
    // treats it like any run boundary.
    const char* const lines = "ab\ncd";
    CHECK(ws::ui::gui::word_start(lines, 5, 5) == 3);
    CHECK(ws::ui::gui::word_start(lines, 5, 3) == 0);
    CHECK(ws::ui::gui::word_end(lines, 5, 2) == 5);
}

// --- the line walk -------------------------------------------------------------

TEST_CASE("the line walk bounds the '\n'-delimited lines") {
    using ws::ui::gui::line_count;
    using ws::ui::gui::line_end;
    using ws::ui::gui::line_index;
    using ws::ui::gui::line_start;
    using ws::ui::gui::line_start_of;

    // "ab\ncd\n": a0 b1 '\n'2 c3 d4 '\n'5 - three lines: "ab", "cd"
    // and the empty tail the last break leaves behind.
    const char* const text = "ab\ncd\n";
    const int len = 6;
    CHECK(line_count(text, len) == 3);
    CHECK(line_start(text, len, 0) == 0);
    CHECK(line_start(text, len, 2) == 0);   // the '\n' belongs to line 0
    CHECK(line_start(text, len, 3) == 3);
    CHECK(line_start(text, len, 5) == 3);   // the tail '\n' is line 1's
    CHECK(line_start(text, len, 6) == 6);   // the empty tail line
    CHECK(line_end(text, len, 0) == 2);     // the '\n' is NOT part of it
    CHECK(line_end(text, len, 3) == 5);
    CHECK(line_end(text, len, 6) == 6);     // the empty tail ends at once
    CHECK(line_index(text, len, 0) == 0);
    CHECK(line_index(text, len, 4) == 1);
    CHECK(line_index(text, len, 6) == 2);
    CHECK(line_start_of(text, len, 0) == 0);
    CHECK(line_start_of(text, len, 1) == 3);
    CHECK(line_start_of(text, len, 2) == 6);
    CHECK(line_start_of(text, len, 9) == 6);  // past the end clamps
}

// --- the history -----------------------------------------------------------------

TEST_CASE("the history records a typing run word by word") {
    char storage[16] = "";
    ws::ui::gui::TextInputState edit;
    ws::ui::gui::TextEditHistory& h = edit.history;

    // Type "hi yo": "hi" merges (a word run), the space stands alone,
    // "yo" merges - three entries, the editors' undo granularity.
    const auto type_cp = [&](char c) {
        ws::ui::gui::EditBuffer eb{storage, 15,
                                   static_cast<int>(std::strlen(storage)),
                                   edit.caret, edit.anchor};
        const ws::ui::gui::TextEditCoalesce kind =
            ws::ui::gui::word_class(c) == ws::ui::gui::TextWordClass::word
                ? ws::ui::gui::TextEditCoalesce::typing
                : ws::ui::gui::TextEditCoalesce::none;
        const int pen = h.replace(eb, eb.sel_lo(), eb.sel_hi(), &c, 1, kind);
        edit.caret = edit.anchor = pen;
    };
    type_cp('h');
    type_cp('i');
    type_cp(' ');
    type_cp('y');
    type_cp('o');
    CHECK(std::strcmp(storage, "hi yo") == 0);
    REQUIRE(h.undo_depth() == 3);

    // Undo walks the run back a step at a time, the caret restoring
    // to each step's far side.
    const auto undo_once = [&]() {
        ws::ui::gui::EditBuffer eb{storage, 15,
                                   static_cast<int>(std::strlen(storage)),
                                   edit.caret, edit.anchor};
        REQUIRE(h.undo(eb));
        edit.caret = eb.caret;
        edit.anchor = eb.anchor;
    };
    undo_once();
    CHECK(std::strcmp(storage, "hi ") == 0);
    CHECK(edit.caret == 3);
    undo_once();
    CHECK(std::strcmp(storage, "hi") == 0);
    undo_once();
    CHECK(std::strcmp(storage, "") == 0);
    CHECK(edit.caret == 0);
    CHECK(!h.can_undo());
}

TEST_CASE("the history coalesces word runs and undoes them as one step") {
    char storage[16] = "";
    ws::ui::gui::TextInputState edit;
    ws::ui::gui::TextEditHistory& h = edit.history;

    // "hey" is one word run: three replaces, one entry.
    for (char c : {'h', 'e', 'y'}) {
        ws::ui::gui::EditBuffer eb{storage, 15,
                                   static_cast<int>(std::strlen(storage)),
                                   edit.caret, edit.anchor};
        (void)h.replace(eb, eb.sel_lo(), eb.sel_hi(), &c, 1,
                        ws::ui::gui::TextEditCoalesce::typing);
        edit.caret = edit.anchor = static_cast<int>(std::strlen(storage));
    }
    CHECK(std::strcmp(storage, "hey") == 0);
    REQUIRE(h.undo_depth() == 1);

    // Undo: the whole run leaves, the caret restores to where it
    // stood before the first letter.
    ws::ui::gui::EditBuffer eb{storage, 15,
                               static_cast<int>(std::strlen(storage)),
                               edit.caret, edit.anchor};
    CHECK(h.undo(eb));
    CHECK(eb.len == 0);
    CHECK(eb.caret == 0);
    CHECK(eb.anchor == 0);
    CHECK(!h.can_undo());
    CHECK(h.can_redo());

    // Redo: the run lands again, the caret at its end.
    CHECK(h.redo(eb));
    CHECK(eb.len == 3);
    CHECK(std::strcmp(eb.buf, "hey") == 0);
    CHECK(eb.caret == 3);

    // A new edit after the undo clears the redo stack (the standard).
    CHECK(h.undo(eb));
    char c = 'x';
    (void)h.replace(eb, eb.sel_lo(), eb.sel_hi(), &c, 1,
                    ws::ui::gui::TextEditCoalesce::typing);
    CHECK(std::strcmp(eb.buf, "x") == 0);
    CHECK(!h.can_redo());
}

TEST_CASE("the history merges a held backspace run into one step") {
    char storage[16] = "abc";
    ws::ui::gui::TextInputState edit;
    ws::ui::gui::TextEditHistory& h = edit.history;
    edit.caret = edit.anchor = 3;

    for (int i = 0; i < 3; ++i) {
        ws::ui::gui::EditBuffer eb{storage, 15,
                                   static_cast<int>(std::strlen(storage)),
                                   edit.caret, edit.anchor};
        const int lo = eb.cluster_start(eb.caret);
        (void)h.replace(eb, lo, eb.caret, nullptr, 0,
                        ws::ui::gui::TextEditCoalesce::deletion_back);
        edit.caret = edit.anchor = lo;
    }
    CHECK(std::strcmp(storage, "") == 0);
    REQUIRE(h.undo_depth() == 1);  // one gesture, one step

    ws::ui::gui::EditBuffer eb{storage, 15, 0, edit.caret, edit.anchor};
    CHECK(h.undo(eb));
    CHECK(std::strcmp(eb.buf, "abc") == 0);
    CHECK(eb.caret == 3);
}

TEST_CASE("the history refuses a stale buffer and drops itself") {
    char storage[16] = "hello";
    ws::ui::gui::TextInputState edit;
    ws::ui::gui::TextEditHistory& h = edit.history;

    ws::ui::gui::EditBuffer eb{storage, 15, 5, 5, 5};
    char c = '!';
    (void)h.replace(eb, 5, 5, &c, 1, ws::ui::gui::TextEditCoalesce::none);
    CHECK(std::strcmp(storage, "hello!") == 0);
    REQUIRE(h.can_undo());

    // The caller re-spells the buffer between frames (a commit, a
    // sync): the lengths no longer match, undo answers false and the
    // history voids itself rather than splice into unseen text.
    std::memcpy(storage, "hey", 4);
    ws::ui::gui::EditBuffer stale{storage, 15, 3, 3, 3};
    CHECK(!h.undo(stale));
    CHECK(!h.can_undo());
    CHECK(!h.can_redo());
    CHECK(std::strcmp(storage, "hey") == 0);  // untouched
}

TEST_CASE("the history bounds itself: the oldest steps drop") {
    char storage[64] = "";
    ws::ui::gui::TextInputState edit;
    ws::ui::gui::TextEditHistory& h = edit.history;

    // 200 distinct (non-coalescing) pastes: every one lands at 0 with
    // a fresh word, so each stands alone and the depth caps.
    for (int i = 0; i < 200; ++i) {
        ws::ui::gui::EditBuffer eb{storage, 63,
                                   static_cast<int>(std::strlen(storage)),
                                   edit.caret, edit.anchor};
        (void)h.replace(eb, 0, eb.len, "w", 1,
                        ws::ui::gui::TextEditCoalesce::none);
    }
    CHECK(h.undo_depth() <= ws::ui::gui::TextEditHistory::kMaxEntries);
}

// --- the single-line field's new keys --------------------------------------------

TEST_CASE("the field walks words with Ctrl+arrows and eats them with "
          "Ctrl+Backspace/Delete") {
    using ws::ui::gui::TextInputResult;
    using ws::ui::gui::TextInputState;

    GuiContext gui;
    char buf[32] = "";
    TextInputState edit;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 176.0f, 24.0f};
    const ws::ui::gui::WidgetId id = gui.ids().id("field");

    const auto run = [&](InputFrame frame) -> TextInputResult {
        gui.begin_frame(frame);
        const TextInputResult out = ws::ui::gui::text_input_behavior(
            gui, id, rect, buf, static_cast<int>(sizeof buf), edit,
            kDefaultChrome);
        gui.end_frame();
        return out;
    };

    run(snap(20.0f, 12.0f, true, true));
    run(snap(60.0f, 12.0f, false, false, true));
    (void)run(type("foo bar"));

    // Ctrl+Left from the end: the caret lands on "bar"'s start.
    run(ctrl_press(keys::kLeft));
    CHECK(edit.caret == 4);
    CHECK(edit.anchor == 4);

    // Ctrl+Left again: "foo"'s start; Ctrl+Right: "foo"'s end.
    run(ctrl_press(keys::kLeft));
    CHECK(edit.caret == 0);
    run(ctrl_press(keys::kRight));
    CHECK(edit.caret == 3);

    // Ctrl+Backspace at the end eats "bar" whole; undo gives it back.
    InputFrame end_frame = snap();
    end_frame.key_pressed[static_cast<std::size_t>(keys::kEnd)] = true;
    (void)run(end_frame);
    CHECK(edit.caret == 7);
    InputFrame word_bs = ctrl_press(keys::kBackspace);
    word_bs.key_repeated[static_cast<std::size_t>(keys::kBackspace)] = false;
    const TextInputResult eaten = run(word_bs);
    CHECK(eaten.edited);
    CHECK(std::strcmp(buf, "foo ") == 0);
    CHECK(edit.caret == 4);

    // Ctrl+Z restores, Ctrl+Y (and Ctrl+Shift+Z) redo.
    (void)run(ctrl_press(keys::kZ));
    CHECK(std::strcmp(buf, "foo bar") == 0);
    CHECK(edit.caret == 7);
    (void)run(ctrl_press(keys::kY));
    CHECK(std::strcmp(buf, "foo ") == 0);
    (void)run(ctrl_press(keys::kZ));
    CHECK(std::strcmp(buf, "foo bar") == 0);
    InputFrame shift_z = ctrl_press(keys::kZ);
    shift_z.key_down[static_cast<std::size_t>(keys::kLeftShift)] = true;
    (void)run(shift_z);
    CHECK(std::strcmp(buf, "foo ") == 0);

    // Up/Down read as Home/End on a single line (the classic mapping).
    InputFrame up = snap();
    up.key_pressed[static_cast<std::size_t>(keys::kUp)] = true;
    (void)run(up);
    CHECK(edit.caret == 0);
    InputFrame down = snap();
    down.key_pressed[static_cast<std::size_t>(keys::kDown)] = true;
    (void)run(down);
    CHECK(edit.caret == 4);

    // Ctrl+Delete eats forward from the caret: "foo " at 0 loses "foo".
    InputFrame home = snap();
    home.key_pressed[static_cast<std::size_t>(keys::kHome)] = true;
    (void)run(home);
    const TextInputResult fwd = run(ctrl_press(keys::kDelete));
    CHECK(fwd.edited);
    CHECK(std::strcmp(buf, " ") == 0);
}

TEST_CASE("the field's undo restores a replaced selection") {
    using ws::ui::gui::TextInputState;

    GuiContext gui;
    char buf[32] = "";
    TextInputState edit;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 176.0f, 24.0f};
    const ws::ui::gui::WidgetId id = gui.ids().id("sel");

    const auto run = [&](InputFrame frame) {
        gui.begin_frame(frame);
        (void)ws::ui::gui::text_input_behavior(
            gui, id, rect, buf, static_cast<int>(sizeof buf), edit,
            kDefaultChrome);
        gui.end_frame();
    };

    run(snap(20.0f, 12.0f, true, true));
    run(snap(60.0f, 12.0f, false, false, true));
    (void)run(type("notes"));
    (void)run(ctrl_press(keys::kA));  // select all
    CHECK(edit.caret == 5);
    CHECK(edit.anchor == 0);
    (void)run(type("ok"));  // replaces the selection
    CHECK(std::strcmp(buf, "ok") == 0);

    // The replacement and the typing that followed COALESCE (one
    // gesture): one undo gives the selection back with its text -
    // the detail that makes undo feel like undo - and the second
    // takes the original typing away.
    (void)run(ctrl_press(keys::kZ));
    CHECK(std::strcmp(buf, "notes") == 0);
    CHECK(edit.caret == 5);
    CHECK(edit.anchor == 0);
    (void)run(ctrl_press(keys::kZ));
    CHECK(std::strcmp(buf, "") == 0);
    CHECK(!edit.history.can_undo());
    // Redo walks forward again, one gesture at a time. The redone
    // step lands the pair where TYPING left it (collapsed at the
    // end) - the Ctrl+A that selected the text was no edit, so redo
    // does not restore its selection.
    (void)run(ctrl_press(keys::kY));
    CHECK(std::strcmp(buf, "notes") == 0);
    CHECK(edit.caret == 5);
    CHECK(edit.anchor == 5);
}

// --- the editor -------------------------------------------------------------------

TEST_CASE("the editor breaks lines, walks them with the goal column and "
          "undoes the break") {
    using ws::ui::gui::TextEditorResult;
    using ws::ui::gui::TextEditorState;

    GuiContext gui;
    char buf[64] = "";
    TextEditorState state;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 200.0f, 100.0f};
    const ws::ui::gui::WidgetId id = gui.ids().id("editor");

    const auto run = [&](InputFrame frame) -> TextEditorResult {
        gui.begin_frame(frame);
        const TextEditorResult out = ws::ui::gui::text_editor_behavior(
            gui, id, rect, buf, static_cast<int>(sizeof buf), state,
            kDefaultChrome);
        gui.end_frame();
        return out;
    };
    const auto press = [&](int key) {
        InputFrame frame = snap();
        frame.key_pressed[static_cast<std::size_t>(key)] = true;
        return frame;
    };

    // Focus, then "ab", Enter, "12345".
    run(snap(20.0f, 12.0f, true, true));
    run(snap(60.0f, 12.0f, false, false, true));
    (void)run(type("ab"));
    const TextEditorResult broke = run(press(keys::kEnter));
    CHECK(broke.edited);
    CHECK(broke.ctrl_enter == false);  // plain Enter is the editor's
    (void)run(type("12345"));
    CHECK(std::strcmp(buf, "ab\n12345") == 0);

    // Up: the goal column is the caret's x on the SHORT line's walk -
    // the caret stood at line 1's end (30px), so it lands at "ab"'s
    // end (byte 2), not byte 5.
    const TextEditorResult up = run(press(keys::kUp));
    CHECK(state.field.caret == 2);
    CHECK(up.caret_line == 0);
    CHECK(state.goal_x >= 0.0f);  // the walk remembers the column

    // Down: the remembered column (line 1's end) lands the caret back
    // at byte 8.
    (void)run(press(keys::kDown));
    CHECK(state.field.caret == 8);

    // A Left step kills the goal; the next Up re-derives it from the
    // caret's own x (byte 7 = 24px into line 1), which clamps to line
    // 0's end.
    (void)run(press(keys::kLeft));
    CHECK(state.field.caret == 7);
    CHECK(state.goal_x < 0.0f);
    (void)run(press(keys::kUp));
    CHECK(state.field.caret == 2);

    // Home/End bound the LINE; Ctrl+Home/End the document.
    InputFrame ctrl_home = ctrl_press(keys::kHome);
    (void)run(ctrl_home);
    CHECK(state.field.caret == 0);
    (void)run(press(keys::kEnd));  // line 0's end
    CHECK(state.field.caret == 2);
    InputFrame ctrl_end = ctrl_press(keys::kEnd);
    (void)run(ctrl_end);
    CHECK(state.field.caret == 8);
    (void)run(press(keys::kHome));  // line 1's start
    CHECK(state.field.caret == 3);

    // Undo walks back: "12345" (a word run), then the break, then "ab".
    (void)run(ctrl_press(keys::kZ));
    CHECK(std::strcmp(buf, "ab\n") == 0);
    (void)run(ctrl_press(keys::kZ));
    CHECK(std::strcmp(buf, "ab") == 0);
    (void)run(ctrl_press(keys::kZ));
    CHECK(std::strcmp(buf, "") == 0);
    CHECK(!state.field.history.can_undo());

    // Ctrl+Enter passes through untouched - the app's submit chord,
    // never the editor's: nothing lands, the edge reports true.
    const TextEditorResult chord = run(ctrl_press(keys::kEnter));
    CHECK(chord.ctrl_enter);
    CHECK(std::strcmp(buf, "") == 0);
}

TEST_CASE("the editor places the caret by click, and the wheel scrolls "
          "without the follow fighting back") {
    using ws::ui::gui::TextEditorResult;
    using ws::ui::gui::TextEditorState;

    GuiContext gui;
    char buf[128] = "";
    TextEditorState state;
    // 200x40, fontless: line_h = control_height/2 = 14, the inner view
    // is 16px tall - a tight box the six-line content overflows.
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 200.0f, 40.0f};
    const ws::ui::gui::WidgetId id = gui.ids().id("scrolled");

    const auto run = [&](InputFrame frame) -> TextEditorResult {
        gui.begin_frame(frame);
        const TextEditorResult out = ws::ui::gui::text_editor_behavior(
            gui, id, rect, buf, static_cast<int>(sizeof buf), state,
            kDefaultChrome);
        gui.end_frame();
        return out;
    };
    const auto press = [&](int key) {
        InputFrame frame = snap();
        frame.key_pressed[static_cast<std::size_t>(key)] = true;
        return frame;
    };

    run(snap(20.0f, 12.0f, true, true));
    run(snap(60.0f, 12.0f, false, false, true));

    // Six two-char lines: "l0\nl1\nl2\nl3\nl4\nl5" (17 bytes), the
    // caret landing at the buffer's end, line 5. The follow pinned the
    // scroll to the bottom along the way.
    for (int i = 0; i < 6; ++i) {
        (void)run(type("l" + std::to_string(i)));
        if (i < 5) {
            (void)run(press(keys::kEnter));
        }
    }
    CHECK(std::strcmp(buf, "l0\nl1\nl2\nl3\nl4\nl5") == 0);
    CHECK(state.field.caret == 17);
    const TextEditorResult bottom = run(snap());
    CHECK(bottom.content_h == 6.0f * 14.0f);
    CHECK(bottom.scroll_y == bottom.content_h - bottom.view_h);

    // A click on the visible band places the caret by (line, x): the
    // view is pinned at the bottom (68px), so y 26 names line 5, and
    // x 15 (3px into the content) lands one cluster into "l5".
    (void)run(snap(15.0f, 26.0f, true, true));
    CHECK(state.field.caret == 16);
    CHECK(state.goal_x < 0.0f);  // a placed caret re-derives the goal

    // The wheel away from the caret: the claim settles one frame late
    // (the scroll panel's rule) and resolves against the POINTER, so
    // the notches ride with the mouse parked over the box. The follow
    // does NOT drag the scroll back on quiet frames (the follow_line
    // memory; a same-line caret never fights the wheel).
    InputFrame wheel_up = snap(100.0f, 20.0f);  // over the box
    wheel_up.scroll_dy = 1.0f;  // up: earlier content
    (void)run(wheel_up);        // the settle frame
    const TextEditorResult scrolled = run(wheel_up);
    CHECK(scrolled.scroll_y < bottom.scroll_y);
    const TextEditorResult quiet = run(snap(100.0f, 20.0f));
    CHECK(quiet.scroll_y == scrolled.scroll_y);

    // Many notches clamp at the content's top.
    for (int i = 0; i < 30; ++i) {
        (void)run(wheel_up);
    }
    CHECK(run(snap()).scroll_y == 0.0f);

    // A caret that moves lines pulls the view back: Ctrl+Home walks
    // the caret to line 0 and the follow brings it on screen.
    InputFrame ctrl_home = ctrl_press(keys::kHome);
    const TextEditorResult followed = run(ctrl_home);
    CHECK(state.field.caret == 0);
    CHECK(followed.scroll_y == 0.0f);
    CHECK(followed.caret_y == 12.0f);  // the content top, on screen
}

TEST_CASE("the editor draws its lines, bands and thumb through the stub") {
    using ws::ui::gui::TextEditorResult;
    using ws::ui::gui::TextEditorState;

    std::array<ws::ui::r2d::Font::Glyph, 95> glyphs{};
    for (ws::ui::r2d::Font::Glyph& glyph : glyphs) {
        glyph.advance = 10.0f;
    }
    const ws::ui::r2d::Font font(16.0f, 20.0f, glyphs.data(), 95);
    ws::ui::gui::ChromeTheme themed = kDefaultChrome;
    themed.font = &font;

    GuiContext gui;
    char buf[64] = "ab\ncd";
    TextEditorState state;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 200.0f, 64.0f};
    const ws::ui::gui::WidgetId id = gui.ids().id("drawn");

    const auto run = [&](InputFrame frame) -> TextEditorResult {
        gui.begin_frame(frame);
        const TextEditorResult out = ws::ui::gui::text_editor_behavior(
            gui, id, rect, buf, static_cast<int>(sizeof buf), state, themed);
        ws::ui::gui::text_editor_draw(rect, out, themed, "notes", buf,
                                      static_cast<int>(sizeof buf));
        gui.end_frame();
        return out;
    };

    run(snap(20.0f, 12.0f, true, true));
    run(snap(60.0f, 12.0f, false, false, true));  // the focus settled

    // One quiet focused frame: the chrome (ring + face), the two
    // lines as two text runs, the caret quad - the content fits, so
    // no thumb draws. The focus click parked the caret mid "ab"
    // (line 0, one 10px glyph in).
    ws::ui::gui_test::stub.reset();
    const TextEditorResult drawn = run(snap());
    CHECK(ws::ui::gui_test::stub.rounded == 2);
    CHECK(ws::ui::gui_test::stub.texts == 2);
    CHECK(ws::ui::gui_test::stub.last_text == std::string("cd"));
    CHECK(ws::ui::gui_test::stub.quads == 1);  // the caret
    CHECK(drawn.caret_line == 0);
    CHECK(drawn.caret_y == 12.0f);             // line 0, unscrolled
    CHECK(drawn.caret_x == 22.0f);             // 12 + one 10px glyph

    // Select all: the bands draw per line (2 quads beside the caret).
    ws::ui::gui_test::stub.reset();
    (void)run(ctrl_press(keys::kA));
    CHECK(ws::ui::gui_test::stub.texts == 2);
    CHECK(ws::ui::gui_test::stub.quads == 3);  // 2 line bands + caret
}

TEST_CASE("the dead editor rides a click and keys without pretending") {
    using ws::ui::gui::TextEditorState;

    GuiContext gui;
    TextEditorState state;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 200.0f, 64.0f};
    const ws::ui::gui::WidgetId id = gui.ids().id("dead");

    gui.begin_frame(snap(20.0f, 12.0f, true, true));
    const ws::ui::gui::TextEditorResult dead =
        ws::ui::gui::text_editor_behavior(
            gui, id, rect, nullptr, 1, state, kDefaultChrome);
    gui.end_frame();
    CHECK(!dead.edited);
    CHECK(dead.preedit_text == nullptr);

    gui.begin_frame(type("x"));
    const ws::ui::gui::TextEditorResult quiet =
        ws::ui::gui::text_editor_behavior(
            gui, id, rect, nullptr, 1, state, kDefaultChrome);
    gui.end_frame();
    CHECK(!quiet.edited);
    CHECK(!quiet.ctrl_enter);
}
