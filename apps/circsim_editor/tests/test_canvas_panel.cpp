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
using ws::circsim::Pos;

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

    // One clicked frame: the mouse at `world`'s own screen position
    // (the same view the pointer handler reads), `button` pressed -
    // the edge the wire tool arms and commits on (button 0) or
    // cancels on (button 2). The first call draws a prime frame
    // first: the view's viewport (vx..vh) is adopted inside
    // draw_canvas_panel, so w2s before any draw would map through a
    // zero rect and land the click outside the panel.
    void click_at(Pos world, int button = 0) {
        if (!ed.view.valid()) {
            draw();
        }
        const ws::math::Vec2 sp = ed.view.w2s(world);
        clc.cmds.clear();
        ws::input::InputFrame in{};
        in.mouse_x = sp.x;
        in.mouse_y = sp.y;
        in.mouse_present = true;
        in.mouse_pressed[button] = true;
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

    // The two-click wire draft over `world` points: arm the tool, click
    // the start, click the end.
    void wire_click(Pos a, Pos b) {
        ed.tool = editor::Tool::Wire;
        click_at(a);
        click_at(b);
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

// --- wire-on-wire snap (stage 18b): the manual-wiring fix ---------------------
//
// The wire tool's snap was pin -> junction -> grid, and never a wire's
// body. A click near another wire landed a hair off the grid, the new
// wire's endpoint sat at the wrong position, and nets.cpp's exact
// on_polyline missed the connection - the "manually setting up the
// wire goes wrong" symptom. The fix snaps to the nearest grid point on
// a wire's path and mints a junction there (the standard EDA T-tap);
// the cases below pin the geometry helper that does that snap.

TEST_CASE("hit_wire_point: a click near a wire's body snaps onto it") {
    Bench b;
    b.place_wire();  // path (1,1) -> (4,1), grid=1
    const auto hit = b.ed.hit_wire_point({2.7, 1.05}, 0.4);
    REQUIRE(hit.has_value());
    CHECK(hit->pos.x == 3.0);  // the nearest grid x on the segment
    CHECK(hit->pos.y == 1.0);  // the segment's stationary y
}

TEST_CASE("hit_wire_point: a click far from every wire returns nothing") {
    Bench b;
    b.place_wire();
    CHECK_FALSE(b.ed.hit_wire_point({20.0, 20.0}, 0.4).has_value());
}

TEST_CASE("hit_wire_point: the snap lands on the nearer endpoint when "
          "the grid snap would drift off the segment") {
    Bench b;
    b.place_wire();  // (1,1) -> (4,1); a click at (4.6, 1) is past b
    const auto hit = b.ed.hit_wire_point({4.6, 1.0}, 0.7);
    REQUIRE(hit.has_value());
    CHECK(hit->pos.x == 4.0);
    CHECK(hit->pos.y == 1.0);
}

TEST_CASE("hit_wire_point: the snap is bit-equal to a path vertex when "
          "the cursor sits on one") {
    Bench b;
    // A two-segment path (an L): (0,0) -> (3,0) -> (3,3). route_wire
    // produces ortho-on-grid vertices; the elbow at (3,0) is the
    // vertex a T-tap would land on.
    ws::circsim::Junction a;
    a.id = b.doc.schem.alloc_id();
    a.pos = {0.0, 0.0};
    ws::circsim::Junction c;
    c.id = b.doc.schem.alloc_id();
    c.pos = {3.0, 3.0};
    b.doc.schem.junctions_mut().push_back(a);
    b.doc.schem.junctions_mut().push_back(c);
    ws::circsim::Wire w;
    w.a = ws::circsim::JunctionRef{a.id};
    w.b = ws::circsim::JunctionRef{c.id};
    w.waypoints.push_back({3.0, 0.0});
    w.ortho = true;
    b.doc.schem.wires_mut().push_back(w);
    const auto hit = b.ed.hit_wire_point({3.2, 0.1}, 0.4);
    REQUIRE(hit.has_value());
    CHECK(hit->pos.x == 3.0);
    CHECK(hit->pos.y == 0.0);
}

TEST_CASE("hit_wire_point: among several wires, the nearest segment wins") {
    Bench b;
    // Two horizontal wires on y=0 and y=5.
    auto add_wire = [&](double y) {
        ws::circsim::Junction a;
        a.id = b.doc.schem.alloc_id();
        a.pos = {0.0, y};
        ws::circsim::Junction c;
        c.id = b.doc.schem.alloc_id();
        c.pos = {10.0, y};
        b.doc.schem.junctions_mut().push_back(a);
        b.doc.schem.junctions_mut().push_back(c);
        ws::circsim::Wire w;
        w.a = ws::circsim::JunctionRef{a.id};
        w.b = ws::circsim::JunctionRef{c.id};
        w.ortho = true;
        b.doc.schem.wires_mut().push_back(w);
    };
    add_wire(0.0);
    add_wire(5.0);
    // A click at (5, 0.2) is much closer to the y=0 wire.
    const auto hit = b.ed.hit_wire_point({5.0, 0.2}, 1.0);
    REQUIRE(hit.has_value());
    CHECK(hit->pos.y == 0.0);
}

// --- route_wire: the reversal keeps its lead, the degrees normalize ----

TEST_CASE("route_wire: a reversal target keeps the lead-out corner") {
    // A pin at (0,0) facing +x (dir 0) with the target at (-5,0): the
    // collinear fold once read the reversal as a continuation and
    // swallowed the (1,0) lead - the wire ran straight back through
    // the part body it just left.
    const std::vector<Pos> route =
        editor::Editor::route_wire({0.0, 0.0}, 0, {-5.0, 0.0}, -1);
    REQUIRE(route.size() == 1u);
    CHECK(route[0].x == 1.0);
    CHECK(route[0].y == 0.0);
}

TEST_CASE("route_wire: 360 degrees routes exactly as 0") {
    // start_horizontal compared raw degrees, so 360 fell through the
    // 0/180 test and turned the L vertical-first. The normalization
    // makes every spelling of east route the same.
    const std::vector<Pos> a =
        editor::Editor::route_wire({0.0, 0.0}, 0, {4.0, 3.0}, -1);
    const std::vector<Pos> b =
        editor::Editor::route_wire({0.0, 0.0}, 360, {4.0, 3.0}, -1);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].x == b[i].x);
        CHECK(a[i].y == b[i].y);
    }
}

