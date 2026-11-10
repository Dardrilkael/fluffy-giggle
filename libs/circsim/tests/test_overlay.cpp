// overlay (patch 0012): the interactive canvas's pure math - one case
// per contract in overlay.hpp's docs, in the four shapes the patch
// proposal named: the divider's steady .op, the rc_step's scrubbed
// transient, the LED sine's glow, and the no-table silence. Tables
// and back-maps are built by hand (the run path is test_export's and
// the editor suites' own gate); every rule here answers doubles and
// packed colors, no ui anywhere.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ws/spice/results.hpp>

#include <ws/circsim/export.hpp>
#include <ws/circsim/overlay.hpp>
#include <ws/circsim/plot.hpp>

using ws::circsim::OverlayWindow;
using ws::circsim::SpiceExport;
using ws::circsim::overlay_led_brightness;
using ws::circsim::overlay_playhead_sample;
using ws::circsim::overlay_rate_label;
using ws::circsim::overlay_rate_next;
using ws::circsim::overlay_scrub;
using ws::circsim::overlay_snapshot;
using ws::circsim::overlay_step;
using ws::circsim::overlay_voltage_color;
using ws::circsim::overlay_window;

namespace {

// 0xRRGGBBAA channels (the r2d packing overlay.hpp spells locally).
[[nodiscard]] int chan(std::uint32_t rgba, int shift) {
    return static_cast<int>((rgba >> shift) & 0xFFu);
}

}  // namespace

TEST_CASE("the divider: a steady .op snapshot paints its ratio") {
    ws::spice::Table t;
    t.analysis = ws::spice::AnalysisKind::Op;
    t.sweep = ws::spice::SweepKind::None;
    t.signals.push_back({"v(in)", {5.0}});
    t.signals.push_back({"v(out)", {2.5}});
    t.signals.push_back({"i(V1)", {-2.5e-3}});

    SpiceExport map;
    map.node_net["in"] = 100;
    map.node_net["out"] = 101;
    map.element_part["V1"] = 7;

    const ws::circsim::OverlaySnapshot snap =
        overlay_snapshot(t, map, 0.123 /* any playhead: .op is settled */);
    CHECK(snap.ok);
    REQUIRE(snap.volts.size() == 2u);
    CHECK(snap.volts.at(100) == doctest::Approx(5.0));
    CHECK(snap.volts.at(101) == doctest::Approx(2.5));
    CHECK(snap.vmax == doctest::Approx(5.0));
    REQUIRE(snap.amps.count(7) == 1u);
    CHECK(snap.amps.at(7) == doctest::Approx(-2.5e-3));

    // A .op answers its last row whatever the playhead - the settled
    // state, what an .op reader means by "the answer".
    CHECK(overlay_playhead_sample(t, 1e9) == 0u);
    // And the loop's own half: nothing conducted means nothing glows.
    CHECK(overlay_led_brightness(0.0) == 0.0);
}

