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

#include <ws/ui/gui/command_views.hpp>
#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/job_progress.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/theme_color.hpp>
#include <ws/ui/gui/theme_data.hpp>
#include <ws/ui/gui/theme_json.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/app/command.hpp>
#include <ws/app/shortcut.hpp>
#include <ws/input/input.hpp>
#include <ws/jobs/errors.hpp>
#include <ws/jobs/job_system.hpp>
#include <ws/settings/filesystem.hpp>
#include <ws/settings/json.hpp>
#include <ws/settings/migration.hpp>
#include <ws/ui/renderer2d.hpp>

#include "ui_gui_stub.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using ws::ui::gui::GuiContext;
using ws::input::InputFrame;
using ws::ui::gui::LayoutCursor;
using ws::ui::gui::kDefaultChrome;
using ws::ui::gui::kLightChrome;

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

TEST_CASE("oklch conversions round-trip the stock faces") {
    using ws::ui::gui::oklch_from_rgb;
    using ws::ui::gui::rgb_from_oklch;

    const std::uint32_t faces[] = {
        kDefaultChrome.panel_bg,       kDefaultChrome.control_bg,
        kDefaultChrome.control_on,     kDefaultChrome.control_pressed,
        kDefaultChrome.control_hovered,
        kDefaultChrome.control_disabled, kDefaultChrome.text,
        kLightChrome.panel_bg,         kLightChrome.control_bg,
        kLightChrome.control_on,       kLightChrome.control_pressed,
        kLightChrome.control_hovered,  kLightChrome.control_disabled,
        kLightChrome.text,
    };
    const auto channel = [](std::uint32_t color, int shift) {
        return (color >> shift) & 0xFFu;
    };
    for (const std::uint32_t face : faces) {
        CAPTURE(face);
        const std::uint32_t back = rgb_from_oklch(oklch_from_rgb(face));
        // One step of 8-bit quantization slack per channel - an
        // in-gamut color survives the double round trip almost
        // exactly.
        CHECK(std::abs(static_cast<int>(channel(back, 16)) -
                       static_cast<int>(channel(face, 16))) <= 1);
        CHECK(std::abs(static_cast<int>(channel(back, 8)) -
                       static_cast<int>(channel(face, 8))) <= 1);
        CHECK(std::abs(static_cast<int>(channel(back, 0)) -
                       static_cast<int>(channel(face, 0))) <= 1);
    }

    // Anchors: black is no lightness, white is all of it, and the
    // perceptual order tracks the obvious one on the light theme.
    CHECK(oklch_from_rgb(0x000000FFu).L ==
          doctest::Approx(0.0).epsilon(0.001));
    CHECK(oklch_from_rgb(0xFFFFFFFFu).L ==
          doctest::Approx(1.0).epsilon(0.001));
    CHECK(oklch_from_rgb(kLightChrome.panel_bg).L >
          oklch_from_rgb(kLightChrome.text).L);
}

TEST_CASE("contrast_ratio anchors") {
    using ws::ui::gui::contrast_ratio;

    // The ends of the scale: the textbook 21:1, and a color against
    // itself.
    CHECK(contrast_ratio(0xFFFFFFFFu, 0x000000FFu) ==
          doctest::Approx(21.0).epsilon(0.001));
    CHECK(contrast_ratio(0x808080FFu, 0x808080FFu) ==
          doctest::Approx(1.0));
    // Order-independent.
    CHECK(contrast_ratio(0x000000FFu, 0xFFFFFFFFu) ==
          contrast_ratio(0xFFFFFFFFu, 0x000000FFu));
}

TEST_CASE("the stock themes pass their contrast checks") {
    using ws::ui::gui::contrast_failures;
    using ws::ui::gui::oklch_from_rgb;

    CHECK(contrast_failures(kDefaultChrome).empty());
    CHECK(contrast_failures(kLightChrome).empty());

    // Hover is a perceptual contract, not a WCAG pair: the shift off
    // the control face clears 0.05 Oklab lightness on both themes -
    // the pointer is the indicator, the shift just has to be visible.
    for (const ws::ui::gui::ChromeTheme& theme :
         {kDefaultChrome, kLightChrome}) {
        const double gap = oklch_from_rgb(theme.control_hovered).L -
                           oklch_from_rgb(theme.control_bg).L;
        CHECK(std::abs(gap) >= 0.05);
    }
}

TEST_CASE("apply_accent derives contrast-safe faces across accents") {
    using ws::ui::gui::apply_accent;
    using ws::ui::gui::contrast_failures;

    // A spread: brandish hues, pastels, and the awkward ends - a
    // near-black accent on the dark chrome and a near-white on the
    // light have to TRAVEL to clear their ratios, which is the
    // walk's whole job.
    const std::uint32_t accents[] = {
        0x4C84C0FFu,  // blue
        0x54A054FFu,  // green
        0xD08830FFu,  // orange
        0xC04040FFu,  // red
        0xE090B0FFu,  // pink
        0x808080FFu,  // mid gray
        0x202020FFu,  // near-black
        0xF0F0F0FFu,  // near-white
    };
    for (const std::uint32_t accent : accents) {
        CAPTURE(accent);
        ws::ui::gui::ChromeTheme dark = kDefaultChrome;
        apply_accent(dark, accent);
        CHECK(contrast_failures(dark).empty());

        ws::ui::gui::ChromeTheme light = kLightChrome;
        apply_accent(light, accent);
        CHECK(contrast_failures(light).empty());
    }
}

TEST_CASE("apply_accent leaves the semantic faces alone") {
    using ws::ui::gui::apply_accent;

    const ws::ui::gui::ChromeTheme stock = kDefaultChrome;
    ws::ui::gui::ChromeTheme dark = kDefaultChrome;
    apply_accent(dark, 0xC04040FFu);

    // The faces that carry meaning - the veil, the severities, text,
    // the backgrounds - are never derived: they keep their shipped
    // values verbatim.
    CHECK(dark.veil == stock.veil);
    CHECK(dark.toast_info == stock.toast_info);
    CHECK(dark.toast_warning == stock.toast_warning);
    CHECK(dark.toast_error == stock.toast_error);
    CHECK(dark.text == stock.text);
    CHECK(dark.panel_bg == stock.panel_bg);
    CHECK(dark.control_bg == stock.control_bg);
    CHECK(dark.border == stock.border);

    // The four accent-driven faces all moved.
    CHECK(dark.control_on != stock.control_on);
    CHECK(dark.control_pressed != stock.control_pressed);
    CHECK(dark.control_hovered != stock.control_hovered);
    CHECK(dark.control_disabled != stock.control_disabled);
}

TEST_CASE("the derived hover follows the background polarity") {
    using ws::ui::gui::apply_accent;
    using ws::ui::gui::oklch_from_rgb;

    // Dark chrome: hover lightens - further from the dark panel.
    ws::ui::gui::ChromeTheme dark = kDefaultChrome;
    apply_accent(dark, 0x4C84C0FFu);
    const double dark_gap = oklch_from_rgb(dark.control_hovered).L -
                            oklch_from_rgb(dark.control_bg).L;
    CHECK(dark_gap >= 0.05);

    // Light chrome: hover darkens - further from the light panel.
    ws::ui::gui::ChromeTheme light = kLightChrome;
    apply_accent(light, 0x4C84C0FFu);
    const double light_gap = oklch_from_rgb(light.control_hovered).L -
                             oklch_from_rgb(light.control_bg).L;
    CHECK(light_gap <= -0.05);
}

TEST_CASE("ThemeData carries the stock documents and the accent") {
    using ws::ui::gui::ThemeData;
    using ws::ui::gui::apply_accent;
    using ws::ui::gui::default_dark;
    using ws::ui::gui::default_light;

    const ThemeData dark = default_dark();
    CHECK(dark.chrome.panel_bg == kDefaultChrome.panel_bg);
    CHECK(dark.chrome.text == kDefaultChrome.text);
    CHECK(dark.accent == 0u);  // the stock faces: never derived
    CHECK(dark.regular.size > 0.0f);  // roles carry real sizes
    CHECK(dark.panel_shadow.blur > 0.0f);

    const ThemeData light = default_light();
    CHECK(light.chrome.panel_bg == kLightChrome.panel_bg);
    CHECK(light.chrome.control_on == kLightChrome.control_on);

    // The document-level apply_accent derives into the chrome AND
    // remembers the accent, forced opaque - even a grubby-alpha
    // input lands clean.
    ThemeData styled = default_dark();
    apply_accent(styled, 0x54A05499u);
    CHECK(styled.accent == 0x54A054FFu);
    CHECK(styled.chrome.control_on != dark.chrome.control_on);
    CHECK(styled.chrome.panel_bg == dark.chrome.panel_bg);
}

// --- Stage 10 phase 2: theme json + ThemeFile -------------------------------

namespace {

// The settings suite's in-memory FileSystem, mirrored: one map, a read
// counter (poller gating asserts it), and a write breaker. The errors
// imitate StdFileSystem's - code and category do not matter to
// ThemeFile, only the message rides its reports.
class MemoryFs final : public ws::settings::FileSystem {
public:
    bool fail_read = false;
    bool fail_write = false;
    mutable int reads = 0;
    int writes = 0;

    ws::core::Result<std::string> read(
        std::string_view path) const override {
        ++reads;
        if (fail_read) {
            return ws::core::fail(1, "settings", "read (fake)");
        }
        const auto it = files.find(path);
        if (it == files.end()) {
            return ws::core::fail(1, "settings", "no such file (fake)");
        }
        return it->second;
    }

    ws::core::Status write(std::string_view path,
                           std::string_view bytes) override {
        if (fail_write) {
            return ws::core::fail(1, "settings", "write (fake)");
        }
        files.insert_or_assign(std::string{path}, std::string{bytes});
        ++writes;
        return {};
    }

    ws::core::Status rename(std::string_view from,
                            std::string_view to) override {
        const auto it = files.find(from);
        if (it == files.end()) {
            return ws::core::fail(1, "settings", "rename (fake)");
        }
        std::string bytes = std::move(it->second);
        files.erase(it);
        files.insert_or_assign(std::string{to}, std::move(bytes));
        return {};
    }

    bool remove(std::string_view path) override {
        return files.erase(std::string{path}) > 0;
    }

    [[nodiscard]] bool exists(std::string_view path) const override {
        return files.contains(path);
    }

    std::map<std::string, std::string, std::less<>> files;
};

// A fake steady clock: the test moves time by writing `now`, the file
// holds the clock (a lambda over this) - the Debouncer test shape.
// Must outlive every ThemeFile it feeds.
struct FakeClock {
    std::int64_t now = 0;
    ws::ui::gui::ThemeFile::Clock clock() {
        return [this] { return now; };
    }
};

}  // namespace

TEST_CASE("theme json round-trips a full document byte-stably") {
    using ws::ui::gui::ThemeData;
    using ws::ui::gui::default_dark;
    using ws::ui::gui::default_light;
    using ws::ui::gui::read_theme;
    using ws::ui::gui::write_theme;

    ThemeData styled = default_dark();
    styled.accent = 0x54A054FFu;
    styled.chrome.panel_bg = 0x20222AFFu;
    styled.chrome.control_height = 30.0f;
    styled.chrome.tooltip_delay = 0.25f;
    styled.regular.family = "Segoe UI";
    styled.regular.size = 18.0f;
    styled.mono.family = "Cascadia Mono";
    styled.icon_set = "ws-stock";
    styled.panel_shadow.blur = 12.0f;
    styled.control_shadow.color = 0x00000080u;

    const std::string text = write_theme(styled);

    // The canonical shape: version first, 8-digit uppercase hex,
    // metrics as reals.
    CHECK(text.find("\"version\": 1") != std::string::npos);
    CHECK(text.find("\"accent\": \"#54A054FF\"") != std::string::npos);
    CHECK(text.find("\"panel_bg\": \"#20222AFF\"") != std::string::npos);
    CHECK(text.find("\"control_height\": 30.0") != std::string::npos);

    // Reading over a DIFFERENT base recovers the whole document - the
    // text is complete, the base only fills gaps.
    const auto over_light = read_theme(text, default_light());
    REQUIRE(over_light);
    CHECK(over_light->accent == styled.accent);
    CHECK(over_light->chrome.panel_bg == styled.chrome.panel_bg);
    CHECK(over_light->chrome.control_height == styled.chrome.control_height);
    CHECK(over_light->chrome.tooltip_delay == styled.chrome.tooltip_delay);
    CHECK(over_light->regular.family == styled.regular.family);
    CHECK(over_light->regular.size == styled.regular.size);
    CHECK(over_light->mono.family == styled.mono.family);
    CHECK(over_light->icon_set == styled.icon_set);
    CHECK(over_light->panel_shadow.blur == styled.panel_shadow.blur);
    CHECK(over_light->control_shadow.color == styled.control_shadow.color);

    // write(read(text)) is byte-stable.
    CHECK(write_theme(*over_light) == text);
}