// --- hit_wire_point rides the segment, hit_any walks the draw stack ----

TEST_CASE("hit_wire_point: a cursor riding the wire between grid lines "
          "still lands") {
    // The acceptance test once measured the distance to the SNAPPED
    // candidate: at zoom 400 the pick radius is 0.0225 world units,
    // and a cursor on the wire halfway between grid intersections
    // missed - its snap sat half a cell away. The segment's own
    // distance is what a wire pick means.
    Bench b;
    b.place_wire();  // (1,1) -> (4,1), grid 1; (2.5,1) has no grid line
    const double pick = 9.0 / 400.0;
    const auto hit = b.ed.hit_wire_point({2.5, 1.02}, pick);
    REQUIRE(hit.has_value());
    CHECK(hit->pos.x == 3.0);  // the candidate is still grid-aligned
    CHECK(hit->pos.y == 1.0);
}

TEST_CASE("hit_any: the nearest wire wins, not the first in the list") {
    Bench b;
    // Two wires on y=0 and y=0.4 (the nearer one SECOND in the list -
    // the old loop returned the first wire in range and never
    // compared). Wires need ids: the bench's own helper leaves them 0.
    auto add_wire = [&](double y, std::uint32_t id) {
        ws::circsim::Junction a;
        a.id = b.doc.schem.alloc_id();
        a.pos = {0.0, y};
        ws::circsim::Junction c;
        c.id = b.doc.schem.alloc_id();
        c.pos = {10.0, y};
        b.doc.schem.junctions_mut().push_back(a);
        b.doc.schem.junctions_mut().push_back(c);
        ws::circsim::Wire w;
        w.id = id;
        w.a = ws::circsim::JunctionRef{a.id};
        w.b = ws::circsim::JunctionRef{c.id};
        b.doc.schem.wires_mut().push_back(w);
    };
    add_wire(0.4, 101);
    add_wire(0.0, 102);
    const editor::Selection hit = b.ed.hit_any({5.0, 0.05}, 1.0, 1.0);
    CHECK(hit.kind == editor::Selection::Kind::Wire);
    CHECK(hit.id == 102);
}

