// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the plot - the waveform widget: the axis math, the fit, the scope
// gestures and the series stroking. The behavior half owns geometry
// and interaction (handle_plot's whole job, lifted from the editor's
// hand-rolled Results panel); the draw half owns the pixels (draw_plot's
// whole job). The axis math is PUBLIC pure functions - the popup_placement
// precedent - so the headless suite gates the ladder, the labels and the
// view invariants the same way circsim's suite gates its copy.

#include "widgets_detail.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace ws::ui::gui {

// --- the plot's file-local geometry -------------------------------------------
// The frame's margins, hardcoded pixels like the separator's 2 (the
// theme carries no plot tokens yet): the left margin carries the y
// labels (and is the left spine's drag strip), the bottom one the x
// labels (the x strip), the top a breath, the right either a breath
// or - when a visible series rides the right spine - the second label
// strip (and the right spine's drag handle). The guards: a frame that
// cannot hold at least 40x30 of plot is not worth drawing.
namespace {
constexpr float kPlotMarginLeft = 46.0f;
constexpr float kPlotMarginBottom = 17.0f;
constexpr float kPlotMarginTop = 6.0f;
constexpr float kPlotMarginRight = 10.0f;
constexpr float kPlotMarginRightAxis = 46.0f;
constexpr float kPlotMinInnerW = 40.0f;
constexpr float kPlotMinInnerH = 30.0f;

// The stock series wheel (color 0 rides it): the same 8 hues the
// editor's hand-rolled panel spun, re-homed so the widget's default
// look matches what the app already shows - data colors, not chrome,
// the one place a widget owns a palette on purpose.
constexpr std::uint32_t kPlotSeriesColors[] = {
    r2d::rgba(224, 90, 78),    // red
    r2d::rgba(78, 161, 224),   // blue
    r2d::rgba(88, 181, 92),    // green
    r2d::rgba(211, 154, 42),   // amber
    r2d::rgba(154, 111, 211),  // purple
    r2d::rgba(78, 201, 192),   // teal
    r2d::rgba(211, 90, 158),   // pink
    r2d::rgba(143, 154, 78),   // olive
};
constexpr std::size_t kPlotSeriesColorCount =
    sizeof kPlotSeriesColors / sizeof kPlotSeriesColors[0];

// r2d packs 0xRRGGBBAA, so the alpha is the low byte - the band's
// wash/outline split (a fill at ~14% under the series, a near-solid
// line around the box) is one color, two alphas.
[[nodiscard]] constexpr std::uint32_t with_alpha(std::uint32_t color,
                                                 std::uint8_t a) {
    return (color & 0xFFFFFF00u) | static_cast<std::uint32_t>(a);
}

// The scope's zoom factors: one wheel notch is 15%, and an axis strip
// drag compounds exp(0.005) per pixel - pulling the strip its own
// height open doubles the zoom, a resistance that reads like a real
// axis under the finger.
constexpr double kPlotWheelFactor = 1.15;
constexpr double kPlotStripExp = 0.005;

// The double-click pair: two presses, the same 5-px spot, within 0.35
// s. Headless the frame clock reads 0 (dt_seconds), so the clock never
// advances and every same-spot press pair reads as a pair - the honest
// answer without time, and the seam the suite drives the reset through.
constexpr float kPlotDoubleClickSecs = 0.35f;
constexpr float kPlotDoubleClickPx = 5.0f;

// A band smaller than 4 px on either axis is a click, not a zoom - it
// falls through so the second press of a double-click still lands.
constexpr float kPlotBandMinPx = 4.0f;

// The series stroke and the grid/frame ink, hardcoded pixels like the
// separator's 2 (a plot's ink is data density, not chrome).
constexpr float kPlotSeriesWidth = 2.0f;
constexpr float kPlotHairWidth = 1.0f;

// The tooltip: rows capped so a 30-signal legend does not build a
// window taller than the frame; the overflow rides one "+N more" row.
constexpr std::size_t kPlotTooltipMaxRows = 8;

[[nodiscard]] bool strictly_increasing(const double* xs, int n) {
    for (int i = 1; i < n; ++i) {
        if (!(xs[i - 1] < xs[i])) {
            return false;
        }
    }
    return true;
}

// The hover sample: the index whose x is nearest the cursor. A sorted
// axis binary-searches; anything else scans (the only unsorted cases
// are a wrapped .dc axis or a hand-built test axis).
[[nodiscard]] int nearest_sample(const double* xs, double x, bool sorted,
                                 int n) {
    if (n <= 0) {
        return -1;
    }
    if (sorted && n >= 2) {
        int lo = 0;
        int hi = n - 1;
        while (hi - lo > 1) {
            const int mid = lo + (hi - lo) / 2;
            if (xs[mid] <= x) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        return std::fabs(xs[lo] - x) <= std::fabs(xs[hi] - x) ? lo : hi;
    }
    int best = 0;
    double best_d = std::numeric_limits<double>::infinity();
    for (int i = 0; i < n; ++i) {
        const double d = std::fabs(xs[i] - x);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}
}  // namespace

// --- the pure axis math (public - the header carries the contracts) -----------

PlotTicks plot_ticks(double lo, double hi, unsigned target) {
    PlotTicks out;
    if (std::isnan(lo) || std::isnan(hi) || lo > hi) {
        return out;
    }
    if (!(lo < hi)) {
        // Degenerate: one honest tick (the !(<) spelling also catches
        // the -inf == -inf corner after the isnan guard).
        out.ticks[0] = lo;
        out.count = 1;
        return out;
    }
    if (target == 0) {
        target = 1;
    }
    const double span = hi - lo;
    if (std::isinf(span)) {
        return out;  // an infinite span cannot walk a ladder
    }

    // The 1/2/5 ladder: the raw step normalized to its decade, snapped
    // down to the nice rung at or under it.
    const double raw = span / static_cast<double>(target);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double norm = raw / mag;
    double step = 10.0;
    if (norm < 1.5) {
        step = 1.0;
    } else if (norm < 3.0) {
        step = 2.0;
    } else if (norm < 7.0) {
        step = 5.0;
    }
    step *= mag;

    // First tick at or above lo (ceil on the grid), then every step
    // until past hi; k*step from a fixed k0 keeps the ticks exact
    // multiples of the step, no accumulation, and the epsilon guard
    // keeps a tick that IS an endpoint from falling to representable
    // noise. The fixed 32-slot budget bounds the walk by construction
    // (span/step is at most ~1.4 x target), and the guard makes a
    // pathological target the caller's bug, not an overflow.
    const double eps = step * 1e-9;
    const double k0 = std::ceil((lo - eps) / step);
    const double k1 = std::floor((hi + eps) / step);
    if (k1 < k0) {
        return out;
    }
    for (double k = k0; k <= k1; ++k) {
        if (out.count >= out.ticks.size()) {
            break;
        }
        double t = k * step;
        if (t == 0.0) {
            t = 0.0;  // normalize -0.0 so labels print "0", not "-0"
        }
        out.ticks[out.count++] = t;
    }
    return out;
}

namespace {

// 10^e without std::pow's round-trip surprises on exact decades: the
// table covers every exponent the prefix walk visits, so the multiply
// stays a single rounding (the circsim spelling, kept byte-for-byte -
// the two libraries' answers must agree while both exist).
[[nodiscard]] double pow10(int e) {
    static constexpr double kTable[] = {
        1e-15, 1e-12, 1e-9, 1e-6, 1e-3, 1e0,  1e3, 1e6,
        1e9,   1e12,  1e15,
    };
    constexpr int kBase = -15;
    constexpr int kStep = 3;
    const int at = e - kBase;
    if (at < 0 || at % kStep != 0) {
        return std::pow(10.0, static_cast<double>(e));
    }
    const int slot = at / kStep;
    if (slot < 0 || slot >= static_cast<int>(sizeof kTable /
                                              sizeof kTable[0])) {
        return std::pow(10.0, static_cast<double>(e));
    }
    return kTable[static_cast<std::size_t>(slot)];
}

// The mantissa spelling: plain %g at the digit budget, into the
// caller's buffer. Locale-independent in practice (the gui never calls
// setlocale; %g's decimal point is the C locale's "." while it stands).
void fmt_g(char* buf, std::size_t cap, double v, int digits) {
    std::snprintf(buf, cap, "%.*g", digits, v);
}

}  // namespace

const char* plot_format_eng(char* buf, std::size_t cap, double v,
                            int digits) {
    if (cap == 0u) {
        return buf;
    }
    if (std::isnan(v)) {
        const char* s = "nan";
        std::size_t n = std::min(cap - 1, std::char_traits<char>::length(s));
        std::copy_n(s, n, buf);
        buf[n] = '\0';
        return buf;
    }
    if (std::isinf(v)) {
        const char* s = v > 0.0 ? "inf" : "-inf";
        std::size_t n = std::min(cap - 1, std::char_traits<char>::length(s));
        std::copy_n(s, n, buf);
        buf[n] = '\0';
        return buf;
    }
    if (v == 0.0) {
        const char* s = "0";
        std::size_t n = std::min(cap - 1, std::char_traits<char>::length(s));
        std::copy_n(s, n, buf);
        buf[n] = '\0';
        return buf;
    }
    digits = std::clamp(digits, 1, 6);

    // The prefix table: femto through tera, three decades per rung,
    // 0 = no prefix (the unit rung). Beyond the table both ways, plain
    // %g keeps the magnitude honest ("1e+18") where a clamped prefix
    // ("1000T") would already be a rounding story.
    static constexpr char kPrefixes[] = {'f', 'p', 'n', 'u',
                                         'm', 0,   'k', 'M', 'G', 'T'};
    constexpr int kFirstExp = -15;  // femto
    constexpr int kLastExp = 12;    // tera
    constexpr int kStep = 3;

    const bool neg = v < 0.0;
    double av = neg ? -v : v;
    if (!(av >= 1e-15) || !(av < 1e15)) {
        fmt_g(buf, cap, v, digits);
        return buf;
    }

    // The initial bucket, clamped to the table before any
    // renormalization walks it upward.
    int e = static_cast<int>(std::floor(std::log10(av) / kStep) * kStep);
    e = std::clamp(e, kFirstExp, kLastExp);
    for (int pass = 0; pass < 2; ++pass) {
        char mant[32];
        fmt_g(mant, sizeof mant, av / pow10(e), digits);
        // The round-trip check: a mantissa that printed at 1000 or
        // beyond ("999.9" at 4 digits) rounds up through its decade,
        // so the BUCKET renormalizes (e += 3) rather than the label
        // printing 1000m. from_chars is the locale-deaf parse.
        double printed = 1000.0;
        (void)std::from_chars(mant, mant + std::strlen(mant), printed);
        if (printed < 1000.0) {
            const int slot = (e - kFirstExp) / kStep;
            const char prefix = kPrefixes[static_cast<std::size_t>(slot)];
            const char* sign = (neg && printed != 0.0) ? "-" : "";
            if (prefix == 0) {
                std::snprintf(buf, cap, "%s%s", sign, mant);
            } else {
                std::snprintf(buf, cap, "%s%s%c", sign, mant, prefix);
            }
            return buf;
        }
        e += kStep;
        if (e > kLastExp) {
            fmt_g(buf, cap, v, digits);  // rounded up past tera
            return buf;
        }
    }
    // Unreachable for in-table inputs (the renormalized value
    // re-buckets strictly below the ladder top); the honest raw tail.
    fmt_g(buf, cap, v, digits);
    return buf;
}

PlotAxisRange plot_expand_range(double lo, double hi, double frac) {
    if (std::isnan(lo) || std::isnan(hi) || std::isinf(lo) ||
        std::isinf(hi)) {
        return {0.0, 1.0};
    }
    if (frac < 0.0 || std::isnan(frac)) {
        frac = 0.0;
    }
    if (!(lo < hi)) {
        // Degenerate: pad by frac of the magnitude floor so a constant
        // line still gets a visible axis.
        const double d = std::max(std::fabs(lo), 1.0) * frac;
        return {lo - d, lo + d};
    }
    const double pad = (hi - lo) * frac;
    return {lo - pad, hi + pad};
}

PlotAxisRange plot_clamp_view(PlotAxisRange view, PlotAxisRange home) {
    if (std::isnan(home.lo) || std::isnan(home.hi) || std::isinf(home.lo) ||
        std::isinf(home.hi) || !(home.lo < home.hi)) {
        return home;  // nothing sane to clamp against
    }
    if (std::isnan(view.lo) || std::isnan(view.hi) || std::isinf(view.lo) ||
        std::isinf(view.hi)) {
        return home;
    }
    // Degenerate, or a window at or past home's span (it cannot slide
    // inside; spilling both ways is the same case): the home fit wins.
    if (!(view.lo < view.hi) ||
        (view.hi - view.lo) >= (home.hi - home.lo)) {
        return home;
    }
    // Both ends inside and narrower than home: slide the minimal
    // distance. At most one shift fires (the span fits), but run both
    // so an already-clamped caller stays clamped.
    if (view.lo < home.lo) {
        const double d = home.lo - view.lo;
        view.lo += d;
        view.hi += d;
    }
    if (view.hi > home.hi) {
        const double d = view.hi - home.hi;
        view.lo -= d;
        view.hi -= d;
    }
    return view;
}

PlotAxisRange plot_zoom_range(PlotAxisRange view, PlotAxisRange home,
                              double anchor, double factor) {
    if (!(factor > 0.0) || std::isnan(factor) || factor == 1.0) {
        return view;
    }
    if (std::isnan(view.lo) || std::isnan(view.hi) || std::isinf(view.lo) ||
        std::isinf(view.hi) || !(view.lo < view.hi)) {
        return view;  // degenerate/non-finite: no direction to grow in
    }
    if (std::isnan(anchor)) {
        anchor = view.lo;
    }
    anchor = std::clamp(anchor, view.lo, view.hi);
    const double k = 1.0 / factor;
    PlotAxisRange out{anchor + (view.lo - anchor) * k,
                      anchor + (view.hi - anchor) * k};
    // The float-mush floor: a window narrower than 1e-6 of home's span
    // prints degenerate tick ladders and loses the data to rounding.
    if (std::isnan(out.lo) || std::isnan(out.hi) || std::isinf(out.lo) ||
        std::isinf(out.hi) || !(out.lo < out.hi) ||
        (out.hi - out.lo) < (home.hi - home.lo) * 1e-6) {
        const double span = (home.hi - home.lo) * 1e-6;
        out = {anchor - span * 0.5, anchor + span * 0.5};
    }
    return plot_clamp_view(out, home);
}

PlotAxisRange plot_pan_range(PlotAxisRange view, PlotAxisRange home,
                             double delta) {
    if (std::isnan(delta) || std::isinf(delta)) {
        return view;
    }
    return plot_clamp_view({view.lo + delta, view.hi + delta}, home);
}

// --- the behavior half ---------------------------------------------------------

PlotResult plot_behavior(GuiContext& gui, const Rect& rect,
                         const double* xs, const PlotSeries* series,
                         int series_count, int sample_count,
                         PlotFrame& frame, PlotState& state) {
    const ws::input::InputFrame& in = gui.input();
    PlotResult result;

    // The geometry: the margins decide themselves (a visible right
    // spine widens the right one to its own label strip), then the
    // guards - an empty plot or a frame too narrow resolves sane-empty
    // views, claims nothing, hovers nothing.
    frame = PlotFrame{};
    frame.rect = rect;
    frame.by_index = xs == nullptr;
    bool has_right = false;
    for (int i = 0; i < series_count; ++i) {
        if (series[i].visible && series[i].spine == PlotSpine::right) {
            has_right = true;
            break;
        }
    }
    frame.has_right = has_right;
    frame.margin_right = has_right ? kPlotMarginRightAxis : kPlotMarginRight;
    frame.fx = rect.x + kPlotMarginLeft;
    frame.fy = rect.y + kPlotMarginTop;
    frame.iw = rect.width - kPlotMarginLeft - frame.margin_right;
    frame.ih = rect.height - kPlotMarginTop - kPlotMarginBottom;
    frame.cursor_x = in.mouse_x;
    frame.cursor_y = in.mouse_y;
    if (series_count <= 0 || series == nullptr || sample_count <= 0 ||
        frame.iw < kPlotMinInnerW || frame.ih < kPlotMinInnerH) {
        frame.view_x = frame.view_y = frame.view_y1 = {0.0, 1.0};
        return result;
    }
    const int n = sample_count;

    // The x extent: the shared axis's own run, or the index when the
    // caller passed none (the table-without-a-sweep-axis shape).
    double x_lo = 0.0;
    double x_hi = static_cast<double>(n - 1);
    if (!frame.by_index) {
        x_lo = xs[0];
        x_hi = xs[n - 1];
    }
    // The axis's sortedness: a per-frame walk (the decimation and the
    // binary hover both gate on it). The mapping walk below spends the
    // same O(n) anyway, so the check rides along; a NaN anywhere in
    // the axis fails the chain and lands in the scan paths, which are
    // the honest fallbacks.
    frame.x_sorted = frame.by_index || strictly_increasing(xs, n);

    // The y fits, one per spine over its OWN visible company (the
    // two-axis split: the volts left, the amps right, never one
    // flattened scale), finite samples only - a NaN column must not
    // poison the fit - with the empty-but-sane {0, 1} for a spine
    // nothing visible rides.
    double y_lo = std::numeric_limits<double>::infinity();
    double y_hi = -std::numeric_limits<double>::infinity();
    double y1_lo = y_lo;
    double y1_hi = y_hi;
    for (int i = 0; i < series_count; ++i) {
        if (!series[i].visible || series[i].ys == nullptr) {
            continue;
        }
        const bool right = series[i].spine == PlotSpine::right;
        for (int k = 0; k < n; ++k) {
            const double v = series[i].ys[k];
            if (!std::isfinite(v)) {
                continue;
            }
            if (right) {
                y1_lo = std::min(y1_lo, v);
                y1_hi = std::max(y1_hi, v);
            } else {
                y_lo = std::min(y_lo, v);
                y_hi = std::max(y_hi, v);
            }
        }
    }
    if (y_lo > y_hi) {
        y_lo = 0.0;
        y_hi = 1.0;
    }
    if (y1_lo > y1_hi) {
        y1_lo = 0.0;
        y1_hi = 1.0;
    }

    // Home: the padded auto fit, the outermost view a gesture can ever
    // leave. The effective view: the manual window clamped inside it,
    // or home itself. The right spine additionally refuses a degenerate
    // clamp (a zero span can survive when the axis membership changed
    // under a stored view - a zero-span spine cannot map a pixel).
    const PlotAxisRange home_x = plot_expand_range(x_lo, x_hi);
    const PlotAxisRange home_y = plot_expand_range(y_lo, y_hi);
    const PlotAxisRange home_y1 = plot_expand_range(y1_lo, y1_hi);
    frame.view_x =
        state.view_active ? plot_clamp_view(state.x, home_x) : home_x;
    frame.view_y =
        state.view_active ? plot_clamp_view(state.y, home_y) : home_y;
    frame.view_y1 = home_y1;
    if (has_right && state.view_active) {
        const PlotAxisRange c = plot_clamp_view(state.y1, home_y1);
        if (c.hi > c.lo) {
            frame.view_y1 = c;
        }
    }

    // The frame's opening windows - what every gesture below found.
    // view_changed compares against these at the frame's end: a
    // gesture that leaves the windows where it found them (a
    // zero-distance pan arm, a zoom that settles where it started)
    // is activity, not a move, and the answer stays honest.
    const PlotAxisRange was_x = frame.view_x;
    const PlotAxisRange was_y = frame.view_y;
    const PlotAxisRange was_y1 = frame.view_y1;

    // The claimable rect: frame + every strip, so the wheel works over
    // the axes too. The key is the state's address - stable for the
    // side-table entry's lifetime, the ScrollState precedent.
    const Rect claimable{
        frame.fx - kPlotMarginLeft, frame.fy - kPlotMarginTop,
        frame.iw + kPlotMarginLeft + frame.margin_right,
        frame.ih + kPlotMarginTop + kPlotMarginBottom};
    gui.register_wheel_claim(claimable, &state);

    // The pointer zones: the frame, and the three strips (the x strip
    // below, the left spine's left, the right spine's right - the
    // strips are the axis-drag zooms' handles).
    const bool in_frame =
        in.mouse_x >= frame.fx && in.mouse_x < frame.fx + frame.iw &&
        in.mouse_y >= frame.fy && in.mouse_y < frame.fy + frame.ih;
    const bool on_x_axis =
        in.mouse_x >= frame.fx && in.mouse_x < frame.fx + frame.iw &&
        in.mouse_y >= frame.fy + frame.ih &&
        in.mouse_y < frame.fy + frame.ih + kPlotMarginBottom;
    const bool on_y_axis =
        in.mouse_x >= frame.fx - kPlotMarginLeft && in.mouse_x < frame.fx &&
        in.mouse_y >= frame.fy && in.mouse_y < frame.fy + frame.ih;
    const bool on_y1_axis =
        has_right && in.mouse_x >= frame.fx + frame.iw &&
        in.mouse_x < frame.fx + frame.iw + frame.margin_right &&
        in.mouse_y >= frame.fy && in.mouse_y < frame.fy + frame.ih;
    const bool over = in_frame || on_x_axis || on_y_axis || on_y1_axis;

    // The double-click clock: runs while the pointer is over the plot,
    // resets on every press there, dies when it leaves - the "two
    // presses, same spot, soon" detector the reset rides.
    state.click_clock =
        over ? state.click_clock + in.dt_seconds : 100.0f;

    // A raised modal veil freezes the plot: no gesture, no hover (the
    // canvas's rule; the wheel claim under a veil never resolves won
    // either, so nothing settles through).
    if (!gui.claims_open()) {
        return result;
    }

    // A gesture frame owns the arming block below (the hand-roll's
    // early returns, spelled as one flag so the hover - which the
    // hand-roll resolves in its draw, after the gestures - still lands
    // on gesture frames here).
    bool pointer_owned = false;

    // The right press cancels the in-flight gesture, restoring the
    // press-time view exactly. The frame's windows snap back - a real
    // move this frame, which the end-of-frame comparison reports.
    if (state.kind != PlotGestureKind::none &&
        in.mouse_pressed[ws::input::mouse::kRight] != 0) {
        state.view_active = state.was_active;
        if (state.was_active) {
            state.x = state.gx;
            state.y = state.gy;
            state.y1 = state.gy1;
            frame.view_x = plot_clamp_view(state.x, home_x);
            frame.view_y = plot_clamp_view(state.y, home_y);
            frame.view_y1 =
                has_right ? plot_clamp_view(state.y1, home_y1) : home_y1;
        } else {
            frame.view_x = home_x;
            frame.view_y = home_y;
            frame.view_y1 = home_y1;
        }
        state.kind = PlotGestureKind::none;
        pointer_owned = true;
    }

    // The wheel: zoom x toward the cursor and each y spine toward its
    // own mapped anchor (the same screen point, mapped per spine) -
    // but never mid-gesture (a band's press point is captured in the
    // view it started in) and only once the claim has settled.
    if (in_frame && in.scroll_dy != 0.0f &&
        state.kind == PlotGestureKind::none && gui.wheel_won(&state)) {
        if (!state.view_active) {
            state.view_active = true;
            state.x = frame.view_x;
            state.y = frame.view_y;
            state.y1 = frame.view_y1;
        }
        const double wxa = frame.wx(in.mouse_x);
        const double wya = frame.wy(in.mouse_y);
        const double wy1a = frame.wy1(in.mouse_y);
        const double f = in.scroll_dy > 0.0f ? kPlotWheelFactor
                                             : 1.0 / kPlotWheelFactor;
        state.x = plot_zoom_range(state.x, home_x, wxa, f);
        state.y = plot_zoom_range(state.y, home_y, wya, f);
        state.y1 = plot_zoom_range(state.y1, home_y1, wy1a, f);
        frame.view_x = state.x;
        frame.view_y = state.y;
        frame.view_y1 = has_right ? state.y1 : frame.view_y1;
    }

    // The pan: middle-drag, or Space+left-drag. Content follows the
    // cursor - dragging right slides the window left, dragging down
    // slides it up (the y flip) - and the same pixel shift maps through
    // each spine's own span, so the two y windows slide by different
    // data amounts and stay in step on screen.
    const bool space_down = in.key_down[ws::input::keys::kSpace] != 0;
    const bool pan_held =
        in.mouse_down[ws::input::mouse::kMiddle] != 0 ||
        (space_down && in.mouse_down[ws::input::mouse::kLeft] != 0);
    if (pan_held && (in_frame || state.kind == PlotGestureKind::pan)) {
        if (state.kind != PlotGestureKind::pan) {
            state.kind = PlotGestureKind::pan;
            state.from_x = in.mouse_x;
            state.from_y = in.mouse_y;
            state.gx = frame.view_x;
            state.gy = frame.view_y;
            state.gy1 = frame.view_y1;
            state.was_active = state.view_active;
            if (!state.view_active) {
                state.view_active = true;
                state.x = frame.view_x;
                state.y = frame.view_y;
                state.y1 = frame.view_y1;
            }
        }
        const double dx = -(static_cast<double>(in.mouse_x - state.from_x) /
                            static_cast<double>(frame.iw)) *
                          (state.gx.hi - state.gx.lo);
        const double frac =
            static_cast<double>(in.mouse_y - state.from_y) /
            static_cast<double>(frame.ih);
        state.x = plot_pan_range(state.gx, home_x, dx);
        state.y =
            plot_pan_range(state.gy, home_y, frac * (state.gy.hi - state.gy.lo));
        state.y1 = plot_pan_range(state.gy1, home_y1,
                                  frac * (state.gy1.hi - state.gy1.lo));
        frame.view_x = state.x;
        frame.view_y = state.y;
        frame.view_y1 = has_right ? state.y1 : frame.view_y1;
        pointer_owned = true;  // a pan frame owns the pointer
    } else if (state.kind == PlotGestureKind::pan) {
        state.kind = PlotGestureKind::none;
    }

    // The left-button gestures run to release wherever the pointer
    // goes (the press owns it): the band commits its box on release,
    // the axis zooms land live as the drag moves.
    if (state.kind == PlotGestureKind::band ||
        state.kind == PlotGestureKind::zoom_x ||
        state.kind == PlotGestureKind::zoom_y) {
        if (in.mouse_down[ws::input::mouse::kLeft] == 0) {
            const bool was_band = state.kind == PlotGestureKind::band;
            if (was_band &&
                std::fabs(in.mouse_x - state.from_x) >= kPlotBandMinPx &&
                std::fabs(in.mouse_y - state.from_y) >= kPlotBandMinPx) {
                // The box's corners map through each spine's OWN
                // transform: one screen band, three windows, each
                // clamped into its own home.
                const double x0 = frame.wx(state.from_x);
                const double x1 = frame.wx(in.mouse_x);
                const double y0 = frame.wy(state.from_y);
                const double y1 = frame.wy(in.mouse_y);
                const double y10 = frame.wy1(state.from_y);
                const double y11 = frame.wy1(in.mouse_y);
                state.view_active = true;
                state.x = plot_clamp_view(
                    {std::min(x0, x1), std::max(x0, x1)}, home_x);
                state.y = plot_clamp_view(
                    {std::min(y0, y1), std::max(y0, y1)}, home_y);
                state.y1 = plot_clamp_view(
                    {std::min(y10, y11), std::max(y10, y11)}, home_y1);
                frame.view_x = state.x;
                frame.view_y = state.y;
                frame.view_y1 = has_right ? state.y1 : frame.view_y1;
            }
            state.kind = PlotGestureKind::none;
            pointer_owned = true;  // the release frame, whatever it did
        } else {
            if (state.kind == PlotGestureKind::zoom_x) {
                // Dragging right stretches the axis: zoom in, pivoting
                // on the press-time anchor.
                const double f =
                    std::exp((static_cast<double>(in.mouse_x) -
                              state.from_x) * kPlotStripExp);
                state.x = plot_zoom_range(state.gx, home_x, state.anchor_x, f);
                state.view_active = true;
                frame.view_x = state.x;
            } else if (state.kind == PlotGestureKind::zoom_y &&
                       state.zoom_axis == 0) {
                // Dragging up on the left strip zooms the window in
                // (pulling the range open reads like pulling the axis
                // up).
                const double f =
                    std::exp((state.from_y - static_cast<double>(in.mouse_y)) *
                             kPlotStripExp);
                state.y = plot_zoom_range(state.gy, home_y, state.anchor_y, f);
                state.view_active = true;
                frame.view_y = state.y;
            } else if (state.kind == PlotGestureKind::zoom_y) {
                // The right strip's drag: the amp window's own zoom,
                // same gesture language, its own anchor.
                const double f =
                    std::exp((state.from_y - static_cast<double>(in.mouse_y)) *
                             kPlotStripExp);
                state.y1 =
                    plot_zoom_range(state.gy1, home_y1, state.anchor_y1, f);
                state.view_active = true;
                if (has_right) {
                    frame.view_y1 = state.y1;
                }
            }
            pointer_owned = true;  // a gesture frame owns the pointer
        }
    }

    // Arming: the second press of a pair inside the frame resets the
    // view; the first click starts the band (frame) or an axis zoom
    // (strips - the left one owns the left window, the right one the
    // right window).
    if (!pointer_owned && in.mouse_pressed[ws::input::mouse::kLeft] != 0 &&
        over) {
        if (in_frame && state.click_clock < kPlotDoubleClickSecs &&
            std::fabs(in.mouse_x - state.click_x) < kPlotDoubleClickPx &&
            std::fabs(in.mouse_y - state.click_y) < kPlotDoubleClickPx) {
            state.view_active = false;
            state.kind = PlotGestureKind::none;
            frame.view_x = home_x;
            frame.view_y = home_y;
            frame.view_y1 = home_y1;
            state.click_clock = 100.0f;
            result.view_reset = true;
            return result;
        }
        state.click_clock = 0.0f;
        state.click_x = in.mouse_x;
        state.click_y = in.mouse_y;
        state.from_x = in.mouse_x;
        state.from_y = in.mouse_y;
        state.gx = frame.view_x;
        state.gy = frame.view_y;
        state.gy1 = frame.view_y1;
        state.was_active = state.view_active;
        // The strip zooms write one axis over the others' landed
        // windows: seed the manual view with the effective one (the
        // wheel and the pan seed the same way). A band arms bare - it
        // commits its box whole at release.
        const auto seed = [&] {
            if (!state.view_active) {
                state.view_active = true;
                state.x = frame.view_x;
                state.y = frame.view_y;
                state.y1 = frame.view_y1;
            }
        };
        if (on_x_axis) {
            state.kind = PlotGestureKind::zoom_x;
            state.anchor_x = frame.wx(in.mouse_x);
            seed();
        } else if (on_y_axis) {
            state.kind = PlotGestureKind::zoom_y;
            state.zoom_axis = 0;
            state.anchor_y = frame.wy(in.mouse_y);
            seed();
        } else if (on_y1_axis) {
            state.kind = PlotGestureKind::zoom_y;
            state.zoom_axis = 1;
            state.anchor_y1 = frame.wy1(in.mouse_y);
            seed();
        } else if (in_frame) {
            state.kind = PlotGestureKind::band;  // the box lands at release
        }
    }

    // The hover: the nearest sample to the cursor, through the CURRENT
    // effective view (a gesture that just moved the window re-aims the
    // hover with it - the hand-roll resolves hover after the input pass
    // for exactly this reason).
    if (in_frame) {
        result.hovered = true;
        const double wxa = frame.wx(in.mouse_x);
        if (frame.by_index) {
            // The axis IS the index: round and clamp, no scan.
            result.hover = static_cast<int>(std::clamp<long long>(
                std::llround(wxa), 0, static_cast<long long>(n) - 1));
            result.hover_x = static_cast<double>(result.hover);
        } else {
            result.hover = nearest_sample(xs, wxa, frame.x_sorted, n);
            result.hover_x =
                result.hover >= 0 ? xs[result.hover] : 0.0;
        }
    }

    // The frame's view news, resolved the one honest way: the windows
    // the gestures left against the ones they found. A reset answers
    // view_reset alone (disjoint by design); a cancel's snap-back and
    // every real move answer view_changed.
    result.view_changed =
        frame.view_x.lo != was_x.lo || frame.view_x.hi != was_x.hi ||
        frame.view_y.lo != was_y.lo || frame.view_y.hi != was_y.hi ||
        frame.view_y1.lo != was_y1.lo || frame.view_y1.hi != was_y1.hi;
    return result;
}

// --- the draw half -------------------------------------------------------------

namespace {

// One series' polyline, streamed: maps kept samples to frame pixels
// and strokes the segments straight through r2d (no intermediate
// point vector, so nothing allocates and nothing is walked twice).
// Non-finite samples break the line into runs rather than poisoning a
// vertex with NaN.
//
// The kept set: every sample when the window is short, or the min/max
// decimation when a long sweep meets a narrow frame - each bucket of
// the x span contributes its y-minimum and y-maximum sample in
// occurrence order (so spikes survive: the bucket's extremes ARE the
// pixels that matter), plus the window's first and last. The budget is
// ~2 vertices per pixel of frame width, the scope's honest ceiling.
class SeriesWalker {
public:
    SeriesWalker(const PlotFrame& f, const double* xs, const double* ys,
                 int n, PlotSpine spine, std::uint32_t color)
        : f_(f), xs_(xs), ys_(ys), n_(n), spine_(spine), color_(color) {}

    void run() {
        // The x window: the visible samples plus one of slack each
        // side, so a zoomed-in view spends the pixel budget on the
        // samples it shows (the auto view's padded home spans all the
        // data, so the slice is transparent there - one code path, no
        // special case).
        int i0 = 0;
        int i1 = n_;
        if (f_.x_sorted) {
            if (f_.by_index) {
                i0 = static_cast<int>(std::clamp<long long>(
                    std::llround(std::floor(f_.view_x.lo)) - 1, 0,
                    static_cast<long long>(n_)));
                i1 = static_cast<int>(std::clamp<long long>(
                    std::llround(std::ceil(f_.view_x.hi)) + 1, 0,
                    static_cast<long long>(n_)));
            } else {
                const int rows = n_;
                i0 = static_cast<int>(
                    std::lower_bound(xs_, xs_ + rows, f_.view_x.lo) - xs_);
                i1 = static_cast<int>(
                    std::upper_bound(xs_, xs_ + rows, f_.view_x.hi) - xs_);
                if (i0 > 0) {
                    --i0;  // one sample of slack each side
                }
                if (i1 < rows) {
                    ++i1;
                }
            }
        }
        if (i1 - i0 < 2) {
            return;  // nothing drawable (a single sample has no segment)
        }

        const std::size_t budget =
            std::max<std::size_t>(64u, static_cast<std::size_t>(f_.iw));
        const std::size_t window = static_cast<std::size_t>(i1 - i0);
        if (!f_.x_sorted || window <= budget) {
            for (int k = i0; k < i1; ++k) {
                emit(k);
            }
            return;
        }
        decimated(i0, i1, budget);
    }

private:
    double x_at(int k) const {
        return f_.by_index ? static_cast<double>(k) : xs_[k];
    }
    float sy_of(double v) const {
        return spine_ == PlotSpine::right ? f_.sy1(v) : f_.sy(v);
    }

    // Emits one kept sample: maps it, strokes from the previous kept
    // sample when both ends are finite, and remembers the run state.
    void emit(int k) {
        const double yv = ys_[k];
        const float px = f_.sx(x_at(k));
        const float py = std::isfinite(yv) ? sy_of(yv)
                                           : std::numeric_limits<float>::
                                                 quiet_NaN();
        if (prev_ok_ && k != prev_k_ && px == px && py == py) {
            r2d::draw_line(prev_x_, prev_y_, px, py, kPlotSeriesWidth,
                           color_);
        }
        prev_ok_ = px == px && py == py;  // NaN != NaN: the break test
        prev_x_ = px;
        prev_y_ = py;
        prev_k_ = k;
    }

    // The decimated walk: buckets over the window's x span, each
    // contributing its extreme samples in occurrence order, the
    // window's first and last forced in (edge timing is load-bearing
    // for a waveform). The bucket boundaries are exact cuts of the
    // span, so a sample ON a boundary belongs to the earlier bucket
    // (the < below) and the tail rides the last bucket whole.
    void decimated(int i0, int i1, std::size_t budget) {
        emit(i0);
        const std::size_t buckets = std::max<std::size_t>(1, budget / 2u);
        const double x_lo = x_at(i0);
        const double x_hi = x_at(i1 - 1);
        const double bw = (x_hi - x_lo) / static_cast<double>(buckets);
        int k = i0 + 1;
        for (std::size_t b = 0; b < buckets && k < i1 - 1; ++b) {
            const double b_hi =
                (b + 1u == buckets) ? x_hi : x_lo + static_cast<double>(b + 1u) * bw;
            int min_k = -1;
            int max_k = -1;
            double min_v = 0.0;
            double max_v = 0.0;
            while (k < i1 - 1 && x_at(k) < b_hi) {
                const double v = ys_[k];
                if (std::isfinite(v)) {
                    if (min_k < 0 || v < min_v) {
                        min_k = k;
                        min_v = v;
                    }
                    if (max_k < 0 || v > max_v) {
                        max_k = k;
                        max_v = v;
                    }
                }
                ++k;
            }
            if (min_k < 0) {
                continue;  // a bucket of non-finite samples only
            }
            if (min_k == max_k) {
                emit(min_k);
            } else if (min_k < max_k) {
                emit(min_k);
                emit(max_k);
            } else {
                emit(max_k);
                emit(min_k);
            }
        }
        emit(i1 - 1);
    }

    const PlotFrame& f_;
    const double* xs_;
    const double* ys_;
    int n_;
    PlotSpine spine_;
    std::uint32_t color_;
    float prev_x_ = 0.0f;
    float prev_y_ = 0.0f;
    int prev_k_ = -1;
    bool prev_ok_ = false;
};

// One tooltip row, formatted: the swatch color (0 = no swatch - the x
// header and the overflow row) and the row's text, composed here so
// the draw below only lays them out. The value rides engineering
// notation plus the series' unit when it carries one.
struct TooltipRow {
    std::uint32_t color = 0;
    char text[96] = "";
};

}  // namespace

void plot_draw(const Rect& rect, const PlotFrame& frame,
               const PlotSeries* series, int series_count,
               int sample_count, const double* xs,
               const char* x_name, const PlotResult& result,
               const PlotState& state, const ChromeTheme& theme) {
    (void)rect;  // the frame carries the geometry; the row rect rides
                 // the painter row's signature, like table_row_draw's
    if (frame.iw < kPlotMinInnerW || frame.ih < kPlotMinInnerH) {
        return;  // too narrow to be worth a frame
    }
    const float fx = frame.fx;
    const float fy = frame.fy;
    const float iw = frame.iw;
    const float ih = frame.ih;
    const bool can_label = theme.font != nullptr && !theme.font->empty();

    // The ticks, resolved once: the grid (inside the clip) and the
    // labels (outside it, in the margins) walk the same answer.
    const PlotTicks x_ticks = plot_ticks(frame.view_x.lo, frame.view_x.hi, 6u);
    const PlotTicks y_ticks = plot_ticks(frame.view_y.lo, frame.view_y.hi, 5u);

    // Everything that must stay inside the frame - grid, zero rail,
    // series, crosshair, markers - draws under one clip pair, so a
    // zoomed window's off-screen data never paints over the margins or
    // the caller's chrome beside the plot (the hand-rolled panel had
    // no such clip; this is the fix its overdraw owed).
    r2d::push_clip(fx, fy, iw, ih);

    // The grid: a line per tick that lands inside the frame.
    const std::uint32_t grid_color = dim(theme.text, 0.12f);
    for (double tick : x_ticks) {
        const float px = frame.sx(tick);
        if (px >= fx && px <= fx + iw) {
            r2d::draw_line(px, fy, px, fy + ih, kPlotHairWidth, grid_color);
        }
    }
    for (double tick : y_ticks) {
        const float py = frame.sy(tick);
        if (py >= fy && py <= fy + ih) {
            r2d::draw_line(fx, py, fx + iw, py, kPlotHairWidth, grid_color);
        }
    }
    // The zero rail: a brighter line where the left spine crosses 0,
    // so a signal's sign reads without counting ticks.
    if (frame.view_y.lo < 0.0 && frame.view_y.hi > 0.0) {
        r2d::draw_line(fx, frame.sy(0.0), fx + iw, frame.sy(0.0),
                       kPlotHairWidth, dim(theme.text, 0.32f));
    }

    // The series, each through its own spine.
    bool any_visible = false;
    for (int i = 0; i < series_count; ++i) {
        if (!series[i].visible || series[i].ys == nullptr) {
            continue;
        }
        any_visible = true;
        const std::uint32_t color =
            series[i].color != 0u
                ? series[i].color
                : kPlotSeriesColors[static_cast<std::size_t>(i) %
                                    kPlotSeriesColorCount];
        SeriesWalker(frame, xs, series[i].ys, sample_count,
                     series[i].spine, color)
            .run();
    }

    // The crosshair + per-series markers at the hover sample.
    if (result.hover >= 0) {
        const float px = frame.sx(result.hover_x);
        r2d::draw_line(px, fy, px, fy + ih, kPlotHairWidth,
                       dim(theme.text, 0.5f));
        for (int i = 0; i < series_count; ++i) {
            if (!series[i].visible || series[i].ys == nullptr) {
                continue;
            }
            const double yv = series[i].ys[result.hover];
            if (!std::isfinite(yv)) {
                continue;
            }
            const float py = series[i].spine == PlotSpine::right
                                 ? frame.sy1(yv)
                                 : frame.sy(yv);
            const std::uint32_t color =
                series[i].color != 0u
                    ? series[i].color
                    : kPlotSeriesColors[static_cast<std::size_t>(i) %
                                        kPlotSeriesColorCount];
            r2d::draw_rounded_quad(px - 3.0f, py - 3.0f, 6.0f, 6.0f, 3.0f,
                                   color);
        }
    }
    r2d::pop_clip();

    // The frame outline: four hairlines, the frame's own border.
    const std::uint32_t frame_color = dim(theme.text, 0.35f);
    r2d::draw_line(fx, fy, fx + iw, fy, kPlotHairWidth, frame_color);
    r2d::draw_line(fx + iw, fy, fx + iw, fy + ih, kPlotHairWidth, frame_color);
    r2d::draw_line(fx + iw, fy + ih, fx, fy + ih, kPlotHairWidth, frame_color);
    r2d::draw_line(fx, fy + ih, fx, fy, kPlotHairWidth, frame_color);

    // The tick labels, engineering notation: x centered under its
    // gridlines, the left spine right-aligned in its margin. Fontless
    // themes draw the geometry and no text - the can_label gate, the
    // hand-rolled panel's own rule.
    const std::uint32_t label_color = dim(theme.text, 0.6f);
    if (can_label) {
        const float lh = theme.font->line_height();
        for (double tick : x_ticks) {
            const float px = frame.sx(tick);
            if (px < fx || px > fx + iw) {
                continue;
            }
            char label[32];
            (void)plot_format_eng(label, sizeof label, tick);
            const float w = theme.font->measure_text(label);
            (void)r2d::draw_text(px - w * 0.5f, fy + ih + 2.0f, label,
                                 *theme.font, label_color);
        }
        for (double tick : y_ticks) {
            const float py = frame.sy(tick);
            if (py < fy || py > fy + ih) {
                continue;
            }
            char label[32];
            (void)plot_format_eng(label, sizeof label, tick);
            const float w = theme.font->measure_text(label);
            (void)r2d::draw_text(fx - w - 4.0f, py - lh * 0.5f, label,
                                 *theme.font, label_color);
        }
        // The right spine's ticks: marks on the frame's right edge and
        // labels in the widened margin - no grid lines (the left
        // axis's rows already carry the horizontal rails).
        if (frame.has_right) {
            const PlotTicks y1_ticks =
                plot_ticks(frame.view_y1.lo, frame.view_y1.hi, 5u);
            for (double tick : y1_ticks) {
                const float py = frame.sy1(tick);
                if (py < fy || py > fy + ih) {
                    continue;
                }
                r2d::draw_line(fx + iw, py, fx + iw + 3.0f, py,
                               kPlotHairWidth, grid_color);
                char label[32];
                (void)plot_format_eng(label, sizeof label, tick);
                (void)r2d::draw_text(fx + iw + 4.0f, py - lh * 0.5f, label,
                                     *theme.font, label_color);
            }
        }
    }

    // The x axis's name, quiet, inside the frame's top-left.
    if (can_label && x_name != nullptr) {
        (void)r2d::draw_text(fx + 5.0f, fy + 3.0f, x_name, *theme.font,
                             dim(theme.text, 0.45f));
    }

    // The empty states: no series at all, or every one hidden - say so
    // instead of showing a mystery frame.
    if (can_label && (series_count <= 0 || series == nullptr ||
                      !any_visible)) {
        const char* msg = theme_str(theme, series_count <= 0 || series == nullptr
                                             ? Str::no_data
                                             : Str::all_signals_hidden);
        const float w = theme.font->measure_text(msg);
        (void)r2d::draw_text(fx + (iw - w) * 0.5f,
                             fy + (ih - theme.font->line_height()) * 0.5f,
                             msg, *theme.font, dim(theme.text, 0.5f));
    }

    // The in-flight band: the accent's wash under a near-solid
    // outline, and the dx/dy readout of the box's data-space size (dx
    // on the sweep, dy on the LEFT spine - the primary scale). The
    // live corner is the cursor the behavior captured into the frame
    // - the draw reads no context, the whole point of the seam.
    if (state.kind == PlotGestureKind::band) {
        const float bx0 = std::min(state.from_x, frame.cursor_x);
        const float bx1 = std::max(state.from_x, frame.cursor_x);
        const float by0 = std::min(state.from_y, frame.cursor_y);
        const float by1 = std::max(state.from_y, frame.cursor_y);
        r2d::draw_quad(bx0, by0, bx1 - bx0, by1 - by0,
                       with_alpha(theme.control_on, 36));
        const std::uint32_t edge = with_alpha(theme.control_on, 200);
        r2d::draw_line(bx0, by0, bx1, by0, kPlotHairWidth, edge);
        r2d::draw_line(bx1, by0, bx1, by1, kPlotHairWidth, edge);
        r2d::draw_line(bx1, by1, bx0, by1, kPlotHairWidth, edge);
        r2d::draw_line(bx0, by1, bx0, by0, kPlotHairWidth, edge);
        if (can_label) {
            const double dx =
                std::fabs(frame.wx(frame.cursor_x) - frame.wx(state.from_x));
            const double dy =
                std::fabs(frame.wy(frame.cursor_y) - frame.wy(state.from_y));
            char dxs[32];
            char dys[32];
            (void)plot_format_eng(dxs, sizeof dxs, dx);
            (void)plot_format_eng(dys, sizeof dys, dy);
            char readout[96];
            std::snprintf(readout, sizeof readout, "dx %s  dy %s", dxs, dys);
            (void)r2d::draw_text(bx0 + 4.0f, by0 + 2.0f, readout,
                                 *theme.font, dim(theme.text, 0.75f));
        }
    } else if (state.view_active && can_label) {
        // The manual-view hint, quiet, inside the frame's top-right.
        const char* hint = theme_str(theme, Str::double_click_resets);
        const float w = theme.font->measure_text(hint);
        (void)r2d::draw_text(fx + iw - w - 5.0f, fy + 3.0f, hint,
                             *theme.font, dim(theme.text, 0.35f));
    }

    // The floating tooltip: x on top, then one row per visible series
    // (capped, "+N more" past the cap). It flips to the cursor's left
    // near the right edge and stays inside the frame; it hides while a
    // gesture flies (the band's readout and the tooltip would fight
    // for the same pixels).
    if (can_label && result.hover >= 0 && state.kind == PlotGestureKind::none) {
        TooltipRow rows[kPlotTooltipMaxRows + 2];
        std::size_t row_count = 0;
        std::size_t total = 0;  // the visible, finite series at the sample
        for (int i = 0; i < series_count; ++i) {
            if (series[i].visible && series[i].ys != nullptr &&
                std::isfinite(series[i].ys[result.hover])) {
                ++total;
            }
        }
        {
            TooltipRow& head = rows[row_count++];
            char val[32];
            (void)plot_format_eng(val, sizeof val, result.hover_x, 5);
            std::snprintf(head.text, sizeof head.text, "%s = %s",
                          x_name != nullptr ? x_name : "x", val);
        }
        std::size_t shown = 0;
        for (int i = 0; i < series_count && shown < kPlotTooltipMaxRows;
             ++i) {
            if (!series[i].visible || series[i].ys == nullptr ||
                !std::isfinite(series[i].ys[result.hover])) {
                continue;
            }
            TooltipRow& row = rows[row_count];
            char val[32];
            (void)plot_format_eng(val, sizeof val, series[i].ys[result.hover]);
            std::snprintf(row.text, sizeof row.text, "%s  %s%s",
                          series[i].label != nullptr ? series[i].label : "",
                          val,
                          series[i].unit != nullptr ? series[i].unit : "");
            row.color =
                series[i].color != 0u
                    ? series[i].color
                    : kPlotSeriesColors[static_cast<std::size_t>(i) %
                                        kPlotSeriesColorCount];
            ++row_count;
            ++shown;
        }
        if (total > shown) {
            TooltipRow& more = rows[row_count++];
            std::snprintf(more.text, sizeof more.text, "+%zu more",
                          total - shown);
        }

        const float lh = theme.font->line_height();
        float text_w = 0.0f;
        for (std::size_t r = 0; r < row_count; ++r) {
            text_w = std::max(text_w, theme.font->measure_text(rows[r].text));
        }
        const float bw = text_w + 26.0f;
        const float bh = lh * static_cast<float>(row_count) + 10.0f;
        const float px = frame.sx(result.hover_x);
        float bx = px + 12.0f;
        if (bx + bw > fx + iw) {
            bx = px - 12.0f - bw;
        }
        bx = std::max(bx, fx);
        float by = fy + 6.0f;
        by = std::max(fy, std::min(by, fy + ih - bh));
        // The floating chrome: the popup's own frame, so a skin's
        // window look and the plot's hint read as one family.
        popup_frame_draw({bx, by, bw, bh}, theme);
        for (std::size_t r = 0; r < row_count; ++r) {
            const float ty = by + 5.0f + lh * static_cast<float>(r);
            if (rows[r].color != 0u) {
                r2d::draw_rounded_quad(bx + 7.0f, ty + lh * 0.5f - 3.0f,
                                       6.0f, 6.0f, 3.0f, rows[r].color);
            }
            (void)r2d::draw_text(bx + 18.0f, ty, rows[r].text, *theme.font,
                                 rows[r].color != 0u
                                     ? theme.text
                                     : theme.text_secondary);
        }
    }
}

// --- the composed widget --------------------------------------------------------

PlotResult plot(GuiContext& gui, LayoutCursor& layout,
                const ChromeTheme& theme, const char* label,
                const double* xs, const PlotSeries* series,
                int series_count, int sample_count,
                const char* x_name, float height, float width) {
    resolve_width(width, layout);
    height = std::max(height, 0.0f);
    const WidgetId id = gui.ids().id(label);
    const Rect rect = layout.next(width, height);
    PlotState& state = gui.persistent_state<PlotState>(id);
    PlotFrame frame;
    const PlotResult result =
        plot_behavior(gui, rect, xs, series, series_count, sample_count,
                      frame, state);
    paint(theme, &WidgetPainters::plot, &plot_draw, rect, frame, series,
          series_count, sample_count, xs, x_name, result, state, theme);
    return result;
}

}  // namespace ws::ui::gui
