// apps/circsim_editor - the waveform render profiling gate.
// Opt-in headless test (CIRCSIM_PLOT_PROF=1) that measures the CPU cost
// of the Results panel's series loop using Renderer2DCanvas + r2d stub.
// Reports prep (mapping/decimation) and stroke (tessellation + r2d) times
// for round vs bevel joins, and tracks heap allocations from the
// >64-point stack-to-heap threshold in Renderer2DCanvas.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/editor.hpp"
#include "../src/results_panel.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <ws/settings/filesystem.hpp>
#include <ws/spice/results.hpp>
#include <ws/circsim/plot.hpp>
#include <ws/ui/canvas/canvas2d.hpp>
#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>

#include "ui_gui_stub.hpp"

namespace {

namespace gui = ws::ui::gui;
namespace canvas = ws::ui::canvas;
using editor::Editor;
using editor::PlotView;
using editor::PlotGesture;
using editor::ResultsCache;
using editor::SchematicDocument;
using ws::spice::Table;
using ws::spice::Signal;
using ws::spice::AnalysisKind;
using ws::spice::SweepKind;

// Build a large table: num_signals signals x num_rows rows.
// The sweep axis is linear time; each signal gets a distinct waveform.
[[nodiscard]] Table build_large_table(std::size_t num_signals, std::size_t num_rows) {
    Table t;
    t.analysis = AnalysisKind::Tran;
    t.sweep = SweepKind::Time;
    t.sweep_names = {"time"};
    t.sweep_axes.emplace_back();
    t.sweep_axes.back().reserve(num_rows);
    for (std::size_t i = 0; i < num_rows; ++i) {
        t.sweep_axes.back().push_back(
            1.0e-4 * static_cast<double>(i));  // 0 .. (rows-1)*100us
    }
    for (std::size_t s = 0; s < num_signals; ++s) {
        Signal sig;
        sig.name = "v(n" + std::to_string(1000 + s) + ")";
        sig.data.reserve(num_rows);
        for (std::size_t i = 0; i < num_rows; ++i) {
            const double x = static_cast<double>(i) /
                             static_cast<double>(num_rows - 1u);
            // Different phase per signal so they don't overlay perfectly
            sig.data.push_back(3.3 * (0.5 + 0.5 * std::sin(6.28318530718 * x + s)));
        }
        t.signals.push_back(std::move(sig));
    }
    return t;
}

// Bench harness: Editor + Renderer2DCanvas (wired to r2d stub) + GuiContext.
// The Renderer2DCanvas is the real CPU tessellator; the r2d stub records
// its draw calls so we can verify geometry without a GPU.
struct ProfBench {
    ws::settings::StdFileSystem fs;
    SchematicDocument doc{std::string{}, fs};
    Editor ed;
    gui::GuiContext gui;
    canvas::Renderer2DCanvas r2d_canvas;
    gui::ChromeTheme theme = gui::kDefaultChrome;

    ProfBench() {
        ed.doc = &doc;
        ed.plot_canvas = &r2d_canvas;  // inject real renderer, not CommandListCanvas
        ed.results.has = true;
        ed.results.version = doc.schem.version();
        theme.anim_enabled = false;  // no motion in headless profiling
    }

    void load(Table table) {
        ed.results.run.results.tables.clear();
        ed.results.run.results.tables.push_back(std::move(table));
        ed.results.table = 0;
        ++ed.results.run_id;
        ed.results.visible_for = ResultsCache::kNoTable;
        ed.results.visible.clear();
        ed.results.hover = -1;
        ed.results.view = PlotView{};
        ed.results.view_for = ResultsCache::kNoTable;
        ed.results.gesture = PlotGesture{};
        ed.results.x_sorted_for = ResultsCache::kNoTable;
        ed.results.x_sorted = false;
        ed.results.x_runs_for = ResultsCache::kNoTable;
    }

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

    // Draws the panel once. Returns the frame's command count for verification.
    std::size_t draw() { return draw(Mouse{}); }