TEST_CASE("the rc_step: the playhead samples, scrubs and steps") {
    // 51 rows at 100 us: 0 .. 5 ms, the cap charging toward the rail.
    constexpr std::size_t kRows = 51;
    constexpr double kStep = 1e-4;
    constexpr double kTau = 1e-3;
    ws::spice::Table t;
    t.analysis = ws::spice::AnalysisKind::Tran;
    t.sweep = ws::spice::SweepKind::Time;
    t.sweep_names = {"time"};
    t.sweep_axes.emplace_back();
    ws::spice::Signal cap;
    cap.name = "v(cap)";
    for (std::size_t i = 0; i < kRows; ++i) {
        const double time = kStep * static_cast<double>(i);
        t.sweep_axes.back().push_back(time);
        cap.data.push_back(5.0 * (1.0 - std::exp(-time / kTau)));
    }
    t.signals.push_back(std::move(cap));

    const OverlayWindow w = overlay_window(t);
    CHECK(w.t0 == doctest::Approx(0.0));
    CHECK(w.t1 == doctest::Approx(5e-3));

    // The sample rule: the LAST sample at or before the playhead -
    // the plot hover's own rule, so the surfaces cannot disagree.
    CHECK(overlay_playhead_sample(t, 0.0) == 0u);
    CHECK(overlay_playhead_sample(t, 2.323e-3) == 23u);
    CHECK(overlay_playhead_sample(t, 2.3e-3) == 23u);
    CHECK(overlay_playhead_sample(t, 4.999e-3) ==
          49u);  // 4.9 ms: the LAST sample at or before
    CHECK(overlay_playhead_sample(t, 5.0e-3) == 50u);
    CHECK(overlay_playhead_sample(t, 1e9) == 50u);

    // The scrub: a track fraction lands proportionally, clamped.
    CHECK(overlay_scrub(w, 0.5) == doctest::Approx(2.5e-3));
    CHECK(overlay_scrub(w, -0.25) == doctest::Approx(0.0));
    CHECK(overlay_scrub(w, 1.75) == doctest::Approx(5e-3));

    // The step: dt seconds at `rate` sweeps the span in
    // loop_seconds / rate of wall clock, wrapping at the end.
    CHECK(overlay_step(0.0, w, 1.0, 1.0, 4.0) ==
          doctest::Approx(1.25e-3));  // span * 1 / 4 per second
    CHECK(overlay_step(0.0, w, 1.0, 2.0, 4.0) ==
          doctest::Approx(2.5e-3));  // 2x halves the wall clock
    CHECK(overlay_step(0.0, w, 1.0, 0.25, 4.0) ==
          doctest::Approx(3.125e-4));  // 1/4x stretches it
    CHECK(overlay_step(4.999e-3, w, 1.0, 2.0, 4.0) ==
          doctest::Approx(2.499e-3));  // the wrap loops the sweep
    CHECK(overlay_step(3.0e-3, w, 0.0, 1.0, 4.0) ==
          doctest::Approx(3.0e-3));  // no dt: parked

    // The rate ladder cycles 1/4 -> 1/2 -> 1 -> 2 -> 1/4.
    CHECK(overlay_rate_next(1.0) == doctest::Approx(2.0));
    CHECK(overlay_rate_next(2.0) == doctest::Approx(0.25));
    CHECK(overlay_rate_next(0.25) == doctest::Approx(0.5));
    CHECK(overlay_rate_next(0.5) == doctest::Approx(1.0));
    CHECK(std::string(overlay_rate_label(0.25)) == "1/4x");
    CHECK(std::string(overlay_rate_label(0.5)) == "1/2x");
    CHECK(std::string(overlay_rate_label(1.0)) == "1x");
    CHECK(std::string(overlay_rate_label(2.0)) == "2x");
}

TEST_CASE("the led sine: the glow follows |i|, the ramp follows sign") {
    // One full 2 ms period: the source's sine and the LED's current.
    constexpr std::size_t kRows = 41;
    constexpr double kPeriod = 2e-3;
    constexpr double kPi = 3.141592653589793;
    ws::spice::Table t;
    t.analysis = ws::spice::AnalysisKind::Tran;
    t.sweep = ws::spice::SweepKind::Time;
    t.sweep_names = {"time"};
    t.sweep_axes.emplace_back();
    ws::spice::Signal src;
    src.name = "v(src)";
    ws::spice::Signal led;
    led.name = "i(D1)";
    for (std::size_t i = 0; i < kRows; ++i) {
        const double time =
            kPeriod * static_cast<double>(i) /
            static_cast<double>(kRows - 1u);
        const double s = std::sin(2.0 * kPi * time / kPeriod);
        t.sweep_axes.back().push_back(time);
        src.data.push_back(5.0 * s);
        led.data.push_back(15e-3 * s);
    }
    t.signals.push_back(std::move(src));
    t.signals.push_back(std::move(led));

    SpiceExport map;
    map.node_net["src"] = 300;
    map.element_part["D1"] = 7;

    // The peak (t = 0.5 ms): +15 mA through the probe, +5 V on the
    // node - the snapshot resolves both at the playhead's sample.
    const ws::circsim::OverlaySnapshot peak =
        overlay_snapshot(t, map, 0.5e-3);
    CHECK(peak.ok);
    REQUIRE(peak.amps.count(7) == 1u);
    CHECK(peak.amps.at(7) == doctest::Approx(15e-3));
    CHECK(peak.volts.at(300) == doctest::Approx(5.0));
    CHECK(peak.vmax == doctest::Approx(5.0));

    // The brightness curve: sqrt from the 100 uA floor to full-on at
    // 20 mA, sign-blind, clamped past full scale.
    CHECK(overlay_led_brightness(15e-3) ==
          doctest::Approx(std::sqrt(0.75)));
    CHECK(overlay_led_brightness(1e-3) ==
          doctest::Approx(std::sqrt(0.05)));
    CHECK(overlay_led_brightness(20e-3) == doctest::Approx(1.0));
    CHECK(overlay_led_brightness(100e-3) == doctest::Approx(1.0));
    CHECK(overlay_led_brightness(99e-6) == 0.0);  // below the floor
    CHECK(overlay_led_brightness(-15e-3) ==
          overlay_led_brightness(15e-3));  // |i|, not the sign

    // The ramp: neutral at zero, red-dominant positive, blue-dominant
    // negative, symmetric about zero at the run's own scale.
    const std::uint32_t neutral = overlay_voltage_color(0.0, 5.0);
    CHECK(chan(neutral, 24) == 122);  // the neutral ink, byte-exact
    CHECK(chan(neutral, 16) == 126);
    CHECK(chan(neutral, 8) == 134);
    const std::uint32_t hot = overlay_voltage_color(5.0, 5.0);
    CHECK(chan(hot, 24) == 255);  // full red
    CHECK(chan(hot, 24) > chan(hot, 16));
    CHECK(chan(hot, 24) > chan(hot, 8));
    const std::uint32_t cold = overlay_voltage_color(-5.0, 5.0);
    CHECK(chan(cold, 8) == 255);  // full blue
    CHECK(chan(cold, 8) > chan(cold, 24));
    CHECK(chan(cold, 8) > chan(cold, 16));
    const std::uint32_t half = overlay_voltage_color(2.5, 5.0);
    CHECK(chan(half, 24) > chan(neutral, 24));  // warming, monotonely
    CHECK(chan(half, 24) < chan(hot, 24));
}