TEST_CASE("hit_any: a wire crossing a part's box stays selectable") {
    Bench b;
    ws::circsim::Part p = ws::circsim::Part::make("r", "R1", "1k",
                                                  {5.0, 0.0});
    p.id = b.doc.schem.alloc_id();
    b.doc.schem.parts_mut().push_back(p);
    // A horizontal wire straight through the part's box center (the
    // box is read from the editor's own bounds, so the case needs no
    // assumption about the r symbol's shape).
    const ws::circsim::Bounds box = b.ed.part_bounds(p.id).value_or(
        ws::circsim::Bounds{4.0, -1.0, 6.0, 1.0});
    const double cy = (box.min_y + box.max_y) * 0.5;
    ws::circsim::Junction a;
    a.id = b.doc.schem.alloc_id();
    a.pos = {box.min_x - 2.0, cy};
    ws::circsim::Junction c;
    c.id = b.doc.schem.alloc_id();
    c.pos = {box.max_x + 2.0, cy};
    b.doc.schem.junctions_mut().push_back(a);
    b.doc.schem.junctions_mut().push_back(c);
    ws::circsim::Wire w;
    w.id = b.doc.schem.alloc_id();
    w.a = ws::circsim::JunctionRef{a.id};
    w.b = ws::circsim::JunctionRef{c.id};
    b.doc.schem.wires_mut().push_back(w);
    // The click lands inside the part's box AND on the wire.
    const editor::Selection hit = b.ed.hit_any(
        {(box.min_x + box.max_x) * 0.5, cy}, 0.35, 0.35);
    CHECK(hit.kind == editor::Selection::Kind::Wire);
    // Without the wire the same click is the part's.
    b.doc.schem.wires_mut().clear();
    b.doc.schem.junctions_mut().clear();
    const editor::Selection again = b.ed.hit_any(
        {(box.min_x + box.max_x) * 0.5, cy}, 0.35, 0.35);
    CHECK(again.kind == editor::Selection::Kind::Part);
    CHECK(again.id == p.id);
}

TEST_CASE("hit_any: the label pick reach rides the zoom") {
    Bench b;
    ws::circsim::Label l;
    l.id = 7;
    l.pos = {0.0, 0.0};
    l.name = "net";
    b.doc.schem.labels_mut().push_back(l);
    // At zoom 400 a fixed world-unit margin once answered clicks 1
    // unit (400px!) away. The zoom-scaled reach is a few px.
    const double pick = 9.0 / 400.0;
    const double label_pick = pick + 6.0 / 400.0;
    CHECK_FALSE(b.ed.hit_any({0.5, 0.0}, pick, label_pick)
                     .kind == editor::Selection::Kind::Label);
    const editor::Selection near =
        b.ed.hit_any({0.01, 0.0}, pick, label_pick);
    CHECK(near.kind == editor::Selection::Kind::Label);
    CHECK(near.id == 7);
}

TEST_CASE("route_wire: a free start flips the L with a_vertical") {
    const std::vector<Pos> h =
        editor::Editor::route_wire({0.0, 0.0}, -1, {4.0, 3.0}, -1);
    const std::vector<Pos> v =
        editor::Editor::route_wire({0.0, 0.0}, -1, {4.0, 3.0}, -1, true);
    REQUIRE(h.size() == 1u);  // horizontal-first: run east, then north
    CHECK(h[0].x == 4.0);
    CHECK(h[0].y == 0.0);
    REQUIRE(v.size() == 1u);  // vertical-first: climb, then run east
    CHECK(v[0].x == 0.0);
    CHECK(v[0].y == 3.0);
}

// --- the zero-length commit guard (stage 18c) --------------------------------
//
// The commit guard fired for Free landings only, so a second click on
// the START entity committed a degenerate self-loop wire: identical
// endpoints, an empty folded route - and for a wire-point start, two
// junctions minted at one position. The preview drew nothing for the
// case (its own t.pos == a_pos check), so the user saw no preview and
// a junk wire: the exact preview-vs-commit disagreement the wire tool
// must never have. The guard now covers every landing kind; the cases
// below drive the real two-click draft through draw_canvas_panel.

TEST_CASE("the wire tool: a second click on the start pin commits nothing") {
    Bench b;
    ws::circsim::Part p = ws::circsim::Part::make("r", "R1", "1k", {0.0, 0.0});
    p.id = b.doc.schem.alloc_id();
    b.doc.schem.parts_mut().push_back(p);
    // Pin 1 of the built-in r sits at (3, 0). Click it twice: the draft
    // arms on the first click and stays armed on the second - no wire,
    // no disarm, the user can still reach a real target.
    b.wire_click({3.0, 0.0}, {3.0, 0.0});
    CHECK(b.doc.schem.wires().empty());
    CHECK(b.ed.wire_draft.active);
    CHECK(b.ed.tool == editor::Tool::Wire);
}

TEST_CASE("the wire tool: a second click on the start junction commits "
          "nothing") {
    Bench b;
    b.place_wire();  // junctions at (1,1) and (4,1), one wire
    b.wire_click({1.0, 1.0}, {1.0, 1.0});
    CHECK(b.doc.schem.wires().size() == 1u);
    CHECK(b.doc.schem.junctions().size() == 2u);
    CHECK(b.ed.wire_draft.active);
}

TEST_CASE("the wire tool: a second click on the start wire-point mints "
          "nothing") {
    Bench b;
    b.place_wire();  // path (1,1) -> (4,1); (3,1) is its grid-snapped body
    b.wire_click({2.9, 1.0}, {2.9, 1.0});
    CHECK(b.doc.schem.wires().size() == 1u);
    CHECK(b.doc.schem.junctions().size() == 2u);  // no minted pair
    CHECK(b.ed.wire_draft.active);
}

