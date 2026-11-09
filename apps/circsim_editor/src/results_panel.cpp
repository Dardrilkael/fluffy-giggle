// apps/circsim_editor - the Results panel (see results_panel.hpp).
//
// The layout, top to bottom: the header row (the table combo over the
// run's analyses, the export pair), the legend (one row per signal - a
// color swatch, the visibility checkbox, and the live readout at the
// hover index, falling back to the last sample), the plot itself (a
// fixed-height flow rect the canvas draws into in screen space; the
// scroll panel's clip keeps it inside the panel), and the measurement
// rows. A table with no sweep axis (.op) has no plot - its signals
// render as value rows, the ngspice op printout shape.
//
// The y range spans the VISIBLE signals only, x spans the analysis's
// own axis (the step axes are skipped, per results.hpp's contract) or
// the sample index when there is none; both ride ws::circsim::plot's
// padding and tick ladder, and long sweeps ride its min/max
// decimation so a 100k-point tran still shows its envelope at ~1
// vertex per pixel.
//
// Stage 5 adds the plot's hands: the wheel zooms both axes toward the
// cursor (the plot registers its own wheel claim - the innermost rect
// beats the enclosing scroll panel's), a left-drag rubber band zooms
// to the box, dragging on an axis strip stretches that axis, the
// middle/space drag pans, a double-click resets, a right-click
// cancels an in-flight drag. Every gesture rides ws::circsim::plot's
// clamp/zoom/pan math (headless, tested) against the HOME range - the
// padded auto fit - so the view can never leave the data; the
// decimation slices to the visible x window so a zoomed-in view
// spends its pixel budget on the samples it shows.
//
// Stage 6 gives a mixed V/I legend its second spine. The y fit splits
// per axis (ws::circsim::signal_y_axis): the voltage columns keep the
// left scale, the current columns earn a right one - own padded fit,
// own tick ladder, own label strip (the frame's right margin widens
// for it), own drag strip. Every stage-5 gesture now works on three
// windows: the wheel zooms x toward the cursor and each y spine
// toward its own mapped anchor, the band commits its box through each
// spine's own transform, the pan slides both y windows by the same
// pixels, and the right strip's drag stretches the amp window alone.
// A table with no visible current column draws exactly the stage-5
// shape - the right margin, strip and ticks only exist when the
// second spine does.
//
// Stage 7 is the polish pass: a floating hover tooltip (x plus every
// visible signal, units included), a sparkline per legend row, show
// all / hide all and a "fit view" button, a zero rail, an empty-state
// message, aligned value rows with units, and a one-line gesture
// cheat sheet under the plot. The per-frame debug logging and the
// per-frame index vector in the hover path are gone.

#include "results_panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include <span>

#include <ws/input/input.hpp>
#include <ws/spice/results.hpp>
#include <ws/circsim/overlay.hpp>
#include <ws/ui/renderer2d.hpp>

