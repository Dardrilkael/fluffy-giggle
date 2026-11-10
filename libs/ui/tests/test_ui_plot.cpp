// The plot suite: the waveform widget, end to end. The pure axis math
// (the ladder, the engineering-notation labels, the view invariants) is
// gated directly - the popup_placement precedent - and the widget is
// driven through hand-built frames the way the gui suite builds them,
// so every gesture lands exactly as the platform layers would deliver
// it: the wheel zoom against the claim's one-frame settle, the rubber
// band, the axis-strip drags, the pan, the double-click reset, the
// right-click cancel. The draws are asserted through the r2d stub's
// counters (lines, texts, clip pushes) - the same purity class as
// test_ui_gui: the gui archive, the real font bake, the stub seam; no
// renderer, no window, no platform TU.
//
// Headless notes the cases ride: the frame clock reads 0 (dt_seconds),
// so the double-click pair never ages - every same-spot press pair
// counts (the spinner's held-phase honesty, and the seam these cases
// drive the reset through); and the wheel claim settles one frame
// after the plot first registers, so a zoom case always runs one
// idle hover frame before the notch.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/painters.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/input/input.hpp>

#include "ui_gui_stub.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using ws::ui::gui::GuiContext;
using ws::input::InputFrame;
using ws::ui::gui::LayoutCursor;
using ws::ui::gui::kDefaultChrome;
namespace keys = ws::input::keys;

// The gui suite's snap(): a present mouse parked far off-panel by
// default, so nothing hovers by accident.
InputFrame snap(float x = -100.0f, float y = -100.0f) {
    InputFrame frame;
    frame.mouse_x = x;
    frame.mouse_y = y;
    frame.mouse_present = true;
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

// A ramp 0..n-1, strictly increasing - the honest sweep axis.
std::vector<double> ramp(std::size_t n, double lo = 0.0, double hi = 1.0) {
    std::vector<double> xs(n);
    for (std::size_t i = 0; i < n; ++i) {
        xs[i] = lo + (hi - lo) * static_cast<double>(i) /
                         static_cast<double>(n <= 1u ? 1u : n - 1u);
    }
    return xs;
}

}  // namespace

// --- the pure axis math ---------------------------------------------------------

TEST_CASE("plot_ticks: the 1/2/5 ladder, degenerate and inverted honesty") {
    using ws::ui::gui::plot_ticks;

    // 0..10 at target 5: step 2 - the ladder's first rung that fits.
    ws::ui::gui::PlotTicks t = plot_ticks(0.0, 10.0, 5u);
    REQUIRE(t.count == 6);
    CHECK(t.ticks[0] == 0.0);
    CHECK(t.ticks[1] == 2.0);
    CHECK(t.ticks[5] == 10.0);

    // 0..1 at target 5: step 0.2.
    t = plot_ticks(0.0, 1.0, 5u);
    REQUIRE(t.count == 6);
    CHECK(t.ticks[1] == doctest::Approx(0.2));

    // 0..10 at target 6: step 2 (10/6 normalizes to 1.67 -> 2).
    t = plot_ticks(0.0, 10.0, 6u);
    REQUIRE(t.count == 6);

    // A span that does not land on its endpoints: the ticks stay ON
    // the grid, inside the span (a padded axis shows its true bounds).
    t = plot_ticks(0.3, 9.7, 5u);
    REQUIRE(t.count >= 4);
    for (unsigned i = 0; i < t.count; ++i) {
        CHECK(t.ticks[i] >= 0.3);
        CHECK(t.ticks[i] <= 9.7);
    }

    // Degenerate: one honest tick.
    t = plot_ticks(5.0, 5.0, 5u);
    REQUIRE(t.count == 1);
    CHECK(t.ticks[0] == 5.0);

    // Inverted and non-finite: nothing (a caller bug labels itself by
    // drawing no grid at all).
    CHECK(plot_ticks(1.0, 0.0, 5u).count == 0);
    CHECK(plot_ticks(std::nan(""), 1.0, 5u).count == 0);

    // -0 normalizes: the tick at 0 prints "0", never "-0".
    t = plot_ticks(-1.0, 1.0, 2u);
    for (unsigned i = 0; i < t.count; ++i) {
        if (t.ticks[i] == 0.0) {
            CHECK(std::signbit(t.ticks[i]) == false);
        }
    }
}

