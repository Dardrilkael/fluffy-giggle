// Fourier analysis (§7.9): the trapezoidal DFT at the window's exact
// harmonic bins, on the adaptive grid. No resampling, no FFT size games
// - the bins wanted are few, so direct sums are both exact for periodic
// content over an integer-period window and cheap (O(points * bins)).

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <ws/spice/fourier.hpp>

namespace ws::spice::fourier {

namespace {

constexpr double kTwoPi = 6.283185307179586476925287;

// The window's sample list: the in-window samples plus interpolated
// edges, so the trapezoid runs over exactly [t0, t1] whatever the grid
// did (the measure:: window convention).
std::vector<std::pair<double, double>> window_points(
    std::span<const double> axis, std::span<const double> sig, double t0,
    double t1) {
    std::vector<std::pair<double, double>> pts;
    if (axis.size() != sig.size() || axis.empty()) {
        return pts;
    }
    const auto value_at = [&](double t) -> double {
        // The axis is ascending (time); bracket t and interpolate.
        const std::size_t i = static_cast<std::size_t>(
            std::upper_bound(axis.begin(), axis.end(), t) - axis.begin());
        if (i == 0) {
            return sig.front();
        }
        if (i >= axis.size()) {
            return sig.back();
        }
        const double a0 = axis[i - 1];
        const double a1 = axis[i];
        if (a1 == a0) {
            return sig[i];
        }
        return sig[i - 1] + (sig[i] - sig[i - 1]) * (t - a0) / (a1 - a0);
    };
    pts.emplace_back(t0, value_at(t0));
    for (std::size_t i = 0; i < axis.size(); ++i) {
        if (axis[i] > t0 && axis[i] < t1) {
            pts.emplace_back(axis[i], sig[i]);
        }
    }
    pts.emplace_back(t1, value_at(t1));
    return pts;
}

}  // namespace

Series series(std::span<const double> axis, std::span<const double> sig,
              double fundamental_hz, int harmonics) {
    Series out;
    out.fundamental = fundamental_hz;
    if (axis.size() != sig.size() || axis.size() < 2) {
        out.note = "the signal holds fewer than two samples";
        return out;
    }
    if (!std::isfinite(fundamental_hz) || !(fundamental_hz > 0.0)) {
        out.note = "the fundamental frequency must be > 0";
        return out;
    }
    if (harmonics < 1 || harmonics > 10000) {
        out.note = "the harmonic count must be between 1 and 10000";
        return out;
    }
    const double period = 1.0 / fundamental_hz;
    const double t_end = axis.back();
    if (!(t_end - axis.front() > 0.0)) {
        out.note = "the time axis holds no span";
        return out;
    }
    // The window: the last whole fundamental period (the settled-state
    // read); when the run is shorter than one period, the run itself.
    double t0 = t_end - period;
    if (t0 < axis.front()) {
        t0 = axis.front();
    }
    const double length = t_end - t0;
    out.periods = length >= period * (1.0 - 1e-9) ? 1u : 0u;

    const std::vector<std::pair<double, double>> pts =
        window_points(axis, sig, t0, t_end);
    if (pts.size() < 2) {
        out.note = "the window holds no samples";
        return out;
    }
    // Every sample's phase relative to t0, in radians of the FUNDAMENTAL
    // (harmonic n sits at n * theta).
    std::vector<double> theta(pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        theta[i] = kTwoPi * (pts[i].first - t0) / length;
    }

    // DC: the plain trapezoidal mean.
    double sum = 0.0;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        sum += 0.5 * (pts[i - 1].second + pts[i].second) *
               (pts[i].first - pts[i - 1].first);
    }
    out.dc = sum / length;

    double fundamental_mag = -1.0;  // > 0 once harmonic 1 is in
    for (int n = 1; n <= harmonics; ++n) {
        const double nd = static_cast<double>(n);
        double cos_int = 0.0;
        double sin_int = 0.0;
        double prev_c = std::cos(nd * theta[0]);
        double prev_s = std::sin(nd * theta[0]);
        for (std::size_t i = 1; i < pts.size(); ++i) {
            const double c = std::cos(nd * theta[i]);
            const double s = std::sin(nd * theta[i]);
            const double dt = pts[i].first - pts[i - 1].first;
            cos_int += 0.5 * (pts[i - 1].second * prev_c + pts[i].second * c) * dt;
            sin_int += 0.5 * (pts[i - 1].second * prev_s + pts[i].second * s) * dt;
            prev_c = c;
            prev_s = s;
        }
        const double a = 2.0 * cos_int / length;  // the cosine coefficient
        const double b = 2.0 * sin_int / length;  // the sine coefficient
        Harmonic h;
        h.n = n;
        h.freq = nd * fundamental_hz;
        h.magnitude = std::hypot(a, b);
        h.phase_deg = std::atan2(-b, a) * (180.0 / 3.14159265358979323846);
        out.harmonics.push_back(h);
        if (n == 1) {
            fundamental_mag = h.magnitude;
        }
    }
    if (fundamental_mag > 0.0) {
        double sumsq = 0.0;
        for (Harmonic& h : out.harmonics) {
            h.normalized = h.magnitude / fundamental_mag * 100.0;
            if (h.n >= 2) {
                sumsq += h.magnitude * h.magnitude;
            }
        }
        out.thd_percent = std::sqrt(sumsq) / fundamental_mag * 100.0;
    }
    return out;
}

}  // namespace ws::spice::fourier