TEST_CASE("read_theme fills gaps from the base and never derives") {
    using ws::ui::gui::default_dark;
    using ws::ui::gui::read_theme;

    // A minimal hand edit: the accent only. No derivation on the way
    // in - the stock faces stay, an app wanting them re-derived calls
    // apply_accent(theme, theme.accent) itself.
    const auto minimal = read_theme(
        R"json({
            "version": 1,
            "accent": "#54A054"
        })json",
        default_dark());
    REQUIRE(minimal);
    CHECK(minimal->accent == 0x54A054FFu);  // #RRGGBB implies alpha FF
    CHECK(minimal->chrome.panel_bg == kDefaultChrome.panel_bg);
    CHECK(minimal->chrome.control_on == kDefaultChrome.control_on);
    CHECK(minimal->icon_set.empty());

    // A missing "version" reads as shape 1.
    const auto unversioned =
        read_theme(R"json({"accent": "#54A054FF"})json", default_dark());
    REQUIRE(unversioned);
    CHECK(unversioned->accent == 0x54A054FFu);

    // Metrics fold as reals or integers; font sizes too; untouched
    // members keep the base.
    const auto sizes = read_theme(
        R"json({
            "version": 1,
            "chrome": {"control_height": 28, "corner_radius": 6.5},
            "fonts": {"regular": {"family": "Trebuchet", "size": 20}}
        })json",
        default_dark());
    REQUIRE(sizes);
    CHECK(sizes->chrome.control_height == 28.0f);
    CHECK(sizes->chrome.corner_radius == 6.5f);
    CHECK(sizes->regular.family == "Trebuchet");
    CHECK(sizes->regular.size == 20.0f);
    CHECK(sizes->chrome.padding == kDefaultChrome.padding);
}

TEST_CASE("read_theme refuses what it cannot honor") {
    using ws::ui::gui::ThemeData;
    using ws::ui::gui::ThemeError;
    using ws::ui::gui::describe;
    using ws::ui::gui::read_theme;

    CHECK(describe(ThemeError::parse_failed) ==
          std::string_view{"parse failed"});

    const auto bad_json = read_theme("{ nope", ThemeData{});
    REQUIRE_FALSE(bad_json);
    CHECK(bad_json.error().code ==
          static_cast<std::int32_t>(ThemeError::parse_failed));
    CHECK(std::string_view{bad_json.error().category} == "ui");
    CHECK(bad_json.error().message.find("parse: ") == 0);

    const auto bad_root = read_theme("[1, 2]", ThemeData{});
    REQUIRE_FALSE(bad_root);
    CHECK(bad_root.error().code ==
          static_cast<std::int32_t>(ThemeError::bad_value));

    const auto bad_face =
        read_theme(R"json({"chrome": {"text": 235}})json", ThemeData{});
    REQUIRE_FALSE(bad_face);
    CHECK(bad_face.error().message.find("chrome.text") != std::string::npos);

    const auto bad_hex =
        read_theme(R"json({"accent": "#54A05G"})json", ThemeData{});
    REQUIRE_FALSE(bad_hex);
    CHECK(bad_hex.error().message.find("accent") != std::string::npos);

    const auto bad_metric =
        read_theme(R"json({"chrome": {"padding": "wide"}})json",
                   ThemeData{});
    REQUIRE_FALSE(bad_metric);
    CHECK(bad_metric.error().message.find("chrome.padding") !=
          std::string::npos);

    // A newer shape is refused, never guessed at.
    const auto future = read_theme(R"json({"version": 2})json", ThemeData{});
    REQUIRE_FALSE(future);
    CHECK(future.error().code ==
          static_cast<std::int32_t>(ThemeError::migrate_failed));
}

TEST_CASE("ThemeFile loads and keeps unknown top-level members") {
    using ws::ui::gui::ThemeFile;
    using ws::ui::gui::default_dark;

    MemoryFs fs;
    fs.files["theme.json"] = R"json({
    "version": 1,
    "accent": "#54A054FF",
    "my_note": "a hand annotation",
    "experiments": {"hue": 42},
    "chrome": {"panel_bg": "#20222AFF", "control_height": 28.0}
})json";
    ws::settings::Migrations migrations;
    ThemeFile file("theme.json", fs, migrations, default_dark());

    const auto load = file.load();
    REQUIRE(load.ok());
    CHECK_FALSE(load.missing);
    CHECK(load.theme.accent == 0x54A054FFu);
    CHECK(load.theme.chrome.panel_bg == 0x20222AFFu);
    CHECK(load.theme.chrome.control_height == 28.0f);
    CHECK(file.theme().accent == 0x54A054FFu);

    // Saving re-emits the knowns (the live theme) PLUS the remembered
    // unknowns, in their file order.
    REQUIRE(file.save());
    const std::string saved = fs.files["theme.json"];
    CHECK(saved.find("\"my_note\": \"a hand annotation\"") !=
          std::string::npos);
    CHECK(saved.find("\"hue\": 42") != std::string::npos);
    CHECK(saved.find("\"control_height\": 28.0") != std::string::npos);

    // load -> save -> load is byte-stable the second time.
    REQUIRE(file.load().ok());
    REQUIRE(file.save());
    CHECK(fs.files["theme.json"] == saved);
}

TEST_CASE("ThemeFile answers the first run and stays quiet") {
    using ws::ui::gui::ThemeFile;
    using ws::ui::gui::default_dark;

    MemoryFs fs;  // empty: no file anywhere
    ws::settings::Migrations migrations;
    FakeClock clock;
    ThemeFile file("theme.json", fs, migrations, default_dark(), 250,
                   clock.clock());

    // Poll before load owes nothing - and does not consume the gate.
    const auto pre = file.poll();
    CHECK_FALSE(pre.reloaded);
    CHECK_FALSE(pre.failed);

    const auto load = file.load();
    CHECK(load.missing);
    CHECK(load.ok());
    CHECK(load.theme.chrome.panel_bg == kDefaultChrome.panel_bg);
    CHECK(file.theme().chrome.panel_bg == kDefaultChrome.panel_bg);

    // Nothing to watch, and the cadence starts at load(): polls stay
    // silent, no failure spam, and an under-cadence poll reads
    // nothing at all.
    CHECK(fs.reads == 1);  // load()'s read
    clock.now = 100;
    auto quiet = file.poll();
    CHECK_FALSE(quiet.reloaded);
    CHECK_FALSE(quiet.failed);
    CHECK(fs.reads == 1);  // under the gate: no read happened
    clock.now = 300;
    quiet = file.poll();
    CHECK_FALSE(quiet.reloaded);
    CHECK_FALSE(quiet.failed);
    CHECK(fs.reads == 2);  // past the gate, read, still nothing there
}

TEST_CASE("ThemeFile hot-reloads and keeps the last good theme") {
    using ws::ui::gui::ThemeFile;
    using ws::ui::gui::default_dark;

    MemoryFs fs;
    fs.files["theme.json"] =
        R"json({"version": 1, "accent": "#54A054FF"})json";
    ws::settings::Migrations migrations;
    FakeClock clock;
    ThemeFile file("theme.json", fs, migrations, default_dark(), 250,
                   clock.clock());
    REQUIRE(file.load().ok());
    REQUIRE(file.theme().accent == 0x54A054FFu);
    const int reads_after_load = fs.reads;

    // Under the cadence: silent, and no read happens.
    clock.now = 100;
    auto poll = file.poll();
    CHECK_FALSE(poll.reloaded);
    CHECK_FALSE(poll.failed);
    CHECK(fs.reads == reads_after_load);

    // Past the cadence, bytes unchanged: a read, no transition.
    clock.now = 300;
    poll = file.poll();
    CHECK_FALSE(poll.reloaded);
    CHECK_FALSE(poll.failed);
    CHECK(fs.reads == reads_after_load + 1);

    // The dev edits the file: reloaded.
    fs.files["theme.json"] =
        R"json({"version": 1, "accent": "#C04040FF", "chrome": {"control_on": "#3070D0FF"}})json";
    clock.now = 600;
    poll = file.poll();
    CHECK(poll.reloaded);
    CHECK_FALSE(poll.failed);
    CHECK(file.theme().accent == 0xC04040FFu);
    CHECK(file.theme().chrome.control_on == 0x3070D0FFu);

    // The dev saves garbage: reported once, the last good kept, and
    // the poll stays quiet while the bytes do not change.
    fs.files["theme.json"] = "{ not json";
    clock.now = 900;
    poll = file.poll();
    CHECK_FALSE(poll.reloaded);
    CHECK(poll.failed);
    CHECK_FALSE(poll.error.empty());
    CHECK(file.theme().accent == 0xC04040FFu);

    clock.now = 1200;
    poll = file.poll();
    CHECK_FALSE(poll.reloaded);
    CHECK_FALSE(poll.failed);  // same bytes: quiet

    // A known key with the wrong type: the same contract.
    fs.files["theme.json"] =
        R"json({"version": 1, "chrome": {"text": 7}})json";
    clock.now = 1500;
    poll = file.poll();
    CHECK_FALSE(poll.reloaded);
    CHECK(poll.failed);
    CHECK(file.theme().accent == 0xC04040FFu);
    clock.now = 1800;
    poll = file.poll();
    CHECK_FALSE(poll.failed);  // bad bytes remembered: quiet

    // The dev fixes it: reloaded again.
    fs.files["theme.json"] =
        R"json({"version": 1, "accent": "#54A054FF"})json";
    clock.now = 2100;
    poll = file.poll();
    CHECK(poll.reloaded);
    CHECK(file.theme().accent == 0x54A054FFu);

    // Deleting the file after a good load: reported once, the theme
    // kept, then quiet until a file reappears.
    fs.files.erase("theme.json");
    clock.now = 2400;
    poll = file.poll();
    CHECK_FALSE(poll.reloaded);
    CHECK(poll.failed);
    CHECK(file.theme().accent == 0x54A054FFu);
    clock.now = 2700;
    poll = file.poll();
    CHECK_FALSE(poll.failed);
    CHECK_FALSE(poll.reloaded);
}

TEST_CASE("a bad first load reports and the poll recovers") {
    using ws::ui::gui::ThemeFile;
    using ws::ui::gui::default_dark;

    MemoryFs fs;
    fs.files["theme.json"] = "{ broken at startup";
    ws::settings::Migrations migrations;
    FakeClock clock;
    ThemeFile file("theme.json", fs, migrations, default_dark(), 250,
                   clock.clock());

    const auto load = file.load();
    CHECK(load.failed);
    CHECK_FALSE(load.missing);
    CHECK_FALSE(load.error.empty());
    CHECK(load.theme.chrome.panel_bg == kDefaultChrome.panel_bg);

    // The failed load remembered its bad bytes: a poll before the dev
    // touches anything stays quiet - the load already reported.
    clock.now = 1000;
    auto poll = file.poll();
    CHECK_FALSE(poll.reloaded);
    CHECK_FALSE(poll.failed);

    // The dev fixes the file; the poll picks it up.
    fs.files["theme.json"] =
        R"json({"version": 1, "accent": "#D08830FF"})json";
    clock.now = 2000;
    poll = file.poll();
    CHECK(poll.reloaded);
    CHECK_FALSE(poll.failed);
    CHECK(file.theme().accent == 0xD08830FFu);
}

TEST_CASE("set_theme + save writes the file and the poll stays quiet") {
    using ws::ui::gui::ThemeData;
    using ws::ui::gui::ThemeFile;
    using ws::ui::gui::default_dark;

    MemoryFs fs;
    fs.files["theme.json"] = R"json({"version": 1, "unknown_key": 11})json";
    ws::settings::Migrations migrations;
    FakeClock clock;
    ThemeFile file("theme.json", fs, migrations, default_dark(), 250,
                   clock.clock());
    REQUIRE(file.load().ok());
    CHECK(fs.writes == 0);

    ThemeData edited = file.theme();
    edited.accent = 0x8060C0FFu;
    edited.chrome.padding = 20.0f;
    file.set_theme(std::move(edited));

    REQUIRE(file.save());
    CHECK(fs.writes == 1);
    const std::string& saved = fs.files["theme.json"];
    CHECK(saved.find("\"accent\": \"#8060C0FF\"") != std::string::npos);
    CHECK(saved.find("\"padding\": 20.0") != std::string::npos);
    CHECK(saved.find("\"unknown_key\": 11") != std::string::npos);

    // The poll remembers the written bytes: our own save is not a
    // reload.
    clock.now = 1000;
    const auto poll = file.poll();
    CHECK_FALSE(poll.reloaded);
    CHECK_FALSE(poll.failed);

    // A failing save reports through the Status, io_failed.
    fs.fail_write = true;
    const ws::core::Status failed = file.save();
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code ==
          static_cast<std::int32_t>(ws::ui::gui::ThemeError::io_failed));
    CHECK(std::string_view{failed.error().category} == "ui");
    CHECK(fs.writes == 1);  // nothing landed
}

TEST_CASE("ThemeFile round-trips through the real StdFileSystem") {
    using ws::ui::gui::ThemeData;
    using ws::ui::gui::ThemeFile;
    using ws::ui::gui::default_dark;
    using ws::ui::gui::default_light;

    ws::settings::StdFileSystem fs;
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::u8string upath = (dir / u8"ws_theme_smoke.json").u8string();
    const std::string path{upath.begin(), upath.end()};
    (void)fs.remove(path);  // a previous run may have littered

    ws::settings::Migrations migrations;
    ThemeData edited = default_dark();
    edited.icon_set = "smoke";
    edited.chrome.spacing = 10.0f;
    {
        ThemeFile writer(path, fs, migrations, default_dark());
        writer.set_theme(std::move(edited));
        REQUIRE(writer.save());
    }
    {
        ThemeFile reader(path, fs, migrations, default_light());
        const auto load = reader.load();
        REQUIRE(load.ok());
        CHECK_FALSE(load.missing);
        CHECK(load.theme.icon_set == "smoke");
        CHECK(load.theme.chrome.spacing == 10.0f);
        CHECK(load.theme.chrome.panel_bg == default_dark().chrome.panel_bg);
    }
    (void)fs.remove(path);
}

