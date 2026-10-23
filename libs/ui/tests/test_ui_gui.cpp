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
#include <ws/ui/gui/job_progress.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/input/input.hpp>
#include <ws/jobs/errors.hpp>
#include <ws/jobs/job_system.hpp>
#include <ws/ui/renderer2d.hpp>

#include "ui_gui_stub.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace {

using ws::ui::gui::GuiContext;
using ws::input::InputFrame;
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

TEST_CASE("text_input edits through its verbs: type, delete, replace") {
    namespace keys = ws::input::keys;

    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    LayoutCursor layout;

    // Capacity 4 means room for 3 bytes plus the NUL - the overfill
    // tail below drops at that wall. The same panel'd field runs every
    // frame, so the id (and the persistent caret under it) stays one
    // widget's across the whole case.
    char buf[4] = "";
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

    // A press inside the field claims the focus the same frame; the
    // release that follows settles it. Neither is an edit.
    CHECK(!run_field(snap(60.0f, 20.0f, true, true)));
    CHECK(!run_field(snap(60.0f, 20.0f, false, false, true)));

    // Typing through the filter: insertable codepoints land in order,
    // and cp 300 - not a byte - drops instead of aliasing into ','.
    InputFrame typing = snap(-100.0f, -100.0f);
    typing.text_input = {'a', 300u, 'b'};
    CHECK(run_field(typing));
    CHECK(std::strcmp(buf, "ab") == 0);

    // Backspace eats the byte before the caret.
    InputFrame del = snap(-100.0f, -100.0f);
    del.key_pressed[static_cast<std::size_t>(keys::kBackspace)] = true;
    CHECK(run_field(del));
    CHECK(std::strcmp(buf, "a") == 0);

    // End then typing appends: the caret key runs before the frame's
    // codepoints land.
    InputFrame append = snap(-100.0f, -100.0f);
    append.key_pressed[static_cast<std::size_t>(keys::kEnd)] = true;
    append.text_input = {'c'};
    CHECK(run_field(append));
    CHECK(std::strcmp(buf, "ac") == 0);

    // Select all: a selection alone is not an edit - the frame reports
    // false while the anchor and caret spread.
    InputFrame select = snap(-100.0f, -100.0f);
    select.key_down[static_cast<std::size_t>(keys::kLeftControl)] = true;
    select.key_pressed[static_cast<std::size_t>(keys::kA)] = true;
    CHECK(!run_field(select));

    // Typing over the selection replaces it: one byte where two were.
    InputFrame replace = snap(-100.0f, -100.0f);
    replace.text_input = {'z'};
    CHECK(run_field(replace));
    CHECK(std::strcmp(buf, "z") == 0);

    // The full-buffer drop: room for two more, four typed - the rest of
    // the frame's text never lands and never splits the capacity.
    InputFrame overfill = snap(-100.0f, -100.0f);
    overfill.text_input = {'a', 'b', 'c', 'd'};
    CHECK(run_field(overfill));
    CHECK(std::strcmp(buf, "zab") == 0);

    // An idle frame over the same field: nothing to report, nothing
    // moved.
    CHECK(!run_field(snap(-100.0f, -100.0f)));
    CHECK(std::strcmp(buf, "zab") == 0);
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
    using ws::input::PointerId;
    using ws::input::PointerView;

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

TEST_CASE("the JobProgress mirror stores what the drain delivers") {
    ws::ui::gui::JobProgress mirror;
    CHECK(mirror.fraction() == 0.0f);
    CHECK(mirror.message().empty());

    // The wired callbacks ARE the drain's delivery shape - calling
    // them directly is exactly what drain() does on the main thread.
    ws::jobs::JobCallbacks<int> cbs = mirror.callbacks<int>();
    cbs.on_progress(0.5f, "halfway");
    CHECK(mirror.fraction() == 0.5f);
    CHECK(mirror.message() == "halfway");

    // The delivery's pointer dies with the call: the mirror copied it
    // out, so the word survives the buffer it came from.
    {
        char stack_message[24];
        std::snprintf(stack_message, sizeof stack_message, "step %d", 37);
        cbs.on_progress(0.37f, stack_message);
        CHECK(mirror.fraction() == 0.37f);
    }
    CHECK(mirror.message() == "step 37");

    // Clamped like the record clamps; a null message clears, it does
    // not keep the stale one.
    cbs.on_progress(2.0f, nullptr);
    CHECK(mirror.fraction() == 1.0f);
    CHECK(mirror.message().empty());
    cbs.on_progress(-1.0f, "never mind");
    CHECK(mirror.fraction() == 0.0f);

    // reset(): a new job on the same mirror starts clean.
    mirror.reset();
    CHECK(mirror.fraction() == 0.0f);
    CHECK(mirror.message().empty());
}

TEST_CASE("job_view maps every job state to its face") {
    using ws::jobs::JobState;
    const ws::ui::gui::ChromeTheme& chrome = kDefaultChrome;
    ws::ui::gui::JobProgress mirror;
    mirror.report(0.5f, "halfway");

    // The live states wear the accent, carry the mirror's fraction,
    // and keep the cancel armed; the running word is the mirror's
    // latest message, falling back to "running" before the first
    // report.
    ws::ui::gui::JobView view =
        ws::ui::gui::job_view(JobState::running, mirror, chrome);
    CHECK(view.fraction == 0.5f);
    CHECK(view.fill == chrome.control_pressed);
    CHECK(std::string(view.word) == "halfway");
    CHECK(view.cancellable);

    mirror.report(0.5f, "");
    view = ws::ui::gui::job_view(JobState::running, mirror, chrome);
    CHECK(std::string(view.word) == "running");

    view = ws::ui::gui::job_view(JobState::queued, mirror, chrome);
    CHECK(view.fraction == 0.5f);
    CHECK(view.fill == chrome.control_pressed);
    CHECK(std::string(view.word) == "queued");
    CHECK(view.cancellable);

    // The terminal states land a FULL bar - the row shows its outcome,
    // not its last report. done wears control_on, failed the toast
    // red (the row and the toast it posts read as one story), and the
    // cancel is dead on every one of them.
    view = ws::ui::gui::job_view(JobState::done, mirror, chrome);
    CHECK(view.fraction == 1.0f);
    CHECK(view.fill == chrome.control_on);
    CHECK(std::string(view.word) == "done");
    CHECK(!view.cancellable);

    view = ws::ui::gui::job_view(JobState::failed, mirror, chrome);
    CHECK(view.fraction == 1.0f);
    CHECK(view.fill == chrome.toast_error);
    CHECK(std::string(view.word) == "failed");
    CHECK(!view.cancellable);

    // Cancelled keeps the half-run fraction - a cancelled job stays
    // visibly half-run - but the dead face.
    view = ws::ui::gui::job_view(JobState::cancelled, mirror, chrome);
    CHECK(view.fraction == 0.5f);
    CHECK(view.fill == chrome.control_disabled);
    CHECK(std::string(view.word) == "cancelled");
    CHECK(!view.cancellable);

    // The null handle: the idle row.
    view = ws::ui::gui::job_view(JobState::invalid, mirror, chrome);
    CHECK(view.fraction == 0.0f);
    CHECK(view.fill == 0u);
    CHECK(std::string(view.word) == "no job");
    CHECK(!view.cancellable);
}

TEST_CASE("the job row draws through the seam and its cancel button cancels") {
    ws::ui::gui_test::stub.reset();
    // A gated worker: the row's cancel click has something live to
    // cancel, deterministically - the gate holds the callable until
    // the click has landed, then check_cancel honours the request.
    ws::jobs::JobSystem jobs{1};
    std::atomic<bool> gate{false};
    ws::jobs::JobHandle<int> handle = jobs.submit<int>(
        [&gate](ws::jobs::JobContext& ctx) -> ws::core::Result<int> {
            while (!gate.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            WS_TRY(ctx.check_cancel());
            return 42;
        });

    GuiContext gui;
    LayoutCursor layout;
    ws::ui::gui::JobProgress mirror;
    const auto draw_row = [&](const InputFrame& frame) -> bool {
        gui.begin_frame(frame);
        (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                       {0.0f, 0.0f, 400.0f, 300.0f});
        const bool cancelled = ws::ui::gui::job_progress(
            gui, layout, kDefaultChrome, "job", handle, mirror);
        ws::ui::gui::end_panel(gui, layout);
        gui.end_frame();
        return cancelled;
    };

    // The row really drew. The test theme is fontless, so both label
    // lines draw their v1 bars (plain quads) rather than draw_texts;
    // the row's rounded quads are the bar's track, the bar's fill and
    // the cancel button's face. Frame 1 with an empty mirror draws
    // exactly track + button - no fill - plus the panel chrome's two
    // plain quads. The pointer parks on the cancel button's center:
    // the row's second line, right end - content 12 px in, the bar
    // taking everything but the button's 80 px and one spacing, so the
    // button spans x 308..388, y 44..68.
    CHECK(!draw_row(snap(348.0f, 56.0f)));
    CHECK(ws::ui::gui_test::stub.quads == 4);  // panel chrome + 2 label bars
    CHECK(ws::ui::gui_test::stub.rounded == 2);  // track + button, no fill

    // Script the mirror with what the drain would have delivered and
    // redraw: the fill appears - the frame's third rounded quad, the
    // row's fraction flowing from the mirror to the bar. The counters
    // are cumulative, so frame 2 adds its whole redraw: track + the
    // new fill + the button face.
    mirror.report(0.9f, "almost there");
    CHECK(!draw_row(snap(348.0f, 56.0f)));
    CHECK(ws::ui::gui_test::stub.rounded == 5);

    // The full click fires exactly once and requests the cancel.
    CHECK(!draw_row(snap(348.0f, 56.0f, true, true)));       // press
    CHECK(draw_row(snap(348.0f, 56.0f, false, false, true)));  // release

    // Release the worker: the honoured cancel lands the cancelled
    // state - the click did the asking, the job did the answering.
    gate.store(true);
    handle.wait();
    CHECK(handle.state() == ws::jobs::JobState::cancelled);

    // The terminal row draws its dead look without flinching.
    CHECK(!draw_row(snap(348.0f, 56.0f)));
}

TEST_CASE("toast_on_failure toasts only failures") {
    ws::jobs::JobSystem jobs{1};
    ws::ui::gui::Toasts toasts;
    std::string text;

    // A failed job: the helper formats the error into the long-lived
    // buffer and posts exactly one error toast that borrows it.
    ws::jobs::JobHandle<int> failed = jobs.submit<int>(
        [](ws::jobs::JobContext&) -> ws::core::Result<int> {
            return ws::jobs::fail(ws::jobs::JobError::invalid_argument,
                                   "the fake failure");
        });
    failed.wait();
    const ws::core::Result<int> failed_result = ws::ui::gui::toast_on_failure(
        toasts, text, failed, "the demo job");
    CHECK(!failed_result.has_value());
    CHECK(toasts.count() == 1);
    CHECK(text == "the demo job: the fake failure");

    // A cancelled job is not a failure: no toast, and the borrowed
    // buffer untouched - the Result crosses back for the caller's own
    // quiet handling.
    ws::jobs::JobHandle<int> cancelled = jobs.submit<int>(
        [](ws::jobs::JobContext&) -> ws::core::Result<int> {
            return ws::jobs::fail(ws::jobs::JobError::cancelled);
        });
    cancelled.wait();
    const ws::core::Result<int> cancelled_result =
        ws::ui::gui::toast_on_failure(toasts, text, cancelled,
                                      "the cancelled job");
    CHECK(!cancelled_result.has_value());
    CHECK(toasts.count() == 1);  // still only the failure's
    CHECK(text == "the demo job: the fake failure");

    // A success crosses the value straight through, no toast.
    ws::jobs::JobHandle<int> done = jobs.submit<int>(
        [](ws::jobs::JobContext&) -> ws::core::Result<int> { return 42; });
    done.wait();
    const ws::core::Result<int> done_result =
        ws::ui::gui::toast_on_failure(toasts, text, done);
    CHECK(done_result.has_value());
    CHECK(done_result.value() == 42);
    CHECK(toasts.count() == 1);
}