TEST_CASE("no table: the overlay is a silent no-op") {
    // A fresh document, never Run: no tables, no map, no playhead.
    const ws::spice::Table empty;
    SpiceExport map;

    const ws::circsim::OverlaySnapshot snap =
        overlay_snapshot(empty, map, 1.0);
    CHECK_FALSE(snap.ok);
    CHECK(snap.volts.empty());
    CHECK(snap.amps.empty());
    CHECK(snap.sample == 0u);

    // The window degenerates; the scrub parks; the step clamps.
    const OverlayWindow w = overlay_window(empty);
    CHECK(w.t0 == doctest::Approx(0.0));
    CHECK(w.t1 == doctest::Approx(0.0));
    CHECK(overlay_scrub(w, 0.5) == doctest::Approx(0.0));
    CHECK(overlay_step(1.0, w, 0.016, 2.0, 4.0) ==
          doctest::Approx(0.0));

    // A table whose axis is empty answers the same silence (the
    // transport still draws, it just has nothing to sweep).
    ws::spice::Table no_axis;
    no_axis.sweep = ws::spice::SweepKind::Time;
    no_axis.signals.push_back({"v(out)", {1.0, 2.0}});
    CHECK(overlay_window(no_axis).t1 == doctest::Approx(0.0));
    CHECK(overlay_playhead_sample(no_axis, 1.0) == 1u);  // the last row
}

// --- the .dc sweep (the transport's second axis) -------------------------------
//
// The same scrub/play/loop machinery a .tran rides, on the swept
// source's own axis: strictly increasing values, the playhead in the
// source's units, the readout naming the source and its unit. The
// .step family's leading axes never answer - the analysis's own x
// does.

TEST_CASE("the .dc sweep: the transport scrubs the swept source") {
    ws::spice::Table t;
    t.analysis = ws::spice::AnalysisKind::Dc;
    t.sweep = ws::spice::SweepKind::Swept;
    t.sweep_names = {"V1"};
    t.sweep_axes.push_back(
        {0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0});
    t.signals.push_back({"v(out)",
                         {0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8,
                          0.9, 1.0}});

    CHECK(ws::circsim::overlay_scrubbable(t));
    const std::span<const double> axis =
        ws::circsim::overlay_sweep_axis(t);
    REQUIRE(axis.size() == 11u);

    // The window is the swept source's own range.
    const OverlayWindow w = overlay_window(t);
    CHECK(w.t0 == doctest::Approx(0.0));
    CHECK(w.t1 == doctest::Approx(5.0));

    // The playhead resolves onto the swept axis exactly as a .tran's
    // resolves onto time: the last sample at or before it.
    CHECK(ws::circsim::overlay_playhead_sample(t, 0.0) == 0u);
    CHECK(ws::circsim::overlay_playhead_sample(t, 0.75) == 1u);
    CHECK(ws::circsim::overlay_playhead_sample(t, 4.99) == 9u);
    CHECK(ws::circsim::overlay_playhead_sample(t, 5.0) == 10u);
    CHECK(ws::circsim::overlay_playhead_sample(t, 99.0) == 10u);

    // The scrub walks the same window; the step loops it.
    CHECK(overlay_scrub(w, 0.5) == doctest::Approx(2.5));
    CHECK(overlay_step(2.0, w, 0.016, 1.0, 4.0) ==
          doctest::Approx(2.0 + 0.02));

    // The readout names the source and reads its unit off the name's
    // leading letter (V is volts, I is amps - the DcSpec grammar).
    using ws::circsim::overlay_axis_readout;
    CHECK(overlay_axis_readout(t, 3.4) ==
          std::string{"V1 = "} + ws::circsim::format_eng(3.4) + "V");
}

