// ws::plotmath - the one authoritative plotting math core.
//
// The generic gui plot widget (libs/ui's widgets_plot.cpp) and the
// Circsim Results panel (apps/circsim_editor's results_panel.cpp)
// used to each carry their own copy of every plotting decision: the
// nice-tick ladder, the engineering-notation label, the range padding,
// the view clamp/zoom/pan invariants, the nearest-sample hover lookup,
// and the min/max decimation. The two spellings started as one
// (widgets_plot.cpp's comments said so out loud: "the circsim
// spelling, kept byte-for-byte") and then drifted - the guard sets
// diverged at the edges (circsim's ladder carries the deep-zoom
// 2^53 guard, the widget's capped its walk at 32 slots; circsim's
// format walks the exponent-form mantissa, the widget's parser
// refused it), so identical inputs could label differently.
//
// This library is the merge: one implementation, both spellings'
// guard sets unioned, and both consumers call it. It is deliberately
// BELOW both tiers - ui links it, circsim links it, and neither
// reaches into the other. It knows nothing about Canvas2D, Vulkan,
// the GuiContext, ws::spice tables or the editor: everything here
// answers doubles, plain index lists, and strings/char buffers. Pure
// <cmath>/<algorithm> + std - the headless suites gate every decision
// the two plotting surfaces render.
//
// What is NOT here, on purpose: rendering, styling, tick LABEL
// placement, legends, tooltips, gesture STATE MACHINES (which button
// arms which zoom - the widget's and the panel's interaction wiring
// are presentation, and they stay where they were), and the app's
// per-table caches. The constants the two surfaces' gestures must
// agree on (the wheel factor, the strip exponential, the
// double-click pair's window) ARE here, spelled once, so the two
// surfaces' navigation FEELS the same - the behavioral contract -
// while each keeps its own input plumbing.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ws::plotmath {

// --- the axis ladder ----------------------------------------------------------

// A closed interval with the endpoints in order - the one range
// spelling both consumers' math answers in. (The widget's
// PlotAxisRange and circsim's PlotRange remain their own types for
// API stability; both convert implicitly - same two members.)
struct PlotRange {
    double lo = 0.0;
    double hi = 0.0;
};

// Tick positions covering [lo, hi] at roughly `target` intervals,
// snapped to the 1/2/5 x 10^k ladder every scope walks. The
// endpoints are NOT forced onto ticks - a padded axis that shows its
// true bounds reads better than one that lies by a half step.
//
//   * lo == hi (or either NaN): the single tick {lo} - a degenerate
//     axis is the caller's to pad (expand_range), but the answer
//     stays honest rather than empty. (The !(lo < hi) spelling also
//     catches the -inf == -inf corner after the isnan guard.)
//   * lo > hi: empty (an inverted range is a caller bug; silently
//     swapping it would plot mirrored data).
//   * target 0: treated as 1.
//   * an infinite span answers empty (it cannot walk a ladder), and
//     a span that underflowed against the target (raw or its decade
//     at/below zero - a denormal window) answers empty too: the
//     ladder's own refusal, never garbage ticks.
//   * a deep zoom at a large offset can ask for k0/k1 past 2^53,
//     where a double no longer resolves ++k - the honest answer is
//     none, not a hang.
//   * a pathological huge target cannot overflow the walk: the
//     answer is capped at 32 ticks (both historical spellings agree
//     on the cap's value; a target that asks for more is the
//     caller's bug, and the cap makes it visible, not fatal).
[[nodiscard]] std::vector<double> nice_ticks(double lo, double hi,
                                             unsigned target);

// SI engineering notation, the EDA plot convention: 4700 -> "4.7k",
// -12.3e-3 -> "-12.3m", 2e-9 -> "2n", 0 -> "0". Prefixes run f p n u m
// (none) k M G T - ASCII "u" for micro (the SPICE spelling), and case
// matters: m is milli, M is mega. `digits` is the significant-digit
// budget (1..6, clamped); a mantissa that rounds up through its
// decade ("999.9m" -> "1") renormalizes to the next prefix rather
// than printing 1000m, a mantissa %g slid into exponent form ("2e+01")
// is spelled back out in plain digits at the digit budget, and
// magnitudes beyond the table both ways fall back to plain %g (the
// magnitude stays honest where a clamped prefix would round). Non-
// finite inputs answer "nan"/"inf"/"-inf" so a bad column labels
// itself. Two spellings over one walk:
//   * std::string format_eng(double, int digits = 3) - the panel's.
//   * const char* format_eng_buf(char* buf, std::size_t cap, double,
//     int digits = 3) - the widget's caller-scratch discipline (no
//     allocation mid-frame); always NUL-terminated, truncated to
//     fit. The two answers are byte-identical for the same input.
[[nodiscard]] std::string format_eng(double v, int digits = 3);
const char* format_eng_buf(char* buf, std::size_t cap, double v,
                           int digits = 3);