TEST_CASE("the wire tool: a commit chains the next draft at the landing") {
    Bench b;
    b.place_wire();  // junctions (1,1) and (4,1), one wire
    b.wire_click({1.0, 1.0}, {4.0, 1.0});
    CHECK(b.doc.schem.wires().size() == 2u);
    // The tool stays armed and the draft chains: the next start is
    // the landing junction, so a multi-segment net is one run of
    // clicks (every segment used to cost a W press and a re-pick).
    CHECK(b.ed.tool == editor::Tool::Wire);
    CHECK(b.ed.wire_draft.active);
    CHECK(b.ed.wire_draft.a_pos.x == 4.0);
    CHECK(b.ed.wire_draft.a_pos.y == 1.0);
    const auto* j =
        std::get_if<ws::circsim::JunctionRef>(&b.ed.wire_draft.a);
    REQUIRE(j != nullptr);
    bool landing_exists = false;
    for (const ws::circsim::Junction& jj : b.doc.schem.junctions()) {
        landing_exists = landing_exists || jj.id == j->junction;
    }
    CHECK(landing_exists);
    // A third click commits the second segment with no re-arm.
    b.click_at({7.0, 1.0});
    CHECK(b.doc.schem.wires().size() == 3u);
}

TEST_CASE("the wire tool: a click on free space starts a draft now") {
    Bench b;
    ws::circsim::Part p =
        ws::circsim::Part::make("r", "R1", "1k", {0.0, 0.0});
    p.id = b.doc.schem.alloc_id();
    b.doc.schem.parts_mut().push_back(p);
    b.ed.tool = editor::Tool::Wire;
    b.click_at({6.0, 3.0});  // free grid - the old start ignored it
    CHECK(b.ed.wire_draft.active);
    CHECK(b.ed.wire_draft.a_free);
    CHECK(b.ed.wire_draft.a_mint);
    CHECK(b.ed.wire_draft.a_dir == -1);
    // The commit mints the start junction and lands on the pin.
    b.click_at({3.0, 0.0});  // pin 1 of r
    CHECK(b.doc.schem.wires().size() == 1u);
    CHECK(b.doc.schem.junctions().size() == 1u);  // the minted start
    const ws::circsim::Wire& w = b.doc.schem.wires().front();
    CHECK(std::holds_alternative<ws::circsim::JunctionRef>(w.a));
    CHECK(std::holds_alternative<ws::circsim::PinRef>(w.b));
    const auto* pin =
        std::get_if<ws::circsim::PinRef>(&b.ed.wire_draft.a);
    REQUIRE(pin != nullptr);  // the chain starts at the pin
    CHECK(pin->part == p.id);
}

TEST_CASE("the wire tool: the first right click cancels only the draft") {
    Bench b;
    b.place_wire();
    b.ed.tool = editor::Tool::Wire;
    b.click_at({1.0, 1.0});
    CHECK(b.ed.wire_draft.active);
    b.click_at({9.0, 9.0}, 2);
    CHECK_FALSE(b.ed.wire_draft.active);
    CHECK(b.ed.tool == editor::Tool::Wire);  // the tool survives
    b.click_at({9.0, 9.0}, 2);
    CHECK(b.ed.tool == editor::Tool::Select);  // the second disarms
}

TEST_CASE("the wire tool: a start anchor deleted mid-draft cancels at "
          "commit") {
    Bench b;
    ws::circsim::Junction a;
    a.id = b.doc.schem.alloc_id();
    a.pos = {1.0, 1.0};
    ws::circsim::Junction c;
    c.id = b.doc.schem.alloc_id();
    c.pos = {4.0, 1.0};
    b.doc.schem.junctions_mut().push_back(a);
    b.doc.schem.junctions_mut().push_back(c);
    ws::circsim::Wire w;
    w.id = b.doc.schem.alloc_id();
    w.a = ws::circsim::JunctionRef{a.id};
    w.b = ws::circsim::JunctionRef{c.id};
    b.doc.schem.wires_mut().push_back(w);
    b.ed.tool = editor::Tool::Wire;
    b.click_at({1.0, 1.0});  // the draft starts on the junction
    CHECK(b.ed.wire_draft.active);
    // The wire - and with it both junctions - dies before the commit.
    editor::push_delete_wire(b.doc, w.id);
    CHECK(b.doc.schem.junctions().empty());
    b.click_at({7.0, 1.0});
    // No dangling endpoint landed: the commit revalidated, cancelled
    // the draft with a word, and the schematic is untouched.
    CHECK(b.doc.schem.wires().empty());
    CHECK_FALSE(b.ed.wire_draft.active);
    CHECK(b.ed.tool == editor::Tool::Wire);
}

}  // namespace
