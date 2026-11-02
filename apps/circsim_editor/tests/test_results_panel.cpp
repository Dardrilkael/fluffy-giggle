// apps/circsim_editor stage 4 - the Results panel's headless gate.
// The panel is app code, so its purity seam is the Editor's
// plot_canvas injection (a CommandListCanvas stands in for the r2d
// canvas): the cases below drive draw_results_panel the way the shell
// does - a GuiContext frame, a panel rect, a scripted mouse - and
// assert the recorded command stream: the plot frame, the grid, the
// series polylines (point counts, colors, and the data-to-screen
// mapping), the decimation of a long sweep, the hover crosshair
// markers, and the .op table's value-row shape (no plot at all).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/editor.hpp"
#include "../src/results_panel.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include <ws/settings/filesystem.hpp>
#include <ws/spice/results.hpp>
#include <ws/spice/run.hpp>
#include <ws/ui/canvas/canvas2d.hpp>
#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>

namespace {

namespace gui = ws::ui::gui;
namespace canvas = ws::ui::canvas;
using editor::Editor;
using editor::ResultsCache;
using editor::SchematicDocument;

// The series palette the panel draws with (results_panel.cpp's wheel).
[[nodiscard]] std::uint32_t series_color(std::size_t i) {
    constexpr std::uint32_t kSeries[] = {
        ws::ui::r2d::rgba(224, 90, 78),   ws::ui::r2d::rgba(78, 161, 224),
        ws::ui::r2d::rgba(88, 181, 92),   ws::ui::r2d::rgba(211, 154, 42),
        ws::ui::r2d::rgba(154, 111, 211), ws::ui::r2d::rgba(78, 201, 192),
        ws::ui::r2d::rgba(211, 90, 158),  ws::ui::r2d::rgba(143, 154, 78),
    };
    return kSeries[i % 8u];
}

[[nodiscard]] ws::spice::Table ramp_table(std::size_t rows) {
    ws::spice::Table t;
    t.analysis = ws::spice::AnalysisKind::Tran;
    t.sweep = ws::spice::SweepKind::Time;
    t.sweep_names = {"time"};
    t.sweep_axes.emplace_back();
    t.sweep_axes.back().reserve(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        t.sweep_axes.back().push_back(
            1.0e-4 * static_cast<double>(i));  // 0 .. (rows-1)*100u
    }
    ws::spice::Signal out;
    out.name = "v(out)";
    out.data.reserve(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        // 0 -> 5: a clean ramp, monotonically rising.
        out.data.push_back(5.0 * static_cast<double>(i) /
                           static_cast<double>(rows - 1u));
    }
    ws::spice::Signal cur;
    cur.name = "i(V1)";
    cur.data.reserve(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        const double x = static_cast<double>(i) /
                         static_cast<double>(rows - 1u);
        cur.data.push_back(1.0e-3 * std::sin(2.0 * 3.14159265358979 * x));
    }
    t.signals.push_back(std::move(out));
    t.signals.push_back(std::move(cur));
    return t;
}

[[nodiscard]] ws::spice::Table op_table() {
    ws::spice::Table t;
    t.analysis = ws::spice::AnalysisKind::Op;
    t.sweep = ws::spice::SweepKind::None;
    t.signals.push_back({"v(N001)", {5.0}});
    t.signals.push_back({"v(out)", {2.5}});
    t.signals.push_back({"i(V1)", {-2.5e-4}});
    return t;
}

// The bench: an Editor over an untitled document, a CommandListCanvas
// injected at the plot seam, and one drawn frame per call.
struct Bench {
    ws::settings::StdFileSystem fs;
    SchematicDocument doc{std::string{}, fs};
    Editor ed;
    gui::GuiContext gui;
    canvas::CommandListCanvas clc;

    Bench() {
        ed.doc = &doc;
        ed.plot_canvas = &clc;
        ed.results.has = true;
        ed.results.version = doc.schem.version();
    }

    void load(ws::spice::Table table) {
        ed.results.run.results.tables.clear();
        ed.results.run.results.tables.push_back(std::move(table));
        ed.results.table = 0;
        ed.results.visible_for = ResultsCache::kNoTable;
        ed.results.visible.clear();
        ed.results.hover = -1;
    }