TEST_CASE("the split behaviors drive the interaction and draw nothing") {
    using ws::ui::gui::WidgetState;

    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 120.0f, 24.0f};

    // One frame per behavior, a one-frame tap inside the rect: each fold
    // answers the interaction (click, flip, move) while not one draw
    // call reaches the r2d seam the stub records.
    gui.begin_frame(snap(60.0f, 10.0f, false, true, true));
    const ws::ui::gui::ButtonResult pressed =
        ws::ui::gui::button_behavior(gui, gui.ids().id("btn"), rect);
    gui.end_frame();
    CHECK(pressed.clicked);
    CHECK(pressed.hit.pressed);
    CHECK(pressed.state == WidgetState::Pressed);

    gui.begin_frame(snap(60.0f, 10.0f, false, true, true));
    bool check = false;
    const ws::ui::gui::CheckboxResult flipped =
        ws::ui::gui::checkbox_behavior(gui, gui.ids().id("cb"), rect, check);
    gui.end_frame();
    CHECK(flipped.toggled);
    CHECK(check);

    gui.begin_frame(snap(60.0f, 10.0f, false, true, true));
    int choice = 1;
    const ws::ui::gui::RadioResult moved = ws::ui::gui::radio_behavior(
        gui, gui.ids().id("rd"), rect, choice, 2);
    gui.end_frame();
    CHECK(moved.moved);
    CHECK(choice == 2);

    gui.begin_frame(snap(60.0f, 10.0f, false, true, true));
    bool on = false;
    const ws::ui::gui::ToggleSwitchResult lit =
        ws::ui::gui::toggle_switch_behavior(gui, gui.ids().id("sw"), rect, on);
    gui.end_frame();
    CHECK(lit.toggled);
    CHECK(on);

    CHECK(ws::ui::gui_test::stub.quads == 0);
    CHECK(ws::ui::gui_test::stub.rounded == 0);
    CHECK(ws::ui::gui_test::stub.sprites == 0);
    CHECK(ws::ui::gui_test::stub.texts == 0);
    CHECK(ws::ui::gui_test::stub.clip_pushes == 0);
    CHECK(ws::ui::gui_test::stub.clip_pops == 0);
    CHECK(ws::ui::gui_test::stub.layer_sets == 0);
}

TEST_CASE("custom looks paint over the behaviors, pixel for pixel theirs") {
    using ws::ui::r2d::draw_quad;
    using ws::ui::r2d::draw_rounded_quad;

    // The seam from the other side: the behavior answers, then whatever
    // the caller draws IS the look. Each sub-block resets the stub, runs
    // one behavior with a tap, paints a hand-rolled face, and checks the
    // counters carry exactly the custom draws - no stock look leaked.
    GuiContext gui;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 120.0f, 24.0f};
    const auto tap = [&gui]() { gui.begin_frame(snap(60.0f, 10.0f, false, true, true)); };

    // button: a flat face and a bar for the label.
    ws::ui::gui_test::stub.reset();
    tap();
    {
        const ws::ui::gui::ButtonResult result =
            ws::ui::gui::button_behavior(gui, gui.ids().id("flat"), rect);
        draw_quad(rect.x, rect.y, rect.width, rect.height,
                  kDefaultChrome.control_pressed);
        draw_quad(rect.x + 8.0f, rect.y + 8.0f, 40.0f, 8.0f,
                  kDefaultChrome.text);
        gui.end_frame();
        CHECK(result.clicked);
    }
    CHECK(ws::ui::gui_test::stub.quads == 2);
    CHECK(ws::ui::gui_test::stub.rounded == 0);

    // checkbox: a square that lights the accent while on.
    ws::ui::gui_test::stub.reset();
    tap();
    {
        bool value = false;
        const ws::ui::gui::CheckboxResult result =
            ws::ui::gui::checkbox_behavior(gui, gui.ids().id("mine"), rect,
                                           value);
        draw_quad(0.0f, 0.0f, 24.0f, 24.0f,
                  value ? kDefaultChrome.control_on
                        : kDefaultChrome.control_bg);
        gui.end_frame();
        CHECK(result.toggled);
        CHECK(value);
    }
    CHECK(ws::ui::gui_test::stub.quads == 1);
    CHECK(ws::ui::gui_test::stub.rounded == 0);

    // radio: a circle, filled while selected.
    ws::ui::gui_test::stub.reset();
    tap();
    {
        int choice = 1;
        const ws::ui::gui::RadioResult result = ws::ui::gui::radio_behavior(
            gui, gui.ids().id("dot"), rect, choice, 2);
        draw_rounded_quad(0.0f, 0.0f, 24.0f, 24.0f, 12.0f,
                          choice == 2 ? kDefaultChrome.control_on
                                      : kDefaultChrome.control_bg);
        gui.end_frame();
        CHECK(result.moved);
        CHECK(choice == 2);
    }
    CHECK(ws::ui::gui_test::stub.rounded == 1);
    CHECK(ws::ui::gui_test::stub.quads == 0);

    // toggle_switch: a track and the knob at its end.
    ws::ui::gui_test::stub.reset();
    tap();
    {
        bool value = false;
        const ws::ui::gui::ToggleSwitchResult result =
            ws::ui::gui::toggle_switch_behavior(gui, gui.ids().id("sw"),
                                                rect, value);
        draw_quad(0.0f, 0.0f, 48.0f, 24.0f,
                  value ? kDefaultChrome.control_on
                        : kDefaultChrome.control_bg);
        draw_quad(40.0f, 2.0f, 20.0f, 20.0f, kDefaultChrome.text);
        gui.end_frame();
        CHECK(result.toggled);
        CHECK(value);
    }
    CHECK(ws::ui::gui_test::stub.quads == 2);
    CHECK(ws::ui::gui_test::stub.texts == 0);
}

TEST_CASE("the basic-row behaviors drive the interaction and draw nothing") {
    using ws::ui::gui::WidgetState;

    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 120.0f, 24.0f};

    // collapsing_header: the tap flips the caller's bool.
    gui.begin_frame(snap(60.0f, 10.0f, false, true, true));
    bool open = false;
    const ws::ui::gui::CollapsingHeaderResult header =
        ws::ui::gui::collapsing_header_behavior(gui, gui.ids().id("hdr"),
                                                rect, open);
    gui.end_frame();
    CHECK(header.toggled);
    CHECK(header.state == WidgetState::Pressed);
    CHECK(open);

    // selectable: the click reports; the selection stays the caller's.
    gui.begin_frame(snap(60.0f, 10.0f, false, true, true));
    const ws::ui::gui::SelectableResult row =
        ws::ui::gui::selectable_behavior(gui, gui.ids().id("row"), rect);
    gui.end_frame();
    CHECK(row.clicked);

    // spinner: the behavior advances the persistent phase on the frame
    // clock - 0.4 s is half the 0.8 s turn - and the next half wraps.
    InputFrame busy = snap(-100.0f, -100.0f);
    busy.dt_seconds = 0.4f;
    gui.begin_frame(busy);
    const ws::ui::gui::SpinnerResult spun =
        ws::ui::gui::spinner_behavior(gui, gui.ids().id("spin"));
    gui.end_frame();
    CHECK(spun.phase == 0.5f);
    gui.begin_frame(busy);
    const ws::ui::gui::SpinnerResult wrapped =
        ws::ui::gui::spinner_behavior(gui, gui.ids().id("spin"));
    gui.end_frame();
    CHECK(wrapped.phase == 0.0f);

    // progress_indeterminate: 0.6 s is half the 1.2 s sweep.
    InputFrame sweep = snap(-100.0f, -100.0f);
    sweep.dt_seconds = 0.6f;
    gui.begin_frame(sweep);
    const ws::ui::gui::IndeterminateResult half =
        ws::ui::gui::progress_indeterminate_behavior(gui,
                                                     gui.ids().id("bar"));
    gui.end_frame();
    CHECK(half.phase == 0.5f);

    CHECK(ws::ui::gui_test::stub.quads == 0);
    CHECK(ws::ui::gui_test::stub.rounded == 0);
    CHECK(ws::ui::gui_test::stub.sprites == 0);
    CHECK(ws::ui::gui_test::stub.texts == 0);
    CHECK(ws::ui::gui_test::stub.clip_pushes == 0);
    CHECK(ws::ui::gui_test::stub.clip_pops == 0);
    CHECK(ws::ui::gui_test::stub.layer_sets == 0);
}

TEST_CASE("custom looks paint over the basic-row behaviors") {
    using ws::ui::r2d::draw_quad;
    using ws::ui::r2d::draw_rounded_quad;

    // The seam from the other side, the basic rows' turn: each sub-block
    // resets the stub, runs one behavior, paints a hand-rolled look, and
    // checks the counters carry exactly the custom draws - no stock look
    // leaked.
    GuiContext gui;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 120.0f, 24.0f};
    const auto tap = [&gui]() {
        gui.begin_frame(snap(60.0f, 10.0f, false, true, true));
    };
    const auto busy = [&gui](float dt) {
        InputFrame frame = snap(-100.0f, -100.0f);
        frame.dt_seconds = dt;
        gui.begin_frame(frame);
    };

    // collapsing_header: a flat face and the open marker square.
    ws::ui::gui_test::stub.reset();
    tap();
    {
        bool open = false;
        const ws::ui::gui::CollapsingHeaderResult result =
            ws::ui::gui::collapsing_header_behavior(gui, gui.ids().id("hdr"),
                                                    rect, open);
        draw_quad(rect.x, rect.y, rect.width, rect.height,
                  kDefaultChrome.control_pressed);
        draw_quad(rect.x + 4.0f, rect.y + 4.0f, 16.0f, 16.0f,
                  kDefaultChrome.control_on);
        gui.end_frame();
        CHECK(result.toggled);
        CHECK(open);
    }
    CHECK(ws::ui::gui_test::stub.quads == 2);
    CHECK(ws::ui::gui_test::stub.rounded == 0);

    // selectable: the selected row as a flat face and an accent bar.
    ws::ui::gui_test::stub.reset();
    tap();
    {
        const ws::ui::gui::SelectableResult result =
            ws::ui::gui::selectable_behavior(gui, gui.ids().id("row"), rect);
        draw_quad(rect.x, rect.y, rect.width, rect.height,
                  kDefaultChrome.control_pressed);
        draw_quad(rect.x, rect.y, 3.0f, rect.height, kDefaultChrome.control_on);
        gui.end_frame();
        CHECK(result.clicked);
    }
    CHECK(ws::ui::gui_test::stub.quads == 2);

    // spinner: one dot stands for the whole chase (the phase is the
    // look's to spend).
    ws::ui::gui_test::stub.reset();
    busy(0.4f);
    {
        const ws::ui::gui::SpinnerResult result =
            ws::ui::gui::spinner_behavior(gui, gui.ids().id("spin"));
        draw_rounded_quad(56.0f, 8.0f, 8.0f, 8.0f, 4.0f,
                          kDefaultChrome.control_on);
        gui.end_frame();
        CHECK(result.phase == 0.5f);
    }
    CHECK(ws::ui::gui_test::stub.rounded == 1);
    CHECK(ws::ui::gui_test::stub.quads == 0);

    // progress_indeterminate: a sharp-cornered track and band.
    ws::ui::gui_test::stub.reset();
    busy(0.6f);
    {
        const ws::ui::gui::IndeterminateResult result =
            ws::ui::gui::progress_indeterminate_behavior(gui,
                                                          gui.ids().id("bar"));
        draw_quad(rect.x, rect.y, rect.width, rect.height,
                  kDefaultChrome.control_bg);
        draw_quad(rect.x + 10.0f, rect.y + 1.0f, 40.0f, 22.0f,
                  kDefaultChrome.control_on);
        gui.end_frame();
        CHECK(result.phase == 0.5f);
    }
    CHECK(ws::ui::gui_test::stub.quads == 2);
    CHECK(ws::ui::gui_test::stub.texts == 0);
}

TEST_CASE("the slider behaviors drive the interaction and draw nothing") {
    using ws::ui::gui::WidgetState;

    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 120.0f, 24.0f};

    // slider_behavior: the one-frame tap IS click-to-set - the value
    // jumps to the pointer's fraction of the track.
    gui.begin_frame(snap(60.0f, 10.0f, false, true, true));
    float value = 0.0f;
    const ws::ui::gui::SliderResult jumped = ws::ui::gui::slider_behavior(
        gui, gui.ids().id("slide"), rect, value, 0.0f, 100.0f, 1.0f);
    gui.end_frame();
    CHECK(jumped.changed);
    CHECK(jumped.hit.pressed);
    CHECK(jumped.state == WidgetState::Pressed);
    CHECK(value == 50.0f);  // dead center of the track: exact in float

    // slider_behavior: the arrows step the focused slider - key_step
    // per press, clamped to the range.
    gui.set_focus(gui.ids().id("step"));
    InputFrame keys = snap(-100.0f, -100.0f);
    keys.key_pressed[static_cast<std::size_t>(ws::input::keys::kRight)] =
        true;
    gui.begin_frame(keys);
    float stepped = 10.0f;
    const ws::ui::gui::SliderResult nudged = ws::ui::gui::slider_behavior(
        gui, gui.ids().id("step"), rect, stepped, 0.0f, 100.0f, 2.5f);
    gui.end_frame();
    CHECK(nudged.changed);
    CHECK(stepped == 12.5f);

    // drag_float_behavior: a one-frame tap is a plain click - grab and
    // lift, no move (the pointer is never seen down).
    gui.begin_frame(snap(60.0f, 10.0f, false, true, true));
    float dragged = 0.0f;
    const ws::ui::gui::DragFloatResult clicked = ws::ui::gui::drag_float_behavior(
        gui, gui.ids().id("drag"), rect, dragged, -100.0f, 100.0f, 1.0f);
    gui.end_frame();
    CHECK(!clicked.changed);
    CHECK(dragged == 0.0f);

    // drag_float_behavior: the held drag - the press frame anchors, the
    // next frame maps the travel from the anchor.
    gui.begin_frame(snap(60.0f, 10.0f, true, true));
    const ws::ui::gui::DragFloatResult grabbed = ws::ui::gui::drag_float_behavior(
        gui, gui.ids().id("drag"), rect, dragged, -100.0f, 100.0f, 1.0f);
    gui.end_frame();
    CHECK(!grabbed.changed);  // the anchor lands; the finger hasn't moved
    gui.begin_frame(snap(100.0f, 10.0f, true));
    const ws::ui::gui::DragFloatResult swept = ws::ui::gui::drag_float_behavior(
        gui, gui.ids().id("drag"), rect, dragged, -100.0f, 100.0f, 1.0f);
    gui.end_frame();
    CHECK(swept.changed);
    CHECK(dragged == 40.0f);  // 40 px of travel at speed 1

    CHECK(ws::ui::gui_test::stub.quads == 0);
    CHECK(ws::ui::gui_test::stub.rounded == 0);
    CHECK(ws::ui::gui_test::stub.sprites == 0);
    CHECK(ws::ui::gui_test::stub.texts == 0);
    CHECK(ws::ui::gui_test::stub.clip_pushes == 0);
    CHECK(ws::ui::gui_test::stub.clip_pops == 0);
    CHECK(ws::ui::gui_test::stub.layer_sets == 0);
}

