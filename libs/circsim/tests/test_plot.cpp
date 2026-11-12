// plot (stage 4): the axis math the Results panel renders - nice
// ticks, engineering labels, range padding, min/max decimation. Pure
// doubles in, doubles and strings out; one case per contract in
// plot.hpp's docs.

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ws/circsim/plot.hpp>

using ws::circsim::PlotRange;
using ws::circsim::PlotYAxis;
using ws::circsim::clamp_view;
using ws::circsim::AxisRun;
using ws::circsim::AxisRuns;
using ws::circsim::decimate_minmax;
using ws::circsim::decimate_minmax_index;
using ws::circsim::split_axis_runs;
using ws::circsim::expand_range;
using ws::circsim::format_eng;
using ws::circsim::nice_ticks;
using ws::circsim::pan_range;
using ws::circsim::signal_y_axis;
using ws::circsim::zoom_range;

namespace {

// A tick list is inside [lo, hi], sorted, unique, and every entry is
// a whole multiple of a 1/2/5 step.
void check_ticks(double lo, double hi, const std::vector<double>& ts) {
    REQUIRE_FALSE(ts.empty());
    double step = 0.0;
    for (std::size_t i = 0; i < ts.size(); ++i) {
        CHECK(ts[i] >= lo - 1e-9 * (1.0 + std::fabs(lo)));
        CHECK(ts[i] <= hi + 1e-9 * (1.0 + std::fabs(hi)));
        if (i > 0) {
            CHECK(ts[i] > ts[i - 1]);
            step = ts[i] - ts[i - 1];
        }
    }
    REQUIRE(step > 0.0);
    // The step must be a 1/2/5 decade: mantissa in {1,2,5,10}.
    const double mant = step / std::pow(10.0, std::floor(std::log10(step)));
    bool on_ladder = false;
    for (double m : {1.0, 2.0, 5.0, 10.0}) {
        if (std::fabs(mant - m) < 0.01) {
            on_ladder = true;
        }
    }
    CHECK(on_ladder);
}

}  // namespace

TEST_CASE("nice ticks snap to the 1/2/5 ladder") {
    check_ticks(0.0, 10.0, nice_ticks(0.0, 10.0, 5));
    check_ticks(-3.3, 3.3, nice_ticks(-3.3, 3.3, 6));
    check_ticks(0.0, 1.0e-3, nice_ticks(0.0, 1.0e-3, 4));
    check_ticks(4.7e3, 5.3e3, nice_ticks(4.7e3, 5.3e3, 5));
    check_ticks(-1.0, 0.0, nice_ticks(-1.0, 0.0, 5));
}

TEST_CASE("nice ticks count stays near the target") {
    for (unsigned target = 3; target <= 9; target += 2) {
        const std::vector<double> ts =
            nice_ticks(-0.37, 2.91, target);
        check_ticks(-0.37, 2.91, ts);
        // 1/2/5 snapping can only land between half and ~2.5x the
        // target; anything outside is a ladder walk gone wrong.
        CHECK(ts.size() >= target / 2u);
        CHECK(ts.size() <= target * 3u);
    }
}

TEST_CASE("nice ticks cover the whole span at a coarse target") {
    // A one-tick ask still spans: the step halves until something
    // lands inside.
    const std::vector<double> ts = nice_ticks(0.0, 10.0, 1);
    check_ticks(0.0, 10.0, ts);
    CHECK(ts.size() >= 2u);
}

TEST_CASE("nice ticks answer the degenerate and broken ranges") {
    const std::vector<double> same = nice_ticks(2.5, 2.5, 5);
    REQUIRE(same.size() == 1u);
    CHECK(same[0] == 2.5);

    CHECK(nice_ticks(5.0, 1.0, 5).empty());       // inverted: empty
    CHECK(nice_ticks(std::nan(""), 1.0, 5).empty());
    CHECK(nice_ticks(0.0, std::nan(""), 5).empty());
    CHECK(nice_ticks(-std::numeric_limits<double>::infinity(), 5.0, 5).empty());  // infinite span
    CHECK(nice_ticks(0.0, std::numeric_limits<double>::infinity(), 5).empty());
}

