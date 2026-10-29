// Measurement library (§0.4 rev): every function works on the adaptive
// sample grid through linear interpolation, exactly the convention the
// ngspice ref harness compares with (§10.2). nullopt means "cannot be
// measured" - never a fabricated number.

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <vector>

#include <ws/spice/measure.hpp>

namespace ws::spice::measure {

namespace {

// Clamps the window to the axis range; returns [i0, i1) sample range plus
// the interpolated edge abscissas. nullopt when the window misses the
// axis entirely.
struct SampleRange {
    std::size_t i0 = 0;
    std::size_t i1 = 0;  // one past
    double a0 = 0.0;
    double a1 = 0.0;
};

std::optional<SampleRange> window_range(std::span<const double> axis,
                                        const Window& w) {
    if (axis.empty() || axis.size() < 2) {
        return std::nullopt;
    }
    double lo = w.whole ? axis.front() : w.t0;
    double hi = w.whole ? axis.back() : w.t1;
    if (lo > hi) {
        std::swap(lo, hi);
    }
    // Interpolated edges (clamped into the axis).
    lo = std::clamp(lo, axis.front(), axis.back());
    hi = std::clamp(hi, axis.front(), axis.back());
    SampleRange r;
    r.a0 = lo;
    r.a1 = hi;
    // First sample index with axis[i] >= lo.
    r.i0 = static_cast<std::size_t>(
        std::lower_bound(axis.begin(), axis.end(), lo) - axis.begin());
    // One past the last sample index with axis[i] <= hi.
    r.i1 = static_cast<std::size_t>(
        std::upper_bound(axis.begin(), axis.end(), hi) - axis.begin());
    if (r.i1 <= r.i0 || r.i1 > axis.size()) {
        return std::nullopt;
    }
    return r;
}

// Linear interpolation between (a0,v0) and (a1,v1) at a.
double interp(double a0, double v0, double a1, double v1, double a) {
    if (a1 == a0) {
        return v0;
    }
    return v0 + (v1 - v0) * (a - a0) / (a1 - a0);
}

}  // namespace

std::optional<double> value_at(std::span<const double> axis,
                               std::span<const double> sig, double t) {
    if (axis.size() != sig.size() || axis.size() < 2) {
        return std::nullopt;
    }
    if (t < axis.front() || t > axis.back()) {
        return std::nullopt;
    }
    const std::size_t i = static_cast<std::size_t>(
        std::upper_bound(axis.begin(), axis.end(), t) - axis.begin());
    if (i == 0) {
        return sig.front();
    }
    if (i >= axis.size()) {
        return sig.back();
    }
    return interp(axis[i - 1], sig[i - 1], axis[i], sig[i], t);
}

std::optional<double> cross_time_n(std::span<const double> axis,
                                   std::span<const double> sig, double level,
                                   Dir dir, int n, const Window& w) {
    if (n < 1) {
        return std::nullopt;
    }
    const auto range = window_range(axis, w);
    if (!range) {
        return std::nullopt;
    }
    // Walk consecutive samples (with interpolated window edges) counting
    // crossings in the requested direction.
    int seen = 0;
    double prev_a = range->a0;
    std::optional<double> prev_v = value_at(axis, sig, range->a0);
    if (!prev_v) {
        return std::nullopt;
    }
    for (std::size_t i = range->i0; i < range->i1; ++i) {
        const double a = std::min(axis[i], range->a1);
        const double v = sig[i];
        const bool rise = *prev_v < level && v >= level;
        const bool fall = *prev_v > level && v <= level;
        const bool hit = (dir == Dir::Rise && rise) || (dir == Dir::Fall && fall) ||
                         (dir == Dir::Either && (rise || fall));
        if (hit) {
            ++seen;
            if (seen == n) {
                // Invert the segment: where does it PASS level? (The
                // segment is non-degenerate in v by the crossing test.)
                if (v == *prev_v) {
                    return prev_a;
                }
                return prev_a + (a - prev_a) * (level - *prev_v) / (v - *prev_v);
            }
        }
        prev_a = a;
        prev_v = v;
        if (a >= range->a1) {
            break;
        }
    }
    return std::nullopt;
}

std::optional<double> cross_time(std::span<const double> axis,
                                 std::span<const double> sig, double level, Dir dir,
                                 const Window& w) {
    return cross_time_n(axis, sig, level, dir, 1, w);
}

std::optional<double> average(std::span<const double> axis,
                              std::span<const double> sig, const Window& w) {
    const auto range = window_range(axis, w);
    if (!range) {
        return std::nullopt;
    }
    // Trapezoidal integral / interval length, edges interpolated.
    double integ = 0.0;
    double prev_a = range->a0;
    double prev_v = value_at(axis, sig, range->a0).value_or(sig[range->i0]);
    for (std::size_t i = range->i0; i < range->i1; ++i) {
        const double a = std::min(axis[i], range->a1);
        const double v = sig[i];
        integ += 0.5 * (prev_v + v) * (a - prev_a);
        prev_a = a;
        prev_v = v;
        if (a >= range->a1) {
            break;
        }
    }
    const double span = range->a1 - range->a0;
    if (!(span > 0.0)) {
        return std::nullopt;
    }
    return integ / span;
}

std::optional<double> rms(std::span<const double> axis, std::span<const double> sig,
                          const Window& w) {
    const auto range = window_range(axis, w);
    if (!range) {
        return std::nullopt;
    }
    double integ = 0.0;
    double prev_a = range->a0;
    double prev_v = value_at(axis, sig, range->a0).value_or(sig[range->i0]);
    for (std::size_t i = range->i0; i < range->i1; ++i) {
        const double a = std::min(axis[i], range->a1);
        const double v = sig[i];
        integ += 0.5 * (prev_v * prev_v + v * v) * (a - prev_a);
        prev_a = a;
        prev_v = v;
        if (a >= range->a1) {
            break;
        }
    }
    const double span = range->a1 - range->a0;
    if (!(span > 0.0)) {
        return std::nullopt;
    }
    return std::sqrt(integ / span);
}

std::optional<double> maximum(std::span<const double> sig, const Window&) {
    if (sig.empty()) {
        return std::nullopt;
    }
    double best = sig.front();
    for (const double v : sig) {
        best = std::max(best, v);
    }
    return best;
}

std::optional<double> minimum(std::span<const double> sig, const Window&) {
    if (sig.empty()) {
        return std::nullopt;
    }
    double best = sig.front();
    for (const double v : sig) {
        best = std::min(best, v);
    }
    return best;
}

std::optional<double> peak_to_peak(std::span<const double> sig, const Window& w) {
    const auto hi = maximum(sig, w);
    const auto lo = minimum(sig, w);
    if (!hi || !lo) {
        return std::nullopt;
    }
    return *hi - *lo;
}

std::optional<double> integral(std::span<const double> axis,
                               std::span<const double> sig, const Window& w) {
    const auto range = window_range(axis, w);
    if (!range) {
        return std::nullopt;
    }
    double integ = 0.0;
    double prev_a = range->a0;
    double prev_v = value_at(axis, sig, range->a0).value_or(sig[range->i0]);
    for (std::size_t i = range->i0; i < range->i1; ++i) {
        const double a = std::min(axis[i], range->a1);
        const double v = sig[i];
        integ += 0.5 * (prev_v + v) * (a - prev_a);
        prev_a = a;
        prev_v = v;
        if (a >= range->a1) {
            break;
        }
    }
    return integ;
}

std::optional<double> derivative_at(std::span<const double> axis,
                                    std::span<const double> sig, double t) {
    if (axis.size() != sig.size() || axis.size() < 2) {
        return std::nullopt;
    }
    if (t < axis.front() || t > axis.back()) {
        return std::nullopt;
    }
    const std::size_t i = static_cast<std::size_t>(
        std::upper_bound(axis.begin(), axis.end(), t) - axis.begin());
    // Symmetric difference: the interval before and after t.
    const std::size_t lo = (i >= 2) ? i - 2 : 0;
    const std::size_t hi = (i + 1 < axis.size()) ? i + 1 : axis.size() - 1;
    if (axis[hi] == axis[lo]) {
        return std::nullopt;
    }
    return (sig[hi] - sig[lo]) / (axis[hi] - axis[lo]);
}

std::optional<double> rise_time(std::span<const double> axis,
                                std::span<const double> sig, const Window& w,
                                double lo, double hi) {
    const auto vmin = minimum(sig, w);
    const auto vmax = maximum(sig, w);
    if (!vmin || !vmax) {
        return std::nullopt;
    }
    const double swing = *vmax - *vmin;
    if (!(swing > 0.0)) {
        return std::nullopt;
    }
    const auto t_lo = cross_time(axis, sig, *vmin + lo * swing, Dir::Rise, w);
    const auto t_hi = cross_time(axis, sig, *vmin + hi * swing, Dir::Rise, w);
    if (!t_lo || !t_hi || !(*t_hi > *t_lo)) {
        return std::nullopt;
    }
    return *t_hi - *t_lo;
}

std::optional<double> fall_time(std::span<const double> axis,
                                std::span<const double> sig, const Window& w,
                                double lo, double hi) {
    const auto vmin = minimum(sig, w);
    const auto vmax = maximum(sig, w);
    if (!vmin || !vmax) {
        return std::nullopt;
    }
    const double swing = *vmax - *vmin;
    if (!(swing > 0.0)) {
        return std::nullopt;
    }
    const auto t_lo = cross_time(axis, sig, *vmin + lo * swing, Dir::Fall, w);
    const auto t_hi = cross_time(axis, sig, *vmin + hi * swing, Dir::Fall, w);
    if (!t_lo || !t_hi || !(*t_hi > *t_lo)) {
        return std::nullopt;
    }
    return *t_hi - *t_lo;
}

std::optional<double> trig_targ(std::span<const double> trig_axis,
                                std::span<const double> trig_sig, double trig_level,
                                Dir trig_dir, int trig_n,
                                std::span<const double> targ_axis,
                                std::span<const double> targ_sig, double targ_level,
                                Dir targ_dir, int targ_n) {
    const auto t0 = cross_time_n(trig_axis, trig_sig, trig_level, trig_dir, trig_n);
    if (!t0) {
        return std::nullopt;
    }
    // The target crossing must be AT OR AFTER the trigger (the classic
    // propagation-delay convention); search the tail window.
    const auto t1 = cross_time_n(targ_axis, targ_sig, targ_level, targ_dir, targ_n,
                                 Window::between(*t0, targ_axis.empty() ? 0.0
                                                                        : targ_axis.back()));
    if (!t1) {
        return std::nullopt;
    }
    return *t1 - *t0;
}

std::optional<double> find_when(std::span<const double> axis,
                                std::span<const double> sig,
                                std::span<const double> when_sig, double level,
                                Dir dir, const Window& w) {
    const auto t = cross_time(axis, when_sig, level, dir, w);
    if (!t) {
        return std::nullopt;
    }
    return value_at(axis, sig, *t);
}

std::optional<double> find_at(std::span<const double> axis,
                              std::span<const double> sig, double t) {
    return value_at(axis, sig, t);
}

}  // namespace ws::spice::measure
