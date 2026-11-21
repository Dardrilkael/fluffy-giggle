// ws::plotmath's own gate: every decision the two plotting surfaces
// render, in one place. The suite's shape follows the two it replaces
// (circsim's test_plot.cpp and the ui plot suite's pure-math cases)
// and adds the contracts the merge itself owes: the two format_eng
// spellings byte-identical, the tick cap visible, the data<->screen
// round trips through an explicit frame transform, and the
// navigation invariants (anchor-pinned zoom, pan direction, resize
// not moving the data range).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/plotmath/plotmath.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

namespace pm = ws::plotmath;
using pm::PlotRange;

const double kInf = std::numeric_limits<double>::infinity();
const double kNaN = std::nan("");

// The tick sanity every ladder case walks: strictly increasing, on
// the step grid, inside the span (endpoints not forced).
void check_ticks(double lo, double hi, const std::vector<double>& ts) {
    CHECK_FALSE(ts.empty());
    for (std::size_t i = 0; i < ts.size(); ++i) {
        CHECK(ts[i] >= lo);
        CHECK(ts[i] <= hi);
        if (i > 0) {
            CHECK(ts[i] > ts[i - 1]);
        }
    }
}

// A ramp 0..n-1, strictly increasing - the honest sweep axis.
std::vector<double> ramp(std::size_t n, double lo = 0.0,
                          double hi = 1.0) {
    std::vector<double> xs(n);
    for (std::size_t i = 0; i < n; ++i) {
        xs[i] = lo + (hi - lo) * static_cast<double>(i) /
                         static_cast<double>(n <= 1u ? 1u : n - 1u);
    }
    return xs;
}

}  // namespace

// --- the axis ladder -------------------------------------------------------

TEST_CASE("nice_ticks: the 1/2/5 ladder, degenerate and inverted honesty") {
    // 0..10 at target 5: step 2 - the ladder's first rung that fits.
    std::vector<double> t = pm::nice_ticks(0.0, 10.0, 5u);
    REQUIRE(t.size() == 6u);
    CHECK(t[0] == 0.0);
    CHECK(t[1] == 2.0);
    CHECK(t[5] == 10.0);

    // 0..1 at target 5: step 0.2.
    t = pm::nice_ticks(0.0, 1.0, 5u);
    REQUIRE(t.size() == 6u);
    CHECK(t[1] == doctest::Approx(0.2));

    // A span that does not land on its endpoints: the ticks stay ON
    // the grid, inside the span.
    t = pm::nice_ticks(0.3, 9.7, 5u);
    REQUIRE(t.size() >= 4u);
    for (double v : t) {
        CHECK(v >= 0.3);
        CHECK(v <= 9.7);
    }

    // Degenerate: one honest tick (either non-finite endpoint too,
    // through the isnan guard first).
    t = pm::nice_ticks(5.0, 5.0, 5u);
    REQUIRE(t.size() == 1u);
    CHECK(t[0] == 5.0);
    REQUIRE(pm::nice_ticks(-kInf, -kInf, 5u).size() == 1u);

    // Inverted and non-finite: empty.
    CHECK(pm::nice_ticks(1.0, 0.0, 5u).empty());
    CHECK(pm::nice_ticks(kNaN, 1.0, 5u).empty());
    CHECK(pm::nice_ticks(0.0, kNaN, 5u).empty());
    CHECK(pm::nice_ticks(-kInf, 5.0, 5u).empty());
    CHECK(pm::nice_ticks(0.0, kInf, 5u).empty());
}