TEST_CASE("plot_format_eng: the scope's engineering notation") {
    using ws::ui::gui::plot_format_eng;
    char buf[32];

    CHECK(std::strcmp(plot_format_eng(buf, sizeof buf, 4700.0), "4.7k") == 0);
    CHECK(std::strcmp(plot_format_eng(buf, sizeof buf, -12.3e-3), "-12.3m") == 0);
    CHECK(std::strcmp(plot_format_eng(buf, sizeof buf, 2e-9), "2n") == 0);
    CHECK(std::strcmp(plot_format_eng(buf, sizeof buf, 0.0), "0") == 0);
    CHECK(std::strcmp(plot_format_eng(buf, sizeof buf, -0.0), "0") == 0);
    CHECK(std::strcmp(plot_format_eng(buf, sizeof buf, 1.0), "1") == 0);
    CHECK(std::strcmp(plot_format_eng(buf, sizeof buf, 1e6), "1M") == 0);
    CHECK(std::strcmp(plot_format_eng(buf, sizeof buf, 2.5e-12), "2.5p") == 0);

    // The mantissa that rounds up through its decade renormalizes:
    // 999.9m at the default 3 digits prints as 1, not 1000m (the
    // mantissa itself rounds to "1e+03", and the bucket walks).
    CHECK(std::strcmp(plot_format_eng(buf, sizeof buf, 0.9999), "1") == 0);
    CHECK(std::strcmp(plot_format_eng(buf, sizeof buf, 999.9e3), "1M") == 0);

    // The digit budget clamps both ways without changing the prefix
    // decision; non-finite answers spell themselves.
    CHECK(std::strcmp(plot_format_eng(buf, sizeof buf, 4700.0, 1), "5k") == 0);
    CHECK(std::strcmp(plot_format_eng(buf, sizeof buf, 4700.0, 6), "4.7k") == 0);
    CHECK(std::strcmp(plot_format_eng(buf, sizeof buf, std::nan("")), "nan") == 0);
    CHECK(std::strcmp(plot_format_eng(buf, sizeof buf,
                                      std::numeric_limits<double>::infinity()),
                      "inf") == 0);

    // Beyond the prefix table: plain %g keeps the magnitude honest.
    CHECK(std::strcmp(plot_format_eng(buf, sizeof buf, 1e18), "1e+18") == 0);
}