    std::size_t draw(const Mouse& m) {
        ws::ui::gui_test::stub.reset();  // clear r2d stub counters
        ws::input::InputFrame in{};
        in.mouse_x = m.x;
        in.mouse_y = m.y;
        in.mouse_present = true;
        in.mouse_down[0] = m.left_down;
        in.mouse_pressed[0] = m.left_pressed;
        in.mouse_released[0] = m.left_released;
        in.mouse_down[ws::input::mouse::kMiddle] = m.middle_down;
        in.mouse_pressed[ws::input::mouse::kRight] = m.right_pressed;
        in.scroll_dy = m.scroll_dy;
        in.dt_seconds = m.dt;
        gui.begin_frame(in);
        gui::LayoutCursor layout;
        (void)layout.begin_panel(gui::Rect{0.0f, 0.0f, 640.0f, 520.0f},
                                 12.0f, 8.0f);
        editor::draw_results_panel(ed, gui, layout, theme);
        layout.end_panel();
        gui.end_frame();
        return ws::ui::gui_test::stub.quads;  // proxy for draw activity
    }

    // Run N frames and return average frame time (ms).
    double run_frames(int frames) {
        using Clock = std::chrono::steady_clock;
        const auto t0 = Clock::now();
        for (int i = 0; i < frames; ++i) {
            draw();
        }
        const auto t1 = Clock::now();
        return std::chrono::duration<double, std::milli>(t1 - t0).count() / frames;
    }
};

// Read the WaveProf state from results_panel.cpp's static.
// The patch exposes wave_prof() as a function returning WaveProf&.
extern "C" {
    // These are defined in results_panel.cpp - we declare them to access
    // the profiling counters from the test.
    struct WaveProf;
    WaveProf& wave_prof();
    bool wave_env_prof();
    canvas::StrokeJoin wave_env_join();
}

}  // namespace

// Helper to check if profiling is enabled via env var.
// Mirrors results_panel.cpp's wave_env().prof logic.
[[nodiscard]] int prof_level() {
#if defined(_WIN32)
    char buffer[MAX_PATH];
    const DWORD size = GetEnvironmentVariableA("CIRCSIM_PLOT_PROF", buffer, MAX_PATH);
    if (size == 0 || size >= MAX_PATH) {
        return 0;
    }
    if (buffer[0] == '2') return 2;
    return (buffer[0] != '\0' && buffer[0] != '0') ? 1 : 0;
#else
    const char* v = std::getenv("CIRCSIM_PLOT_PROF");
    if (v == nullptr || v[0] == '\0' || v[0] == '0') return 0;
    return (v[0] == '2') ? 2 : 1;
#endif
}

[[nodiscard]] bool is_prof_enabled() {
    return prof_level() != 0;
}

// Helper to read the join mode from env var.
[[nodiscard]] canvas::StrokeJoin get_join_mode() {
#if defined(_WIN32)
    char buffer[MAX_PATH];
    const DWORD size = GetEnvironmentVariableA("CIRCSIM_PLOT_JOIN", buffer, MAX_PATH);
    if (size == 0 || size >= MAX_PATH) {
        return canvas::StrokeJoin::round;
    }
    if (std::strcmp(buffer, "bevel") == 0) {
        return canvas::StrokeJoin::bevel;
    }
    if (std::strcmp(buffer, "miter") == 0) {
        return canvas::StrokeJoin::miter;
    }
    return canvas::StrokeJoin::round;
#else
    const char* v = std::getenv("CIRCSIM_PLOT_JOIN");
    if (v != nullptr) {
        if (std::strcmp(v, "bevel") == 0) {
            return canvas::StrokeJoin::bevel;
        }
        if (std::strcmp(v, "miter") == 0) {
            return canvas::StrokeJoin::miter;
        }
    }
    return canvas::StrokeJoin::round;
#endif
}

