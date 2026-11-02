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

#include "results_panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>
#include <span>

#include <ws/input/input.hpp>
#include <ws/spice/results.hpp>
#include <ws/ui/renderer2d.hpp>

namespace editor {
namespace {

namespace canvas = ws::ui::canvas;
using canvas::Paint;
using canvas::Point;

// The series palette: data colors, not chrome - a fixed wheel the
// legend swatches and the polylines share (i mod N, like every scope).
constexpr std::uint32_t kSeries[] = {
    ws::ui::r2d::rgba(224, 90, 78),    // red
    ws::ui::r2d::rgba(78, 161, 224),   // blue
    ws::ui::r2d::rgba(88, 181, 92),    // green
    ws::ui::r2d::rgba(211, 154, 42),   // amber
    ws::ui::r2d::rgba(154, 111, 211),  // purple
    ws::ui::r2d::rgba(78, 201, 192),   // teal
    ws::ui::r2d::rgba(211, 90, 158),   // pink
    ws::ui::r2d::rgba(143, 154, 78),   // olive
};
constexpr std::size_t kSeriesCount = sizeof kSeries / sizeof kSeries[0];

[[nodiscard]] std::uint32_t series_color(std::size_t i) {
    return kSeries[i % kSeriesCount];
}

[[nodiscard]] const char* analysis_name(ws::spice::AnalysisKind kind) {
    switch (kind) {
        case ws::spice::AnalysisKind::Op: return ".op";
        case ws::spice::AnalysisKind::Dc: return ".dc";
        case ws::spice::AnalysisKind::Tran: return ".tran";
        case ws::spice::AnalysisKind::Ac: return ".ac";
    }
    return "?";
}

// Keeps the per-signal visibility flags aligned with the table being
// shown (all-visible on a table switch; the flags survive frames).
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
                   const std::vector<Point>& pts, std::uint32_t color) {
    Paint paint;
    paint.width = 2.0f;
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
// interaction geometry, not just spacing.
constexpr float kPlotHeight = 220.0f;
constexpr float kMarginLeft = 46.0f;
constexpr float kMarginBottom = 17.0f;
constexpr float kMarginTop = 6.0f;
constexpr float kMarginRight = 10.0f;

// The band's fill/outline: the theme's accent, alpha-split (a wash
// under the series, a line around the box). r2d packs 0xRRGGBBAA, so
// the alpha is the low byte.
[[nodiscard]] std::uint32_t with_alpha(std::uint32_t color,
                                       std::uint8_t a) {
    return (color & 0xFFFFFF00u) | std::uint32_t(a);
}

// The frame geometry plus the two range pairs the interaction pass
// works on: home (the auto fit - the outermost view) and the
// effective view (the manual window riding over it). The input pass
// mutates the effective pair; the draw pass reads it. wx/wy map
// screen px to data through the CURRENT effective view.
struct PlotFrame {
    float fx = 0.0f, fy = 0.0f, iw = 0.0f, ih = 0.0f;
    ws::circsim::PlotRange xr{};   // home x
    ws::circsim::PlotRange yr{};   // home y
    ws::circsim::PlotRange vxr{};  // effective x
    ws::circsim::PlotRange vyr{};  // effective y

    [[nodiscard]] double wx(float px) const {
        return vxr.lo + (static_cast<double>(px) - fx) / iw *
                            (vxr.hi - vxr.lo);
    }
    [[nodiscard]] double wy(float py) const {
        return vyr.hi - (static_cast<double>(py) - fy) / ih *
                            (vyr.hi - vyr.lo);
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

    // The effective view: the manual window clamped into home, or
    // home itself.
    pf.vxr = cache.view.active
                 ? clamp_view({cache.view.x_lo, cache.view.x_hi}, pf.xr)
                 : pf.xr;
    pf.vyr = cache.view.active
                 ? clamp_view({cache.view.y_lo, cache.view.y_hi}, pf.yr)
                 : pf.yr;

    // The claimable rect: frame + both strips, so the wheel works over
    // the axes too. The innermost rect beats the scroll panel's claim;
    // the win settles one frame later (the gui's contract), which is
    // why the consumption gates on wheel_won - the first notch after
    // arriving over the plot still scrolls the panel.
    const gui::Rect claimable{
        pf.fx - kMarginLeft, pf.fy - kMarginTop,
        pf.iw + kMarginLeft + kMarginRight,
        pf.ih + kMarginTop + kMarginBottom};
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
    const bool over = in_frame || on_x_axis || on_y_axis;

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
    // press-time view exactly.
    if (g.kind != PlotGesture::Kind::None && in.mouse_pressed[2] != 0) {
        cache.view.active = g.was_active;
        if (g.was_active) {
            cache.view.x_lo = g.x_lo;
            cache.view.x_hi = g.x_hi;
            cache.view.y_lo = g.y_lo;
            cache.view.y_hi = g.y_hi;
        }
        g.kind = PlotGesture::Kind::None;
        return;
    }

    // The wheel: zoom both axes toward the cursor at the canvas's
    // 1.15-per-notch factor - but never mid-gesture (the band's press
    // point is captured in the view it started in).
    if (in_frame && in.scroll_dy != 0.0f &&
        g.kind == PlotGesture::Kind::None &&
        gui.wheel_won(&cache.view)) {
        if (!cache.view.active) {
            cache.view = PlotView{true, pf.vxr.lo, pf.vxr.hi,
                                  pf.vyr.lo, pf.vyr.hi};
        }
        const double wx = pf.wx(in.mouse_x);
        const double wy = pf.wy(in.mouse_y);
        const double f = in.scroll_dy > 0.0f ? 1.15 : 1.0 / 1.15;
        const ws::circsim::PlotRange nx =
            zoom_range({cache.view.x_lo, cache.view.x_hi}, pf.xr, wx, f);
        const ws::circsim::PlotRange ny =
            zoom_range({cache.view.y_lo, cache.view.y_hi}, pf.yr, wy, f);
        cache.view.x_lo = nx.lo;
        cache.view.x_hi = nx.hi;
        cache.view.y_lo = ny.lo;
        cache.view.y_hi = ny.hi;
        pf.vxr = nx;
        pf.vyr = ny;
    }

    // The pan: middle-drag, or space+left-drag (the Schematic panel's
    // rule). Content follows the cursor: dragging right slides the
    // window left, dragging down slides it up (the y flip).
    const bool space_down = in.key_down[ws::input::keys::kSpace] != 0;
    const bool pan_held =
        in.mouse_down[1] != 0 || (space_down && in.mouse_down[0] != 0);
    if (pan_held && (in_frame || g.kind == PlotGesture::Kind::Pan)) {
        if (g.kind != PlotGesture::Kind::Pan) {
            g.kind = PlotGesture::Kind::Pan;
            g.from_x = in.mouse_x;
            g.from_y = in.mouse_y;
            g.x_lo = pf.vxr.lo;
            g.x_hi = pf.vxr.hi;
            g.y_lo = pf.vyr.lo;
            g.y_hi = pf.vyr.hi;
            g.was_active = cache.view.active;
            if (!cache.view.active) {
                cache.view = PlotView{true, pf.vxr.lo, pf.vxr.hi,
                                      pf.vyr.lo, pf.vyr.hi};
            }
        }
        const double dx =
            -(static_cast<double>(in.mouse_x - g.from_x) /
              static_cast<double>(pf.iw)) *
            (g.x_hi - g.x_lo);
        const double dy =
            (static_cast<double>(in.mouse_y - g.from_y) /
             static_cast<double>(pf.ih)) *
            (g.y_hi - g.y_lo);
        const ws::circsim::PlotRange nx =
            pan_range({g.x_lo, g.x_hi}, pf.xr, dx);
        const ws::circsim::PlotRange ny =
            pan_range({g.y_lo, g.y_hi}, pf.yr, dy);
        cache.view.x_lo = nx.lo;
        cache.view.x_hi = nx.hi;
        cache.view.y_lo = ny.lo;
        cache.view.y_hi = ny.hi;
        pf.vxr = nx;
        pf.vyr = ny;
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
        if (in.mouse_down[0] == 0) {
            const bool was_band = g.kind == PlotGesture::Kind::Band;
            bool committed = false;
            if (was_band && std::fabs(in.mouse_x - g.from_x) >= 4.0f &&
                std::fabs(in.mouse_y - g.from_y) >= 4.0f) {
                const double x0 = pf.wx(g.from_x);
                const double x1 = pf.wx(in.mouse_x);
                const double y0 = pf.wy(g.from_y);
                const double y1 = pf.wy(in.mouse_y);
                const ws::circsim::PlotRange bx =
                    clamp_view({std::min(x0, x1), std::max(x0, x1)},
                               pf.xr);
                const ws::circsim::PlotRange by =
                    clamp_view({std::min(y0, y1), std::max(y0, y1)},
                               pf.yr);
                cache.view = PlotView{true, bx.lo, bx.hi, by.lo, by.hi};
                pf.vxr = bx;
                pf.vyr = by;
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
            } else if (g.kind == PlotGesture::Kind::ZoomY) {
                // Dragging up on the strip zooms in (pulling the range
                // open reads like pulling the axis up).
                const double f =
                    std::exp((g.from_y - static_cast<double>(in.mouse_y)) *
                             0.005);
                const ws::circsim::PlotRange ny =
                    zoom_range({g.y_lo, g.y_hi}, pf.yr, g.anchor_y, f);
                cache.view.active = true;
                cache.view.y_lo = ny.lo;
                cache.view.y_hi = ny.hi;
                pf.vyr = ny;
            }
            return;  // a gesture frame owns the pointer
        }
    }

    // Arming: the second click of a pair inside the frame resets the
    // view; the first click starts the band (frame) or an axis zoom
    // (strips).
    if (in.mouse_pressed[0] != 0 && over) {
        if (in_frame && g.click_clock < 0.35f &&
            std::fabs(in.mouse_x - g.click_x) < 5.0f &&
            std::fabs(in.mouse_y - g.click_y) < 5.0f) {
            cache.view = PlotView{};
            pf.vxr = pf.xr;
            pf.vyr = pf.yr;
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
        g.was_active = cache.view.active;
        if (on_x_axis) {
            g.kind = PlotGesture::Kind::ZoomX;
            g.anchor_x = pf.wx(in.mouse_x);
            // The axis zooms write one axis over the other's landed
            // window: seed the manual view with the effective one
            // (the wheel and the pan seed the same way).
            if (!cache.view.active) {
                cache.view = PlotView{true, pf.vxr.lo, pf.vxr.hi,
                                      pf.vyr.lo, pf.vyr.hi};
            }
        } else if (on_y_axis) {
            g.kind = PlotGesture::Kind::ZoomY;
            g.anchor_y = pf.wy(in.mouse_y);
            if (!cache.view.active) {
                cache.view = PlotView{true, pf.vxr.lo, pf.vxr.hi,
                                      pf.vyr.lo, pf.vyr.hi};
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
    const float iw = plot.width - kMarginLeft - kMarginRight;
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

    // The home ranges: y over the VISIBLE signals, x over the axis
    // (or the index run); padded by plot's expand_range on both. Home
    // is the outermost view - every manual window clamps inside it.
    double y_lo = std::numeric_limits<double>::infinity();
    double y_hi = -std::numeric_limits<double>::infinity();
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
        const std::span<const double> ys = t.signals[i].data;
        for (std::size_t k = 0; k < ys.size() && k < n; ++k) {
            if (std::isfinite(ys[k])) {
                y_lo = std::min(y_lo, ys[k]);
                y_hi = std::max(y_hi, ys[k]);
            }
        }
    }
    if (y_lo > y_hi) {
        y_lo = 0.0;  // nothing visible: an empty-but-sane frame
        y_hi = 1.0;
    }

    PlotFrame pf;
    pf.fx = fx;
    pf.fy = fy;
    pf.iw = iw;
    pf.ih = ih;
    pf.xr = ws::circsim::expand_range(x_lo, x_hi);
    pf.yr = ws::circsim::expand_range(y_lo, y_hi);
    handle_plot_input(ed, gui, pf);  // the hands; pf carries the effective view

    const auto sx = [&](double v) {
        return static_cast<float>(
            fx + (v - pf.vxr.lo) / (pf.vxr.hi - pf.vxr.lo) * iw);
    };
    const auto sy = [&](double v) {
        return static_cast<float>(
            fy + (pf.vyr.hi - v) / (pf.vyr.hi - pf.vyr.lo) * ih);
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
    // The x axis's name, quiet, inside the frame's top-left.
    if (can_label) {
        (void)rc.draw_text(fx + 5.0f, fy + 3.0f, x_name, *font,
                           gui::dim(theme.text, 0.45f));
    }

    // --- the hover index (before the series, so the readout rows
    // below see this frame's answer)
    const ws::input::InputFrame& in = gui.input();
    const bool hovering = gui.claims_open() &&
                          in.mouse_x >= fx && in.mouse_x < fx + iw &&
                          in.mouse_y >= fy && in.mouse_y < fy + ih;
    const bool x_sorted = !by_index && strictly_increasing(xs);
    if (hovering) {
        const double wx =
            pf.vxr.lo + (static_cast<double>(in.mouse_x) - fx) / iw *
                            (pf.vxr.hi - pf.vxr.lo);
        if (by_index) {
            std::vector<double> idx;
            idx.reserve(n);
            for (std::size_t k = 0; k < n; ++k) {
                idx.push_back(static_cast<double>(k));
            }
            ed.results.hover = nearest_sample(idx, wx, true, n);
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
                                    ? sy(yv)
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
                rc.fill_circle({px, sy(yv)}, 3.0f, series_color(i));
            }
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
            // deltas of its two corners, the scope's pair.
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
                     const std::vector<std::pair<std::string, double>>& rows) {
    const ws::ui::r2d::Font* font =
        ed.font_mono != nullptr && !ed.font_mono->empty() ? ed.font_mono
                                                          : theme.font;
    if (font == nullptr || font->line_height() <= 0.0f) {
        // The fontless fallback: plain text rows.
        for (const auto& [name, value] : rows) {
            char line[192];
            std::snprintf(line, sizeof line, "%s = %s", name.c_str(),
                          ws::circsim::format_eng(value).c_str());
            gui::text(layout, theme, line);
        }
        return;
    }
    const float line_h = font->line_height() + 2.0f;
    for (const auto& [name, value] : rows) {
        const gui::Rect row = layout.next(layout.remaining_width(), line_h);
        char line[192];
        std::snprintf(line, sizeof line, "%s = %s", name.c_str(),
                      ws::circsim::format_eng(value, 5).c_str());
        (void)ws::ui::r2d::draw_text(row.x, row.y, line, *font,
                                     gui::dim(theme.text, 0.85f));
    }
}

}  // namespace

// --- the panel -----------------------------------------------------------------

void draw_results_panel(Editor& ed, gui::GuiContext& gui,
                        gui::LayoutCursor& layout,
                        const gui::ChromeTheme& theme) {
    if (ed.doc == nullptr || ed.schem() == nullptr) {
        gui::text(layout, theme, "no document");
        return;
    }
    if (!ed.results.has) {
        gui::text(layout, theme,
                  "no results yet - Circuit > Simulate (Ctrl+Enter) runs one");
        return;
    }
    const ws::spice::Results& res = ed.results.run.results;
    if (res.tables.empty()) {
        gui::text(layout, theme, "the run produced no tables");
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
        if (gui::button(gui, layout, theme, "export csv", 84.0f) &&
            ed.export_results) {
            ed.export_results(false);
        }
        layout.same_line();
        if (gui::button(gui, layout, theme, "export json", 84.0f) &&
            ed.export_results) {
            ed.export_results(true);
        }
        if (stale) {
            gui::text(layout, theme, "stale - edited since the run");
        }
    }

    // --- the selected table
    if (t.sweep == ws::spice::SweepKind::None) {
        // The .op shape: one value row per signal, no plot.
        gui::separator(layout, theme);
        std::vector<std::pair<std::string, double>> rows;
        rows.reserve(t.signals.size());
        for (const ws::spice::Signal& s : t.signals) {
            if (!s.data.empty()) {
                rows.emplace_back(s.name, s.data.front());
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

        // The legend: swatch + visibility + the live readout (the
        // hover sample, else the last one - the waveform's resting
        // state is the readout people want at a glance).
        gui::separator(layout, theme);
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
            if (at >= 0 && static_cast<std::size_t>(at) < s.data.size()) {
                layout.same_line();
                const std::string value =
                    ws::circsim::format_eng(s.data[static_cast<std::size_t>(at)], 4);
                char readout[96];
                std::snprintf(readout, sizeof readout, "%s @ %d",
                              value.c_str(), at);
                gui::text(layout, theme, readout);
            }
            gui.ids().pop();
        }

        gui::separator(layout, theme);
        draw_plot(ed, gui, layout, theme, t, x_name);
    }

    // --- the measurements
    if (!ed.results.run.measurements.empty()) {
        gui::separator(layout, theme);
        gui::text(layout, theme, "measurements");
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