TEST_CASE("custom looks paint over the slider behaviors") {
    using ws::ui::r2d::draw_quad;
    using ws::ui::r2d::draw_rounded_quad;

    // The seam from the other side, the analog rows' turn: each
    // sub-block resets the stub, runs one behavior, paints a
    // hand-rolled look, and checks the counters carry exactly the
    // custom draws - no stock look leaked.
    GuiContext gui;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 120.0f, 24.0f};
    const auto tap = [&gui]() {
        gui.begin_frame(snap(60.0f, 10.0f, false, true, true));
    };

    // slider: a sharp rail, a flat fill and a rounded handle.
    ws::ui::gui_test::stub.reset();
    tap();
    {
        float value = 0.0f;
        const ws::ui::gui::SliderResult result = ws::ui::gui::slider_behavior(
            gui, gui.ids().id("slide"), rect, value, 0.0f, 100.0f, 1.0f);
        draw_quad(rect.x, rect.y + 9.0f, rect.width, 6.0f,
                  kDefaultChrome.control_bg);
        draw_quad(rect.x + 1.0f, rect.y + 10.0f, 55.0f, 4.0f,
                  kDefaultChrome.control_on);
        draw_rounded_quad(rect.x + 56.0f, rect.y, 8.0f, rect.height, 4.0f,
                          kDefaultChrome.text);
        gui.end_frame();
        CHECK(result.changed);
        CHECK(value == 50.0f);  // dead center of the track
    }
    CHECK(ws::ui::gui_test::stub.quads == 2);
    CHECK(ws::ui::gui_test::stub.rounded == 1);

    // drag_float: the row face and a bar for the value line.
    ws::ui::gui_test::stub.reset();
    tap();
    {
        float value = 0.0f;
        const ws::ui::gui::DragFloatResult result =
            ws::ui::gui::drag_float_behavior(gui, gui.ids().id("drag"),
                                             rect, value, -100.0f, 100.0f,
                                             1.0f);
        draw_quad(rect.x, rect.y, rect.width, rect.height,
                  kDefaultChrome.control_pressed);
        draw_quad(rect.x + 8.0f, rect.y + 8.0f, 40.0f, 8.0f,
                  kDefaultChrome.text);
        gui.end_frame();
        CHECK(!result.changed);  // the one-frame tap moves nothing
    }
    CHECK(ws::ui::gui_test::stub.quads == 2);
    CHECK(ws::ui::gui_test::stub.texts == 0);
}

TEST_CASE("the tab behavior drives the interaction and draws nothing") {
    using ws::ui::gui::WidgetState;

    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 120.0f, 24.0f};

    // The loop-widget seam: the strip owns the id scope (its label, the
    // index, then "tab" under them), so the test spells the id the
    // strip's way too - the behavior takes the finished id per tab.
    gui.ids().push("strip");
    gui.ids().push(0);
    const ws::ui::gui::WidgetId tab_id = gui.ids().id("tab");
    gui.ids().pop();
    gui.ids().pop();

    // A one-frame tap inside clicks - folded with the focused tab's
    // Enter/Space into the one "pick me" signal.
    gui.begin_frame(snap(60.0f, 10.0f, false, true, true));
    const ws::ui::gui::TabResult result =
        ws::ui::gui::tab_behavior(gui, tab_id, rect);
    gui.end_frame();
    CHECK(result.clicked);
    CHECK(result.hit.pressed);
    CHECK(result.state == WidgetState::Pressed);

    // Enter on the focused tab: the keyboard half of the same fold.
    gui.set_focus(tab_id);
    InputFrame keys = snap(-100.0f, -100.0f);
    keys.key_pressed[static_cast<std::size_t>(ws::input::keys::kEnter)] =
        true;
    gui.begin_frame(keys);
    const ws::ui::gui::TabResult keyed =
        ws::ui::gui::tab_behavior(gui, tab_id, rect);
    gui.end_frame();
    CHECK(keyed.clicked);
    CHECK(keyed.focused);

    // A quiet frame claims nothing: the focus is the resting state.
    gui.begin_frame(snap(-100.0f, -100.0f));
    const ws::ui::gui::TabResult quiet =
        ws::ui::gui::tab_behavior(gui, tab_id, rect);
    gui.end_frame();
    CHECK(!quiet.clicked);
    CHECK(quiet.state == WidgetState::Focused);

    CHECK(ws::ui::gui_test::stub.quads == 0);
    CHECK(ws::ui::gui_test::stub.rounded == 0);
    CHECK(ws::ui::gui_test::stub.sprites == 0);
    CHECK(ws::ui::gui_test::stub.texts == 0);
    CHECK(ws::ui::gui_test::stub.clip_pushes == 0);
    CHECK(ws::ui::gui_test::stub.clip_pops == 0);
    CHECK(ws::ui::gui_test::stub.layer_sets == 0);
}

TEST_CASE("custom looks paint over the tab behavior, panel_draw the chrome") {
    using ws::ui::r2d::draw_quad;

    // The seam from the other side, the strip's turn: a custom tab look
    // owns every pixel over the behavior, and panel_draw answers the
    // stock chrome alone - the border frame and the inset face, the
    // same two quads begin_panel paints, with none of its pushes.
    GuiContext gui;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 120.0f, 24.0f};
    const auto tap = [&gui]() {
        gui.begin_frame(snap(60.0f, 10.0f, false, true, true));
    };

    // tab: a flat face with the accent as an underline, no label.
    ws::ui::gui_test::stub.reset();
    tap();
    {
        gui.ids().push("strip");
        gui.ids().push(0);
        const ws::ui::gui::TabResult result = ws::ui::gui::tab_behavior(
            gui, gui.ids().id("tab"), rect);
        gui.ids().pop();
        gui.ids().pop();
        draw_quad(rect.x, rect.y, rect.width, rect.height,
                  kDefaultChrome.control_bg);
        draw_quad(rect.x, rect.y + rect.height - 2.0f, rect.width, 2.0f,
                  kDefaultChrome.control_on);
        gui.end_frame();
        CHECK(result.clicked);
    }
    CHECK(ws::ui::gui_test::stub.quads == 2);
    CHECK(ws::ui::gui_test::stub.texts == 0);

    // panel_draw: the chrome alone for a custom container - exactly the
    // border and the inset face, and no clip, layer or id push rides
    // along (those are begin_panel's).
    ws::ui::gui_test::stub.reset();
    gui.begin_frame(snap(-100.0f, -100.0f));
    ws::ui::gui::panel_draw(ws::ui::gui::Rect{10.0f, 20.0f, 200.0f, 100.0f},
                            kDefaultChrome);
    gui.end_frame();
    CHECK(ws::ui::gui_test::stub.quads == 2);
    CHECK(ws::ui::gui_test::stub.clip_pushes == 0);
    CHECK(ws::ui::gui_test::stub.layer_sets == 0);
}

TEST_CASE("the table keeps its late reads under the grid walk") {
    GuiContext gui;
    LayoutCursor layout;
    static const ws::ui::gui::TableColumn cols[] = {{"name", 100.0f},
                                                    {"value", 0.0f}};
    static const char* const cells[] = {"alpha", "1", "beta", "2",
                                       "gamma", "3"};
    int selected = 0;
    bool changed = false;

    // The rows id under the table's label, then by index - spelled the
    // way the focus checks need it.
    const auto row_w = [&](int i) {
        gui.ids().push("grid");
        gui.ids().push(i);
        const ws::ui::gui::WidgetId w = gui.ids().id("row");
        gui.ids().pop();
        gui.ids().pop();
        return w;
    };
    // The counters reset per frame: each pass asserts ITS frame's draws.
    const auto pass = [&](const InputFrame& frame) {
        ws::ui::gui_test::stub.reset();
        gui.begin_frame(frame);
        (void)layout.begin_panel({0.0f, 0.0f, 300.0f, 100.0f}, 0.0f);
        changed = ws::ui::gui::table(gui, layout, kDefaultChrome, "grid",
                                     cols, 2, cells, 3, &selected, 300.0f);
        layout.end_panel();
        gui.end_frame();
    };

    // A tap on the last row picks it and latches the focus: the pressed
    // face and the ring both draw on the tapped row this frame (with the
    // header bar, three rounded quads).
    pass(snap(150.0f, 80.0f, false, true, true));
    CHECK(selected == 2);
    CHECK(changed);
    CHECK(gui.is_focused(row_w(2)));
    CHECK(ws::ui::gui_test::stub.rounded == 3);

    // Up walks the focus to row 1 and nothing else: the selection
    // holds. The ring reads AFTER the step, so it draws NOWHERE this
    // frame - the handing row no longer holds focus and the landing
    // row's iteration already ran. A fold-captured ring would draw one
    // on the handing row; the header bar is the only rounded quad.
    InputFrame up = snap(-100.0f, -100.0f);
    up.key_pressed[static_cast<std::size_t>(ws::input::keys::kUp)] = true;
    pass(up);
    CHECK(selected == 2);
    CHECK(!changed);
    CHECK(gui.is_focused(row_w(1)));
    CHECK(ws::ui::gui_test::stub.rounded == 1);

    // Again: the focus walks to row 0 the same way.
    pass(up);
    CHECK(gui.is_focused(row_w(0)));
    CHECK(ws::ui::gui_test::stub.rounded == 1);

    // At the top the walk clamps: no step, the focus holds, and the
    // ring draws on the row that keeps it.
    pass(up);
    CHECK(gui.is_focused(row_w(0)));
    CHECK(ws::ui::gui_test::stub.rounded == 2);

    // Enter picks the focused row - the selection follows the walk.
    InputFrame enter = snap(-100.0f, -100.0f);
    enter.key_pressed[static_cast<std::size_t>(ws::input::keys::kEnter)] =
        true;
    pass(enter);
    CHECK(selected == 0);
    CHECK(changed);
    CHECK(gui.is_focused(row_w(0)));
    CHECK(ws::ui::gui_test::stub.rounded == 2);
}

TEST_CASE("popup_placement: flip and clamp as pure geometry") {
    using ws::ui::gui::PopupAnchor;
    const ws::ui::r2d::FrameSize view{800.0f, 600.0f};
    const ws::ui::gui::Rect row{100.0f, 50.0f, 120.0f, 24.0f};

    // below: the dropdown hangs under the anchor's bottom edge.
    const ws::ui::gui::Rect under = ws::ui::gui::popup_placement(
        row, 100.0f, 80.0f, PopupAnchor::below, view);
    CHECK(under.x == 100.0f);
    CHECK(under.y == 74.0f);
    CHECK(under.width == 100.0f);
    CHECK(under.height == 80.0f);

    // below with no room under: flips above the anchor's top.
    const ws::ui::gui::Rect above = ws::ui::gui::popup_placement(
        ws::ui::gui::Rect{100.0f, 550.0f, 120.0f, 24.0f}, 100.0f, 80.0f,
        PopupAnchor::below, view);
    CHECK(above.y == 470.0f);  // 550 - 80: the flip fit, the clamp holds

    // right: the submenu shape hangs past the right edge...
    const ws::ui::gui::Rect right = ws::ui::gui::popup_placement(
        row, 100.0f, 80.0f, PopupAnchor::right, view);
    CHECK(right.x == 220.0f);
    // ...or flips to the left when the viewport lacks the room.
    const ws::ui::gui::Rect flipped = ws::ui::gui::popup_placement(
        ws::ui::gui::Rect{700.0f, 50.0f, 120.0f, 24.0f}, 100.0f, 80.0f,
        PopupAnchor::right, view);
    CHECK(flipped.x == 600.0f);  // 700 - 100

    // The size clamps to the viewport first: an oversized window fills
    // what fits, and the position clamp parks it at the origin.
    const ws::ui::gui::Rect fill = ws::ui::gui::popup_placement(
        row, 2000.0f, 5000.0f, PopupAnchor::below, view);
    CHECK(fill.width == 800.0f);
    CHECK(fill.height == 600.0f);
    CHECK(fill.x == 0.0f);
    CHECK(fill.y == 0.0f);

    // A degenerate anchor - a point, a context menu's press point -
    // hangs below the point at its own x.
    const ws::ui::gui::Rect point = ws::ui::gui::popup_placement(
        ws::ui::gui::Rect{400.0f, 300.0f, 0.0f, 0.0f}, 100.0f, 80.0f,
        PopupAnchor::below, view);
    CHECK(point.x == 400.0f);
    CHECK(point.y == 300.0f);

    // The delegate: headless the frame is empty, so place_popup answers
    // exactly the pure spelling against an empty viewport.
    const ws::ui::gui::Rect live =
        ws::ui::gui::place_popup(row, 100.0f, 80.0f);
    const ws::ui::gui::Rect dead = ws::ui::gui::popup_placement(
        row, 100.0f, 80.0f, PopupAnchor::below,
        ws::ui::r2d::FrameSize{});
    CHECK(live.x == dead.x);
    CHECK(live.y == dead.y);
    CHECK(live.width == dead.width);
    CHECK(live.height == dead.height);
    CHECK(dead.x == 0.0f);  // and that answer is the origin clamp
    CHECK(dead.y == 0.0f);
}