TEST_CASE("waveform render profiling - round joins") {
    // Skip if profiling not enabled (opt-in, not a gate).
    if (!is_prof_enabled()) {
        return;
    }
    REQUIRE(get_join_mode() == canvas::StrokeJoin::round);

    ProfBench b;
    b.load(build_large_table(20, 10000));

    // Warm-up: one frame to build caches.
    b.draw();

    // Profiling window: the patch logs every kProfWindow=120 frames.
    // We run 120 frames to fill one window and read the logged averages.
    const int kProfWindow = 120;
    for (int i = 0; i < kProfWindow; ++i) {
        b.draw();
    }

    // The WaveProf is internal to results_panel.cpp; the test reads it
    // via the extern declarations. The patch's wave_prof_end_frame()
    // logs at the window boundary, so after kProfWindow frames the
    // static WaveProf has been reset and we can't read the last window.
    // Instead, we measure wall-clock here as a cross-check.
    // The real profiling data comes from the INFO log line the patch emits.

    double avg_ms = b.run_frames(kProfWindow);
    CAPTURE(avg_ms);
    // This is a smoke test: the frame should complete without crashing.
    // The real numbers come from the CIRCSIM_PLOT_PROF=1 log output.
    CHECK(avg_ms > 0.0);
}

TEST_CASE("waveform render profiling - bevel joins") {
    if (!is_prof_enabled()) {
        return;
    }
    REQUIRE(get_join_mode() == canvas::StrokeJoin::bevel);

    ProfBench b;
    b.load(build_large_table(20, 10000));

    b.draw();  // warm-up

    const int kProfWindow = 120;
    for (int i = 0; i < kProfWindow; ++i) {
        b.draw();
    }

    double avg_ms = b.run_frames(kProfWindow);
    CAPTURE(avg_ms);
    CHECK(avg_ms > 0.0);
}

TEST_CASE("waveform render profiling - miter joins") {
    if (!is_prof_enabled()) {
        return;
    }
    REQUIRE(get_join_mode() == canvas::StrokeJoin::miter);

    ProfBench b;
    b.load(build_large_table(20, 10000));

    b.draw();  // warm-up

    const int kProfWindow = 120;
    for (int i = 0; i < kProfWindow; ++i) {
        b.draw();
    }

    double avg_ms = b.run_frames(kProfWindow);
    CAPTURE(avg_ms);
    CHECK(avg_ms > 0.0);
}

TEST_CASE("waveform render profiling - round joins PROF=2") {
    if (prof_level() != 2) {
        return;
    }
    REQUIRE(get_join_mode() == canvas::StrokeJoin::round);

    ProfBench b;
    b.load(build_large_table(20, 10000));

    b.draw();  // warm-up

    const int kProfWindow = 120;
    for (int i = 0; i < kProfWindow; ++i) {
        b.draw();
    }

    double avg_ms = b.run_frames(kProfWindow);
    CAPTURE(avg_ms);
    CHECK(avg_ms > 0.0);
}

TEST_CASE("waveform render profiling - bevel joins PROF=2") {
    if (prof_level() != 2) {
        return;
    }
    REQUIRE(get_join_mode() == canvas::StrokeJoin::bevel);

    ProfBench b;
    b.load(build_large_table(20, 10000));

    b.draw();  // warm-up

    const int kProfWindow = 120;
    for (int i = 0; i < kProfWindow; ++i) {
        b.draw();
    }

    double avg_ms = b.run_frames(kProfWindow);
    CAPTURE(avg_ms);
    CHECK(avg_ms > 0.0);
}

TEST_CASE("waveform render profiling - miter joins PROF=2") {
    if (prof_level() != 2) {
        return;
    }
    REQUIRE(get_join_mode() == canvas::StrokeJoin::miter);

    ProfBench b;
    b.load(build_large_table(20, 10000));

    b.draw();  // warm-up

    const int kProfWindow = 120;
    for (int i = 0; i < kProfWindow; ++i) {
        b.draw();
    }

    double avg_ms = b.run_frames(kProfWindow);
    CAPTURE(avg_ms);
    CHECK(avg_ms > 0.0);
}