TEST_CASE("nice_ticks: narrow and wide ranges, tiny and huge magnitudes") {
    // Past 2^53 the walk cannot resolve ++k (k0 here is 2e16, over
    // the exactness bound) - the honest answer is none (the deep-zoom
    // guard). The span itself is representable (2 at an offset whose
    // ulp is 2), so the case reaches the guard rather than rounding
    // lo==hi into the degenerate single tick.
    const double big = 1.0e16;
    CHECK(pm::nice_ticks(big, big + 2.0, 5u).empty());

    // A denormal span underflows against the target: no ladder.
    CHECK(pm::nice_ticks(0.0, 5.0e-324, 5u).empty());

    // Negative-only and positive-only ranges ladder symmetrically.
    check_ticks(-1.0e-3, -1.0e-6, pm::nice_ticks(-1.0e-3, -1.0e-6, 5u));
    check_ticks(1.0e-6, 1.0e-3, pm::nice_ticks(1.0e-6, 1.0e-3, 5u));

    // Very large magnitudes still walk exact multiples.
    check_ticks(4.7e9, 5.3e9, pm::nice_ticks(4.7e9, 5.3e9, 5u));

    // The count stays near the target for every rung.
    for (unsigned target = 3; target <= 9; target += 2) {
        const std::vector<double> ts =
            pm::nice_ticks(-0.37, 2.91, target);
        check_ticks(-0.37, 2.91, ts);
        CHECK(ts.size() >= target / 2u);
        CHECK(ts.size() <= target * 3u);
    }

    // A one-tick ask still spans: the step halves until something
    // lands inside.
    const std::vector<double> one = pm::nice_ticks(0.0, 10.0, 1u);
    check_ticks(0.0, 10.0, one);
    CHECK(one.size() >= 2u);
}

TEST_CASE("nice_ticks: the 32-slot cap and the -0 normalization") {
    // A pathological huge target cannot overflow the walk: the
    // answer stops at 32 ticks - the caller's bug made visible, not
    // fatal (the widget's historical refusal, now both surfaces').
    const std::vector<double> ts = pm::nice_ticks(0.0, 10.0, 1000u);
    CHECK(ts.size() == 32u);
    for (double v : ts) {
        CHECK(v >= 0.0);
        CHECK(v <= 10.0);
    }

    // -0 normalizes: the tick at 0 prints "0", never "-0".
    const std::vector<double> zs = pm::nice_ticks(-1.0, 1.0, 8u);
    REQUIRE(zs.size() >= 2u);
    bool has_zero = false;
    for (double v : zs) {
        if (v == 0.0) {
            has_zero = true;
            CHECK_FALSE(std::signbit(v));
        }
    }
    CHECK(has_zero);  // a -1..1 walk must cross the axis
}

// --- the engineering-notation label ----------------------------------------

TEST_CASE("format_eng: the SI prefix walk") {
    namespace pm = ws::plotmath;
    CHECK(pm::format_eng(0.0) == "0");
    CHECK(pm::format_eng(-0.0) == "0");
    CHECK(pm::format_eng(4700.0) == "4.7k");
    CHECK(pm::format_eng(-12.3e-3) == "-12.3m");
    CHECK(pm::format_eng(2e-9) == "2n");
    CHECK(pm::format_eng(1.5e6) == "1.5M");
    CHECK(pm::format_eng(47e-12) == "47p");
    CHECK(pm::format_eng(3.0) == "3");
    CHECK(pm::format_eng(1e6) == "1M");
    CHECK(pm::format_eng(2.5e-12) == "2.5p");

    // The mantissa that rounds up through its decade renormalizes:
    // 999.9m at the default 3 digits prints as 1, not 1000m.
    CHECK(pm::format_eng(0.9999) == "1");
    CHECK(pm::format_eng(999.9e3) == "1M");

    // A mantissa %g slides into exponent form ("2e+01" for 25 at 1
    // digit) is spelled back out in plain digits.
    CHECK(pm::format_eng(25.0, 1) == "30");

    // The digit budget clamps both ways without changing the prefix.
    CHECK(pm::format_eng(4700.0, 1) == "5k");
    CHECK(pm::format_eng(4700.0, 6) == "4.7k");

    // Non-finite answers spell themselves; beyond the table plain %g
    // keeps the magnitude honest.
    CHECK(pm::format_eng(kNaN) == "nan");
    CHECK(pm::format_eng(kInf) == "inf");
    CHECK(pm::format_eng(-kInf) == "-inf");
    CHECK(pm::format_eng(1e18) == "1e+18");
    CHECK(pm::format_eng(1e-18) == "1e-18");
}

