// The parity suite: identical data and viewport configurations fed
// through BOTH consumers' public plotting APIs - the generic gui
// widget's (ws::ui::gui::plot_*) and the Circsim panel's
// (ws::circsim::plot math, through its facade) - with their outputs
// asserted equal. This is the success criterion of the unify-plotting
// merge spelled as a test: the two surfaces may render differently
// and expose different features, but identical inputs must produce
// consistent axis calculations, coordinate transformations, data
// selection and navigation behavior - because both now answer through
// ws::plotmath, this suite pins that nothing ever regresses to two
// spellings again.
//
// The link shape crosses tiers on purpose (the gui archive AND
// ws::circsim in one TU): no production TU may hold both - that is
// the layering the merge exists to keep - but a TEST is exactly where
// the two spellings may meet, and this one makes them agree.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/circsim/plot.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/plotmath/plotmath.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

namespace pm = ws::plotmath;
namespace gui = ws::ui::gui;
namespace cs = ws::circsim;

// The gui's fixed-slot ticks spell vs the panel's vector spell - the
// same walk, two return shapes. This adapter reads both into a plain
// vector for comparison.
std::vector<double> as_vector(const gui::PlotTicks& t) {
    return std::vector<double>(t.begin(), t.end());
}


}  // namespace

TEST_CASE("parity: the tick ladder answers identically") {
    const double ranges[][2] = {
        {0.0, 10.0}, {0.0, 1.0}, {0.3, 9.7}, {-3.3, 3.3}, {4.7e3, 5.3e3},
        {-1.0, 0.0}, {5.0, 5.0}, {1e-6, 1e-3}, {-1e-3, -1e-6},
        {4.7e9, 5.3e9}, {0.0, 10.0},
    };
    const unsigned targets[] = {1u, 3u, 5u, 6u, 8u, 9u};
    for (const auto& r : ranges) {
        for (unsigned target : targets) {
            const std::vector<double> panel = cs::nice_ticks(r[0], r[1], target);
            const std::vector<double> widget =
                as_vector(gui::plot_ticks(r[0], r[1], target));
            // Both spellings cap at 32; both walk the same ladder.
            REQUIRE(panel.size() == widget.size());
            for (std::size_t i = 0; i < panel.size(); ++i) {
                CHECK(panel[i] == widget[i]);
            }
        }
    }
    // The refusals agree: inverted, NaN, infinite-span, degenerate.
    CHECK(as_vector(gui::plot_ticks(1.0, 0.0, 5u)).empty());
    CHECK(cs::nice_ticks(1.0, 0.0, 5u).empty());
    CHECK(as_vector(gui::plot_ticks(std::nan(""), 1.0, 5u)).empty());
    CHECK(cs::nice_ticks(std::nan(""), 1.0, 5u).empty());
    CHECK(as_vector(gui::plot_ticks(
              0.0, std::numeric_limits<double>::infinity(), 5u))
              .empty());
    CHECK(cs::nice_ticks(
              0.0, std::numeric_limits<double>::infinity(), 5u)
              .empty());
    // Degenerate: one honest tick, both.
    REQUIRE(as_vector(gui::plot_ticks(5.0, 5.0, 5u)).size() == 1u);
    REQUIRE(cs::nice_ticks(5.0, 5.0, 5u).size() == 1u);
    CHECK(as_vector(gui::plot_ticks(5.0, 5.0, 5u))[0] ==
          cs::nice_ticks(5.0, 5.0, 5u)[0]);
}

TEST_CASE("parity: the engineering label answers byte-identically") {
    const double values[] = {
        0.0,    -0.0,   1.0,    -1.0,   4700.0, -12.3e-3, 2e-9,
        1.5e6,  47e-12, 3.0,    0.9999, 999.9e3, 25.0,     1e18,
        1e-18,  2.5e-12, std::nan(""),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
    };
    for (double v : values) {
        for (int digits = 1; digits <= 6; ++digits) {
            char buf[32];
            const char* widget =
                gui::plot_format_eng(buf, sizeof buf, v, digits);
            const std::string panel = cs::format_eng(v, digits);
            // Byte-identical, both digit budgets.
            CHECK(std::string(widget) == panel);
            // And the core's buffer spelling agrees with itself too.
            char core_buf[32];
            CHECK(std::string(pm::format_eng_buf(
                      core_buf, sizeof core_buf, v, digits)) == panel);
        }
    }
}