// --- the range/view invariants ------------------------------------------------

// Pads [lo, hi] by `frac` of its span on both sides (default 5%), the
// breathing room a frame wants so extremal points are not ON the
// border. A degenerate lo == hi expands by frac of max(|lo|, 1.0) -
// a constant line still gets an axis; non-finite endpoints answer
// {0, 1} (the empty-but-sane frame); a span whose pad overflows
// answers [lo, hi] unpadded (padding cannot be honest there); frac
// <= 0 or non-finite is treated as 0 (a clamp, never a narrowing).
[[nodiscard]] PlotRange expand_range(double lo, double hi,
                                     double frac = 0.05);

// Clamps `view` back inside `home`: a non-finite endpoint answers
// home (the caller's auto fit), a view at or past home's span answers
// home (nothing to show beyond the data), otherwise the window slides
// the minimal distance so both ends sit inside. A home that is itself
// non-finite or degenerate (lo >= hi) answers home as-is - a
// degenerate axis never opens a manual view.
[[nodiscard]] PlotRange clamp_view(PlotRange view, PlotRange home);

// Zooms `view` about `anchor` (a data coordinate, clamped into the
// view first - a zoom anchored outside the window acts at the nearest
// edge) by `factor`: > 1 zooms IN (the span shrinks), < 1 out, 1 or a
// non-finite or non-positive factor answers the view unchanged. A
// NaN anchor is treated as the view's lo. The result is floored at
// home's span x 1e-6 - below that the axis is float mush - and
// clamped into home, so a wheel spun out forever settles on home,
// never on an inverted or infinite window. The anchor contract the
// two surfaces both promise: the data coordinate under the cursor
// stays under the cursor (anchor + (view.lo - anchor)/f, the same
// scale both ends ride).
[[nodiscard]] PlotRange zoom_range(PlotRange view, PlotRange home,
                                    double anchor, double factor);

// Slides `view` by `delta` data units (positive = toward hi) and
// clamps the result into home - a pan past the data's edge parks
// flush against it instead of losing the waveform. A non-finite delta
// answers the view unchanged.
[[nodiscard]] PlotRange pan_range(PlotRange view, PlotRange home,
                                  double delta);

// --- the data window ----------------------------------------------------------

// True iff xs is strictly increasing end to end (a NaN anywhere
// fails the chain - the honest fallback answer for both consumers'
// binary searches and bucket walks).
[[nodiscard]] bool strictly_increasing(std::span<const double> xs);

// The index in [0, n) whose xs value is nearest `x` - the hover
// lookup both surfaces run. A sorted axis (strictly_increasing over
// the first n entries) binary-searches; anything else scans (the
// wrapped .dc axis, a step family, a hand-built test axis). n == 0
// answers -1; the binary path breaks ties toward the lower index
// (the <= in the walk), the scan toward the first strictly-closer
// entry - the two paths agree on every input where both are legal
// (the scan is the definition; the search is its fast path).
[[nodiscard]] int nearest_sample(std::span<const double> xs, double x,
                                 bool sorted, std::size_t n);

// The index list that carries a long waveform's shape through a
// narrow pixel budget: the strictly-increasing `xs` split into
// budget/2 buckets, each bucket contributing its y-MINIMUM and
// y-MAXIMUM sample in occurrence order (so spikes survive - the
// bucket's extremes ARE the pixels that matter), plus the global
// first and last forced in (edge timing is load-bearing for a
// waveform). The answer never exceeds ~2 x budget entries, is
// strictly increasing in index, and preserves ORIGINAL sample
// indices - tooltips and markers reference the source rows.
//
// EMPTY means "decimation does not apply - draw every point": the
// input already fits the budget, the spans differ in length, or xs
// is not strictly increasing (bucketing by value would scramble the
// true order). The caller's fallback is the honest full-resolution
// polyline.
[[nodiscard]] std::vector<std::size_t> decimate_minmax(
    std::span<const double> xs, std::span<const double> ys,
    std::size_t budget);

