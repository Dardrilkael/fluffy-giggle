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
#include <string>
#include <unordered_map>

#include <ws/circsim/export.hpp>
#include <ws/spice/results.hpp>

namespace ws::circsim {

// --- the playhead's sample ---------------------------------------------------

// The table's OWN sweep axis - time for a .tran, the swept source
// for a .dc: the leading .step family axes are skipped (step_axes),
// so the answer is always the analysis's own x. Empty for a .op (no
// axis) or an empty table.
[[nodiscard]] std::span<const double> overlay_sweep_axis(
    const ws::spice::Table& t);

// True when the transport scrubs the table: a .tran's time axis or a
// .dc's swept-source axis (strictly increasing, a linear track is
// honest). Frequency tables are NOT scrubbable - their x is a log
// decade grid, and a linear scrub would walk it wrongly; the Results
// row still labels their axis, it just does not drive the clock.
[[nodiscard]] bool overlay_scrubbable(const ws::spice::Table& t);

// The sample a playhead on the table's own axis sits on: the last
// sample at or before it (a .tran/.dc axis is strictly increasing;
// upper_bound lands past it - the plot hover's own rule, so the
// plot's readout and the canvas's paint can never disagree). A table
// the transport does not scrub (an .op's single answer, a frequency
// sweep) answers its LAST row - the settled state; an empty table
// answers 0. The playhead's UNITS are the axis's own: seconds for a
// .tran, the swept source's value (volts or amps) for a .dc.
[[nodiscard]] std::size_t overlay_playhead_sample(const ws::spice::Table& t,
                                                  double playhead);

// The transient's window: the first and last own-axis sample (t0 <=
// t1; both 0 when the table has no axis - the caller's "nothing to
// sweep").
struct OverlayWindow {
    double t0 = 0.0;
    double t1 = 0.0;
};
[[nodiscard]] OverlayWindow overlay_window(const ws::spice::Table& t);

// The transport's readout for a playhead: "1.2ms" for time (the
// strip's own spelling, kept), "V1 = 3.4V" for a swept voltage
// source ("I1 = 2.0mA" for a current one - the unit reads the
// source name's leading letter), "1.0kHz" for a frequency axis. A
// .op answers the empty string (nothing is being swept).
[[nodiscard]] std::string overlay_axis_readout(const ws::spice::Table& t,
                                               double playhead);

// A scrub track fraction in [0, 1] -> a playhead on the window's own
// axis (seconds for a .tran, the swept source's value for a .dc). A
// degenerate window answers t0; the fraction is clamped (a pointer a
// hair outside the track is a hair inside the window, never past its
// ends).
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

// --- the switch blade's state -----------------------------------------------------

// One switch's answer at a playhead: whether the S/W contact is closed
// and whether the run could say at all. `known` is false when anything
// the walk needs is missing - no rows, a control column the table does
// not carry (an unwired pin, an .ac's small-signal columns), a model
// name no .model card answers - and the caller rests the blade OPEN,
// exactly what a switch with no run looks like.
struct OverlaySwitch {
    bool known = false;
    bool closed = false;
};

// The S/W contact's state at the playhead, mirroring ws::spice's SW
// device law exactly: the control signal walks the table's samples
// from row 0 to the playhead's, flipping through the model's
// hysteresis band (on above thr+band, off below thr-band, sticky in
// between - devices_m2's hysteresis_flip, the same law the solver
// ran), starting OPEN (the editor's export writes no ic=on). The
// control is v(c+) - v(c-) for an S (the pins resolve through `nets`
// onto v(token) columns; a ground pin is 0 V) and i(<vs>) for a W (the
// named V source's branch column; the deck's own spelling is looked up
// case-insensitively through the map when the param's differs). The
// thresholds come from the model the part names - the directives
// block's own card wins (export's resolution order), else the built-in
// model library's - parsed with ws::spice's own deck reader, so a
// hand-authored card reads with continuations and glued parens exactly
// as the run read it. Per frame per switch on the canvas: a walk over
// the samples to the playhead and (at most) one small deck parse, the
// same order of work overlay_snapshot already spends on its maps.
[[nodiscard]] OverlaySwitch overlay_switch(const ws::spice::Table& t,
                                           const SpiceExport& map,
                                           const NetList& nets,
                                           const Schematic& s,
                                           const Part& p,
                                           const SymbolDef& sym,
                                           double playhead);

}  // namespace ws::circsim