TEST_CASE("the view invariants: expand, clamp, zoom, pan") {
    using ws::ui::gui::plot_expand_range;
    using ws::ui::gui::plot_clamp_view;
    using ws::ui::gui::plot_zoom_range;
    using ws::ui::gui::plot_pan_range;
    const ws::ui::gui::PlotAxisRange home{0.0, 10.0};

    // Padding: 5% each side of the span; degenerate expands by the
    // magnitude floor; non-finite answers the sane-empty window.
    const ws::ui::gui::PlotAxisRange padded = plot_expand_range(0.0, 10.0);
    CHECK(padded.lo == doctest::Approx(-0.5));
    CHECK(padded.hi == doctest::Approx(10.5));
    const ws::ui::gui::PlotAxisRange flat = plot_expand_range(3.0, 3.0);
    CHECK(flat.lo == doctest::Approx(2.85));
    CHECK(flat.hi == doctest::Approx(3.15));
    const ws::ui::gui::PlotAxisRange bad = plot_expand_range(
        std::nan(""), 1.0);
    CHECK(bad.lo == 0.0);
    CHECK(bad.hi == 1.0);

    // Clamp: a narrower window slides home; a wider one IS home;
    // non-finite answers home.
    CHECK(plot_clamp_view({2.0, 4.0}, home).lo == 2.0);
    CHECK(plot_clamp_view({-2.0, 4.0}, home).lo == 0.0);  // slides right in
    CHECK(plot_clamp_view({-2.0, 40.0}, home).hi == 10.0);  // past home: home
    CHECK(plot_clamp_view({std::nan(""), 4.0}, home).hi == 10.0);

    // Zoom: about the anchor, in for factor > 1, the mush floor, and
    // the settle at home for a factor spun the other way forever.
    const ws::ui::gui::PlotAxisRange z =
        plot_zoom_range({2.0, 4.0}, home, 3.0, 2.0);
    CHECK(z.lo == doctest::Approx(2.5));
    CHECK(z.hi == doctest::Approx(3.5));
    // An anchor outside the window acts at the nearest edge.
    const ws::ui::gui::PlotAxisRange ze =
        plot_zoom_range({2.0, 4.0}, home, 100.0, 2.0);
    CHECK(ze.hi == doctest::Approx(4.0));
    // Factor 1 and garbage answer the view unchanged.
    CHECK(plot_zoom_range({2.0, 4.0}, home, 3.0, 1.0).lo == 2.0);
    CHECK(plot_zoom_range({2.0, 4.0}, home, 3.0,
                          std::nan("")).hi == 4.0);
    // Zooming out past home settles ON home, never beyond it.
    const ws::ui::gui::PlotAxisRange zo =
        plot_zoom_range({2.0, 4.0}, home, 3.0, 0.001);
    CHECK(zo.lo == 0.0);
    CHECK(zo.hi == 10.0);

    // Pan: slides and parks flush; non-finite delta is a no-op.
    CHECK(plot_pan_range({2.0, 4.0}, home, 1.0).lo == 3.0);
    CHECK(plot_pan_range({2.0, 4.0}, home, -5.0).lo == 0.0);
    CHECK(plot_pan_range({2.0, 4.0}, home, -50.0).lo == 0.0);
    CHECK(plot_pan_range({2.0, 4.0}, home,
                         std::nan("")).hi == 4.0);
}

// --- the widget, driven through hand-built frames -------------------------------