TEST_CASE("the combo behavior drives the interaction and draws nothing") {
    using ws::ui::gui::WidgetState;

    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 120.0f, 24.0f};
    const ws::ui::gui::WidgetId id = gui.ids().id("shape");
    bool open = false;

    // A one-frame tap on the row toggles `open` - the caller's bool is
    // the popup's state, and the behavior is the row's whole interaction.
    gui.begin_frame(snap(60.0f, 12.0f, false, true, true));
    const ws::ui::gui::ComboResult tapped =
        ws::ui::gui::combo_behavior(gui, id, rect, open, false);
    gui.end_frame();
    CHECK(tapped.toggled);
    CHECK(tapped.hit.clicked);
    CHECK(open == true);

    // A second tap closes it - the same fold, the other direction.
    gui.begin_frame(snap(60.0f, 12.0f, false, true, true));
    const ws::ui::gui::ComboResult closed =
        ws::ui::gui::combo_behavior(gui, id, rect, open, false);
    gui.end_frame();
    CHECK(closed.toggled);
    CHECK(open == false);

    // Enter on the focused row flips it too, like a header.
    gui.set_focus(id);
    InputFrame keys = snap(-100.0f, -100.0f);
    keys.key_pressed[static_cast<std::size_t>(ws::input::keys::kEnter)] =
        true;
    gui.begin_frame(keys);
    const ws::ui::gui::ComboResult keyed =
        ws::ui::gui::combo_behavior(gui, id, rect, open, false);
    gui.end_frame();
    CHECK(keyed.toggled);
    CHECK(keyed.focused);
    CHECK(open == true);

    // The empty combo refuses everything and drops a pre-latched `open`:
    // the tap lands (the hit is real) but a bare row never pretends, so
    // a later refill starts closed.
    bool empty_open = true;
    gui.begin_frame(snap(60.0f, 12.0f, false, true, true));
    const ws::ui::gui::ComboResult bare =
        ws::ui::gui::combo_behavior(gui, id, rect, empty_open, true);
    gui.end_frame();
    CHECK(bare.hit.clicked);  // the tap landed...
    CHECK(!bare.toggled);     // ...and `empty` refused it anyway
    CHECK(empty_open == false);

    // The trailing lock: a dead row claims nothing, and a dropdown left
    // open by a lock landing mid-flight closes - the whole-widget lock.
    bool locked_open = true;
    gui.begin_frame(snap(-100.0f, -100.0f));
    const ws::ui::gui::ComboResult locked =
        ws::ui::gui::combo_behavior(gui, id, rect, locked_open, false, true);
    gui.end_frame();
    CHECK(!locked.toggled);
    CHECK(locked.state == WidgetState::Disabled);
    CHECK(locked_open == false);

    // The behavior drew nothing across every pass above.
    CHECK(ws::ui::gui_test::stub.quads == 0);
    CHECK(ws::ui::gui_test::stub.rounded == 0);
    CHECK(ws::ui::gui_test::stub.sprites == 0);
    CHECK(ws::ui::gui_test::stub.texts == 0);
    CHECK(ws::ui::gui_test::stub.clip_pushes == 0);
    CHECK(ws::ui::gui_test::stub.clip_pops == 0);
    CHECK(ws::ui::gui_test::stub.layer_sets == 0);
}

TEST_CASE("custom looks paint over the combo behavior") {
    using ws::ui::r2d::draw_quad;

    GuiContext gui;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 120.0f, 24.0f};
    static const char* const kItems[] = {"alpha", "beta"};
    const ws::ui::gui::WidgetId id = gui.ids().id("shape");
    bool open = false;

    // The stock draw over a quiet behavior result: the face (rounded),
    // then the right-edge marker and the "label: item" line - the stub
    // theme is fontless, so the label is its bar quad.
    ws::ui::gui_test::stub.reset();
    gui.begin_frame(snap(-100.0f, -100.0f));
    const ws::ui::gui::ComboResult result =
        ws::ui::gui::combo_behavior(gui, id, rect, open, false);
    ws::ui::gui::combo_draw(rect, result, kDefaultChrome, "shape", kItems, 2,
                            1, open);
    gui.end_frame();
    CHECK(ws::ui::gui_test::stub.rounded == 1);  // the face
    CHECK(ws::ui::gui_test::stub.quads == 2);    // the marker + the label bar
    CHECK(ws::ui::gui_test::stub.texts == 0);

    // A custom look owns every pixel over the behavior: two hand quads
    // replace the stock draw, and the interaction is untouched.
    ws::ui::gui_test::stub.reset();
    bool open2 = false;
    gui.begin_frame(snap(60.0f, 12.0f, false, true, true));
    const ws::ui::gui::ComboResult tapped =
        ws::ui::gui::combo_behavior(gui, id, rect, open2, false);
    draw_quad(rect.x, rect.y, rect.width, rect.height,
              kDefaultChrome.control_bg);
    draw_quad(rect.x + rect.width - 12.0f, rect.y + 8.0f, 8.0f, 8.0f,
              kDefaultChrome.control_on);
    gui.end_frame();
    CHECK(tapped.toggled);
    CHECK(open2 == true);
    CHECK(ws::ui::gui_test::stub.quads == 2);
    CHECK(ws::ui::gui_test::stub.rounded == 0);
}

TEST_CASE("the menu hover engine clocks rest and ripens on its delays") {
    using ws::ui::gui::MenuHoverAction;
    using ws::ui::gui::MenuHoverState;
    using ws::ui::gui::menu_hover_step;

    // Binary-exact dt and delays, so the ripening frame is the third
    // step, not a float hair off it.
    MenuHoverState hover;
    CHECK(menu_hover_step(hover, 0, 0.25f, 0.75f, 1.0f, true, false) ==
          MenuHoverAction::none);
    CHECK(menu_hover_step(hover, 0, 0.25f, 0.75f, 1.0f, true, false) ==
          MenuHoverAction::none);
    CHECK(hover.item == 0);
    CHECK(hover.rest == 0.5f);

    // The rest crosses the submenu delay: the child ripens - and it
    // keeps ripening every frame the pointer stays, the caller's own
    // state applying it once (the chain's open levels).
    CHECK(menu_hover_step(hover, 0, 0.25f, 0.75f, 1.0f, true, false) ==
          MenuHoverAction::open_child);
    CHECK(hover.rest == 0.75f);
    CHECK(menu_hover_step(hover, 0, 0.25f, 0.75f, 1.0f, true, false) ==
          MenuHoverAction::open_child);

    // A move restarts the clock: the ripe row's answer goes quiet, and
    // a plain row never ripens anything however long it rests.
    CHECK(menu_hover_step(hover, 1, 0.25f, 0.75f, 1.0f, true, false) ==
          MenuHoverAction::none);
    CHECK(hover.item == 1);
    CHECK(hover.rest == 0.25f);
    CHECK(menu_hover_step(hover, 1, 2.0f, 0.75f, 1.0f, false, false) ==
          MenuHoverAction::none);
    CHECK(hover.rest == 2.25f);

    // The dead row's tooltip: its own delay, and a rest with no reason
    // to show answers nothing at all.
    MenuHoverState dead;
    CHECK(menu_hover_step(dead, 2, 0.25f, 0.75f, 0.75f, false, true) ==
          MenuHoverAction::none);
    CHECK(menu_hover_step(dead, 2, 0.25f, 0.75f, 0.75f, false, true) ==
          MenuHoverAction::none);
    CHECK(menu_hover_step(dead, 2, 0.25f, 0.75f, 0.75f, false, true) ==
          MenuHoverAction::show_tooltip);
    CHECK(menu_hover_step(dead, 2, 10.0f, 0.75f, 0.75f, false, false) ==
          MenuHoverAction::none);

    // The clear: the pointer left every row - the caller's
    // assignment, the state's own zero spelling. The next rest starts
    // over: had the clock survived, 10.25s would ripen the tooltip.
    dead = {};
    CHECK(dead.item == -1);
    CHECK(dead.rest == 0.0f);
    CHECK(menu_hover_step(dead, 2, 0.25f, 0.75f, 0.75f, false, true) ==
          MenuHoverAction::none);
}

TEST_CASE("the scroll drag behavior drives the flow and draws nothing") {
    using ws::ui::gui::ScrollDragState;
    using ws::ui::gui::ScrollState;
    ws::ui::gui_test::stub.reset();

    GuiContext gui;
    ScrollState scroll;
    ScrollDragState drag;
    const ws::ui::gui::Rect viewport{12.0f, 12.0f, 176.0f, 76.0f};
    const ws::input::PointerId finger{7u};

    // The frames run the platform's own shape: the edges land on the
    // frame, the gui adopts them, and next_frame clears before the
    // next frame accumulates.
    ws::input::InputFrame frame;

    // Frame 1: the finger presses inside the viewport - the press is
    // watched, not taken.
    (void)frame.touch_down(finger, 60.0f, 52.0f);
    gui.begin_frame(frame);
    ws::ui::gui::scroll_drag_behavior(gui, scroll, drag, viewport,
                                      kDefaultChrome);
    gui.end_frame();
    CHECK(scroll.offset == 0.0f);
    frame.next_frame();

    // Frame 2: a 4px wander (threshold 8) - still the child's tap.
    (void)frame.touch_move(finger, 60.0f, 48.0f);
    gui.begin_frame(frame);
    ws::ui::gui::scroll_drag_behavior(gui, scroll, drag, viewport,
                                      kDefaultChrome);
    gui.end_frame();
    CHECK(scroll.offset == 0.0f);
    frame.next_frame();

    // Frame 3: past the threshold - the crossing frame applies no
    // jump; the deltas start at the crossing point.
    (void)frame.touch_move(finger, 60.0f, 20.0f);
    gui.begin_frame(frame);
    ws::ui::gui::scroll_drag_behavior(gui, scroll, drag, viewport,
                                      kDefaultChrome);
    gui.end_frame();
    CHECK(scroll.offset == 0.0f);
    frame.next_frame();

    // Frame 4: the drag follows 1:1 - 28px up is 28px of offset, the
    // finger's capture running past the viewport's edge.
    (void)frame.touch_move(finger, 60.0f, -8.0f);
    gui.begin_frame(frame);
    ws::ui::gui::scroll_drag_behavior(gui, scroll, drag, viewport,
                                      kDefaultChrome);
    gui.end_frame();
    CHECK(scroll.offset == 28.0f);
    frame.next_frame();

    // Frame 5: the lift lands the last delta with it, then the track
    // is gone - a later quiet frame drags nothing.
    frame.touch_up(finger, 60.0f, -20.0f, false);
    gui.begin_frame(frame);
    ws::ui::gui::scroll_drag_behavior(gui, scroll, drag, viewport,
                                      kDefaultChrome);
    gui.end_frame();
    CHECK(scroll.offset == 40.0f);
    frame.next_frame();
    gui.begin_frame(frame);
    ws::ui::gui::scroll_drag_behavior(gui, scroll, drag, viewport,
                                      kDefaultChrome);
    gui.end_frame();
    CHECK(scroll.offset == 40.0f);

    // The behavior is the interaction alone: every counter at zero.
    CHECK(ws::ui::gui_test::stub.quads == 0);
    CHECK(ws::ui::gui_test::stub.rounded == 0);
    CHECK(ws::ui::gui_test::stub.sprites == 0);
    CHECK(ws::ui::gui_test::stub.texts == 0);
    CHECK(ws::ui::gui_test::stub.clip_pushes == 0);
    CHECK(ws::ui::gui_test::stub.clip_pops == 0);
    CHECK(ws::ui::gui_test::stub.layer_sets == 0);
}

