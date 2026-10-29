#pragma once

#include <optional>
#include <span>
#include <string_view>

// Measurements as LIBRARY functions (§0.4 rev): rise_time, rms, trig_targ
// and friends are C++ functions over a signal - .meas is a thin text
// parser onto them, and C++ users get the same answers without writing
// decks. Every function takes the shared sweep axis and one signal column
// (the columnar §7.8 shape), works on the ADAPTIVE grid through linear
// interpolation (the same convention the ngspice ref harness compares
// with), and returns nullopt when the measurement cannot be made (the
// caller turns that into a .meas failure or a library-level
// "measurement impossible" report - never a fabricated number).

namespace ws::spice::measure {

// The measurement window [t0, t1] (time, frequency, or the swept value -
// whatever the axis measures). Default: the whole signal.
struct Window {
    double t0 = 0.0;
    double t1 = 0.0;
    bool whole = true;  // ignore t0/t1

    [[nodiscard]] static Window all() { return {}; }
    [[nodiscard]] static Window between(double a, double b) {
        return Window{a, b, false};
    }
};

// Direction of a threshold crossing.
enum class Dir { Rise, Fall, Either };

// --- primitives ----------------------------------------------------------------

// The linearly-interpolated value at axis position t; nullopt outside
// the axis range.
[[nodiscard]] std::optional<double> value_at(std::span<const double> axis,
                                             std::span<const double> sig, double t);

// The first axis position where sig crosses `level` in direction `dir`,
// searching inside the window; nullopt when it never does.
[[nodiscard]] std::optional<double> cross_time(std::span<const double> axis,
                                               std::span<const double> sig,
                                               double level, Dir dir,
                                               const Window& w = Window::all());

// The Nth crossing (1-based), continuing past the first.
[[nodiscard]] std::optional<double> cross_time_n(std::span<const double> axis,
                                                 std::span<const double> sig,
                                                 double level, Dir dir, int n,
                                                 const Window& w = Window::all());

// The arithmetic mean over the window (AVG).
[[nodiscard]] std::optional<double> average(std::span<const double> axis,
                                            std::span<const double> sig,
                                            const Window& w = Window::all());

// The root-mean-square over the window (RMS).
[[nodiscard]] std::optional<double> rms(std::span<const double> axis,
                                        std::span<const double> sig,
                                        const Window& w = Window::all());

// Extremes over the window (MAX / MIN) and their difference (PP).
[[nodiscard]] std::optional<double> maximum(std::span<const double> sig,
                                            const Window& w = Window::all());
[[nodiscard]] std::optional<double> minimum(std::span<const double> sig,
                                            const Window& w = Window::all());
[[nodiscard]] std::optional<double> peak_to_peak(std::span<const double> sig,
                                                 const Window& w = Window::all());

// The definite integral over the window (INTEG), trapezoidal on the
// sample grid with interpolated edges.
[[nodiscard]] std::optional<double> integral(std::span<const double> axis,
                                             std::span<const double> sig,
                                             const Window& w = Window::all());

// The derivative at axis position t (DERIV), the symmetric difference on
// the two bracketing intervals.
[[nodiscard]] std::optional<double> derivative_at(std::span<const double> axis,
                                                  std::span<const double> sig,
                                                  double t);

// --- composites (what engineers actually write) ---------------------------------

// The 10-90% (by default) rise time of the signal's swing within the
// window: the time from the lo-fraction crossing to the hi-fraction
// crossing of (min + f*swing), rising. RC*ln(9) for an RC step.
[[nodiscard]] std::optional<double> rise_time(std::span<const double> axis,
                                              std::span<const double> sig,
                                              const Window& w = Window::all(),
                                              double lo = 0.1, double hi = 0.9);

// The falling counterpart.
[[nodiscard]] std::optional<double> fall_time(std::span<const double> axis,
                                              std::span<const double> sig,
                                              const Window& w = Window::all(),
                                              double lo = 0.9, double hi = 0.1);

// TRIG/TARG: the time from the trig signal's Nth crossing of trig_level
// to the targ signal's Nth crossing of targ_level (the classic
// propagation-delay measurement, and the rise-time spelling when both
// signals are the same one).
[[nodiscard]] std::optional<double> trig_targ(std::span<const double> trig_axis,
                                              std::span<const double> trig_sig,
                                              double trig_level, Dir trig_dir,
                                              int trig_n,
                                              std::span<const double> targ_axis,
                                              std::span<const double> targ_sig,
                                              double targ_level, Dir targ_dir,
                                              int targ_n);

// FIND v(x) WHEN v(y)=level: the interpolated value of `sig` at the
// axis position where `when_sig` crosses `level`.
[[nodiscard]] std::optional<double> find_when(std::span<const double> axis,
                                              std::span<const double> sig,
                                              std::span<const double> when_sig,
                                              double level, Dir dir,
                                              const Window& w = Window::all());

// FIND v(x) AT t.
[[nodiscard]] std::optional<double> find_at(std::span<const double> axis,
                                            std::span<const double> sig, double t);

}  // namespace ws::spice::measure