TEST_CASE("nice ticks never print -0") {
    const std::vector<double> ts = nice_ticks(-1.0, 1.0, 8);
    REQUIRE(ts.size() >= 2u);
    bool has_zero = false;
    for (double t : ts) {
        if (t == 0.0) {
            has_zero = true;
            CHECK_FALSE(std::signbit(t));
        }
    }
    CHECK(has_zero);  // a -1..1 walk must cross the axis
}

TEST_CASE("format eng walks the SI prefixes") {
    CHECK(format_eng(0.0) == "0");
    CHECK(format_eng(-0.0) == "0");
    CHECK(format_eng(4700.0) == "4.7k");
    CHECK(format_eng(-12.3e-3) == "-12.3m");
    CHECK(format_eng(2e-9) == "2n");
    CHECK(format_eng(1.5e6) == "1.5M");
    CHECK(format_eng(47e-12) == "47p");
    CHECK(format_eng(3.0) == "3");
    CHECK(format_eng(999.0) == "999");
    CHECK(format_eng(-0.5) == "-500m");
    CHECK(format_eng(1e12) == "1T");
    CHECK(format_eng(1e-15) == "1f");
}

TEST_CASE("format eng rounds up through the decade") {
    // 999.9m at 3 digits would print "1" only after renormalizing to
    // the next prefix up: the m bucket loses, the unit bucket wins.
    CHECK(format_eng(999.9e-3) == "1");
    CHECK(format_eng(999.9e3) == "1M");
    CHECK(format_eng(999999.5) == "1M");
    // Digits clamp both ways without changing the prefix decision.
    CHECK(format_eng(4700.0, 1) == "5k");
    CHECK(format_eng(4700.0, 6) == "4.7k");
    CHECK(format_eng(0.0, 0) == "0");
}

TEST_CASE("format eng labels the non-finite and the beyond-table") {
    CHECK(format_eng(std::nan("")) == "nan");
    CHECK(format_eng(std::numeric_limits<double>::infinity()) == "inf");
    CHECK(format_eng(-std::numeric_limits<double>::infinity()) == "-inf");
    // Outside f..T the answer is plain %g, sign and all.
    CHECK(format_eng(5e18) == "5e+18");
    CHECK(format_eng(-5e18) == "-5e+18");
    CHECK(format_eng(2e-16) == "2e-16");
}

TEST_CASE("expand range pads a healthy span") {
    const PlotRange r = expand_range(0.0, 10.0);
    CHECK(r.lo < 0.0);
    CHECK(r.hi > 10.0);
    CHECK(r.lo == doctest::Approx(-0.5));
    CHECK(r.hi == doctest::Approx(10.5));
    CHECK(expand_range(0.0, 10.0, 0.0).lo == 0.0);
    CHECK(expand_range(0.0, 10.0, 0.0).hi == 10.0);
    // A negative frac clamps to zero: never a narrowing.
    CHECK(expand_range(2.0, 4.0, -1.0).lo == 2.0);
}

TEST_CASE("expand range opens a degenerate span around its magnitude") {
    const PlotRange r = expand_range(3.3, 3.3);
    CHECK(r.lo < 3.3);
    CHECK(r.hi > 3.3);
    CHECK((r.hi - r.lo) == doctest::Approx(2.0 * 3.3 * 0.05));

    const PlotRange zero = expand_range(0.0, 0.0);
    CHECK(zero.lo == doctest::Approx(-0.05));
    CHECK(zero.hi == doctest::Approx(0.05));

    // Non-finite endpoints answer the sane-empty frame.
    const PlotRange bad = expand_range(std::nan(""), 5.0);
    CHECK(bad.lo == 0.0);
    CHECK(bad.hi == 1.0);
    CHECK(expand_range(std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()).hi == 1.0);
}