namespace {

// One plot pass, driven through the SPLIT halves - the custom-surface
// shape (place, id, side-table state, behavior, draw), spelled out so
// the test reads the resolved frame the composed widget keeps to
// itself. The font rides the theme (the label paths gate on it); the
// count is the SAMPLE count the series share, the xs span's own
// length - the composed call's `count` parameter, which is samples,
// never the series-list length.
struct PlotPass {
    ws::ui::gui::PlotResult result{};
    ws::ui::gui::PlotFrame frame{};
};

PlotPass run_plot(GuiContext& gui, const InputFrame& in,
                  const std::vector<double>& xs,
                  const std::vector<ws::ui::gui::PlotSeries>& series,
                  const char* x_name = nullptr) {
    LayoutCursor layout;
    ws::ui::gui::ChromeTheme theme = kDefaultChrome;
    static std::array<ws::ui::r2d::Font::Glyph, 95> glyphs = font_glyphs();
    static const ws::ui::r2d::Font font(16.0f, 20.0f, glyphs.data(), 95);
    theme.font = &font;
    const int samples = static_cast<int>(xs.size());
    const double* x_axis = xs.empty() ? nullptr : xs.data();
    const ws::ui::gui::PlotSeries* rows =
        series.empty() ? nullptr : series.data();
    const int row_count = static_cast<int>(series.size());
    gui.begin_frame(in);
    (void)ws::ui::gui::begin_panel(gui, layout, theme, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    const float width = layout.remaining_width();
    const ws::ui::gui::WidgetId id = gui.ids().id("scope");
    const ws::ui::gui::Rect rect = layout.next(width, 220.0f);
    ws::ui::gui::PlotState& state =
        gui.persistent_state<ws::ui::gui::PlotState>(id);
    PlotPass pass;
    pass.result = ws::ui::gui::plot_behavior(gui, rect, x_axis, rows,
                                             row_count, samples, pass.frame,
                                             state);
    ws::ui::gui::plot_draw(rect, pass.frame, rows, row_count, samples,
                           x_axis, x_name, pass.result, state, theme);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    return pass;
}

}  // namespace

TEST_CASE("plot: the auto fit and the hover's nearest sample") {
    ws::ui::gui_test::stub.reset();
    GuiContext gui;

    // A 21-point sweep 0..10 ms, one series riding it 0..5.
    const std::vector<double> xs = ramp(21, 0.0, 10.0);
    std::vector<double> ys(21);
    for (int i = 0; i < 21; ++i) {
        ys[static_cast<std::size_t>(i)] = static_cast<double>(i) * 0.25;
    }
    const std::vector<ws::ui::gui::PlotSeries> series{
        ws::ui::gui::PlotSeries{"v(out)", ys.data(), 0, true,
                                ws::ui::gui::PlotSpine::left, "V"},
    };

    // The cursor at the frame's middle, read through the geometry the
    // behavior resolved: the pixel that maps exactly onto the middle
    // sample's x (5.0) answers that sample - the nearest-sample rule
    // with no tie to break.
    const PlotPass mid = run_plot(gui, snap(218.0f, 100.0f), xs, series,
                                  "time");
    CHECK(mid.result.hovered);
    CHECK(mid.result.hover == 10);   // the middle of 21
    CHECK(mid.result.hover_x == doctest::Approx(5.0));
    CHECK(!mid.result.view_changed);
    CHECK(!mid.result.view_reset);
    // The resolved frame: the right margin stays the breath (no right
    // spine), the auto view is the padded fit of the data.
    CHECK(mid.frame.has_right == false);
    CHECK(mid.frame.margin_right == doctest::Approx(10.0f));
    CHECK(mid.frame.view_x.lo == doctest::Approx(-0.5));
    CHECK(mid.frame.view_x.hi == doctest::Approx(10.5));
    CHECK(mid.frame.view_y.lo == doctest::Approx(-0.25));
    CHECK(mid.frame.view_y.hi == doctest::Approx(5.25));

    // Off the frame: no hover at all.
    const PlotPass away = run_plot(gui, snap(), xs, series);
    CHECK(!away.result.hovered);
    CHECK(away.result.hover == -1);
}

TEST_CASE("plot: the wheel zooms toward the cursor, after the claim settles") {
    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    const std::vector<double> xs = ramp(21, 0.0, 10.0);
    std::vector<double> ys(21, 0.0);
    const std::vector<ws::ui::gui::PlotSeries> series{
        ws::ui::gui::PlotSeries{"a", ys.data(), 0, true,
                                ws::ui::gui::PlotSpine::left, nullptr},
    };

    // One idle hover frame: the claim registers, nothing zooms.
    const PlotPass settle = run_plot(gui, snap(200.0f, 100.0f), xs, series);
    CHECK(!settle.result.view_changed);

    // The notch: wheel-up (positive dy) zooms IN about the cursor's x.
    InputFrame wheel = snap(200.0f, 100.0f);
    wheel.scroll_dy = 1.0f;
    const PlotPass zoomed = run_plot(gui, wheel, xs, series);
    CHECK(zoomed.result.view_changed);
    CHECK(zoomed.frame.view_x.hi - zoomed.frame.view_x.lo <
          settle.frame.view_x.hi - settle.frame.view_x.lo);
    // Toward the cursor: the cursor's data x stays put under the zoom.
    const double before = settle.frame.wx(200.0f);
    const double after = zoomed.frame.wx(200.0f);
    CHECK(before == doctest::Approx(after).epsilon(0.01));
    // The hover follows the zoomed window (the re-aim rule).
    CHECK(zoomed.result.hovered);

    // Wheel-down zooms back out; spun far enough it settles ON home.
    InputFrame unzoom = snap(200.0f, 100.0f);
    unzoom.scroll_dy = -1.0f;
    PlotPass out = run_plot(gui, unzoom, xs, series);
    for (int i = 0; i < 8; ++i) {
        out = run_plot(gui, unzoom, xs, series);
    }
    CHECK(out.frame.view_x.lo == doctest::Approx(-0.5));
    CHECK(out.frame.view_x.hi == doctest::Approx(10.5));
}

TEST_CASE("plot: the rubber band commits its box; a tiny one is a click") {
    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    const std::vector<double> xs = ramp(21, 0.0, 10.0);
    std::vector<double> ys(21, 0.0);
    const std::vector<ws::ui::gui::PlotSeries> series{
        ws::ui::gui::PlotSeries{"a", ys.data(), 0, true,
                                ws::ui::gui::PlotSpine::left, nullptr},
    };

    // Find the frame's pixel geometry first (the auto fit frame).
    const PlotPass probe = run_plot(gui, snap(), xs, series);
    const float fx = probe.frame.fx;
    const float fy = probe.frame.fy;

    // Press in the frame, drag a real box (well past 4 px both ways),
    // release: the commit lands as the box's data corners, clamped
    // into home.
    InputFrame press = snap(fx + 40.0f, fy + 20.0f);
    press.mouse_down[0] = true;
    press.mouse_pressed[0] = true;
    const PlotPass armed = run_plot(gui, press, xs, series);
    CHECK(!armed.result.view_changed);  // arming is not a move

    InputFrame drag = snap(fx + 160.0f, fy + 120.0f);
    drag.mouse_down[0] = true;
    const PlotPass flight = run_plot(gui, drag, xs, series);
    CHECK(!flight.result.view_changed);  // the box lands at release

    InputFrame lift = snap(fx + 160.0f, fy + 120.0f);
    lift.mouse_released[0] = true;
    const PlotPass done = run_plot(gui, lift, xs, series);
    CHECK(done.result.view_changed);
    CHECK(done.frame.view_x.lo > probe.frame.view_x.lo);
    CHECK(done.frame.view_x.hi < probe.frame.view_x.hi);

    // A tiny band (2 px) releases as a click: no commit, and the kind
    // clears so the next press can arm again.
    InputFrame press2 = snap(fx + 40.0f, fy + 20.0f);
    press2.mouse_down[0] = true;
    press2.mouse_pressed[0] = true;
    (void)run_plot(gui, press2, xs, series);
    InputFrame lift2 = snap(fx + 42.0f, fy + 22.0f);
    lift2.mouse_released[0] = true;
    const PlotPass tiny = run_plot(gui, lift2, xs, series);
    CHECK(!tiny.result.view_changed);
}

TEST_CASE("plot: the axis-strip drag stretches one axis") {
    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    const std::vector<double> xs = ramp(21, 0.0, 10.0);
    std::vector<double> ys(21, 0.0);
    const std::vector<ws::ui::gui::PlotSeries> series{
        ws::ui::gui::PlotSeries{"a", ys.data(), 0, true,
                                ws::ui::gui::PlotSpine::left, nullptr},
    };
    const PlotPass probe = run_plot(gui, snap(), xs, series);
    const float fx = probe.frame.fx;
    const float fy = probe.frame.fy;
    const float ih = probe.frame.ih;

    // Press on the BOTTOM strip (below the frame), drag right: the x
    // axis zooms in, the y window stays.
    InputFrame press = snap(fx + 100.0f, fy + ih + 5.0f);
    press.mouse_down[0] = true;
    press.mouse_pressed[0] = true;
    (void)run_plot(gui, press, xs, series);
    InputFrame drag = snap(fx + 200.0f, fy + ih + 5.0f);
    drag.mouse_down[0] = true;
    const PlotPass stretched = run_plot(gui, drag, xs, series);
    CHECK(stretched.result.view_changed);
    CHECK(stretched.frame.view_x.hi - stretched.frame.view_x.lo <
          probe.frame.view_x.hi - probe.frame.view_x.lo);
    CHECK(stretched.frame.view_y.lo == doctest::Approx(
          probe.frame.view_y.lo));
    CHECK(stretched.frame.view_y.hi == doctest::Approx(
          probe.frame.view_y.hi));
}

TEST_CASE("plot: the pan slides the window; double-click resets; right cancels") {
    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    const std::vector<double> xs = ramp(21, 0.0, 10.0);
    std::vector<double> ys(21, 0.0);
    const std::vector<ws::ui::gui::PlotSeries> series{
        ws::ui::gui::PlotSeries{"a", ys.data(), 0, true,
                                ws::ui::gui::PlotSpine::left, nullptr},
    };
    const PlotPass probe = run_plot(gui, snap(), xs, series);
    const float fx = probe.frame.fx;
    const float fy = probe.frame.fy;

    // A middle-drag pans - but only once there is room to pan: the
    // auto fit fills home exactly (a full-span window cannot slide,
    // the clamp's rule), so the zoom comes first. One notch in, then
    // the drag: content follows the cursor, so dragging LEFT slides
    // the window RIGHT.
    const PlotPass settle = run_plot(gui, snap(218.0f, 100.0f), xs, series);
    CHECK(!settle.result.view_changed);  // the claim's settling frame
    InputFrame notch = snap(218.0f, 100.0f);
    notch.scroll_dy = 1.0f;
    const PlotPass zoomed_in = run_plot(gui, notch, xs, series);
    CHECK(zoomed_in.result.view_changed);
    InputFrame press = snap(238.0f, 60.0f);
    press.mouse_down[ws::input::mouse::kMiddle] = true;
    const PlotPass armed = run_plot(gui, press, xs, series);
    CHECK(!armed.result.view_changed);  // arming is not a move
    InputFrame drag = snap(158.0f, 70.0f);
    drag.mouse_down[ws::input::mouse::kMiddle] = true;
    const PlotPass panned = run_plot(gui, drag, xs, series);
    CHECK(panned.result.view_changed);
    CHECK(panned.frame.view_x.lo > zoomed_in.frame.view_x.lo);
    CHECK(panned.frame.view_x.hi > zoomed_in.frame.view_x.hi);

    // The double-click reset: press, release, press again at the same
    // spot (headless dt 0 keeps the pair alive) - the view drops back
    // to the auto fit, and a reset is not a move.
    InputFrame p1 = snap(fx + 100.0f, fy + 50.0f);
    p1.mouse_pressed[0] = true;
    (void)run_plot(gui, p1, xs, series);
    InputFrame r1 = snap(fx + 100.0f, fy + 50.0f);
    r1.mouse_released[0] = true;
    (void)run_plot(gui, r1, xs, series);
    InputFrame p2 = snap(fx + 101.0f, fy + 51.0f);
    p2.mouse_pressed[0] = true;
    const PlotPass reset = run_plot(gui, p2, xs, series);
    CHECK(reset.result.view_reset);
    CHECK(!reset.result.view_changed);
    CHECK(reset.frame.view_x.lo == doctest::Approx(-0.5));
    CHECK(reset.frame.view_x.hi == doctest::Approx(10.5));

    // The right-click cancel: arm a band, right-press mid-flight - the
    // view restores to the press-time one exactly (the snap-back is a
    // move this frame, and the release that follows commits nothing).
    InputFrame arm = snap(fx + 40.0f, fy + 20.0f);
    arm.mouse_down[0] = true;
    arm.mouse_pressed[0] = true;
    (void)run_plot(gui, arm, xs, series);
    InputFrame cancel = snap(fx + 150.0f, fy + 100.0f);
    cancel.mouse_down[0] = true;
    cancel.mouse_pressed[ws::input::mouse::kRight] = true;
    const PlotPass undone = run_plot(gui, cancel, xs, series);
    CHECK(undone.frame.view_x.lo == doctest::Approx(probe.frame.view_x.lo));
    InputFrame lift = snap(fx + 150.0f, fy + 100.0f);
    lift.mouse_released[0] = true;
    const PlotPass settled = run_plot(gui, lift, xs, series);
    CHECK(!settled.result.view_changed);
    CHECK(settled.frame.view_x.lo == doctest::Approx(-0.5));
}

TEST_CASE("plot: the two spines - a right series earns its own window") {
    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    const std::vector<double> xs = ramp(21, 0.0, 10.0);
    std::vector<double> volts(21);
    std::vector<double> amps(21);
    for (int i = 0; i < 21; ++i) {
        volts[static_cast<std::size_t>(i)] = 5.0 - 0.25 * i;  // 5 -> 0
        amps[static_cast<std::size_t>(i)] = 0.001 * i;        // 1 mA scale
    }
    const std::vector<ws::ui::gui::PlotSeries> series{
        ws::ui::gui::PlotSeries{"v(out)", volts.data(), 0, true,
                                ws::ui::gui::PlotSpine::left, "V"},
        ws::ui::gui::PlotSeries{"i(R1)", amps.data(), 0, true,
                                ws::ui::gui::PlotSpine::right, "A"},
    };

    const PlotPass two = run_plot(gui, snap(), xs, series);
    CHECK(two.frame.has_right);
    CHECK(two.frame.margin_right == doctest::Approx(46.0f));
    // Each spine fits its OWN company: the amps (0..20 mA) never
    // stretch the volts window (0..5 V padded), and vice versa.
    CHECK(two.frame.view_y.lo == doctest::Approx(-0.25));
    CHECK(two.frame.view_y.hi == doctest::Approx(5.25));
    CHECK(two.frame.view_y1.lo == doctest::Approx(-0.001));
    CHECK(two.frame.view_y1.hi == doctest::Approx(0.021));

    // Hiding the right series drops the second spine entirely (the
    // margin returns to the breath, the right window goes sane-empty).
    std::vector<ws::ui::gui::PlotSeries> hidden = series;
    hidden[1].visible = false;
    const PlotPass one = run_plot(gui, snap(), xs, hidden);
    CHECK(!one.frame.has_right);
    CHECK(one.frame.margin_right == doctest::Approx(10.0f));
}

TEST_CASE("plot: the draws - grid, series, decimation, clip, labels, empties") {
    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    const std::vector<double> xs = ramp(21, 0.0, 10.0);
    std::vector<double> ys(21);
    for (int i = 0; i < 21; ++i) {
        ys[static_cast<std::size_t>(i)] =
            std::sin(static_cast<double>(i) * 0.3) * 4.0;
    }
    const std::vector<ws::ui::gui::PlotSeries> series{
        ws::ui::gui::PlotSeries{"v(out)", ys.data(), 0, true,
                                ws::ui::gui::PlotSpine::left, "V"},
    };

    // A resting frame: the clip pair, the grid lines, the frame
    // outline, the series' 20 segments, the tick labels.
    const PlotPass drawn = run_plot(gui, snap(), xs, series, "time");
    CHECK(drawn.frame.iw > 0.0f);  // the geometry resolved
    CHECK(ws::ui::gui_test::stub.clip_pushes >= 1);
    CHECK(ws::ui::gui_test::stub.clip_pops >= 1);
    CHECK(ws::ui::gui_test::stub.lines >= 20);      // the series alone
    CHECK(ws::ui::gui_test::stub.texts > 0);        // the tick labels

    // A hover frame: the crosshair line and the marker join in, the
    // tooltip's texts ride the popup chrome.
    ws::ui::gui_test::stub.reset();
    const PlotPass hovered = run_plot(gui, snap(200.0f, 100.0f), xs, series,
                                      "time");
    CHECK(hovered.result.hovered);
    CHECK(ws::ui::gui_test::stub.lines >= 21);      // + the crosshair
    CHECK(ws::ui::gui_test::stub.rounded >= 1);     // the marker dot

    // The long sweep: 20k samples in a ~300-px frame decimate to a
    // couple of vertices per pixel - far under the full 19,999
    // segments, well over zero.
    ws::ui::gui_test::stub.reset();
    const std::vector<double> big_xs = ramp(20000, 0.0, 10.0);
    std::vector<double> big_ys(20000);
    for (std::size_t i = 0; i < 20000; ++i) {
        big_ys[i] = std::sin(static_cast<double>(i) * 0.001) * 4.0;
    }
    const std::vector<ws::ui::gui::PlotSeries> big{
        ws::ui::gui::PlotSeries{"tran", big_ys.data(), 0, true,
                                ws::ui::gui::PlotSpine::left, nullptr},
    };
    (void)run_plot(gui, snap(), big_xs, big);
    CHECK(ws::ui::gui_test::stub.lines > 100);
    CHECK(ws::ui::gui_test::stub.lines < 5000);

    // Every series hidden: no series lines (the grid and frame stay -
    // an empty plot is still a plot), the empty-state message drawn
    // instead of a mystery frame.
    ws::ui::gui_test::stub.reset();
    std::vector<ws::ui::gui::PlotSeries> invisible = series;
    invisible[0].visible = false;
    (void)run_plot(gui, snap(), xs, invisible);
    CHECK(ws::ui::gui_test::stub.lines < 25);      // grid + frame only
    CHECK(ws::ui::gui_test::stub.texts > 0);        // the message

    // No data at all: the honest "no data" frame.
    ws::ui::gui_test::stub.reset();
    const PlotPass empty = run_plot(gui, snap(), {}, {});
    CHECK(empty.frame.view_x.lo == 0.0);
    CHECK(ws::ui::gui_test::stub.texts > 0);

    // A NaN sample breaks the line: 21 points with a hole draw 19
    // segments, not a poisoned vertex.
    ws::ui::gui_test::stub.reset();
    std::vector<double> holed = ys;
    holed[10] = std::numeric_limits<double>::quiet_NaN();
    const std::vector<ws::ui::gui::PlotSeries> holed_series{
        ws::ui::gui::PlotSeries{"v(out)", holed.data(), 0, true,
                                ws::ui::gui::PlotSpine::left, nullptr},
    };
    (void)run_plot(gui, snap(), xs, holed_series);
    // 19 series segments + grid + frame + zero rail - the point is the
    // draw survived the NaN (nothing threw, lines still landed).
    CHECK(ws::ui::gui_test::stub.lines >= 19);
}

TEST_CASE("plot: a skin's painter row replaces the whole look") {
    // The painters seam: a table carrying a plot row routes the
    // composed widget's draw through it, and a null row (the partial
    // skin) falls back to the stock draw. The routing fold is what the
    // test pins - the counter tells which half painted.
    static int skin_plot_calls = 0;  // the lambda's bookkeeping,
                                     // single-threaded like every gui
                                     // global
    skin_plot_calls = 0;
    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    const std::vector<double> xs = ramp(5, 0.0, 4.0);
    std::vector<double> ys = {0.0, 1.0, 0.0, 1.0, 0.0};
    const std::vector<ws::ui::gui::PlotSeries> series{
        ws::ui::gui::PlotSeries{"a", ys.data(), 0, true,
                                ws::ui::gui::PlotSpine::left, nullptr},
    };

    ws::ui::gui::ChromeTheme theme = kDefaultChrome;
    ws::ui::gui::WidgetPainters skin = ws::ui::gui::kStockPainters;
    skin.plot = [](const ws::ui::gui::Rect&, const ws::ui::gui::PlotFrame&,
                   const ws::ui::gui::PlotSeries*, int, int, const double*,
                   const char*, const ws::ui::gui::PlotResult&,
                   const ws::ui::gui::PlotState&,
                   const ws::ui::gui::ChromeTheme&) {
        ++skin_plot_calls;
    };
    theme.painters = &skin;

    LayoutCursor layout;
    gui.begin_frame(snap());
    (void)ws::ui::gui::begin_panel(gui, layout, theme, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    (void)ws::ui::gui::plot(gui, layout, theme, "scope", xs.data(),
                            series.data(), 1, 5);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    CHECK(skin_plot_calls == 1);
    // The skin painted; the stock draw did not (no clip push beyond
    // the panel's own scopes - the stub's counters hold near-zero
    // growth from the plot's half).
    const std::uint32_t lines_with_skin = ws::ui::gui_test::stub.lines;

    // The partial skin (null row): the stock draw paints.
    skin.plot = nullptr;
    theme.painters = &skin;
    ws::ui::gui_test::stub.reset();
    gui.begin_frame(snap());
    (void)ws::ui::gui::begin_panel(gui, layout, theme, "panel",
                                   {0.0f, 0.0f, 400.0f, 300.0f});
    (void)ws::ui::gui::plot(gui, layout, theme, "scope", xs.data(),
                            series.data(), 1, 5);
    ws::ui::gui::end_panel(gui, layout);
    gui.end_frame();
    CHECK(skin_plot_calls == 1);  // unchanged - the stock half ran
    CHECK(ws::ui::gui_test::stub.lines > lines_with_skin);
}