// The renderer backend retains one tessellated quad buffer per signal.
// An idle frame reuses the same key/buffer; a fresh delivery must re-key it.
TEST_CASE("waveform tessellation cache is per series and delivery keyed") {
    ProfBench b;
    b.load(build_large_table(20, 10000));
    b.draw();

    REQUIRE(b.ed.results.geometry.size() == 20u);
    for (const ResultsCache::SeriesGeometry& series : b.ed.results.geometry) {
        CHECK(series.built);
        CHECK(series.key.visible);
        CHECK(series.key.run == b.ed.results.run_id);
        CHECK(series.key.table == b.ed.results.table);
        CHECK(series.buffer.quad_count() > 0u);
    }
    const auto first_keys = [&] {
        std::vector<ResultsCache::SeriesGeometryKey> keys;
        keys.reserve(b.ed.results.geometry.size());
        for (const auto& series : b.ed.results.geometry) keys.push_back(series.key);
        return keys;
    }();

    b.draw();  // identical view: every series is a geometry-cache hit
    REQUIRE(b.ed.results.geometry.size() == first_keys.size());
    for (std::size_t i = 0; i < first_keys.size(); ++i) {
        CHECK(b.ed.results.geometry[i].key == first_keys[i]);
        CHECK(b.ed.results.geometry[i].built);
    }

    const std::uint64_t previous_run = b.ed.results.run_id;
    b.load(build_large_table(20, 10000));
    CHECK(b.ed.results.run_id != previous_run);
    b.draw();
    for (const auto& series : b.ed.results.geometry) {
        CHECK(series.built);
        CHECK(series.key.run == b.ed.results.run_id);
    }
}

// The series loop's clip rect rides a push/pop pair around the whole
// loop; on the Renderer2D backend it is the r2d clip stack (stamped
// into every command, the replayed cached quads included). The stack
// must close on a miss frame (tessellate + replay) and on a hit frame
// (replay only) alike, and cost the same pair either way.
TEST_CASE("the series clip is balanced on cache-miss and cache-hit frames") {
    ProfBench b;
    b.load(build_large_table(4, 2000));

    b.draw();  // every series a geometry miss
    for (const ResultsCache::SeriesGeometry& g : b.ed.results.geometry) {
        CHECK(g.built);
    }
    const std::uint32_t miss_pushes =
        ws::ui::gui_test::stub.clip_pushes;
    CHECK(miss_pushes >= 1u);
    CHECK(ws::ui::gui_test::stub.clip_pops == miss_pushes);

    b.draw();  // identical view: every series a hit
    CHECK(ws::ui::gui_test::stub.clip_pushes == miss_pushes);
    CHECK(ws::ui::gui_test::stub.clip_pops == miss_pushes);
}

// A quick structural test that verifies the series loop produces strokes
// for a large table without crashing (runs without CIRCSIM_PLOT_PROF).
TEST_CASE("large table draws series strokes without crash") {
    ProfBench b;
    b.load(build_large_table(20, 10000));
    b.draw();  // warm-up

    // Run a few frames - should not crash or hang.
    for (int i = 0; i < 5; ++i) {
        const std::size_t quads = b.draw();
        // The r2d stub should have recorded quads from the tessellator.
        // With 20 signals and decimation to ~580 pts each, we expect
        // many quads (each stroke point generates quad strips).
        // Just verify non-zero activity.
        CHECK(quads > 0);
    }

    // Verify the decimation cache is active (second frame reuses it).
    const std::size_t stats = b.ed.results.stats.size();
    CHECK(stats == 20u);
    CHECK(b.ed.results.stats_for == 0u);
    CHECK(b.ed.results.stats_run == b.ed.results.run_id);

    const std::size_t decim = b.ed.results.decim.size();
    CHECK(decim == 20u);
    CHECK(b.ed.results.decim[0].built);
    CHECK(b.ed.results.decim_run == b.ed.results.run_id);
}