TEST_CASE("clamp view keeps the window inside home") {
    const PlotRange home = expand_range(0.0, 10.0);  // -0.5 .. 10.5

    // Already inside: untouched.
    const PlotRange in = clamp_view({2.0, 5.0}, home);
    CHECK(in.lo == doctest::Approx(2.0));
    CHECK(in.hi == doctest::Approx(5.0));

    // Spilling one way slides the minimal distance.
    const PlotRange left = clamp_view({-2.0, 1.0}, home);
    CHECK(left.lo == doctest::Approx(-0.5));
    CHECK(left.hi == doctest::Approx(2.5));
    const PlotRange right = clamp_view({9.0, 12.0}, home);
    CHECK(right.lo == doctest::Approx(7.5));
    CHECK(right.hi == doctest::Approx(10.5));

    // A window at or past home's span cannot slide inside: home.
    CHECK(clamp_view({-5.0, 20.0}, home).lo == doctest::Approx(home.lo));
    CHECK(clamp_view({-5.0, 20.0}, home).hi == doctest::Approx(home.hi));
    CHECK(clamp_view({home.lo, home.hi}, home).lo == home.lo);
    CHECK(clamp_view({home.lo, home.hi}, home).hi == home.hi);

    // Degenerate or inverted windows: home, never a crash or a flip.
    CHECK(clamp_view({3.0, 3.0}, home).lo == doctest::Approx(home.lo));
    CHECK(clamp_view({5.0, 1.0}, home).hi == doctest::Approx(home.hi));

    // Non-finite answers home - the auto fit is the recovery.
    CHECK(clamp_view({std::nan(""), 2.0}, home).hi == doctest::Approx(home.hi));
    CHECK(clamp_view({2.0, std::numeric_limits<double>::infinity()}, home).lo ==
          doctest::Approx(home.lo));

    // An unusable home answers itself.
    const PlotRange bad = clamp_view({2.0, 5.0}, {std::nan(""), 1.0});
    CHECK(std::isnan(bad.lo));
    const PlotRange flat = clamp_view({2.0, 5.0}, {4.0, 4.0});
    CHECK(flat.lo == doctest::Approx(4.0));
    CHECK(flat.hi == doctest::Approx(4.0));
}

TEST_CASE("zoom range pins the anchor and floors at the mush limit") {
    const PlotRange home{0.0, 1000.0};

    // Zoom in 2x about the center: the anchor stays exactly put.
    const PlotRange mid = zoom_range({0.0, 1000.0}, home, 500.0, 2.0);
    CHECK(mid.lo == doctest::Approx(250.0));
    CHECK(mid.hi == doctest::Approx(750.0));

    // Zoom in about an off-center anchor: that data point keeps its
    // fractional position in the window.
    const PlotRange off = zoom_range({0.0, 1000.0}, home, 100.0, 2.0);
    CHECK(off.lo == doctest::Approx(50.0));
    CHECK(off.hi == doctest::Approx(550.0));

    // Zoom out: the window grows, clamped into home - a wheel spun
    // out forever settles on home, never past it.
    const PlotRange out = zoom_range({100.0, 200.0}, home, 150.0, 0.01);
    CHECK(out.lo == doctest::Approx(home.lo));
    CHECK(out.hi == doctest::Approx(home.hi));

    // An anchor outside the view acts at the nearest edge.
    const PlotRange edge = zoom_range({0.0, 100.0}, home, -50.0, 10.0);
    CHECK(edge.lo == doctest::Approx(0.0));
    CHECK(edge.hi == doctest::Approx(10.0));

    // The mush floor: an enormous factor lands on home's span x 1e-6,
    // centered on the anchor, still inside home.
    const PlotRange deep = zoom_range({400.0, 600.0}, home, 500.0, 1e30);
    CHECK((deep.hi - deep.lo) == doctest::Approx(1e-3).epsilon(1e-4));
    CHECK(deep.lo >= home.lo);
    CHECK(deep.hi <= home.hi);
    CHECK((deep.lo + deep.hi) * 0.5 == doctest::Approx(500.0).epsilon(1e-4));

    // No-ops and nonsense: the view comes back untouched.
    CHECK(zoom_range({100.0, 200.0}, home, 150.0, 1.0).lo == 100.0);
    CHECK(zoom_range({100.0, 200.0}, home, 150.0, 0.0).hi == 200.0);
    CHECK(zoom_range({100.0, 200.0}, home, 150.0, -2.0).lo == 100.0);
    CHECK(zoom_range({100.0, 200.0}, home, 150.0, std::nan("")).hi == 200.0);

    // A degenerate view has no direction to grow in.
    CHECK(zoom_range({5.0, 5.0}, home, 5.0, 2.0).lo == 5.0);
    CHECK(zoom_range({5.0, 5.0}, home, 5.0, 2.0).hi == 5.0);
}

