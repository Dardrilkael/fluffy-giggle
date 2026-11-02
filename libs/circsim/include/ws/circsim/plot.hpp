#pragma once

// The results story's headless half (stage 4, docs/CIRCSIM_PLAN.md):
// the pure math every plot axis and readout needs - nice tick
// positions, engineering-notation labels, range padding, and min/max
// decimation for long sweeps. The editor's Results panel is the ui
// consumer; the tests are the headless gate. Nothing here touches a
// font, a canvas, or ws::spice - the functions answer doubles and
// strings, so any plot surface (the panel today, a raster exporter
// later) shares one set of axis decisions.

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace ws::circsim {

// Tick positions covering [lo, hi] at roughly `target` intervals,
// snapped to 1/2/5 x 10^k (the "nice numbers" ladder every scope and
// EDA plot walks). The endpoints themselves are NOT forced onto ticks -
// data ranges rarely land on them, and a padded axis that shows its
// true bounds reads better than one that lies by a half step.
//
//   * lo == hi (or either non-finite): the single tick {lo} - a
//     degenerate axis is the caller's to pad (expand_range), but the
//     answer stays honest rather than empty.
//   * lo > hi: empty (an inverted range is a caller bug; silently
//     swapping it would plot mirrored data).
//   * target 0: treated as 1.
[[nodiscard]] std::vector<double> nice_ticks(double lo, double hi,
                                             unsigned target);

// SI engineering notation, the EDA plot convention: 4700 -> "4.7k",
// -12.3e-3 -> "-12.3m", 2e-9 -> "2n", 0 -> "0". Prefixes run f p n u m
// (none) k M G T - ASCII "u" for micro (the SPICE spelling), and
// case matters: m is milli, M is mega. `digits` is the significant-
// digit budget (1..6, clamped); a mantissa that rounds up past its
// decade ("999.9m" -> "1") renormalizes rather than printing 1000m.
// Non-finite inputs answer "nan"/"inf"/"-inf" so a bad column labels
// itself instead of formatting garbage.
[[nodiscard]] std::string format_eng(double v, int digits = 3);

// A closed interval with the endpoints in order.
struct PlotRange {
    double lo = 0.0;
    double hi = 0.0;
};

// Pads [lo, hi] by `frac` of its span on both sides (default 5%), the
// breathing room a plot frame wants so extremal points are not ON the
// border. A degenerate lo == hi expands by frac of max(|lo|, 1.0)
// instead - a constant line still gets an axis. Non-finite endpoints
// answer {0, 1}: the caller draws an empty-but-sane frame rather than
// a NaN-poisoned transform. frac <= 0 is treated as 0 (a clamp, never
// a narrowing).
[[nodiscard]] PlotRange expand_range(double lo, double hi,
                                     double frac = 0.05);

// The index list that carries a long waveform's shape through a narrow
// pixel budget: the strictly-increasing `xs` split into budget/2
// buckets, each bucket contributing its y-MINIMUM and y-MAXIMUM sample
// in occurrence order (so spikes survive - the bucket's extremes ARE
// the pixels that matter), plus the global first and last. The answer
// never exceeds ~2 x budget entries and is strictly increasing in
// index.
//
// EMPTY means "decimation does not apply - draw every point": either
// the input already fits the budget, the spans differ in length, or xs
// is not strictly increasing (a nested .dc's repeating inner axis),
// where bucketing by value would scramble the true order. The caller's
// fallback is the honest full-resolution polyline.
[[nodiscard]] std::vector<std::size_t> decimate_minmax(
    std::span<const double> xs, std::span<const double> ys,
    std::size_t budget);

}  // namespace ws::circsim