TEST_CASE("the thumb behavior grabs, jumps and follows - the fitted region "
          "shows nothing") {
    using ws::ui::gui::ScrollbarThumb;
    using ws::ui::gui::ScrollDragState;
    using ws::ui::gui::ScrollState;
    ws::ui::gui_test::stub.reset();

    GuiContext gui;
    ScrollState scroll;
    ScrollDragState drag;
    // A 176x75 viewport over 300px of content (max_offset 225): the
    // thumb is 75*75/300 = 18.75px tall, traveling 56.25px - all
    // binary-exact, so the checks pin the math, not a float hair.
    const ws::ui::gui::Rect viewport{12.0f, 12.0f, 176.0f, 75.0f};
    scroll.max_offset = 225.0f;
    const ws::input::PointerId finger{7u};

    // A quiet frame: the thumb sits at the top, nothing grabbed.
    ws::input::InputFrame frame;
    gui.begin_frame(frame);
    const ScrollbarThumb top =
        ws::ui::gui::scroll_thumb_behavior(gui, scroll, drag, viewport,
                                           kDefaultChrome);
    gui.end_frame();
    CHECK(top.shows);
    CHECK(top.top == 12.0f);
    CHECK(top.height == 18.75f);
    frame.next_frame();

    // A press below the thumb jumps it centered under the finger: py
    // 49.5 is half the travel down, so the offset lands at 112.5 and
    // the thumb's top at 40.125 - the pixel under the finger stays
    // under the finger (grab = half the thumb).
    (void)frame.touch_down(finger, 180.0f, 49.5f);
    gui.begin_frame(frame);
    const ScrollbarThumb jumped =
        ws::ui::gui::scroll_thumb_behavior(gui, scroll, drag, viewport,
                                           kDefaultChrome);
    gui.end_frame();
    CHECK(jumped.shows);
    CHECK(scroll.offset == 112.5f);
    CHECK(jumped.top == 40.125f);
    CHECK(scroll.thumb == finger);
    CHECK(scroll.thumb_grab == 9.375f);
    frame.next_frame();

    // The follow maps the held pointer 1:1 through the track: the
    // finger pulled above the viewport's top clamps the offset to 0
    // and the thumb parks at the top.
    (void)frame.touch_move(finger, 180.0f, 6.0f);
    gui.begin_frame(frame);
    const ScrollbarThumb followed =
        ws::ui::gui::scroll_thumb_behavior(gui, scroll, drag, viewport,
                                           kDefaultChrome);
    gui.end_frame();
    CHECK(followed.shows);
    CHECK(scroll.offset == 0.0f);
    CHECK(followed.top == 12.0f);
    frame.next_frame();

    // The lift ends the grab; a fitted region answers shows=false and
    // takes no press at all - the same input, the content shrunk to
    // fit.
    frame.touch_up(finger, 180.0f, 6.0f, false);
    scroll.max_offset = 0.0f;
    gui.begin_frame(frame);
    const ScrollbarThumb fitted =
        ws::ui::gui::scroll_thumb_behavior(gui, scroll, drag, viewport,
                                           kDefaultChrome);
    gui.end_frame();
    CHECK(!fitted.shows);

    // The behavior draws nothing through the whole grab.
    CHECK(ws::ui::gui_test::stub.quads == 0);
    CHECK(ws::ui::gui_test::stub.rounded == 0);
}

TEST_CASE("custom looks paint the scrollbar over the thumb behavior") {
    using ws::ui::r2d::draw_quad;
    using ws::ui::gui::ScrollbarThumb;
    using ws::ui::gui::ScrollDragState;
    using ws::ui::gui::ScrollState;
    ws::ui::gui_test::stub.reset();

    GuiContext gui;
    ScrollState scroll;
    ScrollDragState drag;
    const ws::ui::gui::Rect viewport{12.0f, 12.0f, 176.0f, 75.0f};
    scroll.max_offset = 225.0f;

    // The stock paint over a quiet answer: ONE rounded quad - the
    // thumb, 4px wide at the gutter's left edge, in the border face.
    ws::input::InputFrame frame;
    gui.begin_frame(frame);
    const ScrollbarThumb thumb =
        ws::ui::gui::scroll_thumb_behavior(gui, scroll, drag, viewport,
                                           kDefaultChrome);
    ws::ui::gui::scrollbar_draw(viewport, thumb, kDefaultChrome);
    gui.end_frame();
    CHECK(ws::ui::gui_test::stub.rounded == 1);
    CHECK(ws::ui::gui_test::stub.quads == 0);

    // The fitted answer paints nothing, wherever the caller puts it.
    ws::ui::gui_test::stub.reset();
    const ScrollbarThumb none;
    ws::ui::gui::scrollbar_draw(viewport, none, kDefaultChrome);
    CHECK(ws::ui::gui_test::stub.rounded == 0);

    // A custom look owns every pixel over the behavior: an 8px square
    // accent thumb in place of the stock bar, the rest untouched.
    ws::ui::gui_test::stub.reset();
    scroll.offset = 112.5f;  // mid-content, the same half-travel spot
    gui.begin_frame(frame);
    const ScrollbarThumb mid =
        ws::ui::gui::scroll_thumb_behavior(gui, scroll, drag, viewport,
                                           kDefaultChrome);
    draw_quad(viewport.x + viewport.width - 12.0f, mid.top + mid.height -
                                                       8.0f,
              8.0f, 8.0f, kDefaultChrome.control_on);
    gui.end_frame();
    CHECK(mid.shows);
    CHECK(mid.top == 12.0f + 112.5f / 225.0f * (75.0f - 18.75f));
    CHECK(mid.top == 40.125f);
    CHECK(ws::ui::gui_test::stub.quads == 1);
    CHECK(ws::ui::gui_test::stub.rounded == 0);
}

TEST_CASE("the text input behavior edits and maps the caret, drawing "
          "nothing") {
    namespace keys = ws::input::keys;
    using ws::ui::gui::TextInputResult;
    using ws::ui::gui::TextInputState;

    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    char buf[16] = "";
    TextInputState edit;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 176.0f, 24.0f};
    const ws::ui::gui::WidgetId id = gui.ids().id("field");

    // A press inside the row claims the focus the same frame; an empty
    // buffer clamps the placement to byte 0, the caret at the content
    // edge.
    gui.begin_frame(snap(20.0f, 12.0f, true, true));
    const TextInputResult placed =
        ws::ui::gui::text_input_behavior(gui, id, rect, buf,
                                         static_cast<int>(sizeof buf),
                                         edit, kDefaultChrome);
    gui.end_frame();
    CHECK(placed.focused);
    CHECK(placed.hit.pressed);
    CHECK(edit.caret == 0);
    CHECK(edit.anchor == 0);
    CHECK(placed.caret_x == 12.0f);  // content_x, nothing typed yet

    // Typing lands at the caret; the mapped caret follows the pen and
    // the span stays closed while no selection stands.
    InputFrame typing = snap(-100.0f, -100.0f);
    typing.text_input = {'a', 'b', 'c', 'd'};
    gui.begin_frame(typing);
    const TextInputResult typed =
        ws::ui::gui::text_input_behavior(gui, id, rect, buf,
                                         static_cast<int>(sizeof buf),
                                         edit, kDefaultChrome);
    gui.end_frame();
    CHECK(typed.edited);
    CHECK(std::strcmp(buf, "abcd") == 0);
    CHECK(edit.caret == 4);
    CHECK(typed.caret_x == 36.0f);  // content_x + four fontless chars
    CHECK(typed.sel_x0 == typed.sel_x1);

    // A press into the content places the caret by the mid-advance
    // rule: x 26 is 14px in, past the mid of byte 2 - caret 2, the
    // anchor collapsed onto it by the plain press.
    gui.begin_frame(snap(26.0f, 12.0f, true, true));
    const TextInputResult jumped =
        ws::ui::gui::text_input_behavior(gui, id, rect, buf,
                                         static_cast<int>(sizeof buf),
                                         edit, kDefaultChrome);
    gui.end_frame();
    CHECK(edit.caret == 2);
    CHECK(edit.anchor == 2);
    CHECK(jumped.caret_x == 24.0f);

    // Select all spreads the span over the whole line - and a
    // selection is not an edit: the frame answers false.
    InputFrame all = snap(-100.0f, -100.0f);
    all.key_down[static_cast<std::size_t>(keys::kLeftControl)] = true;
    all.key_pressed[static_cast<std::size_t>(keys::kA)] = true;
    gui.begin_frame(all);
    const TextInputResult selected =
        ws::ui::gui::text_input_behavior(gui, id, rect, buf,
                                         static_cast<int>(sizeof buf),
                                         edit, kDefaultChrome);
    gui.end_frame();
    CHECK(!selected.edited);
    CHECK(edit.anchor == 0);
    CHECK(edit.caret == 4);
    CHECK(selected.sel_x0 == 12.0f);
    CHECK(selected.sel_x1 == 36.0f);

    // The blink phase rides the frame clock through the result: the
    // select-all rewound it solid, a quarter-second frame reads 0.25,
    // the next 0.5 - binary-exact steps.
    InputFrame tick = snap(-100.0f, -100.0f);
    tick.dt_seconds = 0.25f;
    gui.begin_frame(tick);
    const TextInputResult phase1 =
        ws::ui::gui::text_input_behavior(gui, id, rect, buf,
                                         static_cast<int>(sizeof buf),
                                         edit, kDefaultChrome);
    gui.end_frame();
    CHECK(phase1.blink == 0.25f);
    gui.begin_frame(tick);
    const TextInputResult phase2 =
        ws::ui::gui::text_input_behavior(gui, id, rect, buf,
                                         static_cast<int>(sizeof buf),
                                         edit, kDefaultChrome);
    gui.end_frame();
    CHECK(phase2.blink == 0.5f);

    // A null buffer lays the same row inert: no edit, no crash, the
    // caret parked at the content edge.
    const ws::ui::gui::WidgetId bare_id = gui.ids().id("bare");
    TextInputState bare_edit;
    gui.begin_frame(snap(-100.0f, -100.0f));
    const TextInputResult bare =
        ws::ui::gui::text_input_behavior(gui, bare_id, rect, nullptr, 16,
                                         bare_edit, kDefaultChrome);
    gui.end_frame();
    CHECK(!bare.edited);
    CHECK(bare.caret_x == 12.0f);

    // The behavior never drew through any of it.
    CHECK(ws::ui::gui_test::stub.quads == 0);
    CHECK(ws::ui::gui_test::stub.rounded == 0);
    CHECK(ws::ui::gui_test::stub.sprites == 0);
    CHECK(ws::ui::gui_test::stub.texts == 0);
    CHECK(ws::ui::gui_test::stub.clip_pushes == 0);
    CHECK(ws::ui::gui_test::stub.clip_pops == 0);
    CHECK(ws::ui::gui_test::stub.layer_sets == 0);
}

TEST_CASE("custom looks paint over the text input behavior") {
    using ws::ui::r2d::draw_quad;
    namespace keys = ws::input::keys;
    using ws::ui::gui::TextInputResult;
    using ws::ui::gui::TextInputState;

    GuiContext gui;
    char buf[16] = "";
    TextInputState edit;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 176.0f, 24.0f};
    const ws::ui::gui::WidgetId id = gui.ids().id("field");

    // The stock draw over a quiet unfocused answer: the face, and the
    // empty buffer's placeholder bar - the fontless stub theme's two
    // pieces.
    ws::ui::gui_test::stub.reset();
    gui.begin_frame(snap(-100.0f, -100.0f));
    const TextInputResult quiet =
        ws::ui::gui::text_input_behavior(gui, id, rect, buf,
                                         static_cast<int>(sizeof buf),
                                         edit, kDefaultChrome);
    ws::ui::gui::text_input_draw(rect, quiet, kDefaultChrome, "label", buf,
                                 static_cast<int>(sizeof buf));
    gui.end_frame();
    CHECK(ws::ui::gui_test::stub.rounded == 1);  // the face
    CHECK(ws::ui::gui_test::stub.quads == 1);    // the placeholder bar

    // The press claims the focus; typing fills the buffer; the stock
    // draw over the answer paints the ring, the face, the text bar and
    // the solid caret - the standard field's pixels.
    gui.begin_frame(snap(20.0f, 12.0f, true, true));
    (void)ws::ui::gui::text_input_behavior(gui, id, rect, buf,
                                           static_cast<int>(sizeof buf),
                                           edit, kDefaultChrome);
    gui.end_frame();
    ws::ui::gui_test::stub.reset();
    InputFrame typing = snap(-100.0f, -100.0f);
    typing.text_input = {'h', 'i'};
    gui.begin_frame(typing);
    const TextInputResult typed =
        ws::ui::gui::text_input_behavior(gui, id, rect, buf,
                                         static_cast<int>(sizeof buf),
                                         edit, kDefaultChrome);
    ws::ui::gui::text_input_draw(rect, typed, kDefaultChrome, "label", buf,
                                 static_cast<int>(sizeof buf));
    gui.end_frame();
    CHECK(ws::ui::gui_test::stub.rounded == 2);  // the ring + the face
    CHECK(ws::ui::gui_test::stub.quads == 2);    // the text bar + the caret
    CHECK(typed.caret_x == 24.0f);

    // Select-all bands the line: one accent quad under the text, the
    // caret solid at the selection's live end.
    ws::ui::gui_test::stub.reset();
    InputFrame all = snap(-100.0f, -100.0f);
    all.key_down[static_cast<std::size_t>(keys::kLeftControl)] = true;
    all.key_pressed[static_cast<std::size_t>(keys::kA)] = true;
    gui.begin_frame(all);
    const TextInputResult selected =
        ws::ui::gui::text_input_behavior(gui, id, rect, buf,
                                         static_cast<int>(sizeof buf),
                                         edit, kDefaultChrome);
    ws::ui::gui::text_input_draw(rect, selected, kDefaultChrome, "label",
                                 buf, static_cast<int>(sizeof buf));
    gui.end_frame();
    CHECK(ws::ui::gui_test::stub.rounded == 2);  // the ring + the face
    CHECK(ws::ui::gui_test::stub.quads == 3);    // band + text + caret
    CHECK(selected.sel_x0 == 12.0f);
    CHECK(selected.sel_x1 == 24.0f);

    // A custom look owns every pixel over the same behavior: an
    // underline field - the accent baseline, the text bar and a caret
    // block at the mapped x - with the standing selection replaced by
    // the next keystroke through the behavior untouched.
    ws::ui::gui_test::stub.reset();
    InputFrame replace = snap(-100.0f, -100.0f);
    replace.text_input = {'!'};
    gui.begin_frame(replace);
    const TextInputResult swapped =
        ws::ui::gui::text_input_behavior(gui, id, rect, buf,
                                         static_cast<int>(sizeof buf),
                                         edit, kDefaultChrome);
    draw_quad(rect.x, rect.y + rect.height - 2.0f, rect.width, 2.0f,
              kDefaultChrome.control_on);
    draw_quad(rect.x + kDefaultChrome.padding, 6.0f, 6.0f, 12.0f,
              kDefaultChrome.text);
    draw_quad(swapped.caret_x, 4.0f, 2.0f, 16.0f, kDefaultChrome.control_on);
    gui.end_frame();
    CHECK(swapped.edited);
    CHECK(std::strcmp(buf, "!") == 0);
    CHECK(swapped.caret_x == 18.0f);  // content_x + the one char typed
    CHECK(ws::ui::gui_test::stub.quads == 3);
    CHECK(ws::ui::gui_test::stub.rounded == 0);
}