namespace editor {
namespace {

namespace canvas = ws::ui::canvas;
using canvas::Paint;
using canvas::Point;

// The series palette: data colors, not chrome - the shared wheel the
// legend swatches, the polylines AND the canvas's net-colored wires all
// ride (i mod N, like every scope) - editor.hpp's series_color, the
// one definition the app owns.
using editor::series_color;

[[nodiscard]] const char* analysis_name(ws::spice::AnalysisKind kind) {
    switch (kind) {
        case ws::spice::AnalysisKind::Op: return ".op";
        case ws::spice::AnalysisKind::Dc: return ".dc";
        case ws::spice::AnalysisKind::Tran: return ".tran";
        case ws::spice::AnalysisKind::Ac: return ".ac";
        case ws::spice::AnalysisKind::Noise: return ".noise";
    }
    return "?";
}

// The unit a signal is read in: the classifier's spine, spelled out.
[[nodiscard]] const char* unit_of(const std::string& name) {
    return ws::circsim::signal_y_axis(name) ==
                   ws::circsim::PlotYAxis::Right
               ? "A"
               : "V";
}

[[nodiscard]] std::string with_unit(double v, const char* unit,
                                    int digits = 4) {
    std::string out = ws::circsim::format_eng(v, digits);
    out += ' ';
    out += unit;
    return out;
}

struct ValueRow {
    std::string name;
    double value = 0.0;
    const char* unit = "";
};

// Keeps the per-signal visibility flags aligned with the table being
// shown (all-visible on a table switch; the flags survive frames).
//
// The size half of the condition guards the INVARIANT "visible has
// exactly t.signals.size() flags for the shown table": a mismatch can
// only mean the flags and `visible_for` disagree about which table
// they belong to (a delivery that reset one and not the other, or a
// table switch that landed mid-frame), and every reader downstream
// indexes visible[i] against t.signals[i] - the legend's checkbox
// write (which re-checks the size before it indexes, the same guard
// reading the same invariant from the write side) and the plot's
// series loop. Re-assigning on the mismatch restores all-visible,
// the same honest answer as a table switch, instead of drawing
// against flags sized for some other table.
void sync_visibility(Editor& ed, const ws::spice::Table& t) {
    if (ed.results.visible_for != ed.results.table ||
        ed.results.visible.size() != t.signals.size()) {
        ed.results.visible.assign(t.signals.size(), 1u);
        ed.results.visible_for = ed.results.table;
    }
}

[[nodiscard]] bool strictly_increasing(std::span<const double> xs) {
    for (std::size_t i = 1; i < xs.size(); ++i) {
        if (!(xs[i - 1] < xs[i])) {
            return false;
        }
    }
    return true;
}

// The hover sample: the index whose x is nearest the cursor. A sorted
// axis binary-searches; anything else scans (the .dc wrap and short
// tables are the only non-sorted cases).
[[nodiscard]] int nearest_sample(std::span<const double> xs, double x,
                                 bool sorted, std::size_t n) {
    if (n == 0) {
        return -1;
    }
    if (sorted && xs.size() == n && n >= 2u) {
        std::size_t lo = 0;
        std::size_t hi = n - 1;
        while (hi - lo > 1u) {
            const std::size_t mid = lo + (hi - lo) / 2u;
            if (xs[mid] <= x) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        const double dlo = std::fabs(xs[lo] - x);
        const double dhi = std::fabs(xs[hi] - x);
        return static_cast<int>(dlo <= dhi ? lo : hi);
    }
    std::size_t best = 0;
    double best_d = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < n && i < xs.size(); ++i) {
        const double d = std::fabs(xs[i] - x);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return static_cast<int>(best);
}

// One series polyline, mapped and clipped to the frame; non-finite
// samples break the line into segments rather than poisoning the
// tessellator with NaN vertices.
void stroke_series(canvas::Canvas2D& rc,
                   const std::vector<Point>& pts, std::uint32_t color,
                   float width = 2.0f) {
    Paint paint;
    paint.width = width;
    paint.color = color;
    paint.join = canvas::StrokeJoin::round;
    std::size_t run = 0;
    for (std::size_t i = 0; i <= pts.size(); ++i) {
        const bool break_here = i == pts.size() ||
                                std::isnan(pts[i].x) || std::isnan(pts[i].y);
        if (break_here && run >= 2u) {
            rc.stroke_polyline(pts.data() + (i - run),
                               static_cast<int>(run), paint);
        }
        if (i == pts.size()) {
            break;
        }
        run = break_here ? 0u : run + 1u;
    }
}

// --- the plot -----------------------------------------------------------------

// The plot rect's margins: the frame sits inside; the x axis's strip
// is the bottom margin (its labels live there), the y axis's is the
// left one. The strips are the axis-drag zooms' handles, so they are
// interaction geometry, not just spacing. Stage 6: a right axis
// widens the right margin to a label strip of its own - and that
// strip is the right spine's drag handle. The no-axis margin is the
// stage-5 shape exactly.
constexpr float kPlotHeight = 220.0f;
constexpr float kMarginLeft = 46.0f;
constexpr float kMarginBottom = 17.0f;
constexpr float kMarginTop = 6.0f;
constexpr float kMarginRight = 10.0f;
constexpr float kMarginRightAxis = 46.0f;

// The band's fill/outline: the theme's accent, alpha-split (a wash
// under the series, a line around the box). r2d packs 0xRRGGBBAA, so
// the alpha is the low byte.
[[nodiscard]] std::uint32_t with_alpha(std::uint32_t color,
                                       std::uint8_t a) {
    return (color & 0xFFFFFF00u) | std::uint32_t(a);
}

// A legend sparkline: the whole signal squeezed into `r` as a min/max
// envelope (so a spike survives the squeeze), scaled to its own range,
// with a dot at the hover sample. It is a thumbnail of the SHAPE - the
// plot carries the numbers.
void draw_sparkline(canvas::Canvas2D& rc, const gui::Rect& r,
                    std::span<const double> ys, std::uint32_t color,
                    int mark) {
    constexpr std::size_t kBuckets = 40;
    const std::size_t n = ys.size();
    if (n < 2u) {
        return;
    }
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    for (double v : ys) {
        if (std::isfinite(v)) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
    if (lo > hi) {
        return;  // nothing finite to draw
    }
    if (!(hi > lo)) {
        lo -= 1.0;  // a flat signal rides the middle rail
        hi += 1.0;
    }
    const float h = r.height - 4.0f;
    const auto py = [&](double v) {
        return static_cast<float>(r.y + 2.0f + (hi - v) / (hi - lo) * h);
    };
    const std::size_t buckets = std::min(kBuckets, n);
    std::vector<Point> pts;
    pts.reserve(buckets * 2u);
    for (std::size_t b = 0; b < buckets; ++b) {
        const std::size_t i0 = b * n / buckets;
        const std::size_t i1 = std::max(i0 + 1u, (b + 1u) * n / buckets);
        const float px = r.x + static_cast<float>(b) *
                                   (r.width - 1.0f) /
                                   static_cast<float>(buckets - 1u);
        std::size_t imin = n;
        std::size_t imax = n;
        for (std::size_t k = i0; k < i1 && k < n; ++k) {
            if (!std::isfinite(ys[k])) {
                continue;
            }
            if (imin == n || ys[k] < ys[imin]) {
                imin = k;
            }
            if (imax == n || ys[k] > ys[imax]) {
                imax = k;
            }
        }
        if (imin == n) {
            pts.push_back(Point{px, std::numeric_limits<float>::quiet_NaN()});
            continue;
        }
        // Index order inside the bucket, so a falling edge does not zigzag.
        const std::size_t first = std::min(imin, imax);
        const std::size_t second = std::max(imin, imax);
        pts.push_back(Point{px, py(ys[first])});
        if (second != first) {
            pts.push_back(Point{px, py(ys[second])});
        }
    }
    stroke_series(rc, pts, with_alpha(color, 200), 1.2f);
    if (mark >= 0 && static_cast<std::size_t>(mark) < n &&
        std::isfinite(ys[static_cast<std::size_t>(mark)])) {
        const float mx = r.x + static_cast<float>(mark) *
                                   (r.width - 1.0f) /
                                   static_cast<float>(n - 1u);
        rc.fill_circle({mx, py(ys[static_cast<std::size_t>(mark)])}, 2.2f,
                       color);
    }
}

// The frame geometry plus the range pairs the interaction pass works
// on: home (the auto fit - the outermost view) and the effective view
// (the manual window riding over it), per spine - x, the left y and
// (stage 6) the right y. The input pass mutates the effective pair;
// the draw pass reads it. wx/wy/wy1 map screen px to data through the
// CURRENT effective view of their own spine.
struct PlotFrame {
    float fx = 0.0f, fy = 0.0f, iw = 0.0f, ih = 0.0f;
    float mr = kMarginRight;       // the right margin (wider with an axis)
    bool has_right = false;        // a right (current) spine exists
    ws::circsim::PlotRange xr{};   // home x
    ws::circsim::PlotRange yr{};   // home y, left (V)
    ws::circsim::PlotRange yr1{};  // home y, right (A) - sane-empty when absent
    ws::circsim::PlotRange vxr{};  // effective x
    ws::circsim::PlotRange vyr{};  // effective y, left
    ws::circsim::PlotRange vyr1{}; // effective y, right

    [[nodiscard]] double wx(float px) const {
        return vxr.lo + (static_cast<double>(px) - fx) / iw *
                            (vxr.hi - vxr.lo);
    }
    [[nodiscard]] double wy(float py) const {
        return vyr.hi - (static_cast<double>(py) - fy) / ih *
                            (vyr.hi - vyr.lo);
    }
    [[nodiscard]] double wy1(float py) const {
        return vyr1.hi - (static_cast<double>(py) - fy) / ih *
                             (vyr1.hi - vyr1.lo);
    }
};

// One pointer frame over the plot (stage 5): the wheel zoom (its own
// wheel claim - the innermost rect beats the enclosing scroll
// panel's), the rubber-band box zoom in the frame, the axis-drag
// zooms on the two strips, the middle/space pan, the double-click
// reset, and the right-click cancel. The Schematic panel's idioms:
// raw InputFrame edges gated on claims_open, a drag keeps running
// wherever the pointer goes once armed, and every drag rebases from
// its press-time capture each frame so a cancel restores the view
// exactly and a drag never drifts.
void handle_plot_input(Editor& ed, gui::GuiContext& gui, PlotFrame& pf) {
    using ws::circsim::clamp_view;
    using ws::circsim::pan_range;
    using ws::circsim::zoom_range;

    ResultsCache& cache = ed.results;
    PlotGesture& g = cache.gesture;
    const ws::input::InputFrame& in = gui.input();

    // The manual view is per-table: a combo switch (or the fresh run
    // that reset view_for) drops back to the auto fit.
    if (cache.view_for != cache.table) {
        cache.view = PlotView{};
        cache.view_for = cache.table;
    }

    // The effective view: the manual windows clamped into their home,
    // or home itself - x and the left spine exactly as stage 5 left
    // them; the right spine (stage 6) additionally refuses a degenerate
    // window (span 0 can survive a clamp when the axis membership
    // changed under a stored view), because a zero-span spine cannot
    // map a single pixel.
    pf.vxr = cache.view.active
                 ? clamp_view({cache.view.x_lo, cache.view.x_hi}, pf.xr)
                 : pf.xr;
    pf.vyr = cache.view.active
                 ? clamp_view({cache.view.y_lo, cache.view.y_hi}, pf.yr)
                 : pf.yr;
    pf.vyr1 = pf.yr1;
    if (pf.has_right && cache.view.active) {
        const ws::circsim::PlotRange c =
            clamp_view({cache.view.y1_lo, cache.view.y1_hi}, pf.yr1);
        if (c.hi > c.lo) {
            pf.vyr1 = c;
        }
    }

    // The claimable rect: frame + all three strips, so the wheel works
    // over the axes too. The innermost rect beats the scroll panel's
    // claim; the win settles one frame later (the gui's contract),
    // which is why the consumption gates on wheel_won - the first
    // notch after arriving over the plot still scrolls the panel.
    const gui::Rect claimable{
        pf.fx - kMarginLeft, pf.fy - kMarginTop,
        pf.iw + kMarginLeft + pf.mr, pf.ih + kMarginTop + kMarginBottom};
    gui.register_wheel_claim(claimable, &cache.view);

    const bool in_frame =
        in.mouse_x >= pf.fx && in.mouse_x < pf.fx + pf.iw &&
        in.mouse_y >= pf.fy && in.mouse_y < pf.fy + pf.ih;
    const bool on_x_axis =
        in.mouse_x >= pf.fx && in.mouse_x < pf.fx + pf.iw &&
        in.mouse_y >= pf.fy + pf.ih &&
        in.mouse_y < pf.fy + pf.ih + kMarginBottom;
    const bool on_y_axis =
        in.mouse_x >= pf.fx - kMarginLeft && in.mouse_x < pf.fx &&
        in.mouse_y >= pf.fy && in.mouse_y < pf.fy + pf.ih;
    // The right spine's strip (stage 6): right of the frame, inside
    // its height - the amp window's drag handle. It exists only when
    // the second spine does.
    const bool on_y1_axis =
        pf.has_right && in.mouse_x >= pf.fx + pf.iw &&
        in.mouse_x < pf.fx + pf.iw + pf.mr && in.mouse_y >= pf.fy &&
        in.mouse_y < pf.fy + pf.ih;
    const bool over = in_frame || on_x_axis || on_y_axis || on_y1_axis;

    // The double-click clock: runs while the pointer is over the plot,
    // resets on every press there, dies when it leaves - the "two
    // clicks, same spot, soon" detector the reset rides.
    if (over) {
        g.click_clock += in.dt_seconds;
    } else {
        g.click_clock = 100.0f;
    }

    if (!gui.claims_open()) {
        return;  // a modal veil freezes the plot, the canvas's rule
    }

    // The right press cancels the in-flight gesture, restoring the
    // press-time view exactly - both spines included (stage 6).
    if (g.kind != PlotGesture::Kind::None &&
        in.mouse_pressed[ws::input::mouse::kRight] != 0) {
        cache.view.active = g.was_active;
        if (g.was_active) {
            cache.view.x_lo = g.x_lo;
            cache.view.x_hi = g.x_hi;
            cache.view.y_lo = g.y_lo;
            cache.view.y_hi = g.y_hi;
            cache.view.y1_lo = g.y1_lo;
            cache.view.y1_hi = g.y1_hi;
        }
        g.kind = PlotGesture::Kind::None;
        return;
    }

    // The wheel: zoom x toward the cursor and each y spine toward its
    // own mapped anchor (the same screen point, mapped per spine -
    // that is what both spines zooming "toward the cursor" means when
    // they disagree about what the cursor sits on) at the canvas's
    // 1.15-per-notch factor - but never mid-gesture (the band's press
    // point is captured in the view it started in).
    if (in_frame && in.scroll_dy != 0.0f &&
        g.kind == PlotGesture::Kind::None &&
        gui.wheel_won(&cache.view)) {
        if (!cache.view.active) {
            cache.view = PlotView{true, pf.vxr.lo, pf.vxr.hi,
                                  pf.vyr.lo, pf.vyr.hi,
                                  pf.vyr1.lo, pf.vyr1.hi};
        }
        const double wx = pf.wx(in.mouse_x);
        const double wy = pf.wy(in.mouse_y);
        const double wy1 = pf.wy1(in.mouse_y);
        const double f = in.scroll_dy > 0.0f ? 1.15 : 1.0 / 1.15;
        const ws::circsim::PlotRange nx =
            zoom_range({cache.view.x_lo, cache.view.x_hi}, pf.xr, wx, f);
        const ws::circsim::PlotRange ny =
            zoom_range({cache.view.y_lo, cache.view.y_hi}, pf.yr, wy, f);
        const ws::circsim::PlotRange ny1 =
            zoom_range({cache.view.y1_lo, cache.view.y1_hi}, pf.yr1,
                       wy1, f);
        cache.view.x_lo = nx.lo;
        cache.view.x_hi = nx.hi;
        cache.view.y_lo = ny.lo;
        cache.view.y_hi = ny.hi;
        cache.view.y1_lo = ny1.lo;
        cache.view.y1_hi = ny1.hi;
        pf.vxr = nx;
        pf.vyr = ny;
        pf.vyr1 = pf.has_right ? ny1 : pf.vyr1;
    }

    // The pan: middle-drag, or space+left-drag (the Schematic panel's
    // rule). Content follows the cursor: dragging right slides the
    // window left, dragging down slides it up (the y flip). The same
    // pixel shift maps through each spine's own span (stage 6) - the
    // two y windows slide by different data amounts and stay in step
    // on screen.
    const bool space_down = in.key_down[ws::input::keys::kSpace] != 0;
    const bool pan_held =
        in.mouse_down[ws::input::mouse::kMiddle] != 0 ||
        (space_down && in.mouse_down[ws::input::mouse::kLeft] != 0);
    if (pan_held && (in_frame || g.kind == PlotGesture::Kind::Pan)) {
        if (g.kind != PlotGesture::Kind::Pan) {
            g.kind = PlotGesture::Kind::Pan;
            g.from_x = in.mouse_x;
            g.from_y = in.mouse_y;
            g.x_lo = pf.vxr.lo;
            g.x_hi = pf.vxr.hi;
            g.y_lo = pf.vyr.lo;
            g.y_hi = pf.vyr.hi;
            g.y1_lo = pf.vyr1.lo;
            g.y1_hi = pf.vyr1.hi;
            g.was_active = cache.view.active;
            if (!cache.view.active) {
                cache.view = PlotView{true, pf.vxr.lo, pf.vxr.hi,
                                      pf.vyr.lo, pf.vyr.hi,
                                      pf.vyr1.lo, pf.vyr1.hi};
            }
        }
        const double dx =
            -(static_cast<double>(in.mouse_x - g.from_x) /
              static_cast<double>(pf.iw)) *
            (g.x_hi - g.x_lo);
        const double frac =
            static_cast<double>(in.mouse_y - g.from_y) /
            static_cast<double>(pf.ih);
        const double dy0 = frac * (g.y_hi - g.y_lo);
        const double dy1 = frac * (g.y1_hi - g.y1_lo);
        const ws::circsim::PlotRange nx =
            pan_range({g.x_lo, g.x_hi}, pf.xr, dx);
        const ws::circsim::PlotRange ny =
            pan_range({g.y_lo, g.y_hi}, pf.yr, dy0);
        const ws::circsim::PlotRange ny1 =
            pan_range({g.y1_lo, g.y1_hi}, pf.yr1, dy1);
        cache.view.x_lo = nx.lo;
        cache.view.x_hi = nx.hi;
        cache.view.y_lo = ny.lo;
        cache.view.y_hi = ny.hi;
        cache.view.y1_lo = ny1.lo;
        cache.view.y1_hi = ny1.hi;
        pf.vxr = nx;
        pf.vyr = ny;
        if (pf.has_right) {
            pf.vyr1 = ny1;
        }
        return;  // a pan frame owns the pointer
    }
    if (g.kind == PlotGesture::Kind::Pan && !pan_held) {
        g.kind = PlotGesture::Kind::None;
    }

    // The left-button gestures run to release wherever the pointer
    // goes (the press owns it): the band commits its box on release,
    // the axis zooms land live as the drag moves.
    if (g.kind == PlotGesture::Kind::Band ||
        g.kind == PlotGesture::Kind::ZoomX ||
        g.kind == PlotGesture::Kind::ZoomY) {
        if (in.mouse_down[ws::input::mouse::kLeft] == 0) {
            const bool was_band = g.kind == PlotGesture::Kind::Band;
            bool committed = false;
            if (was_band && std::fabs(in.mouse_x - g.from_x) >= 4.0f &&
                std::fabs(in.mouse_y - g.from_y) >= 4.0f) {
                // The box's corners map through each spine's OWN
                // transform (stage 6): one screen band, three windows
                // - x, the left spine's volts, the right spine's amps
                // - each clamped into its own home.
                const double x0 = pf.wx(g.from_x);
                const double x1 = pf.wx(in.mouse_x);
                const double y0 = pf.wy(g.from_y);
                const double y1 = pf.wy(in.mouse_y);
                const double y10 = pf.wy1(g.from_y);
                const double y11 = pf.wy1(in.mouse_y);
                const ws::circsim::PlotRange bx =
                    clamp_view({std::min(x0, x1), std::max(x0, x1)},
                               pf.xr);
                const ws::circsim::PlotRange by =
                    clamp_view({std::min(y0, y1), std::max(y0, y1)},
                               pf.yr);
                const ws::circsim::PlotRange by1 =
                    clamp_view({std::min(y10, y11),
                                std::max(y10, y11)}, pf.yr1);
                cache.view = PlotView{true, bx.lo, bx.hi, by.lo, by.hi,
                                      by1.lo, by1.hi};
                pf.vxr = bx;
                pf.vyr = by;
                if (pf.has_right) {
                    pf.vyr1 = by1;
                }
                committed = true;
            }
            g.kind = PlotGesture::Kind::None;
            if (!was_band || committed) {
                return;
            }
            // A tiny band's release falls through so the second press
            // of a double click still lands on the arming block below.
        } else {
            if (g.kind == PlotGesture::Kind::ZoomX) {
                // Dragging right stretches the axis: zoom in.
                const double f =
                    std::exp((static_cast<double>(in.mouse_x) - g.from_x) *
                             0.005);
                const ws::circsim::PlotRange nx =
                    zoom_range({g.x_lo, g.x_hi}, pf.xr, g.anchor_x, f);
                cache.view.active = true;
                cache.view.x_lo = nx.lo;
                cache.view.x_hi = nx.hi;
                pf.vxr = nx;
            } else if (g.kind == PlotGesture::Kind::ZoomY &&
                       g.zoom_axis == 0) {
                // Dragging up on the left strip zooms the volt window
                // in (pulling the range open reads like pulling the
                // axis up).
                const double f =
                    std::exp((g.from_y - static_cast<double>(in.mouse_y)) *
                             0.005);
                const ws::circsim::PlotRange ny =
                    zoom_range({g.y_lo, g.y_hi}, pf.yr, g.anchor_y, f);
                cache.view.active = true;
                cache.view.y_lo = ny.lo;
                cache.view.y_hi = ny.hi;
                pf.vyr = ny;
            } else if (g.kind == PlotGesture::Kind::ZoomY) {
                // The right strip's drag (stage 6): the amp window's
                // own zoom, same gesture language, its own anchor.
                const double f =
                    std::exp((g.from_y - static_cast<double>(in.mouse_y)) *
                             0.005);
                const ws::circsim::PlotRange ny1 =
                    zoom_range({g.y1_lo, g.y1_hi}, pf.yr1, g.anchor_y1,
                               f);
                cache.view.active = true;
                cache.view.y1_lo = ny1.lo;
                cache.view.y1_hi = ny1.hi;
                if (pf.has_right) {
                    pf.vyr1 = ny1;
                }
            }
            return;  // a gesture frame owns the pointer
        }
    }

    // Arming: the second click of a pair inside the frame resets the
    // view; the first click starts the band (frame) or an axis zoom
    // (strips - the left one owns the volt window, the right one the
    // amp window, stage 6).
    if (in.mouse_pressed[ws::input::mouse::kLeft] != 0 && over) {
        if (in_frame && g.click_clock < 0.35f &&
            std::fabs(in.mouse_x - g.click_x) < 5.0f &&
            std::fabs(in.mouse_y - g.click_y) < 5.0f) {
            cache.view = PlotView{};
            pf.vxr = pf.xr;
            pf.vyr = pf.yr;
            pf.vyr1 = pf.yr1;
            g.click_clock = 100.0f;
            return;
        }
        g.click_clock = 0.0f;
        g.click_x = in.mouse_x;
        g.click_y = in.mouse_y;
        g.from_x = in.mouse_x;
        g.from_y = in.mouse_y;
        g.x_lo = pf.vxr.lo;
        g.x_hi = pf.vxr.hi;
        g.y_lo = pf.vyr.lo;
        g.y_hi = pf.vyr.hi;
        g.y1_lo = pf.vyr1.lo;
        g.y1_hi = pf.vyr1.hi;
        g.was_active = cache.view.active;
        if (on_x_axis) {
            g.kind = PlotGesture::Kind::ZoomX;
            g.anchor_x = pf.wx(in.mouse_x);
            // The axis zooms write one axis over the other's landed
            // window: seed the manual view with the effective one
            // (the wheel and the pan seed the same way).
            if (!cache.view.active) {
                cache.view = PlotView{true, pf.vxr.lo, pf.vxr.hi,
                                      pf.vyr.lo, pf.vyr.hi,
                                      pf.vyr1.lo, pf.vyr1.hi};
            }
        } else if (on_y_axis) {
            g.kind = PlotGesture::Kind::ZoomY;
            g.zoom_axis = 0;
            g.anchor_y = pf.wy(in.mouse_y);
            if (!cache.view.active) {
                cache.view = PlotView{true, pf.vxr.lo, pf.vxr.hi,
                                      pf.vyr.lo, pf.vyr.hi,
                                      pf.vyr1.lo, pf.vyr1.hi};
            }
        } else if (on_y1_axis) {
            g.kind = PlotGesture::Kind::ZoomY;
            g.zoom_axis = 1;
            g.anchor_y1 = pf.wy1(in.mouse_y);
            if (!cache.view.active) {
                cache.view = PlotView{true, pf.vxr.lo, pf.vxr.hi,
                                      pf.vyr.lo, pf.vyr.hi,
                                      pf.vyr1.lo, pf.vyr1.hi};
            }
        } else if (in_frame) {
            g.kind = PlotGesture::Kind::Band;  // the box lands at release
        }
    }
}

void draw_plot(Editor& ed, gui::GuiContext& gui,
               gui::LayoutCursor& layout, const gui::ChromeTheme& theme,
               const ws::spice::Table& t, const char* x_name) {
    const gui::Rect plot =
        layout.next(layout.remaining_width(), kPlotHeight);

    // The two-axis split (stage 6) FIRST: whether the visible signals
    // earn a right spine decides the frame's right margin, so it is
    // plot geometry, not a draw detail. The classification is a
    // per-signal constant of the frame - the flags the mapping,
    // markers and legend all read.
    std::vector<std::uint8_t> on_right(t.signals.size(), 0u);
    bool has_right = false;
    for (std::size_t i = 0; i < t.signals.size(); ++i) {
        if (ws::circsim::signal_y_axis(t.signals[i].name) ==
            ws::circsim::PlotYAxis::Right) {
            on_right[i] = 1u;
            if (i < ed.results.visible.size() &&
                ed.results.visible[i] != 0u) {
                has_right = true;
            }
        }
    }
    const float mr = has_right ? kMarginRightAxis : kMarginRight;
    const float iw = plot.width - kMarginLeft - mr;
    const float ih = plot.height - kMarginTop - kMarginBottom;
    if (iw < 40.0f || ih < 30.0f) {
        return;  // too narrow to be worth a frame
    }
    const float fx = plot.x + kMarginLeft;
    const float fy = plot.y + kMarginTop;

    // The x axis: the analysis's own (step axes skipped), or the
    // sample index when the table has none.
    const std::size_t x_index =
        t.sweep_axes.empty()
            ? 0u
            : std::min(t.step_axes, t.sweep_axes.size() - 1u);
    const std::span<const double> xs = t.axis(x_index);
    const bool by_index = xs.empty();
    const std::size_t n = t.row_count();
    if (n == 0u) {
        return;
    }
    // The axis's sortedness is a per-table constant of the run (the
    // axis data is immutable once delivered), so it rides the cache
    // keyed to the table (ResultsCache::x_sorted_for) instead of the
    // per-frame walk strictly_increasing used to cost - the same key
    // shape `visible` and `view` keep. A delivery resets the key
    // (main.cpp's block beside visible_for): a same-indexed table of
    // a new run is a different axis.
    if (ed.results.x_sorted_for != ed.results.table) {
        ed.results.x_sorted = !by_index && strictly_increasing(xs);
        ed.results.x_sorted_for = ed.results.table;
    }

    // The home ranges: each y spine fits ITS OWN visible company
    // (stage 6 - the volts on the left, the amps on the right), x
    // over the axis (or the index run); padded by plot's expand_range
    // on all three. Home is the outermost view - every manual window
    // clamps inside its own spine's home.
    double y_lo = std::numeric_limits<double>::infinity();
    double y_hi = -std::numeric_limits<double>::infinity();
    double y1_lo = std::numeric_limits<double>::infinity();
    double y1_hi = -std::numeric_limits<double>::infinity();
    bool any_visible = false;
    double x_lo = 0.0;
    double x_hi = static_cast<double>(n - 1u);
    if (!by_index) {
        x_lo = xs.front();
        x_hi = xs.back();
    }
    for (std::size_t i = 0; i < t.signals.size(); ++i) {
        if (i >= ed.results.visible.size() || ed.results.visible[i] == 0u) {
            continue;
        }
        any_visible = true;
        const std::span<const double> ys = t.signals[i].data;
        const bool right = on_right[i] != 0u;
        for (std::size_t k = 0; k < ys.size() && k < n; ++k) {
            if (std::isfinite(ys[k])) {
                if (right) {
                    y1_lo = std::min(y1_lo, ys[k]);
                    y1_hi = std::max(y1_hi, ys[k]);
                } else {
                    y_lo = std::min(y_lo, ys[k]);
                    y_hi = std::max(y_hi, ys[k]);
                }
            }
        }
    }
    if (y_lo > y_hi) {
        y_lo = 0.0;  // nothing visible on the left: an empty-but-sane frame
        y_hi = 1.0;
    }
    if (y1_lo > y1_hi) {
        y1_lo = 0.0;  // same sane-empty answer for the right spine
        y1_hi = 1.0;
    }

    PlotFrame pf;
    pf.fx = fx;
    pf.fy = fy;
    pf.iw = iw;
    pf.ih = ih;
    pf.mr = mr;
    pf.has_right = has_right;
    pf.xr = ws::circsim::expand_range(x_lo, x_hi);
    pf.yr = ws::circsim::expand_range(y_lo, y_hi);
    pf.yr1 = ws::circsim::expand_range(y1_lo, y1_hi);
    handle_plot_input(ed, gui, pf);  // the hands; pf carries the effective view

    const auto sx = [&](double v) {
        return static_cast<float>(
            fx + (v - pf.vxr.lo) / (pf.vxr.hi - pf.vxr.lo) * iw);
    };
    const auto sy = [&](double v) {
        return static_cast<float>(
            fy + (pf.vyr.hi - v) / (pf.vyr.hi - pf.vyr.lo) * ih);
    };
    const auto sy1 = [&](double v) {
        return static_cast<float>(
            fy + (pf.vyr1.hi - v) / (pf.vyr1.hi - pf.vyr1.lo) * ih);
    };
    // A series maps through its OWN spine (stage 6): the flag is the
    // classifier's answer, the visibility gates the caller already ran.
    const auto sy_of = [&](std::size_t i, double v) {
        return on_right[i] != 0u ? sy1(v) : sy(v);
    };

    canvas::Canvas2D& rc = ed.results_draw_canvas();
    rc.set_view_scale(1.0f);  // identity: local == screen

    // --- the frame
    Paint frame_paint;
    frame_paint.width = 1.0f;
    frame_paint.color = gui::dim(theme.text, 0.35f);
    const Point quad[4] = {{fx, fy}, {fx + iw, fy}, {fx + iw, fy + ih},
                           {fx, fy + ih}};
    rc.stroke_polyline(quad, 4, frame_paint, true);

    // --- the grid + tick labels (over the EFFECTIVE view - a manual
    // window relabels its own axis)
    const std::vector<double> x_ticks =
        ws::circsim::nice_ticks(pf.vxr.lo, pf.vxr.hi, 6u);
    const std::vector<double> y_ticks =
        ws::circsim::nice_ticks(pf.vyr.lo, pf.vyr.hi, 5u);
    Paint grid_paint;
    grid_paint.width = 1.0f;
    grid_paint.color = gui::dim(theme.text, 0.12f);
    const ws::ui::r2d::Font* font = theme.font != nullptr ? theme.font
                                                          : ed.font;
    const bool can_label = font != nullptr && font->line_height() > 0.0f;
    for (double tick : x_ticks) {
        const float px = sx(tick);
        if (px < fx || px > fx + iw) {
            continue;
        }
        rc.stroke_line({px, fy}, {px, fy + ih}, grid_paint);
        if (can_label) {
            const std::string label = ws::circsim::format_eng(tick);
            const float w = rc.measure_text(label.c_str(), *font);
            (void)rc.draw_text(px - w * 0.5f, fy + ih + 2.0f,
                               label.c_str(), *font,
                               gui::dim(theme.text, 0.6f));
        }
    }
    for (double tick : y_ticks) {
        const float py = sy(tick);
        if (py < fy || py > fy + ih) {
            continue;
        }
        rc.stroke_line({fx, py}, {fx + iw, py}, grid_paint);
        if (can_label) {
            const std::string label = ws::circsim::format_eng(tick);
            const float w = rc.measure_text(label.c_str(), *font);
            (void)rc.draw_text(fx - w - 4.0f,
                               py - font->line_height() * 0.5f,
                               label.c_str(), *font,
                               gui::dim(theme.text, 0.6f));
        }
    }
    // The right spine's ticks (stage 6): marks on the frame's right
    // edge and labels in the widened margin - no grid lines, the left
    // axis's rows already carry the plot's horizontal rails, and a
    // second set would fight the series for them.
    if (has_right) {
        const std::vector<double> y1_ticks =
            ws::circsim::nice_ticks(pf.vyr1.lo, pf.vyr1.hi, 5u);
        for (double tick : y1_ticks) {
            const float py = sy1(tick);
            if (py < fy || py > fy + ih) {
                continue;
            }
            rc.stroke_line({fx + iw, py}, {fx + iw + 3.0f, py},
                           grid_paint);
            if (can_label) {
                const std::string label = ws::circsim::format_eng(tick);
                (void)rc.draw_text(fx + iw + 4.0f,
                                   py - font->line_height() * 0.5f,
                                   label.c_str(), *font,
                                   gui::dim(theme.text, 0.6f));
            }
        }
    }
    // The zero rail: a brighter line where the left spine crosses 0,
    // so a signal's sign reads without counting ticks.
    if (pf.vyr.lo < 0.0 && pf.vyr.hi > 0.0) {
        Paint zero_paint;
        zero_paint.width = 1.0f;
        zero_paint.color = gui::dim(theme.text, 0.32f);
        rc.stroke_line({fx, sy(0.0)}, {fx + iw, sy(0.0)}, zero_paint);
    }
    // The x axis's name, quiet, inside the frame's top-left.
    if (can_label) {
        (void)rc.draw_text(fx + 5.0f, fy + 3.0f, x_name, *font,
                           gui::dim(theme.text, 0.45f));
    }
    // Everything unchecked: say so instead of showing a mystery frame.
    if (!any_visible && can_label) {
        const char* msg = "all signals hidden - tick one in the legend";
        const float w = rc.measure_text(msg, *font);
        (void)rc.draw_text(fx + (iw - w) * 0.5f,
                           fy + (ih - font->line_height()) * 0.5f, msg,
                           *font, gui::dim(theme.text, 0.5f));
    }

    // --- the hover index (before the series, so the readout rows
    // below see this frame's answer)
    const ws::input::InputFrame& in = gui.input();
    const bool hovering = gui.claims_open() &&
                          in.mouse_x >= fx && in.mouse_x < fx + iw &&
                          in.mouse_y >= fy && in.mouse_y < fy + ih;
    // The x axis's sortedness: the cached per-table answer (computed
    // at the top of the draw, keyed like `visible`).
    const bool x_sorted = ed.results.x_sorted;
    if (hovering) {
        const double wx =
            pf.vxr.lo + (static_cast<double>(in.mouse_x) - fx) / iw *
                            (pf.vxr.hi - pf.vxr.lo);
        if (by_index) {
            // The axis IS the index: round and clamp, no scan.
            ed.results.hover = static_cast<int>(std::clamp<long long>(
                std::llround(wx), 0, static_cast<long long>(n) - 1));
        } else {
            ed.results.hover = nearest_sample(xs, wx, x_sorted, n);
        }
    } else {
        ed.results.hover = -1;
    }

    // --- the series
    const std::size_t budget =
        std::max<std::size_t>(64u, static_cast<std::size_t>(iw));
    for (std::size_t i = 0; i < t.signals.size(); ++i) {
        if (i >= ed.results.visible.size() || ed.results.visible[i] == 0u) {
            continue;
        }
        const std::span<const double> ys = t.signals[i].data;
        if (ys.empty()) {
            continue;
        }
        const auto x_at = [&](std::size_t k) {
            return by_index ? static_cast<double>(k)
                            : (k < xs.size() ? xs[k] : 0.0);
        };
        std::vector<Point> pts;
        const auto push = [&](std::size_t k) {
            if (k >= ys.size()) {
                return;
            }
            const double yv = ys[k];
            pts.push_back(Point{sx(x_at(k)),
                                std::isfinite(yv)
                                    ? sy_of(i, yv)
                                    : std::numeric_limits<float>::
                                          quiet_NaN()});
        };
        if (!by_index && x_sorted) {
            // The decimation window: the visible x slice (one sample
            // of slack each side), so a zoomed-in view spends the
            // pixel budget on the samples it shows. The auto view's
            // padded home spans all the data, so the slice is
            // transparent there - one code path, no special case.
            const std::size_t rows = std::min(n, ys.size());
            const auto b0 = std::lower_bound(xs.begin(),
                                             xs.begin() + rows, pf.vxr.lo);
            const auto b1 = std::upper_bound(xs.begin(),
                                             xs.begin() + rows, pf.vxr.hi);
            const std::size_t i0 =
                b0 != xs.begin()
                    ? static_cast<std::size_t>(b0 - xs.begin()) - 1u
                    : 0u;
            const std::size_t i1 =
                b1 != xs.begin() + rows
                    ? static_cast<std::size_t>(b1 - xs.begin()) + 1u
                    : rows;
            const std::vector<std::size_t> keep =
                ws::circsim::decimate_minmax(xs.subspan(i0, i1 - i0),
                                             ys.subspan(i0, i1 - i0),
                                             budget);
            if (keep.empty()) {
                for (std::size_t k = i0; k < i1; ++k) {
                    push(k);
                }
            } else {
                for (std::size_t k : keep) {
                    push(i0 + k);
                }
            }
        } else {
            for (std::size_t k = 0; k < n; ++k) {
                push(k);
            }
        }
        stroke_series(rc, pts, series_color(i));
    }

    // --- the crosshair + per-series markers at the hover sample
    if (ed.results.hover >= 0) {
        const std::size_t k = static_cast<std::size_t>(ed.results.hover);
        const double hx = by_index ? static_cast<double>(k)
                                   : (k < xs.size() ? xs[k] : 0.0);
        const float px = sx(hx);
        Paint cross;
        cross.width = 1.0f;
        cross.color = gui::dim(theme.text, 0.5f);
        cross.dash.count = 2;
        cross.dash.lengths[0] = 4.0f;
        cross.dash.lengths[1] = 3.0f;
        rc.stroke_line({px, fy}, {px, fy + ih}, cross);
        for (std::size_t i = 0; i < t.signals.size(); ++i) {
            if (i >= ed.results.visible.size() ||
                ed.results.visible[i] == 0u || k >= t.signals[i].data.size()) {
                continue;
            }
            const double yv = t.signals[i].data[k];
            if (std::isfinite(yv)) {
                rc.fill_circle({px, sy_of(i, yv)}, 3.0f, series_color(i));
            }
        }

        // The floating tooltip: x on top, then one colored row per
        // visible signal (capped, "+N more" past the cap). It flips to
        // the cursor's left near the right edge and stays inside the
        // frame; it hides while a gesture is in flight.
        if (can_label &&
            ed.results.gesture.kind == PlotGesture::Kind::None) {
            struct Row {
                std::string text;
                std::uint32_t color;  // 0 = no swatch
            };
            constexpr std::size_t kMaxRows = 8;
            std::vector<Row> rows;
            std::size_t overflow = 0;
            rows.push_back(Row{std::string(x_name) + " = " +
                                   ws::circsim::format_eng(hx, 5),
                               0u});
            for (std::size_t i = 0; i < t.signals.size(); ++i) {
                if (i >= ed.results.visible.size() ||
                    ed.results.visible[i] == 0u ||
                    k >= t.signals[i].data.size() ||
                    !std::isfinite(t.signals[i].data[k])) {
                    continue;
                }
                if (rows.size() > kMaxRows) {
                    ++overflow;
                    continue;
                }
                rows.push_back(Row{
                    t.signals[i].name + "  " +
                        with_unit(t.signals[i].data[k],
                                  unit_of(t.signals[i].name)),
                    series_color(i)});
            }
            if (overflow > 0u) {
                rows.push_back(
                    Row{"+" + std::to_string(overflow) + " more", 0u});
            }
            const float lh = font->line_height();
            float text_w = 0.0f;
            for (const Row& r : rows) {
                text_w = std::max(text_w,
                                  rc.measure_text(r.text.c_str(), *font));
            }
            const float bw = text_w + 26.0f;
            const float bh = lh * static_cast<float>(rows.size()) + 10.0f;
            float bx = px + 12.0f;
            if (bx + bw > fx + iw) {
                bx = px - 12.0f - bw;
            }
            bx = std::max(bx, fx);
            float by = fy + 6.0f;
            by = std::max(fy, std::min(by, fy + ih - bh));
            rc.fill_rounded_rect(bx, by, bw, bh, 4.0f,
                                 ws::ui::r2d::rgba(18, 20, 26, 232));
            const Point tip[4] = {{bx, by}, {bx + bw, by},
                                  {bx + bw, by + bh}, {bx, by + bh}};
            Paint tip_border;
            tip_border.width = 1.0f;
            tip_border.color = ws::ui::r2d::rgba(255, 255, 255, 40);
            rc.stroke_polyline(tip, 4, tip_border, true);
            for (std::size_t r = 0; r < rows.size(); ++r) {
                const float ty = by + 5.0f + lh * static_cast<float>(r);
                if (rows[r].color != 0u) {
                    rc.fill_circle({bx + 10.0f, ty + lh * 0.5f}, 3.0f,
                                   rows[r].color);
                }
                (void)rc.draw_text(
                    bx + 18.0f, ty, rows[r].text.c_str(), *font,
                    rows[r].color != 0u
                        ? ws::ui::r2d::rgba(236, 239, 246)
                        : ws::ui::r2d::rgba(160, 166, 180));
            }
        }
    }

    // --- the playhead: the transient the canvas animates, swept here
    // too (patch 0012). The transport row below and the canvas strip
    // scrub the same playback.t - one clock, so the plot's cursor and
    // the schematic's LED glow can never disagree. A time-swept table
    // only; a pointer zoomed away from the playhead simply loses the
    // line (the view is the truth, the line never stretches to it).
    if (!by_index && t.sweep == ws::spice::SweepKind::Time) {
        const double ph = ed.playback.t;
        if (ph >= pf.vxr.lo && ph <= pf.vxr.hi) {
            const float px = sx(ph);
            Paint sweep;
            sweep.width = 1.5f;
            sweep.color = theme.control_on;
            rc.stroke_line({px, fy}, {px, fy + ih}, sweep);
            const Point cap[3] = {{px - 4.0f, fy},
                                  {px + 4.0f, fy},
                                  {px, fy + 6.0f}};
            rc.fill_polygon(cap, 3, theme.control_on);
        }
    }

    // --- the in-flight band + the manual-view hint -------------------
    const PlotGesture& g = ed.results.gesture;
    if (g.kind == PlotGesture::Kind::Band) {
        const float bx0 = std::min(g.from_x, in.mouse_x);
        const float bx1 = std::max(g.from_x, in.mouse_x);
        const float by0 = std::min(g.from_y, in.mouse_y);
        const float by1 = std::max(g.from_y, in.mouse_y);
        const Point box[4] = {{bx0, by0}, {bx1, by0}, {bx1, by1},
                              {bx0, by1}};
        const std::uint32_t accent = theme.control_on;
        rc.fill_polygon(box, 4, with_alpha(accent, 36));
        Paint band;
        band.width = 1.0f;
        band.color = with_alpha(accent, 200);
        rc.stroke_polyline(box, 4, band, true);
        if (can_label) {
            // The cursor reads the band exists for: the data-space
            // deltas of its two corners - dx on the sweep, dy on the
            // LEFT spine (the primary scale; the right spine's units
            // ride its own legend readouts).
            const double dx = std::fabs(pf.wx(in.mouse_x) -
                                        pf.wx(g.from_x));
            const double dy = std::fabs(pf.wy(in.mouse_y) -
                                        pf.wy(g.from_y));
            char readout[96];
            std::snprintf(readout, sizeof readout, "dx %s  dy %s",
                          ws::circsim::format_eng(dx).c_str(),
                          ws::circsim::format_eng(dy).c_str());
            (void)rc.draw_text(bx0 + 4.0f, by0 + 2.0f, readout, *font,
                               gui::dim(theme.text, 0.75f));
        }
    } else if (ed.results.view.active && can_label) {
        const char* hint = "double-click resets";
        const float w = rc.measure_text(hint, *font);
        (void)rc.draw_text(fx + iw - w - 5.0f, fy + 3.0f, hint, *font,
                           gui::dim(theme.text, 0.35f));
    }
}

// --- the value rows (.op tables, measurements) ----------------------------------

void draw_value_rows(Editor& ed, gui::LayoutCursor& layout,
                     const gui::ChromeTheme& theme,
                     const std::vector<ValueRow>& rows) {
    const ws::ui::r2d::Font* font =
        ed.font_mono != nullptr && !ed.font_mono->empty() ? ed.font_mono
                                                          : theme.font;
    if (font == nullptr || font->line_height() <= 0.0f) {
        // The fontless fallback: plain text rows.
        for (const ValueRow& r : rows) {
            const std::string line =
                r.name + " = " + with_unit(r.value, r.unit, 5);
            gui::text(layout, theme, line.c_str());
        }
        return;
    }
    // Two aligned columns - names left, values on a common edge - so a
    // long .op printout scans like a table instead of ragged "a = b" text.
    canvas::Canvas2D& rc = ed.results_draw_canvas();
    rc.set_view_scale(1.0f);
    float name_w = 0.0f;
    for (const ValueRow& r : rows) {
        name_w = std::max(name_w, rc.measure_text(r.name.c_str(), *font));
    }
    const float col = name_w + 14.0f;
    const float line_h = font->line_height() + 3.0f;
    for (const ValueRow& r : rows) {
        const gui::Rect row = layout.next(layout.remaining_width(), line_h);
        (void)ws::ui::r2d::draw_text(row.x, row.y, r.name.c_str(), *font,
                                     gui::dim(theme.text, 0.65f));
        const std::string value = with_unit(r.value, r.unit, 5);
        (void)ws::ui::r2d::draw_text(row.x + col, row.y, value.c_str(),
                                     *font, gui::dim(theme.text, 0.95f));
    }
}

// The transient's transport row (patch 0012): the SAME playback the
// canvas strip wears - play/pause, a scrub track, the SI readout and
// the rate chip. One clock: a scrub here animates the schematic, a
// scrub on the canvas sweeps this plot's playhead line, and the two
// strips can never disagree because there is only one field. Drawn
// only under a time-swept table (the .op shape has nothing to sweep).
void draw_transport_row(Editor& ed, gui::GuiContext& gui,
                        gui::LayoutCursor& layout,
                        const gui::ChromeTheme& theme,
                        const ws::spice::Table& t) {
    const ws::circsim::OverlayWindow w = ws::circsim::overlay_window(t);
    if (!(w.t1 > w.t0)) {
        return;  // a degenerate transient has nothing to sweep
    }

    // Play/pause: a paused playhead at the run's end restarts (the
    // canvas strip's own rule - the settled state stays one press
    // away from the sweep).
    if (gui::button(gui, layout, theme,
                    ed.playback.playing ? "pause" : "play", 56.0f)) {
        if (!ed.playback.playing && ed.playback.t >= w.t1) {
            ed.playback.t = w.t0;
        }
        ed.playback.playing = !ed.playback.playing;
    }
    layout.same_line();

    // The scrub track: custom chrome over the strip's pointer rule -
    // a press inside arms, the drag owns the pointer until release.
    const float track_w =
        std::max(60.0f, layout.remaining_width() - 170.0f);
    const gui::Rect track = layout.next(track_w, 20.0f);
    const ws::input::InputFrame& in = gui.input();
    const auto frac_of = [&](float mx) {
        return std::clamp(
            (mx - track.x) / static_cast<double>(track.width), 0.0, 1.0);
    };
    const auto over_track = [&]() {
        return gui.claims_open() && in.mouse_x >= track.x &&
               in.mouse_x < track.x + track.width && in.mouse_y >= track.y &&
               in.mouse_y < track.y + track.height;
    };
    if (ed.results_scrub) {
        if (in.mouse_down[ws::input::mouse::kLeft] == 0) {
            ed.results_scrub = false;  // release: the drag lets go
        } else {
            ed.playback.t = ws::circsim::overlay_scrub(w, frac_of(in.mouse_x));
        }
    } else if (in.mouse_pressed[ws::input::mouse::kLeft] != 0 &&
               over_track()) {
        ed.results_scrub = true;
        ed.playback.t = ws::circsim::overlay_scrub(w, frac_of(in.mouse_x));
    }

    // The track's face: the rail, the played span, the knob - the
    // canvas strip's look, at the row's own scale.
    canvas::Canvas2D& rc = ed.results_draw_canvas();
    rc.set_view_scale(1.0f);
    const double frac =
        std::clamp((ed.playback.t - w.t0) / (w.t1 - w.t0), 0.0, 1.0);
    const float ty = track.y + track.height * 0.5f - 1.0f;
    const Point rail{track.x, ty};
    rc.fill_rects(&rail, 1, track.width, 2.0f, gui::dim(theme.text, 0.30f));
    const Point played{track.x, ty};
    rc.fill_rects(&played, 1, static_cast<float>(track.width * frac), 2.0f,
                  theme.control_on);
    rc.fill_circle(Point{track.x + static_cast<float>(track.width * frac),
                         track.y + track.height * 0.5f},
                   4.0f, theme.control_on);

    // The readout, then the rate chip (the ladder's cycle - the same
    // field the canvas chip walks).
    layout.same_line();
    std::string label = ws::circsim::format_eng(ed.playback.t);
    label += 's';
    gui::text(layout, theme, label.c_str());
    layout.same_line();
    if (gui::button(gui, layout, theme,
                    ws::circsim::overlay_rate_label(ed.playback.rate),
                    52.0f)) {
        ed.playback.rate = ws::circsim::overlay_rate_next(ed.playback.rate);
    }
}

}  // namespace

// --- the panel -----------------------------------------------------------------

void draw_results_panel(Editor& ed, gui::GuiContext& gui,
                        gui::LayoutCursor& layout,
                        const gui::ChromeTheme& theme) {
    if (ed.doc == nullptr || ed.schem() == nullptr) {
        gui::text(layout, theme, "No document open");
        return;
    }
    if (!ed.results.has) {
        gui::text(layout, theme,
                  "No results yet - run a simulation with Ctrl+Enter "
                  "(Circuit > Simulate)");
        return;
    }
    const ws::spice::Results& res = ed.results.run.results;
    if (res.tables.empty()) {
        gui::text(layout, theme,
                  "The run produced no results - see the Problems panel");
        return;
    }
    if (ed.results.table >= res.tables.size()) {
        ed.results.table = 0;
    }
    const ws::spice::Table& t = res.tables[ed.results.table];
    sync_visibility(ed, t);

    const bool stale = ed.results.version != ed.schem()->version();

    // --- the header: table combo + the export pair
    {
        std::vector<std::string> labels;
        labels.reserve(res.tables.size());
        for (const ws::spice::Table& table : res.tables) {
            labels.push_back(analysis_name(table.analysis));
        }
        std::vector<const char*> items;
        items.reserve(labels.size());
        for (const std::string& label : labels) {
            items.push_back(label.c_str());
        }
        FieldBuf& combo = ed.fields["results.table"];
        if (combo.stamp != ed.field_stamp) {
            combo.stamp = ed.field_stamp;
            combo.combo_open = false;
        }
        int picked = static_cast<int>(ed.results.table);
        const float combo_w =
            std::max(80.0f, layout.remaining_width() - 190.0f);
        if (gui::combo(gui, layout, theme, "table", items.data(),
                       static_cast<int>(items.size()), picked,
                       combo.combo_open, combo_w)) {
            if (picked >= 0 &&
                static_cast<std::size_t>(picked) < res.tables.size()) {
                ed.results.table = static_cast<std::size_t>(picked);
            }
        }
        layout.same_line();
        if (gui::button(gui, layout, theme, "Export CSV", 84.0f) &&
            ed.export_results) {
            ed.export_results(false);
        }
        layout.same_line();
        if (gui::button(gui, layout, theme, "Export JSON", 84.0f) &&
            ed.export_results) {
            ed.export_results(true);
        }
        if (stale) {
            gui::ChromeTheme warn = theme;
            warn.text = gui::dim(theme.toast_warning, 0.95f);
            gui::text(layout, warn,
                      "Out of date - the schematic changed since the run (Ctrl+Enter re-runs)");
        }
    }

    // --- the selected table
    if (t.sweep == ws::spice::SweepKind::None) {
        // The .op shape: one value row per signal, no plot.
        gui::separator(layout, theme);
        std::vector<ValueRow> rows;
        rows.reserve(t.signals.size());
        for (const ws::spice::Signal& s : t.signals) {
            if (!s.data.empty()) {
                rows.push_back(
                    ValueRow{s.name, s.data.front(), unit_of(s.name)});
            }
        }
        draw_value_rows(ed, layout, theme, rows);
    } else {
        const std::size_t x_index =
            t.sweep_axes.empty()
                ? 0u
                : std::min(t.step_axes, t.sweep_axes.size() - 1u);
        const char* x_name =
            !t.sweep_names.empty() &&
                    x_index < t.sweep_names.size() &&
                    !t.sweep_names[x_index].empty()
                ? t.sweep_names[x_index].c_str()
                : "sample";

        // The legend: swatch + visibility + the spine tag + the live
        // readout (the hover sample, else the last one - the
        // waveform's resting state is the readout people want at a
        // glance). The tag (stage 6) is which y spine the signal
        // rides, spelled in units: V on the left scale, A on the
        // right one - the classifier's answer, quiet, so a mixed
        // legend reads its split without opening the plot.
        gui::separator(layout, theme);
        gui::ChromeTheme quiet = theme;
        quiet.text = gui::dim(theme.text, 0.45f);
        // The toolbar: bulk visibility, the discoverable "fit view"
        // (the double-click still works), and the hover x readout.
        if (gui::button(gui, layout, theme, "Show all", 64.0f)) {
            ed.results.visible.assign(t.signals.size(), 1u);
        }
        layout.same_line();
        if (gui::button(gui, layout, theme, "Hide all", 64.0f)) {
            ed.results.visible.assign(t.signals.size(), 0u);
        }
        if (ed.results.view.active) {
            layout.same_line();
            if (gui::button(gui, layout, theme, "Fit view", 64.0f)) {
                ed.results.view = PlotView{};
            }
        }
        if (ed.results.hover >= 0) {
            const std::span<const double> hxs = t.axis(x_index);
            const std::size_t hk =
                static_cast<std::size_t>(ed.results.hover);
            const double hx = hxs.empty() ? static_cast<double>(hk)
                                          : (hk < hxs.size() ? hxs[hk] : 0.0);
            char xline[128];
            std::snprintf(xline, sizeof xline, "%s = %s  (sample %d)",
                          x_name, ws::circsim::format_eng(hx, 5).c_str(),
                          ed.results.hover);
            layout.same_line();
            gui::text(layout, quiet, xline);
        }
        const int at = ed.results.hover >= 0
                           ? ed.results.hover
                           : static_cast<int>(t.row_count()) - 1;
        for (std::size_t i = 0; i < t.signals.size(); ++i) {
            const ws::spice::Signal& s = t.signals[i];
            gui.ids().push(static_cast<int>(i));
            const gui::Rect swatch = layout.next(14.0f, 14.0f);
            canvas::Canvas2D& sw = ed.results_draw_canvas();
            sw.set_view_scale(1.0f);
            sw.fill_rounded_rect(swatch.x, swatch.y + 3.0f, 12.0f,
                                 12.0f, 2.0f, series_color(i));
            layout.same_line(6.0f);
            bool vis = i < ed.results.visible.size()
                           ? ed.results.visible[i] != 0u
                           : true;
            if (gui::checkbox(gui, layout, theme, s.name.c_str(), vis)) {
                if (ed.results.visible.size() == t.signals.size()) {
                    ed.results.visible[i] = vis ? 1u : 0u;
                }
            }
            layout.same_line();
            gui::text(layout, quiet,
                      ws::circsim::signal_y_axis(s.name) ==
                              ws::circsim::PlotYAxis::Right
                          ? "A"
                          : "V",
                      14.0f);
            layout.same_line();
            const gui::Rect spark = layout.next(48.0f, 14.0f);
            draw_sparkline(sw, spark, s.data, series_color(i),
                           ed.results.hover);
            if (at >= 0 && static_cast<std::size_t>(at) < s.data.size()) {
                layout.same_line();
                const std::string value = with_unit(
                    s.data[static_cast<std::size_t>(at)], unit_of(s.name));
                gui::text(layout, theme,
                          (ed.results.hover >= 0 ? value : value + "  (end)")
                              .c_str());
            }
            gui.ids().pop();
        }

        gui::separator(layout, theme);
        draw_plot(ed, gui, layout, theme, t, x_name);
        draw_transport_row(ed, gui, layout, theme, t);
        gui::text(layout, quiet,
                  "wheel zoom | drag box | drag axis | space+drag pan | "
                  "dbl-click fit | right-click cancel");
    }

    // --- the measurements
    if (!ed.results.run.measurements.empty()) {
        gui::separator(layout, theme);
        gui::text(layout, theme, "Measurements");
        const ws::ui::r2d::Font* font =
            ed.font_mono != nullptr && !ed.font_mono->empty()
                ? ed.font_mono
                : theme.font;
        int index = 0;
        for (const ws::spice::Measurement& m :
             ed.results.run.measurements) {
            gui.ids().push(index);
            char line[192];
            if (m.value.has_value()) {
                std::snprintf(line, sizeof line, "%s = %s",
                              m.name.c_str(),
                              ws::circsim::format_eng(*m.value, 5).c_str());
            } else {
                std::snprintf(line, sizeof line, "%s: failed - %s",
                              m.name.c_str(), m.note.c_str());
            }
            if (font != nullptr && font->line_height() > 0.0f) {
                const float line_h = font->line_height() + 2.0f;
                const gui::Rect row =
                    layout.next(layout.remaining_width(), line_h);
                (void)ws::ui::r2d::draw_text(
                    row.x, row.y, line, *font,
                    m.value.has_value()
                        ? gui::dim(theme.text, 0.85f)
                        : gui::dim(theme.toast_warning, 0.95f));
            } else {
                gui::text(layout, theme, line);
            }
            gui.ids().pop();
            ++index;
        }
    }
}

}  // namespace editor
