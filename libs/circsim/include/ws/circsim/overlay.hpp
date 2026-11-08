#pragma once

// The interactive canvas's headless half (patch 0012, docs/CIRCSIM_PLAN.md):
// the pure math the run-paints-back drawing rides - which sample a
// playhead sits on, the voltage ramp's color, the LED's brightness
// curve, a track fraction's playhead, and one step of the transport's
// sweep. The editor's canvas panel and Results panel are the ui
// consumers; the tests are the headless gate. Nothing here touches a
// font, a canvas, or ws::ui - colors answer packed 0xRRGGBBAA words
// (the r2d spelling, produced locally so this header stays ui-free),
// and every rule that decides pixels on screen answers here first, so
// the canvas strip and the plot's transport row can never disagree.
//
// The lock, as everywhere in this library: reads the run's tables and
// the export back-map, never writes them. No model change, no .csim
// change, no migration.

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include <ws/circsim/export.hpp>
#include <ws/spice/results.hpp>

namespace ws::circsim {

// --- the playhead's sample ---------------------------------------------------

// The sample a playhead in SECONDS sits on: the last sample at or
// before it on the table's first axis (a .tran axis is strictly
// increasing; upper_bound lands past it - the plot hover's own rule,
// so the plot's readout and the canvas's paint can never disagree).
// A table with no time sweep (an .op's single answer, a .dc seen
// through the canvas) answers its LAST row - the settled state; an
// empty table answers 0.
[[nodiscard]] std::size_t overlay_playhead_sample(const ws::spice::Table& t,
                                                  double playhead);

// The transient's window: the first and last axis sample (t0 <= t1;
// both 0 when the table has no axis - the caller's "nothing to
// sweep").
struct OverlayWindow {
    double t0 = 0.0;
    double t1 = 0.0;
};
[[nodiscard]] OverlayWindow overlay_window(const ws::spice::Table& t);

// A scrub track fraction in [0, 1] -> playhead seconds. A degenerate
// window answers t0; the fraction is clamped (a pointer a hair outside
// the track is a hair inside the window, never past its ends).
[[nodiscard]] double overlay_scrub(const OverlayWindow& w, double frac);

// One transport step: the playhead advanced by dt * span * rate /
// loop_seconds (loop_seconds is the 1x pace - the whole run fits in
// it; rate 2 halves the wall clock, 1/4 stretches it), wrapping at
// the window's end so the sweep loops. A non-positive dt or a
// degenerate window answers the playhead clamped into the window.
[[nodiscard]] double overlay_step(double t, const OverlayWindow& w,
                                  double dt, double rate,
                                  double loop_seconds);

// --- the rate ladder -----------------------------------------------------------

// The transport's pace ladder, cycle order: 1/4, 1/2, 1, 2. Doubles
// compare by NEAREST entry (the chip cycles through these exact
// values; a rate that drifted in from a hand-edited future file
// still finds its place in the ladder rather than falling off it).
inline constexpr double kOverlayRates[] = {0.25, 0.5, 1.0, 2.0};

// The entry after `rate` in the ladder, wrapping (2 -> 1/4). An
// unknown rate answers the ladder's first entry past 1x's own nearest
// neighbor - practically: the next of whatever it sits closest to.
[[nodiscard]] double overlay_rate_next(double rate);

// The chip's spelling: "1/4x", "1/2x", "1x", "2x" (nearest entry).
[[nodiscard]] const char* overlay_rate_label(double rate);

// --- the canvas's paint math ----------------------------------------------------

// The voltage ramp's color: v normalized by the run's max |v|, mapped
// through a sqrt (perceptual - the interesting near-zero mids spread)
// from the canvas's neutral gray toward red (positive) or blue
// (negative). `vmax` <= 0 is treated as 1e-9 (a run of pure zeros
// still answers the neutral ink, never a divide by zero). The answer
// is a packed 0xRRGGBBAA word at the wire ink's own alpha (242).
[[nodiscard]] std::uint32_t overlay_voltage_color(double v, double vmax);

// The LED's brightness in [0, 1]: full-on at the nominal 20 mA,
// dark below the 100 uA floor (reverse leakage, the solver's gmin
// ghost), and a sqrt between them - the eye's own curve, so half
// current reads as clearly dimmer, not as 50% of nothing. Sign-blind
// (|i| - the probe column's direction is the deck's convention, not
// the eye's). Above full scale clamps at 1.
[[nodiscard]] double overlay_led_brightness(double amps);

// --- the run painting snapshot ---------------------------------------------------

// What the run paints back, resolved for ONE playhead: every net's
// node voltage and every probed element's current at the playhead's
// sample, plus the voltage ramp's own scale (max |v| over the nets
// that answered). Built from the run's export back-map - node_net
// answers node token -> net id (the v(name) column), element_part
// answers deck element -> part id (the i(name) probe column) - so the
// canvas never translates ids. A net whose column is missing or
// non-finite at the sample stays absent (the wire falls back to the
// plain ink); ok is false when nothing answered (no table the map
// knows about - the overlay is a silent no-op and the canvas paints
// exactly as it would without a run).
struct OverlaySnapshot {
    bool ok = false;
    std::size_t sample = 0;  // the playhead's row in the table
    std::unordered_map<std::uint32_t, double> volts;  // net id -> V
    std::unordered_map<std::uint32_t, double> amps;   // part id -> A
    double vmax = 1e-9;  // the ramp's scale (max |v|; never below 1e-9)
};

[[nodiscard]] OverlaySnapshot overlay_snapshot(const ws::spice::Table& t,
                                               const SpiceExport& map,
                                               double playhead);

}  // namespace ws::circsim