TEST_CASE("pan range slides and parks flush at the data's edge") {
    const PlotRange home{0.0, 100.0};

    // A pan that stays inside: the exact shift.
    const PlotRange moved = pan_range({10.0, 20.0}, home, 5.0);
    CHECK(moved.lo == doctest::Approx(15.0));
    CHECK(moved.hi == doctest::Approx(25.0));

    // Panning right past the edge parks flush against home's hi.
    const PlotRange parked = pan_range({10.0, 20.0}, home, 500.0);
    CHECK(parked.lo == doctest::Approx(90.0));
    CHECK(parked.hi == doctest::Approx(100.0));

    // Panning left past the edge parks flush against home's lo.
    const PlotRange left = pan_range({10.0, 20.0}, home, -500.0);
    CHECK(left.lo == doctest::Approx(0.0));
    CHECK(left.hi == doctest::Approx(10.0));

    // Negative delta pans left; zero and NaN are no-ops.
    CHECK(pan_range({10.0, 20.0}, home, -5.0).lo == doctest::Approx(5.0));
    CHECK(pan_range({10.0, 20.0}, home, 0.0).lo == 10.0);
    CHECK(pan_range({10.0, 20.0}, home, std::nan("")).hi == 20.0);
    CHECK(pan_range({10.0, 20.0}, home,
                    std::numeric_limits<double>::infinity()).lo == 10.0);
}

TEST_CASE("decimate declines work it cannot do") {
    const std::vector<double> xs{0.0, 1.0, 2.0};
    const std::vector<double> ys{0.0, 5.0, 1.0};
    // Fits the budget: raw.
    CHECK(decimate_minmax(xs, ys, 8u).empty());
    // Budget below 2 keeps nothing honest: raw.
    CHECK(decimate_minmax(xs, ys, 1u).empty());
    // Mismatched spans: raw, never an out-of-bounds read.
    CHECK(decimate_minmax(xs, std::vector<double>{1.0}, 8u).empty());
    CHECK(decimate_minmax(std::vector<double>{1.0}, ys, 8u).empty());
    // Not increasing (the nested .dc inner axis): raw, never scrambled.
    const std::vector<double> saw{0.0, 1.0, 2.0, 0.0, 1.0, 2.0};
    CHECK(decimate_minmax(saw, saw, 2u).empty());
    const std::vector<double> flat{1.0, 1.0, 1.0, 1.0};
    CHECK(decimate_minmax(flat, flat, 2u).empty());
    const std::vector<double> hole{0.0, std::nan(""), 2.0};
    CHECK(decimate_minmax(hole, hole, 2u).empty());
    // Trivial sizes: raw.
    CHECK(decimate_minmax(std::vector<double>{},
                          std::vector<double>{}, 4u)
              .empty());
    CHECK(decimate_minmax(std::vector<double>{7.0},
                          std::vector<double>{7.0}, 4u)
              .empty());
}