// --- the ws::app bridge (stage 13 phase 4) ----------------------------------
namespace {

// The bridge cases' toy registry: the three command shapes the views
// read - a chorded action, an argument-asker and a plain one - in a
// registration order the ranking cases depend on. The callers that
// need dead or checked rows set those predicates on their own
// commands; this shared shape stays the plain three.
ws::app::CommandRegistry toy_registry() {
    ws::app::CommandRegistry registry;
    (void)registry.add(ws::app::Command{
        .id = "app.save",
        .label = "Save",
        .category = "File",
        .shortcut = ws::app::Shortcut{83, ws::app::Mod::control},  // Ctrl+S
        .execute = [](std::string_view) {},
    });
    (void)registry.add(ws::app::Command{
        .id = "edit.rename",
        .label = "Rename",
        .category = "File",
        .needs_argument = true,
        .execute = [](std::string_view) {},
    });
    (void)registry.add(ws::app::Command{
        .id = "misc.other",
        .label = "Other",
        .category = "Misc",
        .execute = [](std::string_view) {},
    });
    return registry;
}

}  // namespace

TEST_CASE("the fuzzy scorer ranks word starts, runs and brevity") {
    using ws::ui::gui::fuzzy_score;

    CHECK(fuzzy_score("", "anything") == 0);  // the empty query is recency's
    CHECK(fuzzy_score("zzz", "Save") == 0);   // a missing char is no match
    CHECK(fuzzy_score("save", "Save") > 0);
    // Case-insensitive on ASCII, both directions.
    CHECK(fuzzy_score("save", "Save") == fuzzy_score("SAVE", "Save"));
    // A word-start hit beats a mid-word one ("sa" in "Save All" vs
    // "Quasar" - both carry it, only one starts a word).
    CHECK(fuzzy_score("sa", "Save All") > fuzzy_score("sa", "Quasar"));
    // A contiguous run beats the same letters scattered.
    CHECK(fuzzy_score("ab", "ab") > fuzzy_score("ab", "aabb"));
    // The length penalty: a tighter label wins the same hits.
    CHECK(fuzzy_score("sa", "Save All") >
          fuzzy_score("sa", "Save A Lot More"));
    // Every match outranks none, whatever the length penalty did to it.
    CHECK(fuzzy_score("z", "zzzzzzzzzzzzzzzzzzzz") >= 1);
}

TEST_CASE("palette_rank orders by score, then recency, then registration") {
    using ws::ui::gui::palette_rank;
    const ws::app::CommandRegistry registry = toy_registry();
    // Registration order: app.save (0), edit.rename (1), misc.other (2).

    const auto all = palette_rank("", registry, {});
    REQUIRE(all.size() == 3);
    CHECK(all[0].index == 0);
    CHECK(all[1].index == 1);
    CHECK(all[2].index == 2);

    // "save" narrows to the labels carrying s,a,v,e as a
    // subsequence: of the toy's three, only "Save" does ("Rename"
    // has no 's', "Other" no 'v').
    const auto sav = palette_rank("save", registry, {});
    REQUIRE(sav.size() == 1);
    CHECK(sav[0].index == 0);

    // Recency leads only when the scores tie - the empty query's case.
    const auto rec = palette_rank("", registry, {"edit.rename"});
    REQUIRE(rec.size() == 3);
    CHECK(rec[0].index == 1);  // the recent first
    CHECK(rec[1].index == 0);  // then registration order
    CHECK(rec[2].index == 2);

    // A recent that does not match the query changes nothing.
    const auto mixed = palette_rank("save", registry, {"edit.rename"});
    REQUIRE(mixed.size() == 1);
    CHECK(mixed[0].index == 0);
}

TEST_CASE("command_hint reads the live binding, never a stale default") {
    using ws::ui::gui::command_hint;
    const ws::app::CommandRegistry registry = toy_registry();
    const ws::app::Command* save = registry.find("app.save");
    const ws::app::Command* other = registry.find("misc.other");
    REQUIRE(save != nullptr);
    REQUIRE(other != nullptr);

    // No resolver: the declared chord, or nothing for a chordless one.
    CHECK(command_hint(*save, nullptr, ws::app::KeyContext::global) ==
          "Ctrl+S");
    CHECK(command_hint(*other, nullptr, ws::app::KeyContext::global) ==
          "");

    // A resolver that never seeded: no binding, so no hint - a removed
    // binding must not fall back to hinting the chord it no longer
    // answers, and an unseeded resolver answers the same way.
    ws::app::ShortcutResolver resolver;
    CHECK(command_hint(*save, &resolver, ws::app::KeyContext::global) ==
          "");

    // Seeded: the live binding, which is the declared one.
    REQUIRE(resolver.install_defaults(registry).has_value());
    CHECK(command_hint(*save, &resolver, ws::app::KeyContext::global) ==
          "Ctrl+S");

    // Rebound: the new chord everywhere at once (the one describe_chord
    // spelling every view reads).
    REQUIRE(resolver
                .rebind("app.save", ws::app::KeyContext::global,
                        ws::app::Shortcut{75, ws::app::Mod::control})
                .has_value());
    CHECK(command_hint(*save, &resolver, ws::app::KeyContext::global) ==
          "Ctrl+K");

    // Removed: empty again.
    REQUIRE(resolver
                .rebind("app.save", ws::app::KeyContext::global,
                        ws::app::Shortcut{0, ws::app::Mod::none})
                .has_value());
    CHECK(command_hint(*save, &resolver, ws::app::KeyContext::global) ==
          "");
}

TEST_CASE("the menu builder maps the registry into live menu arrays") {
    bool save_checked = false;
    bool can_quit = true;
    ws::app::CommandRegistry registry;
    (void)registry.add(ws::app::Command{
        .id = "app.save",
        .label = "Save",
        .category = "File",
        .shortcut = ws::app::Shortcut{83, ws::app::Mod::control},
        .checked = [&save_checked] { return save_checked; },
        .execute = [](std::string_view) {},
    });
    (void)registry.add(ws::app::Command{
        .id = "app.quit",
        .label = "Quit",
        .category = "File",
        .enabled = [&can_quit] { return can_quit; },
        .execute = [](std::string_view) {},
    });
    (void)registry.add(ws::app::Command{
        .id = "edit.copy",
        .label = "Copy",
        .category = "Edit",
        .execute = [](std::string_view) {},
    });

    ws::ui::gui::CommandMenuBuilder builder;
    builder.describe({{"&File", "File"},
                     {"&Edit", "Edit"},
                     {"&Nothing", "Empty"}});
    builder.build(registry, nullptr, ws::app::KeyContext::global);

    // The empty category drops its menu; the order follows the specs.
    REQUIRE(builder.count() == 2);
    const ws::ui::gui::MenuDesc* menus = builder.menus();
    CHECK(std::strcmp(menus[0].title, "&File") == 0);
    REQUIRE(menus[0].count == 2);
    CHECK(std::strcmp(menus[0].items[0].label, "Save") == 0);
    CHECK(menus[0].items[0].kind == ws::ui::gui::MenuItemKind::check);
    CHECK(menus[0].items[0].checked == false);  // the predicate, live
    CHECK(std::strcmp(menus[0].items[0].shortcut, "Ctrl+S") == 0);
    CHECK(std::strcmp(menus[0].items[1].label, "Quit") == 0);
    CHECK(menus[0].items[1].kind == ws::ui::gui::MenuItemKind::action);
    CHECK(menus[0].items[1].disabled == false);
    CHECK(menus[0].items[1].shortcut == nullptr);  // chordless: no hint
    CHECK(std::strcmp(menus[1].title, "&Edit") == 0);
    REQUIRE(menus[1].count == 1);
    CHECK(std::strcmp(menus[1].items[0].label, "Copy") == 0);

    // The predicates are re-read per frame: flip the bools, rebuild,
    // the rows flip with them.
    save_checked = true;
    can_quit = false;
    builder.build(registry, nullptr, ws::app::KeyContext::global);
    CHECK(builder.menus()[0].items[0].checked == true);
    CHECK(builder.menus()[0].items[1].disabled == true);

    // command_of decodes this frame's items and nothing else.
    CHECK(builder.command_of(&builder.menus()[0].items[0]) == "app.save");
    CHECK(builder.command_of(&builder.menus()[1].items[0]) == "edit.copy");
    CHECK(builder.command_of(nullptr).empty());
    const ws::ui::gui::MenuItem foreign{"not one of these"};
    CHECK(builder.command_of(&foreign).empty());

    // The honesty rule through the builder: a resolver with the
    // binding removed lays no hint, the declared chord included.
    ws::app::ShortcutResolver resolver;
    REQUIRE(resolver.install_defaults(registry).has_value());
    REQUIRE(resolver
                .rebind("app.save", ws::app::KeyContext::global,
                        ws::app::Shortcut{0, ws::app::Mod::none})
                .has_value());
    builder.build(registry, &resolver, ws::app::KeyContext::global);
    CHECK(builder.menus()[0].items[0].shortcut == nullptr);
}

TEST_CASE("the toolbar fires its buttons and lays the dead rows dead") {
    namespace keys = ws::input::keys;
    ws::ui::gui_test::stub.reset();
    bool can_quit = false;
    ws::app::CommandRegistry registry;
    (void)registry.add(ws::app::Command{
        .id = "app.save",
        .label = "Save",
        .category = "File",
        .execute = [](std::string_view) {},
    });
    (void)registry.add(ws::app::Command{
        .id = "app.quit",
        .label = "Quit",
        .category = "File",
        .enabled = [&can_quit] { return can_quit; },
        .execute = [](std::string_view) {},
    });
    const std::vector<std::string> ids{"app.save", "app.quit", "app.nope"};

    GuiContext gui;
    LayoutCursor layout;
    std::string_view fired;

    // The press inside the first button latches the focus but is not
    // a click yet. The Save button sits at the panel content's head
    // (fitted to its label: 4 chars * 6 + 2 * padding wide), the Quit
    // beside it on the same line, and "app.nope" - unknown - lays
    // nothing at all.
    gui.begin_frame(snap(30.0f, 24.0f, true, true));
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    fired = ws::ui::gui::command_toolbar(gui, layout, kDefaultChrome,
                                         "bar", registry, ids);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    CHECK(fired.empty());

    // The release completes the click: the fired id is the answer.
    gui.begin_frame(snap(30.0f, 24.0f, false, false, true));
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    fired = ws::ui::gui::command_toolbar(gui, layout, kDefaultChrome,
                                         "bar", registry, ids);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    CHECK(fired == "app.save");

    // Enter on the focused button fires it again - the click gave it
    // the focus, and a citizen's keyboard path is the same fire.
    InputFrame enter = snap(-100.0f, -100.0f);
    enter.key_pressed[static_cast<std::size_t>(keys::kEnter)] = true;
    gui.begin_frame(enter);
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    fired = ws::ui::gui::command_toolbar(gui, layout, kDefaultChrome,
                                         "bar", registry, ids);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    CHECK(fired == "app.save");

    // A press-and-release on the dead Quit row claims nothing and
    // fires nothing - the lock is the enabled predicate, read live.
    gui.begin_frame(snap(90.0f, 24.0f, true, true));
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    fired = ws::ui::gui::command_toolbar(gui, layout, kDefaultChrome,
                                         "bar", registry, ids);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    gui.begin_frame(snap(90.0f, 24.0f, false, false, true));
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    fired = ws::ui::gui::command_toolbar(gui, layout, kDefaultChrome,
                                         "bar", registry, ids);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    CHECK(fired.empty());

    // The predicate flipped: the same row now takes the click.
    can_quit = true;
    gui.begin_frame(snap(90.0f, 24.0f, true, true));
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    fired = ws::ui::gui::command_toolbar(gui, layout, kDefaultChrome,
                                         "bar", registry, ids);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    gui.begin_frame(snap(90.0f, 24.0f, false, false, true));
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    fired = ws::ui::gui::command_toolbar(gui, layout, kDefaultChrome,
                                         "bar", registry, ids);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    CHECK(fired == "app.quit");
}

