// apps/circsim_editor - the Schematic panel's headless gate. The panel
// is app code, so its purity seam is the Editor's canvas_panel_canvas
// injection (a CommandListCanvas stands in for the r2d canvas): the
// cases below drive draw_canvas_panel the way the shell does - a
// GuiContext frame, a panel rect, a mouse parked off-panel - and
// assert the camera mapping the panel emits: every element's recorded
// screen geometry must land where EditorView::w2s answers, which is
// the same mapping the grid marks, the snap feedback and the picks
// already speak. The panned-camera case is the regression: the panel
// once dropped the camera's -center translate from its canvas
// transform and drew every element offset by center * zoom from the
// grid the moment the view left the origin.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/canvas_panel.hpp"
#include "../src/editor.hpp"

#include <cmath>
#include <vector>

#include <ws/circsim/model.hpp>
#include <ws/input/input.hpp>
#include <ws/settings/filesystem.hpp>
#include <ws/ui/canvas/canvas2d.hpp>
#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>

namespace {

namespace gui = ws::ui::gui;
namespace canvas = ws::ui::canvas;
using editor::Editor;
using editor::EditorView;
using editor::SchematicDocument;

// The bench: an Editor over an untitled document, a CommandListCanvas
// injected at the schematic seam, and one drawn frame per call. The
// mouse parks off-panel so no tool claims the pointer; the panel body
// (the viewport) is whatever begin_panel's content rect answers.
struct Bench {
    ws::settings::StdFileSystem fs;
    SchematicDocument doc{std::string{}, fs};
    Editor ed;
    gui::GuiContext gui;
    canvas::CommandListCanvas clc;
    gui::Rect body{};  // the viewport the last draw adopted

    Bench() {
        ed.doc = &doc;
        ed.canvas_panel_canvas = &clc;
    }

    // A wire (1,1)->(4,1) over two junctions - the drawn world's
    // simplest honest geometry (one 2-point polyline, two 32-point
    // circle fills).
    void place_wire() {
        ws::circsim::Junction a;
        a.id = doc.schem.alloc_id();
        a.pos = {1.0, 1.0};
        ws::circsim::Junction b;
        b.id = doc.schem.alloc_id();
        b.pos = {4.0, 1.0};
        doc.schem.junctions_mut().push_back(a);
        doc.schem.junctions_mut().push_back(b);
        ws::circsim::Wire w;
        w.a = ws::circsim::JunctionRef{a.id};
        w.b = ws::circsim::JunctionRef{b.id};
        w.ortho = true;
        doc.schem.wires_mut().push_back(w);
    }

    void draw() {
        clc.cmds.clear();
        ws::input::InputFrame in{};
        in.mouse_x = -1000.0f;
        in.mouse_y = -1000.0f;
        in.mouse_present = true;  // a scripted session has a mouse
        gui.begin_frame(in);
        gui::LayoutCursor layout;
        (void)layout.begin_panel(gui::Rect{0.0f, 0.0f, 640.0f, 520.0f},
                                 12.0f, 8.0f);
        body = layout.panel_content();
        const gui::ChromeTheme theme = gui::kDefaultChrome;
        editor::draw_canvas_panel(ed, gui, layout, theme);
        layout.end_panel();
        gui.end_frame();
    }
};

// Per-axis float compare (the assertions' tolerance helper).
[[nodiscard]] bool vec2_near(ws::math::Vec2 a, ws::math::Vec2 b, float tol) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::fabs(dx) <= tol && std::fabs(dy) <= tol;
}

// The wire's stroke: the 2-point polyline whose points are the two
// endpoint answers in either order. Matched by content, not by order
// in the stream - the grid's axis lines are 2-point polylines too and
// they draw first (the y-axis sits at world x=0, snapped, spanning
// the viewport exactly like a wire would if the camera were ignored).
[[nodiscard]] const canvas::CanvasCmd* wire_cmd(const Bench& b) {
    const EditorView& view = b.ed.view;
    const ws::math::Vec2 a = view.w2s({1.0, 1.0});
    const ws::math::Vec2 c = view.w2s({4.0, 1.0});
    for (const canvas::CanvasCmd& cmd : b.clc.cmds) {
        if (cmd.kind != canvas::CanvasCmdKind::stroke_polyline ||
            cmd.points.size() != 2u) {
            continue;
        }
        const ws::math::Vec2& p0 = cmd.points[0];
        const ws::math::Vec2& p1 = cmd.points[1];
        if ((vec2_near(p0, a, 0.01f) && vec2_near(p1, c, 0.01f)) ||
            (vec2_near(p0, c, 0.01f) && vec2_near(p1, a, 0.01f))) {
            return &cmd;
        }
    }
    return nullptr;
}

// The junction fills: the frame's only fill_polygons (32 segments
// each, uniformly sampled - the centroid is the circle's center).
[[nodiscard]] std::vector<const canvas::CanvasCmd*> junction_cmds(
    const Bench& b) {
    std::vector<const canvas::CanvasCmd*> out;
    for (const canvas::CanvasCmd& c : b.clc.cmds) {
        if (c.kind == canvas::CanvasCmdKind::fill_polygon) {
            out.push_back(&c);
        }
    }
    return out;
}