TEST_CASE("format_eng: the two spellings are byte-identical") {
    // The buffer spelling (the widget's caller-scratch discipline)
    // and the string spelling (the panel's) walk one implementation:
    // same input, same bytes, both digit budgets.
    const double values[] = {0.0,   -0.0,  1.0,   -1.0, 4700.0, -12.3e-3,
                             2e-9,  1e6,   0.9999, 999.9e3, 25.0,
                             1e18,  1e-18, kNaN,  kInf, -kInf, 2.5e-12};
    for (double v : values) {
        for (int digits = 1; digits <= 6; ++digits) {
            char buf[32];
            const char* got = pm::format_eng_buf(buf, sizeof buf, v, digits);
            CHECK(std::string(got) == pm::format_eng(v, digits));
        }
    }
    // The buffer discipline: NUL-terminated, truncated to fit.
    char tiny[4];
    CHECK(std::string(pm::format_eng_buf(tiny, sizeof tiny, -12.3e-3)) ==
          "-12");
    char zero[1];
    CHECK(pm::format_eng_buf(zero, sizeof zero, 1.0)[0] == '\0');
}

// --- the range/view invariants ----------------------------------------------

TEST_CASE("expand_range: padding, degenerate, and non-finite honesty") {
    const PlotRange padded = pm::expand_range(0.0, 10.0);
    CHECK(padded.lo == doctest::Approx(-0.5));
    CHECK(padded.hi == doctest::Approx(10.5));

    const PlotRange flat = pm::expand_range(3.0, 3.0);
    CHECK(flat.lo == doctest::Approx(2.85));
    CHECK(flat.hi == doctest::Approx(3.15));

    const PlotRange bad = pm::expand_range(kNaN, 1.0);
    CHECK(bad.lo == 0.0);
    CHECK(bad.hi == 1.0);
    const PlotRange bad_hi = pm::expand_range(0.0, kInf);
    CHECK(bad_hi.lo == 0.0);
    CHECK(bad_hi.hi == 1.0);

    // Negative frac clamps to zero padding, never narrows.
    const PlotRange none = pm::expand_range(0.0, 10.0, -1.0);
    CHECK(none.lo == 0.0);
    CHECK(none.hi == 10.0);

    // Finite endpoints whose PAD overflows: the padding cannot be
    // honest, so the answer keeps [lo, hi] unpadded. (Non-finite
    // endpoints answer {0, 1} - the earlier guard, a different
    // branch than this one.)
    const double kMaxFinite = std::numeric_limits<double>::max();
    const PlotRange huge = pm::expand_range(-kMaxFinite, kMaxFinite);
    CHECK(huge.lo == -kMaxFinite);
    CHECK(huge.hi == kMaxFinite);
}

TEST_CASE("clamp_view: the window never leaves home") {
    const PlotRange home{0.0, 10.0};
    CHECK(pm::clamp_view({2.0, 4.0}, home).lo == 2.0);
    CHECK(pm::clamp_view({-2.0, 4.0}, home).lo == 0.0);   // slides right in
    CHECK(pm::clamp_view({-2.0, 40.0}, home).hi == 10.0);  // past home: home
    CHECK(pm::clamp_view({kNaN, 4.0}, home).hi == 10.0);
    // A degenerate home answers itself - never a manual window.
    const PlotRange flat{3.0, 3.0};
    CHECK(pm::clamp_view({2.0, 4.0}, flat).lo == 3.0);
}

TEST_CASE("zoom_range: the anchor stays under the cursor") {
    const PlotRange home{0.0, 10.0};
    const PlotRange z = pm::zoom_range({2.0, 4.0}, home, 3.0, 2.0);
    CHECK(z.lo == doctest::Approx(2.5));
    CHECK(z.hi == doctest::Approx(3.5));
    // The anchor contract: after the zoom, the anchor coordinate maps
    // back to the same screen fraction of the window it came from.
    // anchor 3.0 sat at (3-2)/(4-2) = 0.5 of the window; after, it
    // sits at (3-2.5)/(3.5-2.5) = 0.5 again.
    CHECK((3.0 - z.lo) / (z.hi - z.lo) == doctest::Approx(0.5));

    // An anchor outside the window acts at the nearest edge.
    const PlotRange ze = pm::zoom_range({2.0, 4.0}, home, 100.0, 2.0);
    CHECK(ze.hi == doctest::Approx(4.0));

    // Factor 1 and garbage answer the view unchanged.
    CHECK(pm::zoom_range({2.0, 4.0}, home, 3.0, 1.0).lo == 2.0);
    CHECK(pm::zoom_range({2.0, 4.0}, home, 3.0, kNaN).hi == 4.0);
    CHECK(pm::zoom_range({2.0, 4.0}, home, 3.0, 0.0).hi == 4.0);
    // A NaN anchor is the view's lo.
    CHECK(pm::zoom_range({2.0, 4.0}, home, kNaN, 2.0).lo ==
          doctest::Approx(2.0));

    // The mush floor: a deep zoom settles at home-span x 1e-6.
    PlotRange deep = {5.0, 5.001};
    for (int i = 0; i < 200; ++i) {
        deep = pm::zoom_range(deep, home, 5.0005, 1.15);
    }
    CHECK((deep.hi - deep.lo) >= (home.hi - home.lo) * 1e-6 * 0.999);
    CHECK(deep.lo >= home.lo);
    CHECK(deep.hi <= home.hi);

    // Zooming OUT past home settles ON home, never beyond it.
    PlotRange wide = {2.0, 4.0};
    for (int i = 0; i < 40; ++i) {
        wide = pm::zoom_range(wide, home, 3.0, 0.5);
    }
    CHECK(wide.lo == home.lo);
    CHECK(wide.hi == home.hi);
}