TEST_CASE("decimate keeps every extremum within the budget") {
    // A slow ramp over y with one spike at i=500: whatever the budget,
    // the spike's sample and both ends survive.
    std::vector<double> xs;
    std::vector<double> ys;
    for (int i = 0; i <= 1000; ++i) {
        xs.push_back(i * 0.001);
        ys.push_back(i == 500 ? 1000.0 : static_cast<double>(i) / 1000.0);
    }
    const std::vector<std::size_t> keep = decimate_minmax(xs, ys, 64u);
    REQUIRE_FALSE(keep.empty());
    CHECK(keep.size() <= 2u * 64u + 2u);
    CHECK(keep.front() == 0u);
    CHECK(keep.back() == 1000u);
    bool spike = false;
    for (std::size_t k : keep) {
        if (k == 500u) {
            spike = true;
        }
        CHECK(k < xs.size());
    }
    CHECK(spike);
    // The kept indices are strictly increasing (bucket order).
    for (std::size_t i = 1; i < keep.size(); ++i) {
        CHECK(keep[i] > keep[i - 1]);
    }
}

TEST_CASE("decimate keeps the exact envelope of a sine") {
    std::vector<double> xs;
    std::vector<double> ys;
    for (int i = 0; i <= 999; ++i) {
        xs.push_back(i * 0.001);
        ys.push_back(std::sin(2.0 * 3.141592653589793 * 20.0 * xs.back()));
    }
    const std::vector<std::size_t> keep = decimate_minmax(xs, ys, 100u);
    REQUIRE_FALSE(keep.empty());
    // The global discrete min and max samples are their own buckets'
    // extremes, so the kept envelope reaches them exactly - not
    // approximately, which is the whole point of min/max (vs stride)
    // decimation.
    double true_lo = ys[0];
    double true_hi = ys[0];
    for (double y : ys) {
        true_lo = std::min(true_lo, y);
        true_hi = std::max(true_hi, y);
    }
    double kept_lo = ys[keep.front()];
    double kept_hi = ys[keep.front()];
    for (std::size_t k : keep) {
        kept_lo = std::min(kept_lo, ys[k]);
        kept_hi = std::max(kept_hi, ys[k]);
    }
    CHECK(kept_lo == true_lo);
    CHECK(kept_hi == true_hi);
    CHECK(true_lo <= -0.99);  // the sine itself reaches its rails
    CHECK(true_hi >= 0.99);
}

TEST_CASE("signal_y_axis splits the V and I columns") {
    // Voltage rides the left spine in every spelling the kernel
    // generates: the node refs and the differential pair.
    CHECK(signal_y_axis("v(out)") == PlotYAxis::Left);
    CHECK(signal_y_axis("v(a,b)") == PlotYAxis::Left);
    CHECK(signal_y_axis("V(out)") == PlotYAxis::Left);  // case cannot flip it
    // Current rides the right spine.
    CHECK(signal_y_axis("i(V1)") == PlotYAxis::Right);
    CHECK(signal_y_axis("i(v1)") == PlotYAxis::Right);
    CHECK(signal_y_axis("I(V1)") == PlotYAxis::Right);
    // .ac wraps both as re()/im() pairs - the unit lives one paren in.
    CHECK(signal_y_axis("re(v(out))") == PlotYAxis::Left);
    CHECK(signal_y_axis("im(v(out))") == PlotYAxis::Left);
    CHECK(signal_y_axis("re(i(V1))") == PlotYAxis::Right);
    CHECK(signal_y_axis("im(i(V1))") == PlotYAxis::Right);
    // Only ONE wrapper unwraps; a current under two is still a
    // current only if the inner ref survives - re() over im() leaves
    // im(i(V1)), which is no i( ref at the front and stays left. The
    // conservative answer for a name the rule cannot vouch for.
    CHECK(signal_y_axis("re(im(i(V1)))") == PlotYAxis::Left);
}

