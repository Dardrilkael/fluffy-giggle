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
#include <ws/ui/gui/theme_color.hpp>
#include <ws/ui/gui/theme_data.hpp>
#include <ws/ui/gui/theme_json.hpp>
#include <ws/ui/gui/widgets.hpp>
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