TEST_CASE("pan_range: data-space slides, parking flush at the edge") {
    const PlotRange home{0.0, 10.0};
    // Positive delta slides toward hi.
    CHECK(pm::pan_range({2.0, 4.0}, home, 1.0).lo == doctest::Approx(3.0));
    // A pan past the edge parks flush against it.
    CHECK(pm::pan_range({2.0, 4.0}, home, 100.0).hi == 10.0);
    CHECK(pm::pan_range({2.0, 4.0}, home, 100.0).lo == 8.0);
    CHECK(pm::pan_range({2.0, 4.0}, home, -100.0).lo == 0.0);
    // A non-finite delta answers the view unchanged.
    CHECK(pm::pan_range({2.0, 4.0}, home, kNaN).lo == 2.0);
    CHECK(pm::pan_range({2.0, 4.0}, home, kInf).hi == 4.0);
}

// --- the coordinate transform round trips -----------------------------------
//
// The screens' transform is a consumer concern (frame geometry), but
// the CONTRACT it must satisfy is this core's: a linear window maps
// data to screen and back losslessly, whatever the frame's size,
// origin, or Y direction. The cases below drive the transform shape
// both surfaces build (lo at the left/top, hi at the right/bottom for
// x; y flipped) as free functions over the same invariants.

namespace {

// The x transform both surfaces spell: data -> px over [fx, fx+iw].
struct Xform {
    double lo = 0.0, hi = 1.0;
    double fx = 0.0, iw = 100.0;
    [[nodiscard]] double to_px(double v) const {
        return fx + (v - lo) / (hi - lo) * iw;
    }
    [[nodiscard]] double to_data(double px) const {
        return lo + (px - fx) / iw * (hi - lo);
    }
};

}  // namespace

TEST_CASE("transforms: data<->screen round trips") {
    // Round trips at several frame shapes: non-square, off-origin,
    // inverted y (the y transform is the x one with the flip: to_data
    // is the same formula with the window's hi/lo swapped in scale).
    const Xform xf[] = {
        {0.0, 10.0, 0.0, 100.0},     // plain
        {0.0, 10.0, 46.0, 337.0},   // the real frame's shape (margins)
        {-1.0, 1.0, 0.0, 40.0},     // negative-only window
        {1.0, 1.0000001, 5.0, 30.0} // a near-degenerate window
    };
    for (const Xform& t : xf) {
        for (double f = 0.0; f <= 1.0; f += 0.1) {
            const double v = t.lo + f * (t.hi - t.lo);
            const double px = t.to_px(v);
            CHECK(px >= t.fx - 1e-9);
            CHECK(px <= t.fx + t.iw + 1e-9);
            CHECK(t.to_data(px) == doctest::Approx(v).epsilon(1e-12));
        }
        // The endpoints land exactly at the frame edges.
        CHECK(t.to_px(t.lo) == doctest::Approx(t.fx));
        CHECK(t.to_px(t.hi) == doctest::Approx(t.fx + t.iw));
        // The maps are mutual inverses at the pixel grid too.
        for (double px = t.fx; px <= t.fx + t.iw; px += 13.0) {
            CHECK(t.to_px(t.to_data(px)) ==
                  doctest::Approx(px).epsilon(1e-9));
        }
    }

    // The y flip: the same formula with hi at the TOP. Screen y grows
    // down; a data range's hi sits at fy, its lo at fy+ih - the
    // transform both surfaces spell for their left spine.
    const auto y_to_px = [](double v, double lo, double hi, double fy,
                            double ih) {
        return fy + (hi - v) / (hi - lo) * ih;
    };
    // A data value above the window maps above fy; below, below.
    CHECK(y_to_px(1.0, 0.0, 1.0, 0.0, 100.0) == doctest::Approx(0.0));
    CHECK(y_to_px(0.0, 0.0, 1.0, 0.0, 100.0) == doctest::Approx(100.0));
    // Inverted screen y round trips.
    const double mid = y_to_px(0.5, 0.0, 1.0, 0.0, 100.0);
    CHECK(mid == doctest::Approx(50.0));

    // Independent left/right windows: the same px column maps through
    // two different y windows to two different data values - the
    // dual-axis contract (each spine its own transform, no shared
    // global y).
    const double lo = y_to_px(0.25, -1.0, 1.0, 0.0, 100.0);
    const double hi = y_to_px(0.25, -0.1, 0.1, 0.0, 100.0);
    CHECK(lo != doctest::Approx(hi));
}