TEST_CASE("signal_y_axis answers left for the names it cannot vouch for") {
    // The empty name and the index fallback are no currents.
    CHECK(signal_y_axis("") == PlotYAxis::Left);
    CHECK(signal_y_axis("sample") == PlotYAxis::Left);
    // A bare letter is not a ref; a broken ref is not one either.
    CHECK(signal_y_axis("i") == PlotYAxis::Left);
    CHECK(signal_y_axis("i(") == PlotYAxis::Left);
    CHECK(signal_y_axis("re(v(out)") == PlotYAxis::Left);  // unclosed wrapper
    // A foreign prefix stays unclassified - and a wrapper glued to
    // nothing inside unwraps to a name with no i( left.
    CHECK(signal_y_axis("pwr(v(out))") == PlotYAxis::Left);
    CHECK(signal_y_axis("im(i") == PlotYAxis::Left);
}

TEST_CASE("decimate answers a monotone but sparse sweep") {
    const std::vector<double> xs{0.0, 10.0, 11.0, 12.0, 13.0, 100.0};
    const std::vector<double> ys{0.0, -5.0, 3.0, 3.0, -2.0, 1.0};
    // 6 <= 6 fits: raw.
    CHECK(decimate_minmax(xs, ys, 6u).empty());
    const std::vector<std::size_t> tight = decimate_minmax(xs, ys, 5u);
    REQUIRE_FALSE(tight.empty());
    CHECK(tight.front() == 0u);
    CHECK(tight.back() == 5u);
    CHECK(tight.size() <= 2u * (5u / 2u) + 2u);
    // The global min (-5 at i=1) and max (3 at i=2) are kept: they are
    // their buckets' extremes.
    bool has_min = false;
    bool has_max = false;
    for (std::size_t k : tight) {
        has_min = has_min || k == 1u;
        has_max = has_max || k == 2u;
    }
    CHECK(has_min);
    CHECK(has_max);
}

// --- the index envelope (stage 8) ---------------------------------------------
//
// decimate_minmax_index: the same min/max envelope for data whose x is
// the sample index - the by-index axis and the non-sorted wrap, where
// decimate_minmax declines (no x to chain). One case per contract in
// plot.hpp's docs.

TEST_CASE("decimate_index declines work it cannot do") {
    const std::vector<double> three{0.0, 5.0, 1.0};
    // Fits the budget: raw.
    CHECK(decimate_minmax_index(three, 8u).empty());
    // Budget below 2 keeps nothing honest: raw.
    CHECK(decimate_minmax_index(three, 1u).empty());
    // Trivial sizes: raw.
    CHECK(decimate_minmax_index(std::vector<double>{}, 4u).empty());
    CHECK(decimate_minmax_index(std::vector<double>{7.0}, 4u).empty());
    // One fold is not a fold.
    CHECK(decimate_minmax_index(std::vector<double>{7.0, 8.0}, 2u)
              .empty());
}

TEST_CASE("decimate_index keeps every extremum within the budget") {
    // The spike shape the x-bucket walk answers, at the index run: a
    // slow ramp with one spike at i=500 - whatever the budget, the
    // spike's sample and both ends survive.
    std::vector<double> ys;
    for (int i = 0; i <= 1000; ++i) {
        ys.push_back(i == 500 ? 1000.0 : static_cast<double>(i) / 1000.0);
    }
    const std::vector<std::size_t> keep = decimate_minmax_index(ys, 64u);
    REQUIRE_FALSE(keep.empty());
    CHECK(keep.size() <= 2u * 64u + 2u);
    CHECK(keep.front() == 0u);
    CHECK(keep.back() == 1000u);
    bool spike = false;
    for (std::size_t k : keep) {
        if (k == 500u) {
            spike = true;
        }
        CHECK(k < ys.size());
    }
    CHECK(spike);
    // The kept indices are strictly increasing (bucket order).
    for (std::size_t i = 1; i < keep.size(); ++i) {
        CHECK(keep[i] > keep[i - 1]);
    }
}

