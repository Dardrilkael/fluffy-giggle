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

#include "results_panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>
#include <span>

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

void draw_plot(Editor& ed, gui::GuiContext& gui,
               gui::LayoutCursor& layout, const gui::ChromeTheme& theme,
               const ws::spice::Table& t, const char* x_name) {
    constexpr float kPlotHeight = 220.0f;
    constexpr float kMarginLeft = 46.0f;
    constexpr float kMarginBottom = 17.0f;
    constexpr float kMarginTop = 6.0f;
    constexpr float kMarginRight = 10.0f;

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

    // The ranges: y over the VISIBLE signals, x over the axis (or the
    // index run); padded by plot's expand_range on both.
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
    const ws::circsim::PlotRange xr = ws::circsim::expand_range(x_lo, x_hi);
    const ws::circsim::PlotRange yr = ws::circsim::expand_range(y_lo, y_hi);

    const auto sx = [&](double v) {
        return static_cast<float>(fx + (v - xr.lo) / (xr.hi - xr.lo) * iw);
    };
    const auto sy = [&](double v) {
        return static_cast<float>(fy + (yr.hi - v) / (yr.hi - yr.lo) * ih);
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

    // --- the grid + tick labels
    const std::vector<double> x_ticks =
        ws::circsim::nice_ticks(xr.lo, xr.hi, 6u);
    const std::vector<double> y_ticks =
        ws::circsim::nice_ticks(yr.lo, yr.hi, 5u);
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
            xr.lo + (static_cast<double>(in.mouse_x) - fx) / iw *
                        (xr.hi - xr.lo);
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
            const std::size_t rows = std::min(n, ys.size());
            const std::vector<std::size_t> keep =
                ws::circsim::decimate_minmax(xs.first(rows), ys.first(rows),
                                             budget);
            if (keep.empty()) {
                for (std::size_t k = 0; k < n; ++k) {
                    push(k);
                }
            } else {
                for (std::size_t k : keep) {
                    push(k);
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