TEST_CASE("the readout spells each axis's own unit") {
    using ws::circsim::overlay_axis_readout;

    ws::spice::Table tran;
    tran.sweep = ws::spice::SweepKind::Time;
    tran.sweep_names = {"time"};
    tran.sweep_axes.push_back({0.0, 1.2e-3});
    CHECK(overlay_axis_readout(tran, 1.2e-3) ==
          ws::circsim::format_eng(1.2e-3) + "s");

    ws::spice::Table isrc;
    isrc.sweep = ws::spice::SweepKind::Swept;
    isrc.sweep_names = {"I2"};
    isrc.sweep_axes.push_back({0.0, 2.0e-3});
    CHECK(overlay_axis_readout(isrc, 2.0e-3) ==
          std::string{"I2 = "} + ws::circsim::format_eng(2.0e-3) + "A");

    ws::spice::Table ac;
    ac.sweep = ws::spice::SweepKind::Frequency;
    ac.sweep_names = {"frequency"};
    ac.sweep_axes.push_back({1.0, 1000.0});
    CHECK(overlay_axis_readout(ac, 1000.0) ==
          ws::circsim::format_eng(1000.0) + "Hz");

    // A .op sweeps nothing: the empty spelling.
    ws::spice::Table op;
    op.sweep = ws::spice::SweepKind::None;
    CHECK(overlay_axis_readout(op, 0.0).empty());
}

TEST_CASE("a frequency table is labeled but never scrubbable") {
    ws::spice::Table ac;
    ac.analysis = ws::spice::AnalysisKind::Ac;
    ac.sweep = ws::spice::SweepKind::Frequency;
    ac.sweep_names = {"frequency"};
    ac.sweep_axes.push_back({1.0, 10.0, 100.0, 1000.0});
    CHECK_FALSE(ws::circsim::overlay_scrubbable(ac));
    // The playhead parks at the last row (the settled spelling) - the
    // canvas never animates a log axis off a linear track.
    CHECK(ws::circsim::overlay_playhead_sample(ac, 10.0) == 3u);
}

TEST_CASE("a .step family's leading axes never answer as the sweep") {
    ws::spice::Table t;
    t.analysis = ws::spice::AnalysisKind::Dc;
    t.sweep = ws::spice::SweepKind::Swept;
    // The step number leads (step_axes = 1), the swept source follows.
    t.sweep_names = {"step", "V1"};
    t.sweep_axes.push_back({1.0, 1.0, 1.0, 1.0, 2.0, 2.0, 2.0, 2.0});
    t.sweep_axes.push_back({0.0, 1.0, 2.0, 3.0, 0.0, 1.0, 2.0, 3.0});
    t.step_axes = 1;
    t.signals.push_back({"v(out)", {0.0, 1.0, 2.0, 3.0, 0.0, 1.0, 2.0,
                                    3.0}});

    CHECK(ws::circsim::overlay_scrubbable(t));
    const std::span<const double> axis =
        ws::circsim::overlay_sweep_axis(t);
    REQUIRE(axis.size() == 8u);
    CHECK(axis.front() == doctest::Approx(0.0));
    CHECK(axis.back() == doctest::Approx(3.0));
    // The playhead 2.5 sits past V1=2 in the FIRST family; the last
    // sample at or before it is the family's own 2.0 (index 6, not
    // the step column's answers).
    CHECK(ws::circsim::overlay_playhead_sample(t, 2.5) == 6u);
}