TEST_CASE("decimate_index keeps the exact envelope of a wrap") {
    // A .dc wrap's inner family: a triangle that rises 0..500 then
    // falls back - the global extremes ARE their buckets' extremes,
    // so the kept envelope reaches them exactly (min/max vs stride).
    std::vector<double> ys;
    for (int i = 0; i < 1000; ++i) {
        ys.push_back(i < 500 ? static_cast<double>(i)
                             : static_cast<double>(999 - i));
    }
    const std::vector<std::size_t> keep = decimate_minmax_index(ys, 100u);
    REQUIRE_FALSE(keep.empty());
    double true_lo = ys[0];
    double true_hi = ys[0];
    for (double y : ys) {
        true_lo = std::min(true_lo, y);
        true_hi = std::max(true_hi, y);
    }
    double kept_lo = ys[keep.front()];
    double kept_hi = ys[keep.front()];
    for (std::size_t k : keep) {
        kept_lo = std::min(kept_lo, ys[k]);
        kept_hi = std::max(kept_hi, ys[k]);
    }
    CHECK(kept_lo == true_lo);
    CHECK(kept_hi == true_hi);
    CHECK(true_lo == 0.0);
    CHECK(true_hi == 499.0);
}

TEST_CASE("decimate_index answers the NaN buckets like the x walk") {
    // A NaN that OPENS a bucket stays (the line break the stroker
    // needs); a NaN behind a finite opener compares false against
    // both extremes and drops - the x-bucket walk's own reading.
    std::vector<double> ys{std::nan(""), std::nan(""), 1.0};
    for (int i = 3; i < 200; ++i) {
        ys.push_back(static_cast<double>(i));
    }
    const std::vector<std::size_t> keep = decimate_minmax_index(ys, 20u);
    REQUIRE_FALSE(keep.empty());
    CHECK(keep.front() == 0u);  // the NaN opener is the forced endpoint
    bool nan_kept = false;
    for (std::size_t k : keep) {
        nan_kept = nan_kept || std::isnan(ys[k]);
    }
    CHECK(nan_kept);
    // The finite tail's maximum survives regardless.
    double kept_hi = -std::numeric_limits<double>::infinity();
    for (std::size_t k : keep) {
        if (std::isfinite(ys[k])) {
            kept_hi = std::max(kept_hi, ys[k]);
        }
    }
    CHECK(kept_hi == ys.back());
}

// --- the axis runs (stage 9) ---------------------------------------------------
//
// split_axis_runs: the monotone passes of an axis - a sorted axis is
// one run, a .step family's repeating inner axis one run per pass -
// plus the finite union envelope the overlaid family fits. One case
// per contract in plot.hpp's docs.

TEST_CASE("axis runs answer one run for a monotone axis") {
    const std::vector<double> xs{0.0, 0.5, 1.0, 1.5, 2.0};
    const AxisRuns r = split_axis_runs(xs);
    REQUIRE(r.runs.size() == 1u);
    CHECK(r.runs[0].begin == 0u);
    CHECK(r.runs[0].end == 5u);
    CHECK(r.runs[0].rising);
    CHECK(r.finite);
    CHECK(r.lo == 0.0);
    CHECK(r.hi == 2.0);
    // A falling axis is one run too, flagged for the reversed draw.
    const std::vector<double> falling{5.0, 4.0, 3.0, 2.0};
    const AxisRuns f = split_axis_runs(falling);
    REQUIRE(f.runs.size() == 1u);
    CHECK_FALSE(f.runs[0].rising);
    CHECK(f.runs[0].begin == 0u);
    CHECK(f.runs[0].end == 4u);
    CHECK(f.lo == 2.0);
    CHECK(f.hi == 5.0);
    // Degenerate sizes: no runs for nothing, one length-1 run for a
    // single sample.
    CHECK(split_axis_runs(std::vector<double>{}).runs.empty());
    const std::vector<double> one_xs{7.0};
    const AxisRuns one = split_axis_runs(one_xs);
    REQUIRE(one.runs.size() == 1u);
    CHECK(one.runs[0].begin == 0u);
    CHECK(one.runs[0].end == 1u);
    CHECK(one.lo == 7.0);
    CHECK(one.hi == 7.0);
}