TEST_CASE("parity: the view invariants answer identically") {
    const gui::PlotAxisRange view{2.0, 4.0};
    const gui::PlotAxisRange home{0.0, 10.0};

    // Padding.
    const gui::PlotAxisRange wg = gui::plot_expand_range(0.0, 10.0);
    const cs::PlotRange pn = cs::expand_range(0.0, 10.0);
    CHECK(wg.lo == pn.lo);
    CHECK(wg.hi == pn.hi);
    const gui::PlotAxisRange wg_flat = gui::plot_expand_range(3.0, 3.0);
    const cs::PlotRange pn_flat = cs::expand_range(3.0, 3.0);
    CHECK(wg_flat.lo == pn_flat.lo);
    CHECK(wg_flat.hi == pn_flat.hi);

    // Clamp.
    const gui::PlotAxisRange wc =
        gui::plot_clamp_view(view, home);
    const cs::PlotRange pc =
        cs::clamp_view({2.0, 4.0}, {0.0, 10.0});
    CHECK(wc.lo == pc.lo);
    CHECK(wc.hi == pc.hi);

    // Zoom - about the anchor, in and out, garbage factors, anchors
    // at and outside the window.
    const double anchors[] = {3.0, 100.0, -100.0, 2.0, 4.0,
                              std::nan("")};
    const double factors[] = {2.0, 0.5, 1.15, 1.0, 0.0, -1.0,
                              std::nan(""), 1e9};
    for (double a : anchors) {
        for (double f : factors) {
            const gui::PlotAxisRange wz =
                gui::plot_zoom_range(view, home, a, f);
            const cs::PlotRange pz =
                cs::zoom_range({2.0, 4.0}, {0.0, 10.0}, a, f);
            CHECK(wz.lo == pz.lo);
            CHECK(wz.hi == pz.hi);
        }
    }

    // Pan - both directions, past the edges, garbage.
    const double deltas[] = {1.0, -1.0, 100.0, -100.0, 0.0,
                             std::nan(""),
                             std::numeric_limits<double>::infinity()};
    for (double d : deltas) {
        const gui::PlotAxisRange wp =
            gui::plot_pan_range(view, home, d);
        const cs::PlotRange pp =
            cs::pan_range({2.0, 4.0}, {0.0, 10.0}, d);
        CHECK(wp.lo == pp.lo);
        CHECK(wp.hi == pp.hi);
    }
}

TEST_CASE("parity: the zoom anchor contract, both spellings") {
    // The cursor-anchored zoom the task's formula spells:
    //   x_anchor = x_min + (p_x - p_left)/W * (x_max - x_min)
    // After zooming, the same data coordinate stays under the same
    // screen position - the invariant BOTH surfaces' wheel zooms ride
    // through the same core call. Drive it as the two spellings
    // would: map the cursor to data, zoom, map back.
    const double lo = 0.0;
    const double hi = 10.0;
    const double W = 500.0;       // the frame's pixel width
    const double p_left = 46.0;   // the left margin
    for (double px = p_left; px <= p_left + W; px += 37.0) {
        // The anchor the cursor sits on, both spellings of the map.
        const double x_anchor =
            lo + (px - p_left) / W * (hi - lo);
        // The zoom, both spellings.
        const gui::PlotAxisRange wz =
            gui::plot_zoom_range({lo, hi}, {lo, hi}, x_anchor, 2.0);
        const cs::PlotRange pz =
            cs::zoom_range({lo, hi}, {lo, hi}, x_anchor, 2.0);
        CHECK(wz.lo == pz.lo);
        CHECK(wz.hi == pz.hi);
        // The anchor still sits at the same screen fraction.
        const double frac =
            (x_anchor - wz.lo) / (wz.hi - wz.lo);
        const double frac_cursor = (px - p_left) / W;
        CHECK(frac == doctest::Approx(frac_cursor).epsilon(1e-12));
        // And the coordinate maps back to the same pixel.
        const double px_back = p_left + frac * W;
        CHECK(px_back == doctest::Approx(px).epsilon(1e-9));
    }
}