[[nodiscard]] ws::math::Vec2 centroid(const canvas::CanvasCmd& c) {
    // The recorded ring closes itself (the last vertex repeats the
    // first); average the distinct vertices only, or the duplicate
    // biases the answer toward vertex 0 by ~r/n px.
    std::size_t n = c.points.size();
    if (n > 1u && vec2_near(c.points.front(), c.points.back(), 1e-4f)) {
        --n;
    }
    ws::math::Vec2 sum{0.0f, 0.0f};
    for (std::size_t i = 0; i < n; ++i) {
        sum.x += c.points[i].x;
        sum.y += c.points[i].y;
    }
    const float f = static_cast<float>(n);
    return {sum.x / f, sum.y / f};
}

// The grid marks: every fill_rects corner the frame recorded.
[[nodiscard]] std::vector<ws::math::Vec2> mark_corners(const Bench& b) {
    std::vector<ws::math::Vec2> out;
    for (const canvas::CanvasCmd& c : b.clc.cmds) {
        if (c.kind == canvas::CanvasCmdKind::fill_rects) {
            out.insert(out.end(), c.points.begin(), c.points.end());
        }
    }
    return out;
}

// The one camera contract, asserted from both ends: the recorded
// element geometry equals w2s's answer (the mapping the grid, the
// feedback and the picks share), and - when the base lattice is one
// of the rungs actually drawn - the wire's endpoint sits on the 1px
// grid mark at its own lattice site: elements ride the same lattice
// the dots mark.
void assert_camera_agreement(Bench& b) {
    const EditorView& view = b.ed.view;
    REQUIRE(view.valid());

    const canvas::CanvasCmd* wire = wire_cmd(b);
    REQUIRE(wire != nullptr);
    REQUIRE(wire->points.size() == 2u);
    CHECK(vec2_near(wire->points[0], view.w2s({1.0, 1.0}), 0.01f));
    CHECK(vec2_near(wire->points[1], view.w2s({4.0, 1.0}), 0.01f));

    const std::vector<const canvas::CanvasCmd*> junctions =
        junction_cmds(b);
    REQUIRE(junctions.size() == 2u);
    CHECK(vec2_near(centroid(*junctions[0]), view.w2s({1.0, 1.0}), 0.05f));
    CHECK(vec2_near(centroid(*junctions[1]), view.w2s({4.0, 1.0}), 0.05f));

    // Is the base lattice among the drawn rungs? The same frame the
    // panel builds, through the grid's own pure half: step == base
    // means the finest drawn rung IS the snap lattice, so a mark sits
    // at (1,1). (Zoomed out, the ladder coarsens past it and the mark
    // legitimately does not exist.)
    canvas::GridFrame frame;
    frame.center_x = view.center.x;
    frame.center_y = view.center.y;
    frame.zoom = view.zoom;
    frame.ox = b.body.x;
    frame.oy = b.body.y;
    frame.view_w = b.body.width;
    frame.view_h = b.body.height;
    frame.y_flip = true;
    const canvas::GridMetrics m =
        canvas::compute_grid(frame, b.ed.grid_spec());
    if (std::fabs(m.step - b.ed.grid_spec().base) > 1e-9) {
        return;
    }

    // The 1px mark at the endpoint's lattice site: snap_mark puts an
    // odd dot's rect corner at floor(centre), so the recorded corner
    // is the floored w2s answer - and the mark's centre is within a
    // half pixel of the drawn wire's endpoint.
    const ws::math::Vec2 end = view.w2s({1.0, 1.0});
    const ws::math::Vec2 corner{std::floor(end.x), std::floor(end.y)};
    bool mark_found = false;
    for (const ws::math::Vec2& mk : mark_corners(b)) {
        if (vec2_near(mk, corner, 0.01f)) {
            mark_found = true;
            break;
        }
    }
    CHECK(mark_found);
    CHECK(vec2_near(wire->points[0],
               {corner.x + 0.5f, corner.y + 0.5f}, 0.5f));
}

TEST_CASE("the schematic panel draws the world at its w2s position") {
    SUBCASE("a panned camera (the regression: center != origin)") {
        Bench b;
        b.place_wire();
        b.ed.view.center = {3.4, 2.7};  // fractional on purpose
        b.ed.view.zoom = 26.0;
        b.draw();
        assert_camera_agreement(b);
    }
    SUBCASE("an origin camera (the fresh-document default)") {
        Bench b;
        b.place_wire();
        b.ed.view.center = {0.0, 0.0};
        b.ed.view.zoom = 26.0;
        b.draw();
        assert_camera_agreement(b);
    }
    SUBCASE("zoomed out over a panned camera") {
        Bench b;
        b.place_wire();
        b.ed.view.center = {-12.75, 8.35};
        b.ed.view.zoom = 5.0;
        b.draw();
        assert_camera_agreement(b);
    }
    SUBCASE("a stored document view rides the mapping too") {
        // document_changed() adopts the schematic's saved view - a
        // file saved away from the origin must land panned, not with
        // its elements off the grid.
        Bench b;
        b.place_wire();
        b.doc.schem.view.center = {3.4, 2.7};
        b.doc.schem.view.zoom = 26.0;
        b.ed.document_changed();
        b.draw();
        assert_camera_agreement(b);
    }
}

}  // namespace