TEST_CASE("axis runs answer one run per step pass") {
    // The .step shape: three passes of the same 0..1 sweep, the axis
    // resetting between them - the SCR trigger table's own form.
    std::vector<double> xs;
    for (int pass = 0; pass < 3; ++pass) {
        for (int i = 0; i < 50; ++i) {
            xs.push_back(static_cast<double>(i) / 49.0);
        }
    }
    const AxisRuns r = split_axis_runs(xs);
    REQUIRE(r.runs.size() == 3u);
    for (std::size_t p = 0; p < 3u; ++p) {
        CHECK(r.runs[p].begin == p * 50u);
        CHECK(r.runs[p].end == (p + 1u) * 50u);
        CHECK(r.runs[p].rising);
    }
    // The union envelope spans a pass's extent (not front/back of the
    // whole column - identical here, but the contract is the union).
    CHECK(r.finite);
    CHECK(r.lo == 0.0);
    CHECK(r.hi == 1.0);
}

TEST_CASE("axis runs split a wrap at the reversal and the plateau") {
    // The nested .dc wrap: rising inner sweep, then walking back down
    // - two runs, the second falling. The equal neighbour at the peak
    // breaks the rise (a plateau is not a direction).
    std::vector<double> xs;
    for (int i = 0; i < 10; ++i) {
        xs.push_back(static_cast<double>(i));
    }
    xs.push_back(9.0);  // the plateau/turn: equal to the peak
    for (int i = 8; i >= 0; --i) {
        xs.push_back(static_cast<double>(i));
    }
    const AxisRuns r = split_axis_runs(xs);
    REQUIRE(r.runs.size() == 2u);
    CHECK(r.runs[0].begin == 0u);
    CHECK(r.runs[0].end == 10u);
    CHECK(r.runs[0].rising);
    CHECK(r.runs[1].begin == 10u);
    CHECK(r.runs[1].end == 20u);
    CHECK_FALSE(r.runs[1].rising);
    CHECK(r.lo == 0.0);
    CHECK(r.hi == 9.0);
}

TEST_CASE("axis runs cover every sample through NaNs and singletons") {
    // A NaN on either side of a comparison reads false both ways: it
    // breaks the run it sits in and stands as its own length-1 run.
    // The runs must cover [0, n) exactly - every row belongs to a
    // pass, whatever the axis looks like.
    const std::vector<double> xs{1.0, 2.0, 3.0,
                                 std::nan(""),
                                 3.0, 2.0,
                                 5.0};
    const AxisRuns r = split_axis_runs(xs);
    REQUIRE_FALSE(r.runs.empty());
    std::size_t covered = 0;
    std::size_t prev_end = 0;
    bool contiguous = true;
    for (const AxisRun& run : r.runs) {
        contiguous = contiguous && run.begin == prev_end && run.end > run.begin;
        prev_end = run.end;
        covered += run.end - run.begin;
    }
    CHECK(contiguous);
    CHECK(covered == xs.size());
    CHECK(prev_end == xs.size());
    // The NaN sample never reaches the envelope.
    CHECK(r.finite);
    CHECK(r.lo == 1.0);
    CHECK(r.hi == 5.0);
    // An all-NaN axis: no finite envelope, the sane-empty answer.
    const std::vector<double> nan_axis_xs{std::nan(""), std::nan("")};
    const AxisRuns nan_axis = split_axis_runs(nan_axis_xs);
    CHECK_FALSE(nan_axis.finite);
    CHECK(nan_axis.lo == 0.0);
    CHECK(nan_axis.hi == 1.0);
}