// The same envelope for data whose x IS the sample index (a table
// with no sweep axis of its own, a non-sorted wrap): buckets split
// the INDEX run (bucket b owns samples [b*n/buckets,
// (b+1)*n/buckets)) instead of the x span. Same min/max-per-bucket
// keeps in occurrence order, same forced endpoints, same ~2 x budget
// ceiling, same empty-means-raw refusal (input fits, budget < 2, or
// n < 2). A bucket that holds no finite sample still answers through
// its opener (a NaN vertex - the line break the strokers know).
[[nodiscard]] std::vector<std::size_t> decimate_minmax_index(
    std::span<const double> ys, std::size_t budget);

// --- the monotone-run family --------------------------------------------------

// One maximal strictly-monotone run of the axis. `begin`/`end` are
// sample positions (end exclusive); `rising` is the run's direction
// - a falling sweep (a .dc walking its source down) is as drawable
// as a rising one, reversed.
struct AxisRun {
    std::size_t begin = 0;
    std::size_t end = 0;
    bool rising = true;
};

// The axis's monotone runs plus its finite envelope. The runs are
// maximal and cover [0, xs.size()) exactly: a run stops where the
// direction fails - a reset (the value drops or jumps back), a
// plateau (equal neighbours), a reversal, or a NaN on either side -
// and the next run opens at that sample. A strictly monotone axis
// answers ONE run covering it; a .step family answers one run per
// pass. `lo`/`hi` are the min/max over every finite x (the union of
// the passes' extents, so a family whose passes differ in span still
// fits); `finite` false means no finite x at all, and lo/hi stay the
// sane-empty 0/1.
//
// This is the "never chord independent runs" contract: a split like
// this, drawn one polyline per run with a break between, keeps .step
// families and independent sweeps visually disconnected - consumers
// that ignore the split and draw one polyline chord every reset into
// the lines-between-vertices mess.
struct AxisRuns {
    std::vector<AxisRun> runs;
    double lo = 0.0;
    double hi = 1.0;
    bool finite = false;
};

[[nodiscard]] AxisRuns split_axis_runs(std::span<const double> xs);

// --- the navigation constants -------------------------------------------------
//
// The two surfaces' gesture FEEL is the behavioral contract this
// merge exists for: the same wheel notch must zoom by the same
// factor, the same strip drag the same exponential, the same
// double-click window the same pair. One spelling of each, shared by
// the widget and the panel (both files' constants were identical
// values in two anonymous namespaces - duplicated contracts, not
// coincidence).

// One wheel notch zooms by 15%.
inline constexpr double kWheelFactor = 1.15;

// An axis-strip drag compounds exp(0.005) per pixel - pulling the
// strip its own height open doubles the zoom, a resistance that
// reads like a real axis under the finger.
inline constexpr double kStripExp = 0.005;

// The double-click pair: two presses, the same 5-px spot, within
// 0.35 s.
inline constexpr double kDoubleClickSecs = 0.35;
inline constexpr float kDoubleClickPx = 5.0f;

// A band smaller than 4 px on either axis is a click, not a zoom.
inline constexpr float kBandMinPx = 4.0f;

// The margins the two surfaces' frames share (the left margin carries
// the y labels and is the left spine's drag strip; the bottom one the
// x labels; the right one widens when a second spine earns its label
// strip) - presentation geometry, but both frames draw identical
// ones and the strips are the interaction strips.
inline constexpr float kMarginLeft = 46.0f;
inline constexpr float kMarginBottom = 17.0f;
inline constexpr float kMarginTop = 6.0f;
inline constexpr float kMarginRight = 10.0f;
inline constexpr float kMarginRightAxis = 46.0f;

// A frame that cannot hold at least 40x30 of plot is not worth
// drawing (both surfaces' guard).
inline constexpr float kMinInnerW = 40.0f;
inline constexpr float kMinInnerH = 30.0f;

}  // namespace ws::plotmath
