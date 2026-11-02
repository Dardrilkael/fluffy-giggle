// apps/circsim_editor stage 4/5 - the Results panel's headless gate.
// The panel is app code, so its purity seam is the Editor's
// plot_canvas injection (a CommandListCanvas stands in for the r2d
// canvas): the cases below drive draw_results_panel the way the shell
// does - a GuiContext frame, a panel rect, a scripted mouse - and
// assert the recorded command stream: the plot frame, the grid, the
// series polylines (point counts, colors, and the data-to-screen
// mapping), the decimation of a long sweep, the hover crosshair
// markers, the .op table's value-row shape (no plot at all), and -
// stage 5 - the plot interaction: the wheel zoom, the rubber band
// (box, outline, commit), the axis-drag zooms, the pan, the
// double-click reset, the right-click cancel, and the per-table view
// reset.

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
using editor::PlotGesture;
using editor::PlotView;
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
        // The app's run delivery resets the manual view and the
        // gesture too - the bench mirrors it (main.cpp's block).
        ed.results.view = PlotView{};
        ed.results.view_for = ResultsCache::kNoTable;
        ed.results.gesture = PlotGesture{};
    }

    // The scripted mouse one draw sees. Defaults keep the stage-4
    // shape: a pointer parked off-panel, no buttons, no scroll, no
    // clock (dt 0 - the double-click detector only runs when a test
    // feeds it time).
    struct Mouse {
        float x = -1000.0f, y = -1000.0f;
        bool left_down = false;
        bool left_pressed = false;
        bool left_released = false;
        bool middle_down = false;
        bool right_pressed = false;
        float scroll_dy = 0.0f;
        float dt = 0.0f;
    };

    // Draws the panel once with the given mouse; clc.cmds holds
    // exactly this frame's commands on return.
    void draw(const Mouse& m) {
        clc.cmds.clear();
        ws::input::InputFrame in{};
        in.mouse_x = m.x;
        in.mouse_y = m.y;
        in.mouse_present = true;  // a scripted session has a mouse - the
                                  // wheel resolution gates on it
        in.mouse_down[0] = m.left_down;
        in.mouse_pressed[0] = m.left_pressed;
        in.mouse_released[0] = m.left_released;
        in.mouse_down[1] = m.middle_down;
        in.mouse_pressed[2] = m.right_pressed;
        in.scroll_dy = m.scroll_dy;
        in.dt_seconds = m.dt;
        gui.begin_frame(in);
        gui::LayoutCursor layout;
        (void)layout.begin_panel(gui::Rect{0.0f, 0.0f, 640.0f, 520.0f},
                                 12.0f, 8.0f);
        const gui::ChromeTheme theme = gui::kDefaultChrome;
        editor::draw_results_panel(ed, gui, layout, theme);
        layout.end_panel();
        gui.end_frame();
    }

    void draw(float mouse_x = -1000.0f, float mouse_y = -1000.0f) {
        Mouse m;
        m.x = mouse_x;
        m.y = mouse_y;
        draw(m);
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

// --- stage 5: the plot's hands --------------------------------------------------
//
// The bench's home windows for ramp_table(41): x = [-2e-4, 4.2e-3]
// (span 4.4e-3); y spans the ramp AND the sine's dip - the sine
// touches exactly -1e-3 at i = 30 - so the padded fit is
// [-0.25105, 5.25005] (span 5.5011), the numbers the band and zoom
// asserts below read against.
constexpr double kHomeXLo = -2.0e-4;
constexpr double kHomeXHi = 4.2e-3;
constexpr double kHomeXSpan = kHomeXHi - kHomeXLo;
constexpr double kHomeYLo = -0.25105;
constexpr double kHomeYHi = 5.25005;
constexpr double kHomeYSpan = kHomeYHi - kHomeYLo;

// The band's fill/outline (results_panel.cpp's with_alpha split of
// kDefaultChrome's accent).
constexpr std::uint32_t kBandFill = ws::ui::r2d::rgba(84, 144, 212, 36);
constexpr std::uint32_t kBandLine = ws::ui::r2d::rgba(84, 144, 212, 200);

TEST_CASE("the wheel zooms both axes toward the cursor") {
    Bench b;
    b.load(ramp_table(41u));
    b.draw();  // pass 0: a frame locates the plot geometry
    // Pass 1: the cursor ARRIVES over the plot - the claim registers
    // and its win settles for the next frame (the gui's one-frame
    // rule: the first notch after arriving still scrolls the panel).
    const Bench::FrameBox f0 = b.frame_box(b.frame_cmd());
    b.draw((f0.fx + f0.fx2) * 0.5f, (f0.fy + f0.fy2) * 0.5f);

    const Bench::FrameBox f = b.frame_box(b.frame_cmd());
    Bench::Mouse m;
    m.x = (f.fx + f.fx2) * 0.5f;
    m.y = (f.fy + f.fy2) * 0.5f;
    m.scroll_dy = 1.0f;  // one notch in
    b.draw(m);           // pass 2: the settled win consumes the notch

    const PlotView& v = b.ed.results.view;
    CHECK(v.active);
    // Both windows narrowed by the 1.15 notch, still centered.
    CHECK((v.x_hi - v.x_lo) ==
          doctest::Approx(kHomeXSpan / 1.15).epsilon(1e-6));
    CHECK((v.y_hi - v.y_lo) ==
          doctest::Approx(kHomeYSpan / 1.15).epsilon(1e-6));

    // A second notch: the x slice bites. The decimation window keeps
    // one sample of slack past each edge (the stroke needs the
    // neighbor to draw across the boundary), so one 1.15 notch still
    // carries every sample; two leave the ends out for real.
    b.draw(m);
    CHECK((v.x_hi - v.x_lo) ==
          doctest::Approx(kHomeXSpan / (1.15 * 1.15)).epsilon(1e-5));
    const std::vector<const canvas::CanvasCmd*> series = b.series_cmds();
    REQUIRE(series.size() == 2u);
    CHECK(series[0]->points.size() < 41u);

    // Two notches back out at the same spot return both spans (the
    // math is invertible about the same anchor).
    m.scroll_dy = -1.0f;
    b.draw(m);
    b.draw(m);
    CHECK((v.x_hi - v.x_lo) ==
          doctest::Approx(kHomeXSpan).epsilon(1e-5));
    CHECK((v.y_hi - v.y_lo) ==
          doctest::Approx(kHomeYSpan).epsilon(1e-5));
}

TEST_CASE("a rubber band zooms to the box and draws itself") {
    Bench b;
    b.load(ramp_table(41u));
    b.draw();  // locate the frame

    const Bench::FrameBox f = b.frame_box(b.frame_cmd());
    const float iw = f.fx2 - f.fx;
    const float ih = f.fy2 - f.fy;
    Bench::Mouse m;
    m.x = f.fx + iw * 0.25f;
    m.y = f.fy + ih * 0.75f;
    m.left_pressed = true;
    m.left_down = true;
    b.draw(m);
    CHECK(b.ed.results.gesture.kind == PlotGesture::Kind::Band);
    CHECK_FALSE(b.ed.results.view.active);  // nothing commits mid-drag

    // The in-flight band paints: an accent wash, an accent outline.
    bool saw_fill = false;
    bool saw_line = false;
    for (const canvas::CanvasCmd& c : b.clc.cmds) {
        saw_fill = saw_fill || (c.kind == canvas::CanvasCmdKind::fill_polygon &&
                                c.color == kBandFill);
        saw_line = saw_line ||
                   (c.kind == canvas::CanvasCmdKind::stroke_polyline &&
                    c.points.size() == 4u && c.color == kBandLine);
    }
    CHECK(saw_fill);
    CHECK(saw_line);

    // Drag to the opposite corner and release: the view is the band.
    m.x = f.fx + iw * 0.75f;
    m.y = f.fy + ih * 0.25f;
    m.left_pressed = false;
    b.draw(m);
    m.left_down = false;
    m.left_released = true;
    b.draw(m);
    CHECK(b.ed.results.gesture.kind == PlotGesture::Kind::None);
    const PlotView& v = b.ed.results.view;
    CHECK(v.active);
    // The middle half of the home window, both axes (the band's box,
    // clamped into home - well inside here).
    CHECK(v.x_lo == doctest::Approx(kHomeXLo + 0.25 * kHomeXSpan)
                       .epsilon(1e-3));
    CHECK(v.x_hi == doctest::Approx(kHomeXLo + 0.75 * kHomeXSpan)
                       .epsilon(1e-3));
    CHECK(v.y_lo == doctest::Approx(kHomeYHi - 0.75 * kHomeYSpan)
                       .epsilon(1e-3));
    CHECK(v.y_hi == doctest::Approx(kHomeYHi - 0.25 * kHomeYSpan)
                       .epsilon(1e-3));
    // The series now covers only the band's samples (the x slice).
    const std::vector<const canvas::CanvasCmd*> series = b.series_cmds();
    REQUIRE(series.size() == 2u);
    CHECK(series[0]->points.size() < 41u);
}

TEST_CASE("dragging an axis strip stretches that axis only") {
    Bench b;
    b.load(ramp_table(41u));
    b.draw();

    const Bench::FrameBox f = b.frame_box(b.frame_cmd());
    const float iw = f.fx2 - f.fx;
    const float ih = f.fy2 - f.fy;

    // The x strip: inside the frame's width, below its bottom edge.
    Bench::Mouse m;
    m.x = (f.fx + f.fx2) * 0.5f;
    m.y = f.fy2 + 5.0f;
    m.left_pressed = true;
    m.left_down = true;
    b.draw(m);
    CHECK(b.ed.results.gesture.kind == PlotGesture::Kind::ZoomX);
    // The arm seeded the manual view with the effective (home) window
    // - the axis zoom lands its axis into that base as the drag moves.
    CHECK(b.ed.results.view.active);
    CHECK(b.ed.results.view.y_lo ==
          doctest::Approx(kHomeYLo).epsilon(1e-6));
    CHECK(b.ed.results.view.y_hi ==
          doctest::Approx(kHomeYHi).epsilon(1e-6));

    // 100 px right: e^0.5 in. The x window narrows about the anchor;
    // the y window is untouched.
    m.x += 100.0f;
    m.left_pressed = false;
    b.draw(m);
    const PlotView& v = b.ed.results.view;
    CHECK(v.active);
    CHECK((v.x_hi - v.x_lo) ==
          doctest::Approx(kHomeXSpan * std::exp(-0.5)).epsilon(1e-3));
    CHECK(v.y_lo == doctest::Approx(kHomeYLo).epsilon(1e-6));
    CHECK(v.y_hi == doctest::Approx(kHomeYHi).epsilon(1e-6));

    // Release, then the y strip: inside the frame's height, left of
    // its left edge. 100 px up: e^0.5 in, y only.
    m.left_down = false;
    m.left_released = true;
    b.draw(m);
    CHECK(b.ed.results.gesture.kind == PlotGesture::Kind::None);

    Bench::Mouse my;
    my.x = f.fx - 5.0f;
    my.y = (f.fy + f.fy2) * 0.5f;
    my.left_pressed = true;
    my.left_down = true;
    b.draw(my);
    CHECK(b.ed.results.gesture.kind == PlotGesture::Kind::ZoomY);
    my.y -= 100.0f;
    my.left_pressed = false;
    b.draw(my);
    CHECK(b.ed.results.view.active);
    CHECK((b.ed.results.view.y_hi - b.ed.results.view.y_lo) ==
          doctest::Approx(kHomeYSpan * std::exp(-0.5)).epsilon(1e-3));
    // The x window kept its drag-1 answer.
    CHECK((b.ed.results.view.x_hi - b.ed.results.view.x_lo) ==
          doctest::Approx(kHomeXSpan * std::exp(-0.5)).epsilon(1e-3));
}

TEST_CASE("middle-drag pans and a double-click resets") {
    Bench b;
    b.load(ramp_table(41u));
    b.draw();

    const Bench::FrameBox f = b.frame_box(b.frame_cmd());
    const float iw = f.fx2 - f.fx;
    const float ih = f.fy2 - f.fy;
    const float cx = f.fx + iw * 0.5f;
    const float cy = f.fy + ih * 0.5f;

    // Establish a manual view (the band's middle half).
    Bench::Mouse m;
    m.x = f.fx + iw * 0.25f;
    m.y = f.fy + ih * 0.75f;
    m.left_pressed = true;
    m.left_down = true;
    b.draw(m);
    m.x = f.fx + iw * 0.75f;
    m.y = f.fy + ih * 0.25f;
    m.left_pressed = false;
    b.draw(m);
    m.left_down = false;
    m.left_released = true;
    b.draw(m);
    const double band_lo = b.ed.results.view.x_lo;
    const double band_span = b.ed.results.view.x_hi - band_lo;
    REQUIRE(b.ed.results.view.active);

    // Age the double-click clock (the pointer stays OVER the plot -
    // the clock dies when it leaves), then pan right 100 px: the
    // window slides LEFT by the data-space shift (content follows
    // the cursor).
    Bench::Mouse gap;
    gap.x = cx;
    gap.y = cy;
    gap.dt = 0.5f;
    b.draw(gap);
    b.draw(gap);
    Bench::Mouse pan;
    pan.x = cx;
    pan.y = cy;
    pan.middle_down = true;
    pan.dt = 0.016f;
    b.draw(pan);
    CHECK(b.ed.results.gesture.kind == PlotGesture::Kind::Pan);
    pan.x = cx + 100.0f;
    b.draw(pan);
    const double shift = -(100.0 / static_cast<double>(iw)) * band_span;
    CHECK(b.ed.results.view.active);
    CHECK(b.ed.results.view.x_lo ==
          doctest::Approx(band_lo + shift).epsilon(1e-3));

    // The middle releases: the pan ends, the view stays where it
    // landed.
    pan.middle_down = false;
    b.draw(pan);
    CHECK(b.ed.results.gesture.kind == PlotGesture::Kind::None);
    CHECK(b.ed.results.view.active);

    // The double click: two presses, same spot, inside the clock.
    // Press 1 arms a band (the clock is aged past 0.35 by the gaps).
    Bench::Mouse click;
    click.x = cx;
    click.y = cy;
    click.left_pressed = true;
    click.left_down = true;
    click.dt = 0.1f;
    b.draw(click);
    CHECK(b.ed.results.gesture.kind == PlotGesture::Kind::Band);
    click.left_pressed = false;  // the release carries no press edge
    click.left_down = false;
    b.draw(click);               // the tiny band's release
    click.left_pressed = true;
    click.left_down = true;
    b.draw(click);               // press 2, ~0.2 s later, same spot
    CHECK(b.ed.results.gesture.kind == PlotGesture::Kind::None);
    CHECK_FALSE(b.ed.results.view.active);
    // The auto fit is back: every sample draws again.
    const std::vector<const canvas::CanvasCmd*> series = b.series_cmds();
    REQUIRE(series.size() == 2u);
    CHECK(series[0]->points.size() == 41u);
}

TEST_CASE("a right-click cancels the drag and restores the press view") {
    Bench b;
    b.load(ramp_table(41u));
    b.draw();  // pass 0: a frame locates the plot geometry
    {   // The cursor arrives over the plot: the claim registers, the
        // win settles for the next frame.
        const Bench::FrameBox f0 = b.frame_box(b.frame_cmd());
        b.draw((f0.fx + f0.fx2) * 0.5f, (f0.fy + f0.fy2) * 0.5f);
    }

    // A manual view to endanger: one wheel notch.
    const Bench::FrameBox f = b.frame_box(b.frame_cmd());
    Bench::Mouse wheel;
    wheel.x = (f.fx + f.fx2) * 0.5f;
    wheel.y = (f.fy + f.fy2) * 0.5f;
    wheel.scroll_dy = 1.0f;
    b.draw(wheel);
    REQUIRE(b.ed.results.view.active);
    const PlotView before = b.ed.results.view;

    // A band drag goes astray; a right-click kills it and restores
    // the press-time window exactly.
    Bench::Mouse m;
    m.x = f.fx + 30.0f;
    m.y = f.fy + 30.0f;
    m.left_pressed = true;
    m.left_down = true;
    b.draw(m);
    REQUIRE(b.ed.results.gesture.kind == PlotGesture::Kind::Band);
    m.x += 200.0f;
    m.y += 100.0f;
    m.left_pressed = false;
    b.draw(m);
    m.right_pressed = true;
    b.draw(m);
    CHECK(b.ed.results.gesture.kind == PlotGesture::Kind::None);
    CHECK(b.ed.results.view.active);
    CHECK(b.ed.results.view.x_lo == before.x_lo);
    CHECK(b.ed.results.view.x_hi == before.x_hi);
    CHECK(b.ed.results.view.y_lo == before.y_lo);
    CHECK(b.ed.results.view.y_hi == before.y_hi);
}

TEST_CASE("a table switch drops the manual view") {
    Bench b;
    b.load(ramp_table(41u));
    b.ed.results.run.results.tables.push_back(ramp_table(10u));
    b.draw();

    // A manual view on table 0.
    const Bench::FrameBox f = b.frame_box(b.frame_cmd());
    const float iw = f.fx2 - f.fx;
    const float ih = f.fy2 - f.fy;
    Bench::Mouse m;
    m.x = f.fx + iw * 0.25f;
    m.y = f.fy + ih * 0.75f;
    m.left_pressed = true;
    m.left_down = true;
    b.draw(m);
    m.x = f.fx + iw * 0.75f;
    m.y = f.fy + ih * 0.25f;
    m.left_pressed = false;
    b.draw(m);
    m.left_down = false;
    m.left_released = true;
    b.draw(m);
    REQUIRE(b.ed.results.view.active);

    // The combo moves to table 1: the view drops (it belonged to
    // table 0), and moving back does not resurrect it.
    b.ed.results.table = 1;
    b.draw();
    CHECK_FALSE(b.ed.results.view.active);
    b.ed.results.table = 0;
    b.draw();
    CHECK_FALSE(b.ed.results.view.active);
}