// --- the data window --------------------------------------------------------

TEST_CASE("strictly_increasing: the chain a NaN breaks") {
    CHECK(pm::strictly_increasing(ramp(10)));
    CHECK(pm::strictly_increasing(std::vector<double>{1.0}));
    CHECK(pm::strictly_increasing(std::vector<double>{}));
    CHECK_FALSE(pm::strictly_increasing(std::vector<double>{1.0, 1.0}));
    CHECK_FALSE(pm::strictly_increasing(
        std::vector<double>{0.0, 1.0, 0.0}));
    CHECK_FALSE(pm::strictly_increasing(
        std::vector<double>{0.0, kNaN, 2.0}));
}

TEST_CASE("nearest_sample: sorted binary path and scan agree") {
    const std::vector<double> xs = ramp(11, 0.0, 10.0);
    // The exact hit.
    CHECK(pm::nearest_sample(xs, 4.0, true, xs.size()) == 4);
    // Between two: the nearer one.
    CHECK(pm::nearest_sample(xs, 4.3, true, xs.size()) == 4);
    CHECK(pm::nearest_sample(xs, 4.8, true, xs.size()) == 5);
    // A tie lands on the LOWER index (both paths agree).
    CHECK(pm::nearest_sample(xs, 4.5, true, xs.size()) == 4);
    CHECK(pm::nearest_sample(xs, 4.5, false, xs.size()) == 4);
    // Past the ends: the nearer end.
    CHECK(pm::nearest_sample(xs, -100.0, true, xs.size()) == 0);
    CHECK(pm::nearest_sample(xs, 100.0, true, xs.size()) == 10);
    // Every query: the binary and scan paths answer identically.
    for (double q = -1.0; q <= 11.0; q += 0.25) {
        CHECK(pm::nearest_sample(xs, q, true, xs.size()) ==
              pm::nearest_sample(xs, q, false, xs.size()));
    }
    // The unsorted axis rides the scan: a wrapped .dc shape.
    const std::vector<double> wrap{0.0, 1.0, 2.0, 1.0, 0.0};
    CHECK(pm::nearest_sample(wrap, 2.0, false, wrap.size()) == 2);
    CHECK(pm::nearest_sample(wrap, 0.4, false, wrap.size()) == 0);
    // Empty answers -1; n 0 answers -1.
    CHECK(pm::nearest_sample(std::vector<double>{}, 1.0, true, 0) ==
          -1);
}

TEST_CASE("decimate_minmax: declines work it cannot do") {
    const std::vector<double> xs{0.0, 1.0, 2.0};
    const std::vector<double> ys{0.0, 5.0, 1.0};
    CHECK(pm::decimate_minmax(xs, ys, 8u).empty());   // fits: raw
    CHECK(pm::decimate_minmax(xs, ys, 1u).empty());   // budget < 2: raw
    CHECK(pm::decimate_minmax(xs, std::vector<double>{1.0}, 8u).empty());
    CHECK(pm::decimate_minmax(std::vector<double>{1.0}, ys, 8u).empty());
    // Not increasing (the nested .dc inner axis): raw, never
    // scrambled.
    const std::vector<double> saw{0.0, 1.0, 2.0, 0.0, 1.0, 2.0};
    CHECK(pm::decimate_minmax(saw, saw, 2u).empty());
    const std::vector<double> flat{1.0, 1.0, 1.0, 1.0};
    CHECK(pm::decimate_minmax(flat, flat, 2u).empty());
    const std::vector<double> hole{0.0, kNaN, 2.0};
    CHECK(pm::decimate_minmax(hole, hole, 2u).empty());
    // Trivial sizes: raw.
    CHECK(pm::decimate_minmax(std::vector<double>{},
                               std::vector<double>{}, 4u)
              .empty());
    CHECK(pm::decimate_minmax(std::vector<double>{7.0},
                              std::vector<double>{7.0}, 4u)
              .empty());
}

