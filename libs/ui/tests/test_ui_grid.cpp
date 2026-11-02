// The stage-16 grid suite: the x5 level ladder, the no-pop fade
// contract, the index math at fractional centers/zooms, the mark cap,
// the y-flip, and the pixel-snapped draw through the recording canvas
// (CommandListCanvas) - the same purity class as test_ui_canvas (the
// gui archive + the r2d stub, no renderer on the link line).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include <ws/ui/canvas/grid.hpp>

namespace {

using ws::ui::canvas::CanvasCmdKind;
using ws::ui::canvas::CommandListCanvas;
using ws::ui::canvas::GridFrame;
using ws::ui::canvas::GridMetrics;
using ws::ui::canvas::GridRung;
using ws::ui::canvas::GridSpec;
using ws::ui::canvas::compute_grid;
using ws::ui::canvas::draw_grid;
using ws::ui::canvas::grid_rung_alpha;
using ws::ui::canvas::snap_coord;
using ws::ui::canvas::snap_mark;

// A stock frame: an 800x600 viewport at the origin, zoom 26 (the
// editor's default), y-up world flipped onto the y-down screen.
[[nodiscard]] GridFrame stock_frame() {
    GridFrame f;
    f.center_x = 0.0;
    f.center_y = 0.0;
    f.zoom = 26.0;
    f.ox = 100.0f;
    f.oy = 50.0f;
    f.view_w = 800.0;
    f.view_h = 600.0;
    f.y_flip = true;
    return f;
}

// The rung with `spacing`, or null.
[[nodiscard]] const GridRung* find_rung(const GridMetrics& m,
                                        double spacing) {
    for (int r = 0; r < m.rung_count; ++r) {
        if (std::fabs(m.rungs[r].spacing - spacing) < 1e-9) {
            return &m.rungs[r];
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("the ladder picks the finest rung above the min cell") {
    const GridSpec spec;  // base 1, x5, min 10px
    for (const double zoom : {0.2, 1.5, 3.0, 9.9, 26.0, 200.0}) {
        GridFrame f = stock_frame();
        f.zoom = zoom;
        const GridMetrics m = compute_grid(f, spec);
        REQUIRE(m.rung_count >= 1);
        // The step is a power-of-5 multiple of base, never below it,
        // and its cell sits in [min_cell, 5 * min_cell) - the level
        // just coarse enough to clear the density floor (at the base
        // level itself the cell is unbounded above).
        CHECK(m.step >= spec.base);
        const double ratio = m.step / spec.base;
        CHECK(ratio >= 0.999);
        const double pow5 = std::log(ratio) / std::log(5.0);
        CHECK(std::fabs(pow5 - std::round(pow5)) < 1e-9);
        CHECK(m.step * zoom >= spec.min_cell_px);
        if (m.step > spec.base * 1.000001) {
            CHECK(m.step * zoom < spec.min_cell_px * 5.0 + 1e-9);
        }
        // The rungs ladder up by the interval, finest first.
        for (int r = 1; r < m.rung_count; ++r) {
            CHECK(m.rungs[r].spacing ==
                  m.rungs[r - 1].spacing * spec.major_interval);
        }
        // The base is never subdivided, at any zoom.
        if (zoom >= spec.min_cell_px) {
            CHECK(m.step == spec.base);
        }
    }
}

TEST_CASE("the fade curve: hidden below min, full by full, gone past the vanish window") {
    const GridSpec spec;
    CHECK(grid_rung_alpha(spec.min_cell_px, spec) == 0.0f);
    CHECK(grid_rung_alpha(spec.min_cell_px - 0.01f, spec) == 0.0f);
    CHECK(grid_rung_alpha(spec.full_cell_px, spec) == doctest::Approx(1.0f));
    CHECK(grid_rung_alpha(spec.vanish_cell_px, spec) == doctest::Approx(1.0f));
    const float mid_fade =
        grid_rung_alpha((spec.min_cell_px + spec.full_cell_px) * 0.5f, spec);
    CHECK(mid_fade > 0.0f);
    CHECK(mid_fade < 1.0f);
    const float mid_vanish = grid_rung_alpha(
        (spec.vanish_cell_px + spec.vanish_cell_px * 5.0f) * 0.5f, spec);
    CHECK(mid_vanish > 0.0f);
    CHECK(mid_vanish < 1.0f);
    CHECK(grid_rung_alpha(spec.vanish_cell_px * 5.0f, spec) == 0.0f);
    CHECK(grid_rung_alpha(spec.vanish_cell_px * 6.0f, spec) == 0.0f);
    // Monotone up to the plateau's end, monotone down through the
    // vanish window: zooming never pulses a mark.
    float prev = -1.0f;
    for (float c = 0.0f; c <= spec.vanish_cell_px; c += 0.37f) {
        CHECK(grid_rung_alpha(c, spec) >= prev - 1e-6f);
        prev = grid_rung_alpha(c, spec);
    }
    prev = 2.0f;
    for (float c = spec.vanish_cell_px; c < 3600.0f; c += 0.37f) {
        CHECK(grid_rung_alpha(c, spec) <= prev + 1e-6f);
        prev = grid_rung_alpha(c, spec);
    }
}

TEST_CASE("a level change never moves or restyles a surviving mark") {
    // The no-pop contract: a mark's alpha/color/size are functions of
    // its OWN cell size, so crossing a level boundary (base * 5^k *
    // zoom == min_cell_px) changes only WHICH rungs iterate. Every
    // spacing visible on one side must appear on the other side with
    // the same look, and any spacing missing on one side must have
    // been invisible already.
    const GridSpec spec;
    const auto check_switch = [&](double z_switch) {
        INFO("switch at zoom ", z_switch);
        GridFrame a = stock_frame();
        a.zoom = z_switch * (1.0 - 1e-7);
        GridFrame b = stock_frame();
        b.zoom = z_switch * (1.0 + 1e-7);
        const GridMetrics ma = compute_grid(a, spec);
        const GridMetrics mb = compute_grid(b, spec);
        for (const GridMetrics* m : {&ma, &mb}) {
            for (int r = 0; r < m->rung_count; ++r) {
                const GridRung& g = m->rungs[r];
                if (g.alpha <= 0.01f) {
                    continue;  // invisible marks may come and go
                }
                const GridMetrics* other = m == &ma ? &mb : &ma;
                const GridRung* twin = find_rung(*other, g.spacing);
                REQUIRE(twin != nullptr);
                CHECK(twin->alpha == doctest::Approx(g.alpha).epsilon(0.02));
                CHECK(twin->major == g.major);
                // Same lattice: the same indices produce the same
                // world positions (marks never jump).
                CHECK(twin->ix0 <= g.ix0 + 1);
                CHECK(twin->ix1 >= g.ix1 - 1);
            }
        }
    };
    check_switch(10.0);   // rung 1 <-> rung 0 handoff
    check_switch(2.0);    // rung 2 <-> rung 1 handoff
    check_switch(0.4);    // rung 3 <-> rung 2 handoff
    check_switch(50.0);   // deep in a level, no switch at all
}

TEST_CASE("index ranges survive fractional centers and zooms") {
    GridSpec spec;
    spec.max_marks = 1000000;  // never the limiter here
    GridFrame f = stock_frame();
    f.center_x = 3.7;
    f.center_y = -2.2;
    f.zoom = 13.37;
    const GridMetrics m = compute_grid(f, spec);
    REQUIRE(m.rung_count >= 1);
    const double half_w = f.view_w * 0.5 / f.zoom;
    const double half_h = f.view_h * 0.5 / f.zoom;
    for (int r = 0; r < m.rung_count; ++r) {
        const GridRung& g = m.rungs[r];
        INFO("rung spacing ", g.spacing);
        REQUIRE(g.ix1 >= g.ix0);
        REQUIRE(g.iy1 >= g.iy0);
        // Tight culling: the marks just outside the range are past
        // the viewport's edge, the ones inside cover it completely.
        CHECK((g.ix0 - 1) * g.spacing < f.center_x - half_w);
        CHECK(g.ix0 * g.spacing >= f.center_x - half_w - 1e-9);
        CHECK((g.ix1 + 1) * g.spacing > f.center_x + half_w);
        CHECK(g.ix1 * g.spacing <= f.center_x + half_w + 1e-9);
        CHECK((g.iy0 - 1) * g.spacing < f.center_y - half_h);
        CHECK(g.iy1 * g.spacing <= f.center_y + half_h + 1e-9);
        // Every drawn mark lands inside the canvas-space viewport
        // (a margin of one mark covers the snapped dot's own extent).
        for (long long i = g.ix0; i <= g.ix1; ++i) {
            CHECK(m.mark_x(g, i) >= f.ox - g.cell_px);
            CHECK(m.mark_x(g, i) <= f.ox + f.view_w + g.cell_px);
        }
        for (long long j = g.iy0; j <= g.iy1; ++j) {
            CHECK(m.mark_y(g, j) >= f.oy - g.cell_px);
            CHECK(m.mark_y(g, j) <= f.oy + f.view_h + g.cell_px);
        }
    }
    // A mark sitting exactly on the viewport edge is kept (the
    // relative epsilon in the index math): center so x=40 is the
    // exact left edge at spacing 20.
    GridFrame edge = stock_frame();
    edge.zoom = 10.0;
    edge.view_w = 800.0;
    edge.center_x = 40.0;  // left edge lands exactly on world x=0
    const GridMetrics me = compute_grid(edge, spec);
    REQUIRE(me.rung_count >= 1);
    CHECK(me.rungs[0].ix0 == 0);
}

TEST_CASE("the mark cap degrades by hiding the finest rungs first") {
    GridSpec spec;  // the stock 20k cap first: the minors are visible
    GridFrame f = stock_frame();
    f.zoom = 26.0;
    // Uncapped the minor rung alone is (800/26+1)*(600/26+1) ~ 741.
    GridMetrics open = compute_grid(f, spec);
    CHECK(open.minors_visible);
    CHECK(open.mark_estimate > 500);
    spec.max_marks = 500;
    const GridMetrics capped = compute_grid(f, spec);
    CHECK(capped.mark_estimate <= 500);
    CHECK_FALSE(capped.minors_visible);
    CHECK(capped.step > open.step);  // the minors were hidden, not thinned
    // Every surviving mark keeps its own rung's spacing (a coarser
    // lattice, never a subsampled finer one).
    for (int r = 1; r < capped.rung_count; ++r) {
        CHECK(capped.rungs[r].spacing ==
              capped.rungs[r - 1].spacing * spec.major_interval);
    }
}

TEST_CASE("y-flip mirrors y and nothing else") {
    const GridSpec spec;
    GridFrame up = stock_frame();
    up.y_flip = true;
    GridFrame down = up;
    down.y_flip = false;
    const GridMetrics mu = compute_grid(up, spec);
    const GridMetrics md = compute_grid(down, spec);
    REQUIRE(mu.rung_count == md.rung_count);
    for (int r = 0; r < mu.rung_count; ++r) {
        CHECK(mu.rungs[r].ix0 == md.rungs[r].ix0);
        CHECK(mu.rungs[r].ix1 == md.rungs[r].ix1);
        CHECK(mu.rungs[r].iy0 == md.rungs[r].iy0);
        CHECK(mu.rungs[r].iy1 == md.rungs[r].iy1);
    }
    const GridRung& g = mu.rungs[0];
    // The world point at the viewport's center maps to the center
    // either way; higher world y goes UP the screen only when
    // flipped.
    CHECK(mu.mark_y(g, 0) == doctest::Approx(up.oy + up.view_h * 0.5));
    CHECK(md.mark_y(g, 0) == doctest::Approx(down.oy + down.view_h * 0.5));
    CHECK(mu.mark_y(g, 1) < mu.mark_y(g, 0));
    CHECK(md.mark_y(g, 1) > md.mark_y(g, 0));
    // x is untouched by the flip.
    CHECK(mu.mark_x(g, 1) - mu.mark_x(g, 0) ==
          doctest::Approx(g.spacing * up.zoom));
}

TEST_CASE("the draw is batched, pixel-snapped and repeatable") {
    GridFrame f = stock_frame();
    f.center_x = 3.7;   // fractional everything: if the math snapped
    f.center_y = -2.2;  // wrong, these would land off the pixel grid
    f.zoom = 13.37;
    CommandListCanvas c;
    draw_grid(c, f);
    REQUIRE(c.count(CanvasCmdKind::fill_rects) > 0);
    REQUIRE(c.count(CanvasCmdKind::fill_circle) == 0);
    REQUIRE(c.count(CanvasCmdKind::fill_polygon) == 0);
    std::size_t dots = 0;
    for (const auto& cmd : c.cmds) {
        if (cmd.kind != CanvasCmdKind::fill_rects) {
            continue;
        }
        dots += cmd.points.size();
        // 1px minor, 2px major, 3px origin - nothing else.
        CHECK((cmd.w == 1.0f || cmd.w == 2.0f || cmd.w == 3.0f));
        CHECK(cmd.w == cmd.h);
        for (const ws::ui::canvas::Point& p : cmd.points) {
            // Integer corners = a 1px dot covers exactly one pixel
            // column/row at any pan/zoom.
            CHECK(std::fabs(p.x - std::round(p.x)) < 0.01);
            CHECK(std::fabs(p.y - std::round(p.y)) < 0.01);
        }
    }
    CHECK(dots > 100);  // a real lattice, not a stub
    // Determinism: the same frame draws the identical stream twice
    // (no state accumulates between draws).
    CommandListCanvas c2;
    draw_grid(c2, f);
    REQUIRE(c2.cmds.size() == c.cmds.size());
    for (std::size_t i = 0; i < c.cmds.size(); ++i) {
        CHECK(c2.cmds[i].kind == c.cmds[i].kind);
        CHECK(c2.cmds[i].color == c.cmds[i].color);
        CHECK(c2.cmds[i].points.size() == c.cmds[i].points.size());
    }
}

TEST_CASE("hairlines sit on pixel centres") {
    GridFrame f = stock_frame();
    f.center_x = 2.31;
    f.zoom = 17.3;
    GridSpec spec;  // lines
    spec.show_dots = false;
    CommandListCanvas c;
    draw_grid(c, f, spec);
    REQUIRE(c.count(CanvasCmdKind::stroke_polyline) > 0);
    for (const auto& cmd : c.cmds) {
        if (cmd.kind != CanvasCmdKind::stroke_polyline) {
            continue;
        }
        REQUIRE(cmd.points.size() == 2);
        // A vertical line's x (or a horizontal's y) is the only
        // position that matters; its centre must sit on floor + 0.5.
        const bool vertical =
            cmd.points[0].x == cmd.points[1].x;
        const float pos = vertical ? cmd.points[0].x : cmd.points[0].y;
        CHECK(std::fabs(std::fmod(pos, 1.0f) - 0.5f) < 1e-4);
    }
}

TEST_CASE("the axes and the origin draw in the axis face") {
    const GridSpec spec;
    CommandListCanvas c;
    draw_grid(c, stock_frame());  // centered on the origin
    bool axis_line = false;
    bool origin_dot = false;
    for (const auto& cmd : c.cmds) {
        if (cmd.color != spec.axis_color) {
            continue;
        }
        if (cmd.kind == CanvasCmdKind::stroke_polyline) {
            axis_line = true;
        }
        if (cmd.kind == CanvasCmdKind::fill_rects && cmd.w == 3.0f) {
            origin_dot = true;
            REQUIRE(cmd.points.size() == 1);
            // The origin is the viewport's centre here (world 0,0).
            CHECK(cmd.points[0].x ==
                  doctest::Approx(stock_frame().ox + 400.0 - 1.0).epsilon(2.0));
        }
    }
    CHECK(axis_line);
    CHECK(origin_dot);

    // Panned far off the origin: no axes, no origin dot.
    GridFrame away = stock_frame();
    away.center_x = 500.0;
    away.center_y = 500.0;
    CommandListCanvas c2;
    draw_grid(c2, away, spec);
    for (const auto& cmd : c2.cmds) {
        CHECK(cmd.color != spec.axis_color);
    }
}

TEST_CASE("brightness scales every face, the extremes still draw") {
    GridFrame f = stock_frame();
    CommandListCanvas full;
    draw_grid(full, f, GridSpec{}, 1.0f);
    CommandListCanvas dim;
    draw_grid(dim, f, GridSpec{}, 0.5f);
    // Every alpha byte halved (the rungs, and the axis face).
    for (std::size_t i = 0; i < full.cmds.size(); ++i) {
        const int a = static_cast<int>(full.cmds[i].color & 0xFFu);
        const int b = static_cast<int>(dim.cmds[i].color & 0xFFu);
        if (a > 2) {
            CHECK(std::abs(b - static_cast<int>(std::lround(a * 0.5f)))
                  <= 1);
        }
    }
    // The acceptance zoom range: something draws at both ends and
    // never explodes past the cap.
    for (const double zoom : {0.2, 200.0}) {
        GridFrame extreme = stock_frame();
        extreme.zoom = zoom;
        const GridMetrics em = compute_grid(extreme, GridSpec{});
        CHECK(em.rung_count >= 1);
        CHECK(em.mark_estimate <= GridSpec{}.max_marks);
        CommandListCanvas c;
        draw_grid(c, extreme);
        CHECK(c.count(CanvasCmdKind::fill_rects) > 0);
    }
}

TEST_CASE("snap_coord reads the same spec the renderer draws") {
    GridSpec spec;
    spec.base = 0.5;
    spec.major_interval = 5;
    CHECK(snap_coord(1.24, spec, false) == doctest::Approx(1.0));
    CHECK(snap_coord(1.26, spec, false) == doctest::Approx(1.5));
    // Coarse = the major lattice (base * interval = 2.5 here)...
    CHECK(snap_coord(1.0, spec, true) == doctest::Approx(0.0));
    CHECK(snap_coord(1.26, spec, true) == doctest::Approx(2.5));
    CHECK(snap_coord(1.9, spec, true) == doctest::Approx(2.5));
    // ...which is an integer multiple of the fine one: every coarse
    // position IS a fine position (wires and pins can never be driven
    // off the fine lattice by the coarse snap).
    for (double v = -3.0; v <= 3.0; v += 0.37) {
        const double coarse = snap_coord(v, spec, true);
        CHECK(coarse == doctest::Approx(snap_coord(coarse, spec, false)));
    }
    // A degenerate interval disables the coarse ladder, not the snap.
    GridSpec flat = spec;
    flat.major_interval = 1;
    CHECK(snap_coord(1.26, flat, true) == doctest::Approx(1.5));
}

TEST_CASE("the exact lattice of a small viewport") {
    // 130x60 at zoom 26: x spans [-2.5, 2.5] (indices -2..2), y spans
    // [-1.15, 1.15] (indices -1..1). Rung 0 draws the 15 minors minus
    // the (0,0) the origin marker owns; rung 1's only mark is that
    // same (0,0), skipped; the top rung's (0,0) is skipped the same
    // way - the 3px origin marker draws instead - and its OTHER marks
    // are outside the range. The axes are the two remaining lines.
    GridFrame f = stock_frame();
    f.view_w = 130.0;
    f.view_h = 60.0;
    CommandListCanvas c;
    draw_grid(c, f);
    std::size_t one_px = 0;
    std::size_t two_px = 0;
    std::size_t three_px = 0;
    for (const auto& cmd : c.cmds) {
        if (cmd.kind != CanvasCmdKind::fill_rects) {
            continue;
        }
        if (cmd.w == 1.0f) {
            one_px += cmd.points.size();
        } else if (cmd.w == 2.0f) {
            two_px += cmd.points.size();
        } else if (cmd.w == 3.0f) {
            three_px += cmd.points.size();
        }
    }
    CHECK(one_px == 14);
    CHECK(two_px == 0);  // the only 2px candidate was (0,0)
    CHECK(three_px == 1);  // the origin
    CHECK(c.count(CanvasCmdKind::stroke_polyline) == 2);  // the axes
}
