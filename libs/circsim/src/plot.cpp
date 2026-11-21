// apps/circsim stage 4 - the plot axis math (see include/ws/circsim/
// plot.hpp for the contracts). This TU is now the FACADE over
// ws::plotmath (the unify-plotting merge): the workspace grew a
// dedicated shared core for the plotting math both the generic gui
// plot widget and this panel ride, and every function here forwards
// to it. The public ws::circsim types stay (PlotRange, AxisRun,
// AxisRuns, PlotYAxis, the function names and signatures) - every
// existing caller and test keeps compiling and answering exactly what
// it always did, because the bodies are the same code, one tier down.
//
// What stays HERE is circsim's own: the signal-name classifier
// (signal_y_axis - SPICE save-name knowledge, not plotting math) and
// the header's contract prose. The headless suite (tests/test_plot.cpp)
// still gates every decision through this facade, so a facade bug
// (a bad forward) fails by name.

#include "ws/circsim/plot.hpp"

#include <span>
#include <string>
#include <string_view>

#include <ws/plotmath/plotmath.hpp>

namespace ws::circsim {

namespace {
// The facade's converters: ws::circsim's public types and ws::plotmath's
// are the same two-doubles (and run-vector) shapes spelled under their
// own namespaces - each forward below converts field-by-field so the
// two spellings stay nominally distinct (a TU holding both headers
// reads unambiguously) while sharing one implementation.
[[nodiscard]] ws::plotmath::PlotRange to_core(PlotRange r) {
    return {r.lo, r.hi};
}
[[nodiscard]] PlotRange to_app(ws::plotmath::PlotRange r) {
    return {r.lo, r.hi};
}
[[nodiscard]] AxisRun to_app(ws::plotmath::AxisRun r) {
    return {r.begin, r.end, r.rising};
}
[[nodiscard]] AxisRuns to_app(ws::plotmath::AxisRuns r) {
    AxisRuns out;
    out.runs.reserve(r.runs.size());
    for (const ws::plotmath::AxisRun& run : r.runs) {
        out.runs.push_back(to_app(run));
    }
    out.lo = r.lo;
    out.hi = r.hi;
    out.finite = r.finite;
    return out;
}
}  // namespace

std::vector<double> nice_ticks(double lo, double hi, unsigned target) {
    return ws::plotmath::nice_ticks(lo, hi, target);
}

std::string format_eng(double v, int digits) {
    return ws::plotmath::format_eng(v, digits);
}

PlotRange expand_range(double lo, double hi, double frac) {
    return to_app(ws::plotmath::expand_range(lo, hi, frac));
}

// --- the manual view (stage 5, see plot.hpp for the contracts) -----------------

PlotRange clamp_view(PlotRange view, PlotRange home) {
    return to_app(ws::plotmath::clamp_view(to_core(view), to_core(home)));
}

PlotRange zoom_range(PlotRange view, PlotRange home, double anchor,
                     double factor) {
    return to_app(ws::plotmath::zoom_range(to_core(view), to_core(home),
                                         anchor, factor));
}

PlotRange pan_range(PlotRange view, PlotRange home, double delta) {
    return to_app(ws::plotmath::pan_range(to_core(view), to_core(home),
                                         delta));
}

std::vector<std::size_t> decimate_minmax(std::span<const double> xs,
                                        std::span<const double> ys,
                                        std::size_t budget) {
    return ws::plotmath::decimate_minmax(xs, ys, budget);
}

std::vector<std::size_t> decimate_minmax_index(
    std::span<const double> ys, std::size_t budget) {
    return ws::plotmath::decimate_minmax_index(ys, budget);
}

AxisRuns split_axis_runs(std::span<const double> xs) {
    return to_app(ws::plotmath::split_axis_runs(xs));
}

// --- the two-axis split (stage 6) --------------------------------------------

namespace {

[[nodiscard]] char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

PlotYAxis signal_y_axis(std::string_view name) {
    // One re()/im() wrapper unwraps: .ac names its columns re(v(out))
    // and im(i(V1)), the unit one paren in. The wrapper must open at
    // byte 0 AND close at the last byte - "re(v(out)" is an unclosed
    // name, "pwr(v(out))" a foreign prefix; neither classifies.
    if (name.size() >= 4 && name.back() == ')') {
        const char c0 = lower_ascii(name[0]);
        const char c1 = lower_ascii(name[1]);
        if (name[2] == '(' && ((c0 == 'r' && c1 == 'e') ||
                               (c0 == 'i' && c1 == 'm'))) {
            name.remove_prefix(3);
            name.remove_suffix(1);
        }
    }
    // The unwrap left the signal ref itself: i( - any case, as the
    // deck parser reads save names - is a current. Everything else
    // (v(...), a bare name, an empty one) keeps the left spine.
    if (name.size() >= 3 && lower_ascii(name[0]) == 'i' &&
        name[1] == '(') {
        return PlotYAxis::Right;
    }
    return PlotYAxis::Left;
}

}  // namespace ws::circsim