    // Draws the panel once with the given mouse; clc.cmds holds
    // exactly this frame's commands on return.
    void draw(float mouse_x = -1000.0f, float mouse_y = -1000.0f) {
        clc.cmds.clear();
        ws::input::InputFrame in{};
        in.mouse_x = mouse_x;
        in.mouse_y = mouse_y;
        gui.begin_frame(in);
        gui::LayoutCursor layout;
        (void)layout.begin_panel(gui::Rect{0.0f, 0.0f, 640.0f, 520.0f},
                                 12.0f, 8.0f);
        const gui::ChromeTheme theme = gui::kDefaultChrome;
        editor::draw_results_panel(ed, gui, layout, theme);
        layout.end_panel();
        gui.end_frame();
    }

    // The plot frame: the first 4-point stroke (the legend swatches are
    // fills; the frame is the only 4-point CLOSED stroke).
    [[nodiscard]] const canvas::CanvasCmd* frame_cmd() const {
        for (const canvas::CanvasCmd& c : clc.cmds) {
            if (c.kind == canvas::CanvasCmdKind::stroke_polyline &&
                c.points.size() == 4u) {
                return &c;
            }
        }
        return nullptr;
    }

    // The series strokes: polylines carrying more than 4 points.
    [[nodiscard]] std::vector<const canvas::CanvasCmd*>
    series_cmds() const {
        std::vector<const canvas::CanvasCmd*> out;
        for (const canvas::CanvasCmd& c : clc.cmds) {
            if (c.kind == canvas::CanvasCmdKind::stroke_polyline &&
                c.points.size() > 4u) {
                out.push_back(&c);
            }
        }
        return out;
    }

    [[nodiscard]] std::size_t stroke_count() const {
        return clc.count(canvas::CanvasCmdKind::stroke_polyline);
    }

    // The frame's bounding box (fx, fy, fx2, fy2).
    struct FrameBox {
        float fx = 0.0f, fy = 0.0f, fx2 = 0.0f, fy2 = 0.0f;
    };
    [[nodiscard]] FrameBox frame_box(const canvas::CanvasCmd* frame) const {
        FrameBox b;
        REQUIRE(frame != nullptr);
        b.fx = b.fx2 = frame->points[0].x;
        b.fy = b.fy2 = frame->points[0].y;
        for (const canvas::Point& p : frame->points) {
            b.fx = std::min(b.fx, p.x);
            b.fy = std::min(b.fy, p.y);
            b.fx2 = std::max(b.fx2, p.x);
            b.fy2 = std::max(b.fy2, p.y);
        }
        return b;
    }
};

}  // namespace

TEST_CASE("a swept table draws frame, grid, and both series") {
    Bench b;
    b.load(ramp_table(41u));
    b.draw();

    // The frame: a closed 4-point polyline somewhere in the panel.
    const Bench::FrameBox f = b.frame_box(b.frame_cmd());
    CHECK(f.fx2 - f.fx > 400.0f);  // the plot spans the panel's width
    CHECK(f.fy2 - f.fy > 180.0f);  // ...and its fixed height minus margins

    // The grid: frame + x/y tick strokes, all 2-point lines.
    CHECK(b.stroke_count() >= 5u);

    // The series: two polylines, the palette's first two colors, one
    // point per sample (41 rows fit any pixel budget - no decimation).
    const std::vector<const canvas::CanvasCmd*> series = b.series_cmds();
    REQUIRE(series.size() == 2u);
    bool saw_red = false;
    bool saw_blue = false;
    for (const canvas::CanvasCmd* s : series) {
        CHECK(s->points.size() == 41u);
        saw_red = saw_red || s->color == series_color(0);
        saw_blue = saw_blue || s->color == series_color(1);
        // x is monotonically increasing (the sweep direction).
        for (std::size_t i = 1; i < s->points.size(); ++i) {
            CHECK(s->points[i].x > s->points[i - 1].x);
        }
    }
    CHECK(saw_red);
    CHECK(saw_blue);

    // The data-to-screen mapping: the ramp's FIRST sample sits at the
    // frame's bottom edge (y=0 of 0..5), the LAST at the top (y=5) -
    // remembering screen y grows DOWN.
    const canvas::CanvasCmd* ramp =
        series[0]->color == series_color(0) ? series[0] : series[1];
    CHECK(ramp->points.front().y > f.fy2 - 14.0f);  // near the bottom
    CHECK(ramp->points.back().y < f.fy + 14.0f);    // near the top

    // The legend swatches: two fills in the series colors left of the
    // plot frame (the legend rows sit above it).
    std::size_t swatches = 0;
    for (const canvas::CanvasCmd& c : b.clc.cmds) {
        if (c.kind != canvas::CanvasCmdKind::fill_polygon) {
            continue;
        }
        if (c.color == series_color(0) || c.color == series_color(1)) {
            float cx = 0.0f;
            for (const canvas::Point& p : c.points) {
                cx += p.x;
            }
            cx /= static_cast<float>(c.points.size());
            if (cx < f.fx) {
                ++swatches;
            }
        }
    }
    CHECK(swatches == 2u);
}