// --- wire routing and the pin direction it reads --------------------------------

namespace {

using ws::circsim::Pos;

[[nodiscard]] int leg_heading(Pos p, Pos q) {
    if (p.y == q.y && p.x != q.x) {
        return q.x > p.x ? 0 : 180;
    }
    if (p.x == q.x && p.y != q.y) {
        return q.y > p.y ? 90 : 270;
    }
    return -1;  // zero-length or diagonal
}

// a -> waypoints -> b, as the canvas draws it.
[[nodiscard]] std::vector<Pos> full_route(Pos a, int ad, Pos b, int bd) {
    std::vector<Pos> p{a};
    for (const Pos& w : Editor::route_wire(a, ad, b, bd)) {
        p.push_back(w);
    }
    p.push_back(b);
    return p;
}

}  // namespace

TEST_CASE("route_wire leaves a pin outward and reaches one from outside") {
    // Resistor-style pins: left pin faces 180, right pin faces 0.
    SUBCASE("facing pins: a straight run, no waypoints") {
        CHECK(Editor::route_wire({3.0, 0.0}, 0, {9.0, 0.0}, 180).empty());
    }
    SUBCASE("the end approaches from outside the part, not through it") {
        // b = (10,0) faces left (180): the wire must arrive travelling
        // +x from x < 10 - never from x = 11 (the body side).
        const std::vector<Pos> p = full_route({0.0, 0.0}, -1, {10.0, 0.0}, 180);
        REQUIRE(p.size() >= 2u);
        CHECK(leg_heading(p[p.size() - 2u], p.back()) == 0);
    }
    SUBCASE("a target behind the start pin wraps around instead of doubling back") {
        // a faces +x but the partner sits to its left.
        const std::vector<Pos> p = full_route({5.0, 0.0}, 0, {0.0, 3.0}, 180);
        REQUIRE(p.size() >= 3u);
        CHECK(leg_heading(p[0], p[1]) == 0);  // out along a_dir first
        int prev = -1;
        for (std::size_t i = 0; i + 1 < p.size(); ++i) {
            const int h = leg_heading(p[i], p[i + 1]);
            CHECK(h >= 0);  // axis-aligned, no zero-length leg
            if (prev >= 0) {
                CHECK((prev + 180) % 360 != h);  // never reverses
            }
            prev = h;
        }
        CHECK(leg_heading(p[p.size() - 2u], p.back()) == 0);  // arrives +x
    }
    SUBCASE("every pin/pin orientation pair stays orthogonal and reversal-free") {
        for (const int ad : {-1, 0, 90, 180, 270}) {
            for (const int bd : {-1, 0, 90, 180, 270}) {
                for (int dx = -3; dx <= 3; ++dx) {
                    for (int dy = -3; dy <= 3; ++dy) {
                        if (dx == 0 && dy == 0) {
                            continue;
                        }
                        const std::vector<Pos> p = full_route(
                            {0.0, 0.0}, ad, {double(dx), double(dy)}, bd);
                        int prev = -1;
                        for (std::size_t i = 0; i + 1 < p.size(); ++i) {
                            const int h = leg_heading(p[i], p[i + 1]);
                            REQUIRE(h >= 0);
                            if (i == 0 && ad >= 0) {
                                REQUIRE(h == ad);
                            }
                            if (prev >= 0) {
                                REQUIRE((prev + 180) % 360 != h);
                            }
                            prev = h;
                        }
                        if (bd >= 0) {
                            REQUIRE(prev == (bd + 180) % 360);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("hit_pin reports the pin's direction in world space") {
    Bench b;
    ws::circsim::Part r = ws::circsim::Part::make("r", "R1", "1k", {0.0, 0.0});
    r.id = b.doc.schem.alloc_id();
    r.rot = 90;  // a (local 180) now faces down, b (local 0) faces up
    b.doc.schem.parts_mut().push_back(r);
    const ws::circsim::Part& placed = b.doc.schem.parts().back();
    const ws::circsim::SymbolDef* sym = b.ed.sym_of(placed);
    REQUIRE(sym != nullptr);

    const Pos a_at = ws::circsim::pin_pos(*sym, placed, 0);
    const auto a_hit = b.ed.hit_pin(a_at, 0.1);
    REQUIRE(a_hit.has_value());
    CHECK(a_hit->dir == 270);

    const Pos b_at = ws::circsim::pin_pos(*sym, placed, 1);
    const auto b_hit = b.ed.hit_pin(b_at, 0.1);
    REQUIRE(b_hit.has_value());
    CHECK(b_hit->dir == 90);
}
