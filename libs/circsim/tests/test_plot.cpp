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
using ws::circsim::decimate_minmax;
using ws::circsim::expand_range;
using ws::circsim::format_eng;
using ws::circsim::nice_ticks;

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