TEST_CASE("a long sweep decimates to the pixel budget") {
    Bench b;
    b.load(ramp_table(1200u));
    b.draw();

    const std::vector<const canvas::CanvasCmd*> series = b.series_cmds();
    REQUIRE(series.size() == 2u);
    for (const canvas::CanvasCmd* s : series) {
        // The budget is the plot's inner width (~570): 1200 samples
        // fold to at most ~budget + 2 vertices...
        CHECK(s->points.size() <= 580u);
        CHECK(s->points.size() >= 8u);  // ...but never collapse
    }
    // The ramp's envelope still reaches both rails: first point low,
    // last point high (decimation keeps the global extremes).
    const canvas::CanvasCmd* ramp =
        series[0]->color == series_color(0) ? series[0] : series[1];
    const Bench::FrameBox f = b.frame_box(b.frame_cmd());
    CHECK(ramp->points.front().y > f.fy2 - 16.0f);
    CHECK(ramp->points.back().y < f.fy + 16.0f);
}

TEST_CASE("hovering the plot drops a crosshair and per-series markers") {
    Bench b;
    b.load(ramp_table(41u));
    b.draw();  // pass 1: locate the frame

    const Bench::FrameBox f = b.frame_box(b.frame_cmd());
    const float cx = (f.fx + f.fx2) * 0.5f;
    const float cy = (f.fy + f.fy2) * 0.5f;

    // Off the plot: no hover state.
    CHECK(b.ed.results.hover == -1);

    // Pass 2: the mouse at the frame's center.
    b.draw(cx, cy);
    REQUIRE(b.ed.results.hover >= 0);
    const std::size_t k = static_cast<std::size_t>(b.ed.results.hover);
    CHECK(k < 41u);

    // The crosshair: a vertical 2-point stroke at the hovered sample's
    // x (the series strokes are the only >4-point polylines).
    const std::vector<const canvas::CanvasCmd*> series = b.series_cmds();
    REQUIRE(series.size() == 2u);
    const float cross_x = series[0]->points[k].x;
    bool cross = false;
    for (const canvas::CanvasCmd& c : b.clc.cmds) {
        if (c.kind == canvas::CanvasCmdKind::stroke_polyline &&
            c.points.size() == 2u &&
            std::fabs(c.points[0].x - cross_x) < 1.5f &&
            std::fabs(c.points[1].x - cross_x) < 1.5f) {
            cross = true;
        }
    }
    CHECK(cross);

    // The markers: one small fill per visible series AT the series'
    // own (x, y) of sample k - in the series colors.
    for (const canvas::CanvasCmd* s : series) {
        const canvas::Point at = s->points[k];
        bool marked = false;
        for (const canvas::CanvasCmd& c : b.clc.cmds) {
            if (c.kind != canvas::CanvasCmdKind::fill_polygon ||
                c.color != s->color) {
                continue;
            }
            float mx = 0.0f;
            float my = 0.0f;
            for (const canvas::Point& p : c.points) {
                mx += p.x;
                my += p.y;
            }
            mx /= static_cast<float>(c.points.size());
            my /= static_cast<float>(c.points.size());
            if (std::fabs(mx - at.x) < 4.0f && std::fabs(my - at.y) < 4.0f) {
                marked = true;
            }
        }
        CHECK(marked);
    }
}

TEST_CASE("an .op table draws value rows and no plot") {
    Bench b;
    b.load(op_table());
    b.draw();

    // No plot for a sweep-less table: nothing through the canvas at
    // all (the value rows are gui text, drawn via the r2d seam).
    CHECK(b.stroke_count() == 0u);
    CHECK(b.clc.cmds.empty());
    CHECK(b.frame_cmd() == nullptr);
}

TEST_CASE("a missing run or empty tables never draws") {
    Bench b;
    b.draw();
    CHECK(b.clc.cmds.empty());  // results.has, but no tables

    b.load(ramp_table(4u));
    b.ed.results.run.results.tables.clear();
    b.draw();
    CHECK(b.clc.cmds.empty());
}

TEST_CASE("stale results note themselves but still draw") {
    Bench b;
    b.load(ramp_table(10u));
    b.ed.results.version = b.doc.schem.version() + 1u;  // edited since
    b.draw();
    CHECK(b.frame_cmd() != nullptr);  // the plot draws regardless
    CHECK(b.series_cmds().size() == 2u);
}