TEST_CASE("the command palette searches, fires, prompts and closes") {
    namespace keys = ws::input::keys;
    ws::ui::gui_test::stub.reset();
    const ws::app::CommandRegistry registry = toy_registry();
    ws::app::ShortcutResolver resolver;
    REQUIRE(resolver.install_defaults(registry).has_value());

    GuiContext gui;
    ws::ui::gui::CommandPalette state;
    bool open = true;

    // The opening frame answers no keys, Enter included - the chord
    // that opened the palette is still in that frame's edges.
    InputFrame open_press = snap(-100.0f, -100.0f);
    open_press.key_pressed[static_cast<std::size_t>(keys::kEnter)] = true;
    gui.begin_frame(open_press);
    ws::ui::gui::PaletteEvent ev = ws::ui::gui::command_palette(
        gui, kDefaultChrome, "palette", open, registry, &resolver,
        ws::app::KeyContext::global, state);
    gui.end_frame();
    CHECK(!ev.dispatch);
    CHECK(!ev.close);
    CHECK(state.was_open);

    // Typing lands in the search (the fresh open focused it) and the
    // ranked list narrows with it; the footer names the row Enter
    // would fire, chord included.
    InputFrame typing = snap(-100.0f, -100.0f);
    typing.text_input = {'s', 'a', 'v'};
    gui.begin_frame(typing);
    ev = ws::ui::gui::command_palette(gui, kDefaultChrome, "palette", open,
                                      registry, &resolver,
                                      ws::app::KeyContext::global, state);
    gui.end_frame();
    CHECK(std::string_view{state.search} == "sav");
    REQUIRE(state.ranked.size() == 1);
    CHECK(state.ranked[0] == 0);  // app.save, registration index 0
    CHECK(state.footer == "Save  Ctrl+S");

    // Enter fires the highlighted row: the dispatch + the close ask,
    // and the command is noted in the recents.
    InputFrame fire = snap(-100.0f, -100.0f);
    fire.key_pressed[static_cast<std::size_t>(keys::kEnter)] = true;
    gui.begin_frame(fire);
    ev = ws::ui::gui::command_palette(gui, kDefaultChrome, "palette", open,
                                      registry, &resolver,
                                      ws::app::KeyContext::global, state);
    gui.end_frame();
    CHECK(ev.dispatch);
    CHECK(ev.command == "app.save");
    CHECK(ev.argument.empty());
    CHECK(ev.close);
    REQUIRE(state.recents.size() == 1);
    CHECK(state.recents[0] == "app.save");
    open = false;  // the caller honors the close

    // A closed frame re-arms the open; the recents survive it.
    gui.begin_frame(snap(-100.0f, -100.0f));
    ev = ws::ui::gui::command_palette(gui, kDefaultChrome, "palette", open,
                                      registry, &resolver,
                                      ws::app::KeyContext::global, state);
    gui.end_frame();
    CHECK(!ev.dispatch);
    CHECK(!state.was_open);

    // The reopen: the search is reset (fresh open), the empty query
    // lists the recents first and then registration order.
    open = true;
    gui.begin_frame(snap(-100.0f, -100.0f));
    ev = ws::ui::gui::command_palette(gui, kDefaultChrome, "palette", open,
                                      registry, &resolver,
                                      ws::app::KeyContext::global, state);
    gui.end_frame();
    CHECK(state.search[0] == '\0');
    REQUIRE(state.ranked.size() == 3);
    CHECK(state.ranked[0] == 0);  // the recent
    CHECK(state.ranked[1] == 1);
    CHECK(state.ranked[2] == 2);

    // Down steps the highlight to row 1; Enter there finds
    // needs_argument and swaps to the prompt instead of dispatching.
    InputFrame down = snap(-100.0f, -100.0f);
    down.key_pressed[static_cast<std::size_t>(keys::kDown)] = true;
    gui.begin_frame(down);
    ev = ws::ui::gui::command_palette(gui, kDefaultChrome, "palette", open,
                                      registry, &resolver,
                                      ws::app::KeyContext::global, state);
    gui.end_frame();
    CHECK(state.selection == 1);
    CHECK(!ev.dispatch);

    InputFrame prompt = snap(-100.0f, -100.0f);
    prompt.key_pressed[static_cast<std::size_t>(keys::kEnter)] = true;
    gui.begin_frame(prompt);
    ev = ws::ui::gui::command_palette(gui, kDefaultChrome, "palette", open,
                                      registry, &resolver,
                                      ws::app::KeyContext::global, state);
    gui.end_frame();
    CHECK(!ev.dispatch);  // the prompt, not the fire
    CHECK(!ev.close);
    CHECK(state.prompting);
    CHECK(state.chosen == "edit.rename");

    // The payload types into the argument field (the search is locked
    // while prompting), and Enter there dispatches with it.
    InputFrame payload = snap(-100.0f, -100.0f);
    payload.text_input = {'x', 'y'};
    gui.begin_frame(payload);
    ev = ws::ui::gui::command_palette(gui, kDefaultChrome, "palette", open,
                                      registry, &resolver,
                                      ws::app::KeyContext::global, state);
    gui.end_frame();
    CHECK(std::string_view{state.argument} == "xy");

    InputFrame arg_fire = snap(-100.0f, -100.0f);
    arg_fire.key_pressed[static_cast<std::size_t>(keys::kEnter)] = true;
    gui.begin_frame(arg_fire);
    ev = ws::ui::gui::command_palette(gui, kDefaultChrome, "palette", open,
                                      registry, &resolver,
                                      ws::app::KeyContext::global, state);
    gui.end_frame();
    CHECK(ev.dispatch);
    CHECK(ev.command == "edit.rename");
    CHECK(ev.argument == "xy");
    CHECK(ev.close);
    open = false;
    gui.begin_frame(snap(-100.0f, -100.0f));
    (void)ws::ui::gui::command_palette(gui, kDefaultChrome, "palette", open,
                                       registry, &resolver,
                                       ws::app::KeyContext::global, state);
    gui.end_frame();

    // Escape closes through the arbitration: the press frame lays
    // quiet, the NEXT frame reads the win and asks to close.
    open = true;
    gui.begin_frame(snap(-100.0f, -100.0f));
    (void)ws::ui::gui::command_palette(gui, kDefaultChrome, "palette", open,
                                       registry, &resolver,
                                       ws::app::KeyContext::global, state);
    gui.end_frame();
    InputFrame esc = snap(-100.0f, -100.0f);
    esc.key_pressed[static_cast<std::size_t>(keys::kEscape)] = true;
    gui.begin_frame(esc);
    ev = ws::ui::gui::command_palette(gui, kDefaultChrome, "palette", open,
                                      registry, &resolver,
                                      ws::app::KeyContext::global, state);
    gui.end_frame();
    CHECK(!ev.close);  // the claim settles at end_frame
    gui.begin_frame(snap(-100.0f, -100.0f));
    ev = ws::ui::gui::command_palette(gui, kDefaultChrome, "palette", open,
                                      registry, &resolver,
                                      ws::app::KeyContext::global, state);
    gui.end_frame();
    CHECK(ev.close);   // and the next frame reads it
    open = false;
}

TEST_CASE("the keymap editor records, warns and cancels") {
    namespace keys = ws::input::keys;
    ws::ui::gui_test::stub.reset();
    ws::app::CommandRegistry registry = toy_registry();
    // A chordless fourth command so the bind path (a command with no
    // binding - rebind would refuse it) is a real row too.
    (void)registry.add(ws::app::Command{
        .id = "edit.copy",
        .label = "Copy",
        .category = "Edit",
        .execute = [](std::string_view) {},
    });
    ws::app::ShortcutResolver resolver;
    REQUIRE(resolver.install_defaults(registry).has_value());

    GuiContext gui;
    LayoutCursor layout;
    ws::ui::gui::KeymapEditor state;
    state.selected = 0;  // the caller's own pick (a row click's write)

    // The Record button's id, replicated through the same scopes the
    // editor derives it under (the panel's, then the editor's label).
    ws::ui::gui::IdStack ids;
    ids.push("panel");
    ids.push("keymap");
    const ws::ui::gui::WidgetId record_id = ids.id("Record...");

    // Frame 1 lays the editor and takes the button's focus (the lay
    // registers it; set_focus after the lay raises the live ring).
    gui.begin_frame(snap(-100.0f, -100.0f));
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    ws::ui::gui::keymap_editor(gui, layout, kDefaultChrome, "keymap",
                               registry, resolver, state);
    gui.set_focus(record_id);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    CHECK(!state.recording);
    // The detail line names the picked row and its live chord.
    CHECK(state.detail == "Save  Ctrl+S");

    // Enter starts the recording with the picked row captured.
    InputFrame press_record = snap(-100.0f, -100.0f);
    press_record.key_pressed[static_cast<std::size_t>(keys::kEnter)] = true;
    gui.begin_frame(press_record);
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    ws::ui::gui::keymap_editor(gui, layout, kDefaultChrome, "keymap",
                               registry, resolver, state);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    CHECK(state.recording);
    CHECK(state.recording_id == "app.save");

    // The capture frame: Ctrl held, K pressed - the chord lands through
    // rebind (Save carried the seeded binding), the status names it,
    // and the recording ends in the same frame.
    InputFrame capture = snap(-100.0f, -100.0f);
    capture.key_down[static_cast<std::size_t>(keys::kLeftControl)] = true;
    capture.key_pressed[75] = true;  // 'K', GLFW numbering
    gui.begin_frame(capture);
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    ws::ui::gui::keymap_editor(gui, layout, kDefaultChrome, "keymap",
                               registry, resolver, state);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    CHECK(!state.recording);
    CHECK(state.status == "bound 'Save' to Ctrl+K");
    const ws::app::Shortcut* bound =
        resolver.binding_for("app.save", ws::app::KeyContext::global);
    REQUIRE(bound != nullptr);
    CHECK(*bound == ws::app::Shortcut{75, ws::app::Mod::control});

    // The conflict warning: record the chordless Copy onto the chord
    // Save now owns - the resolver's own refusal lands verbatim in
    // the status, and neither binding moved.
    state.selected = 3;  // Copy, the registration walk's fourth row
    gui.begin_frame(snap(-100.0f, -100.0f));
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    ws::ui::gui::keymap_editor(gui, layout, kDefaultChrome, "keymap",
                               registry, resolver, state);
    gui.set_focus(record_id);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    InputFrame press_again = snap(-100.0f, -100.0f);
    press_again.key_pressed[static_cast<std::size_t>(keys::kEnter)] = true;
    gui.begin_frame(press_again);
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    ws::ui::gui::keymap_editor(gui, layout, kDefaultChrome, "keymap",
                               registry, resolver, state);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    CHECK(state.recording);
    CHECK(state.recording_id == "edit.copy");

    InputFrame conflict = snap(-100.0f, -100.0f);
    conflict.key_down[static_cast<std::size_t>(keys::kLeftControl)] = true;
    conflict.key_pressed[75] = true;  // Ctrl+K: Save's now
    gui.begin_frame(conflict);
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    ws::ui::gui::keymap_editor(gui, layout, kDefaultChrome, "keymap",
                               registry, resolver, state);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    CHECK(!state.recording);
    CHECK(state.status.find("Ctrl+K") != std::string::npos);
    CHECK(state.status.find("app.save") != std::string::npos);  // the owner, by id
    CHECK(resolver.binding_for("edit.copy", ws::app::KeyContext::global) ==
          nullptr);  // refused: nothing moved

    // Escape cancels a live recording: the edge frame cancels (the
    // editor acts on it and consumes the arbitration), the widgets
    // unlock, and the status says so.
    state.selected = 0;
    gui.begin_frame(snap(-100.0f, -100.0f));
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    ws::ui::gui::keymap_editor(gui, layout, kDefaultChrome, "keymap",
                               registry, resolver, state);
    gui.set_focus(record_id);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    InputFrame press_third = snap(-100.0f, -100.0f);
    press_third.key_pressed[static_cast<std::size_t>(keys::kEnter)] = true;
    gui.begin_frame(press_third);
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    ws::ui::gui::keymap_editor(gui, layout, kDefaultChrome, "keymap",
                               registry, resolver, state);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    CHECK(state.recording);

    InputFrame cancel = snap(-100.0f, -100.0f);
    cancel.key_pressed[static_cast<std::size_t>(keys::kEscape)] = true;
    gui.begin_frame(cancel);
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    ws::ui::gui::keymap_editor(gui, layout, kDefaultChrome, "keymap",
                               registry, resolver, state);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    CHECK(!state.recording);
    CHECK(state.status == "recording cancelled");
    CHECK(resolver.binding_for("app.save", ws::app::KeyContext::global) !=
          nullptr);  // the binding Save carried is untouched

    // The search narrows the rows: "Copy" leaves the one row (a pick
    // pointing past it clamps onto it), and the chordless bind path
    // (no binding - bind(), never rebind()) puts a real chord under it.
    state.selected = 3;  // a stale pick: Copy was the fourth row
    std::snprintf(state.search, sizeof state.search, "Copy");
    gui.begin_frame(snap(-100.0f, -100.0f));
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    ws::ui::gui::keymap_editor(gui, layout, kDefaultChrome, "keymap",
                               registry, resolver, state);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    REQUIRE(state.row_commands.size() == 1);
    CHECK(state.row_commands[0] == 3);
    REQUIRE(state.cells.size() == 2);
    CHECK(std::strcmp(state.cells[0], "Copy") == 0);
    CHECK(std::strcmp(state.cells[1], "<none>") == 0);
    CHECK(state.selected == 0);  // the narrowed list clamps the pick

    gui.begin_frame(snap(-100.0f, -100.0f));
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    ws::ui::gui::keymap_editor(gui, layout, kDefaultChrome, "keymap",
                               registry, resolver, state);
    gui.set_focus(record_id);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    InputFrame press_fourth = snap(-100.0f, -100.0f);
    press_fourth.key_pressed[static_cast<std::size_t>(keys::kEnter)] = true;
    gui.begin_frame(press_fourth);
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    ws::ui::gui::keymap_editor(gui, layout, kDefaultChrome, "keymap",
                               registry, resolver, state);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    CHECK(state.recording);
    CHECK(state.recording_id == "edit.copy");

    InputFrame chordless = snap(-100.0f, -100.0f);
    chordless.key_down[static_cast<std::size_t>(keys::kLeftControl)] = true;
    chordless.key_down[static_cast<std::size_t>(keys::kLeftShift)] = true;
    chordless.key_pressed[67] = true;  // 'C', GLFW numbering
    gui.begin_frame(chordless);
    (void)ws::ui::gui::begin_panel(gui, layout, kDefaultChrome, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    ws::ui::gui::keymap_editor(gui, layout, kDefaultChrome, "keymap",
                               registry, resolver, state);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    CHECK(!state.recording);
    CHECK(state.status == "bound 'Copy' to Ctrl+Shift+C");
    bound = resolver.binding_for("edit.copy", ws::app::KeyContext::global);
    REQUIRE(bound != nullptr);
    CHECK(bound->key == 67);
    CHECK(ws::app::has_mod(bound->mods, ws::app::Mod::control));
    CHECK(ws::app::has_mod(bound->mods, ws::app::Mod::shift));
}