TEST_CASE("decimate_minmax: every extremum survives within the budget") {
    // A slow ramp with one spike at i=500: whatever the budget, the
    // spike's sample and both ends survive - ORIGINAL indices, so
    // tooltips and markers reference the source rows.
    std::vector<double> xs;
    std::vector<double> ys;
    for (int i = 0; i <= 1000; ++i) {
        xs.push_back(i * 0.001);
        ys.push_back(i == 500 ? 1000.0 : static_cast<double>(i) / 1000.0);
    }
    const std::vector<std::size_t> keep = pm::decimate_minmax(xs, ys, 64u);
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

    // The exact envelope of a sine: the kept set's y min/max IS the
    // signal's (every extremum is some bucket's extremum).
    xs.clear();
    ys.clear();
    for (int i = 0; i < 5000; ++i) {
        xs.push_back(static_cast<double>(i));
        ys.push_back(std::sin(static_cast<double>(i) * 0.01));
    }
    const std::vector<std::size_t> sine =
        pm::decimate_minmax(xs, ys, 100u);
    double kmin = kInf, kmax = -kInf;
    for (std::size_t k : sine) {
        kmin = std::min(kmin, ys[k]);
        kmax = std::max(kmax, ys[k]);
    }
    double ymin = kInf, ymax = -kInf;
    for (double v : ys) {
        ymin = std::min(ymin, v);
        ymax = std::max(ymax, v);
    }
    CHECK(kmin == doctest::Approx(ymin));
    CHECK(kmax == doctest::Approx(ymax));
}

TEST_CASE("decimate_minmax_index: the by-index envelope") {
    // Declines: fits, budget < 2, n < 2.
    CHECK(pm::decimate_minmax_index(std::vector<double>{1.0, 2.0}, 8u)
              .empty());
    CHECK(pm::decimate_minmax_index(std::vector<double>{1.0, 2.0}, 1u)
              .empty());
    CHECK(pm::decimate_minmax_index(std::vector<double>{5.0}, 8u)
              .empty());
    CHECK(pm::decimate_minmax_index(std::vector<double>{}, 8u).empty());

    // A long column folds to the budget keeping extrema, first/last.
    std::vector<double> ys;
    for (int i = 0; i < 2000; ++i) {
        ys.push_back(i == 1000 ? -7.0
                               : std::sin(static_cast<double>(i) * 0.02));
    }
    const std::vector<std::size_t> keep =
        pm::decimate_minmax_index(ys, 64u);
    REQUIRE(keep.size() >= 4u);
    CHECK(keep.size() <= 2u * 64u + 2u);
    CHECK(keep.front() == 0u);
    CHECK(keep.back() == 1999u);
    bool dip = false;
    for (std::size_t k : keep) {
        if (k == 1000u) {
            dip = true;
        }
    }
    CHECK(dip);
    for (std::size_t i = 1; i < keep.size(); ++i) {
        CHECK(keep[i] > keep[i - 1]);
    }

    // A NaN column: the bucket opens keep their NaN break vertices
    // (the line-break contract) - the walk answers, never crashes.
    std::vector<double> holed;
    for (int i = 0; i < 100; ++i) {
        holed.push_back(i % 37 == 0 ? kNaN
                                    : static_cast<double>(i) / 100.0);
    }
    const std::vector<std::size_t> hole_keep =
        pm::decimate_minmax_index(holed, 8u);
    CHECK_FALSE(hole_keep.empty());
}

// --- the monotone-run family -------------------------------------------------