TEST_CASE("parity: the decimation and the hover lookup") {
    // One dataset, both consumers' data-selection answers. The
    // panel's decimation is the core's (through the facade); the
    // widget's SeriesWalker decimation is the same core call in its
    // decimated() walk - so the keeps agree by construction. The
    // hover: the panel's nearest_sample is ws::plotmath's (the panel
    // calls it directly) and the widget's behavior half calls the
    // same - one walk.
    std::vector<double> xs;
    std::vector<double> ys;
    for (int i = 0; i <= 1000; ++i) {
        xs.push_back(i * 0.001);
        ys.push_back(i == 500 ? 1000.0
                              : static_cast<double>(i) / 1000.0);
    }
    // The panel's decimation keeps the spike; the core is the same
    // call the widget's walker rides - assert the facade forwards
    // faithfully (the widget's walker holds no public decimation
    // entry point; its keeps ARE this list, streamed).
    const std::vector<std::size_t> keep = cs::decimate_minmax(xs, ys, 64u);
    REQUIRE(keep.size() >= 4u);
    CHECK(keep.front() == 0u);
    CHECK(keep.back() == 1000u);
    bool spike = false;
    for (std::size_t k : keep) {
        if (k == 500u) {
            spike = true;
        }
    }
    CHECK(spike);

    // The hover lookup: both surfaces spell nearest_sample through
    // ws::plotmath now. The panel's call is direct; the widget's
    // behavior half is exercised through its suite (test_ui_plot's
    // hover cases) - here the shared call is pinned once, the
    // definition both consumers read.
    for (double q = -0.05; q <= 1.05; q += 0.037) {
        const int i_sorted = pm::nearest_sample(xs, q, true, xs.size());
        const int i_scan = pm::nearest_sample(xs, q, false, xs.size());
        CHECK(i_sorted == i_scan);
        CHECK(i_sorted >= 0);
        CHECK(static_cast<std::size_t>(i_sorted) < xs.size());
    }

    // The by-index decimation both surfaces' axisless shape rides.
    const std::vector<std::size_t> idx_keep =
        cs::decimate_minmax_index(ys, 64u);
    REQUIRE(idx_keep.size() >= 4u);
    CHECK(idx_keep.front() == 0u);
    CHECK(idx_keep.back() == ys.size() - 1u);
}

TEST_CASE("parity: the monotone-run split, both spellings") {
    // The panel's split (through the facade) vs the core's - the
    // same family contract, one walk. The widget has no public split
    // (it draws single sorted axes today); the panel's is the
    // consumer this pins.
    std::vector<double> wrap;
    for (std::size_t i = 0; i < 4000u; ++i) {
        wrap.push_back(1.0e-4 *
                       static_cast<double>(i < 2000u ? i : 3999u - i));
    }
    const cs::AxisRuns fam = cs::split_axis_runs(wrap);
    const pm::AxisRuns core = pm::split_axis_runs(wrap);
    REQUIRE(fam.runs.size() == core.runs.size());
    REQUIRE(fam.runs.size() == 2u);
    for (std::size_t i = 0; i < fam.runs.size(); ++i) {
        CHECK(fam.runs[i].begin == core.runs[i].begin);
        CHECK(fam.runs[i].end == core.runs[i].end);
        CHECK(fam.runs[i].rising == core.runs[i].rising);
    }
    CHECK(fam.lo == core.lo);
    CHECK(fam.hi == core.hi);
    CHECK(fam.finite == core.finite);
    // The runs stay disconnected in the panel's draw: the regression
    // is pinned in the panel suite (the step-family case); here the
    // contract the split carries is the facade's fidelity.
}

TEST_CASE("parity: the shared navigation constants are one set") {
    // The FEEL contract: the wheel factor, the strip exponential and
    // the double-click window the two surfaces' gestures ride are
    // literally the same constants (ws::plotmath's) - both files'
    // constexpr aliases bind them by name, so the compile itself is
    // the parity and no number can drift again without the alias
    // drifting with it. What is assertable is the contract the
    // constants carry: one wheel notch answers the same zoom through
    // both surfaces' view math, spelled once.
    const double notch = ws::plotmath::kWheelFactor;
    const gui::PlotAxisRange wg =
        gui::plot_zoom_range({0.0, 10.0}, {0.0, 10.0}, 5.0, notch);
    const cs::PlotRange pz =
        cs::zoom_range({0.0, 10.0}, {0.0, 10.0}, 5.0, notch);
    CHECK(wg.lo == pz.lo);
    CHECK(wg.hi == pz.hi);
    CHECK(ws::plotmath::kStripExp == doctest::Approx(0.005));
    CHECK(ws::plotmath::kDoubleClickSecs == doctest::Approx(0.35));
    CHECK(ws::plotmath::kDoubleClickPx == doctest::Approx(5.0f));
    CHECK(ws::plotmath::kBandMinPx == doctest::Approx(4.0f));
}