TEST_CASE("split_axis_runs: sorted is one run; steps are one per pass") {
    // Sorted: exactly one rising run covering the span.
    const std::vector<double> xs = ramp(100);
    const pm::AxisRuns one = pm::split_axis_runs(xs);
    REQUIRE(one.runs.size() == 1u);
    CHECK(one.runs[0].begin == 0u);
    CHECK(one.runs[0].end == 100u);
    CHECK(one.runs[0].rising);
    CHECK(one.finite);
    CHECK(one.lo == 0.0);
    CHECK(one.hi == doctest::Approx(1.0));

    // Descending: one FALLING run, as drawable as a rising one.
    std::vector<double> down = ramp(100, 1.0, 0.0);
    const pm::AxisRuns two = pm::split_axis_runs(down);
    REQUIRE(two.runs.size() == 1u);
    CHECK_FALSE(two.runs[0].rising);

    // The wrap (a .step/.dc family): one rising pass, one falling.
    std::vector<double> wrap = ramp(4000);
    for (std::size_t i = 0; i < wrap.size(); ++i) {
        wrap[i] = 1.0e-4 *
                  static_cast<double>(i < 2000u ? i : 3999u - i);
    }
    const pm::AxisRuns fam = pm::split_axis_runs(wrap);
    REQUIRE(fam.runs.size() == 2u);
    CHECK(fam.runs[0].rising);
    CHECK(fam.runs[0].begin == 0u);
    CHECK(fam.runs[0].end == 2000u);
    CHECK_FALSE(fam.runs[1].rising);
    CHECK(fam.runs[1].begin == 2000u);
    CHECK(fam.runs[1].end == 4000u);
    // The union envelope spans every pass's extent.
    CHECK(fam.finite);
    CHECK(fam.lo == 0.0);
    CHECK(fam.hi == doctest::Approx(0.1999));

    // Plateaus break runs (equal neighbours); NaNs break them too.
    const pm::AxisRuns plateau =
        pm::split_axis_runs(std::vector<double>{0.0, 1.0, 1.0, 2.0});
    REQUIRE(plateau.runs.size() == 2u);
    const pm::AxisRuns holed =
        pm::split_axis_runs(std::vector<double>{0.0, 1.0, kNaN, 2.0});
    REQUIRE(holed.runs.size() >= 2u);

    // Empty and all-NaN axes: no runs, the sane-empty envelope.
    const pm::AxisRuns none =
        pm::split_axis_runs(std::vector<double>{});
    CHECK(none.runs.empty());
    CHECK_FALSE(none.finite);
    CHECK(none.lo == 0.0);
    CHECK(none.hi == 1.0);
    const pm::AxisRuns nan_axis =
        pm::split_axis_runs(std::vector<double>{kNaN, kNaN});
    CHECK(nan_axis.runs.size() == 2u);  // two length-1 runs is legal
    CHECK_FALSE(nan_axis.finite);
}

TEST_CASE("split_axis_runs: the runs cover the whole span exactly") {
    // Every break shape: a plateau, a reversal, a reset, a NaN - the
    // runs partition the span with no gap and no overlap.
    const std::vector<double> xs{0.0, 1.0, 1.0, 0.5, 2.0, kNaN,
                                 -1.0, 3.0, 3.0, 3.0};
    const pm::AxisRuns runs = pm::split_axis_runs(xs);
    CHECK_FALSE(runs.runs.empty());
    std::size_t at = 0;
    for (const pm::AxisRun& r : runs.runs) {
        CHECK(r.begin == at);
        CHECK(r.end > r.begin);
        at = r.end;
    }
    CHECK(at == xs.size());
}

// --- the navigation constants ------------------------------------------------

TEST_CASE("the shared navigation constants") {
    // The behavioral contract both surfaces ride: the same wheel
    // notch, the same strip drag, the same double-click window. The
    // values are pinned - a silent change here changes every plot's
    // feel in both apps at once, which is exactly the point.
    CHECK(pm::kWheelFactor == doctest::Approx(1.15));
    CHECK(pm::kStripExp == doctest::Approx(0.005));
    CHECK(pm::kDoubleClickSecs == doctest::Approx(0.35));
    CHECK(pm::kDoubleClickPx == doctest::Approx(5.0f));
    CHECK(pm::kBandMinPx == doctest::Approx(4.0f));
    CHECK(pm::kMarginLeft == doctest::Approx(46.0f));
    CHECK(pm::kMarginRightAxis == doctest::Approx(46.0f));
    CHECK(pm::kMinInnerW == doctest::Approx(40.0f));
    CHECK(pm::kMinInnerH == doctest::Approx(30.0f));
}
