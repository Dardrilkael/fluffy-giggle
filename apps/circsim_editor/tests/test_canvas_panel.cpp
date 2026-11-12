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
#include <string>
#include <vector>

#include <ws/circsim/export.hpp>
#include <ws/circsim/model.hpp>
#include <ws/circsim/overlay.hpp>
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
using editor::Selection;
using ws::circsim::Pos;
using ws::circsim::Part;
using ws::circsim::Junction;
using ws::circsim::Wire;
using ws::circsim::Label;

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
    // The theme the frames draw with - a copy the case may tune (the
    // glow's ease test parks the motion switch, the way the app's own
    // theme setting does).
    gui::ChromeTheme theme = gui::kDefaultChrome;

    Bench() {
        ed.doc = &doc;
        ed.canvas_panel_canvas = &clc;
    }

    // A wire (1,1)->(4,1) over two junctions - the drawn world's
    // simplest honest geometry (one 2-point polyline, two 32-point
    // circle fills). The wire owns a minted id the way a loaded
    // one does: every entity the schematic stores carries a distinct
    // id (ids start at 1; 0 is the "none" sentinel), and the bench's
    // old id-0 shortcut collided with exactly the bug class the
    // drawn-wire path had.
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
        w.id = doc.schem.alloc_id();
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
        click_screen(sp.x, sp.y, button);
    }

    // One clicked frame at raw SCREEN coordinates - the overlays (the
    // mode toggle, the transport) live there, not in the world.
    void click_screen(float x, float y, int button = 0) {
        if (!ed.view.valid()) {
            draw();
        }
        clc.cmds.clear();
        ws::input::InputFrame in{};
        in.mouse_x = x;
        in.mouse_y = y;
        in.mouse_present = true;
        in.mouse_pressed[button] = true;
        gui.begin_frame(in);
        gui::LayoutCursor layout;
        (void)layout.begin_panel(gui::Rect{0.0f, 0.0f, 640.0f, 520.0f},
                                 12.0f, 8.0f);
        body = layout.panel_content();
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

// The junction fills: the circle rings whose CENTROIDS sit on the two
// junctions' w2s answers. Matched by content, not by order or
// exclusivity in the stream - the interactive canvas's overlays (the
// mode toggle's active face, the transport) draw fills of their own,
// so "the frame's only fill_polygons" stopped being the junctions'
// signature the day the canvas grew chrome.
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

[[nodiscard]] std::vector<const canvas::CanvasCmd*> junction_cmds(
    const Bench& b) {
    const EditorView& view = b.ed.view;
    const ws::math::Vec2 want[2] = {view.w2s({1.0, 1.0}),
                                    view.w2s({4.0, 1.0})};
    std::vector<const canvas::CanvasCmd*> out;
    for (const canvas::CanvasCmd& c : b.clc.cmds) {
        if (c.kind != canvas::CanvasCmdKind::fill_polygon) {
            continue;
        }
        for (const ws::math::Vec2& w : want) {
            if (vec2_near(centroid(c), w, 1.0f)) {
                out.push_back(&c);
                break;
            }
        }
    }
    return out;
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
    const editor::Selection hit = b.ed.hit_any({5.0, 0.05}, 1.0);
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
        {(box.min_x + box.max_x) * 0.5, cy}, 0.35);
    CHECK(hit.kind == editor::Selection::Kind::Wire);
    // Without the wire the same click is the part's.
    b.doc.schem.wires_mut().clear();
    b.doc.schem.junctions_mut().clear();
    const editor::Selection again = b.ed.hit_any(
        {(box.min_x + box.max_x) * 0.5, cy}, 0.35);
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
    const auto hit = b.ed.hit_any({0.5, 0.0}, 9.0 / 400.0);
    CHECK_FALSE(hit.kind == editor::Selection::Kind::Label);
    const auto near_hit = b.ed.hit_any({0.01, 0.0}, 9.0 / 400.0);
    CHECK(near_hit.kind == editor::Selection::Kind::Label);
    CHECK(near_hit.id == 7);
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
    b.click_at({9.0, 9.0}, ws::input::mouse::kRight);
    CHECK_FALSE(b.ed.wire_draft.active);
    CHECK(b.ed.tool == editor::Tool::Wire);  // the tool survives
    b.click_at({9.0, 9.0}, ws::input::mouse::kRight);
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

// --- the move re-route: attached ortho wires keep their legs --------

TEST_CASE("a moved junction re-routes its attached wire, the undo "
          "restores it") {
    Bench b;
    ws::circsim::Junction ja;
    ja.id = b.doc.schem.alloc_id();
    ja.pos = {0.0, 0.0};
    ws::circsim::Junction jb;
    jb.id = b.doc.schem.alloc_id();
    jb.pos = {10.0, 0.0};
    b.doc.schem.junctions_mut().push_back(ja);
    b.doc.schem.junctions_mut().push_back(jb);
    ws::circsim::Wire w;
    w.id = b.doc.schem.alloc_id();
    w.a = ws::circsim::JunctionRef{ja.id};
    w.b = ws::circsim::JunctionRef{jb.id};
    w.ortho = true;  // only ortho wires re-route (freeform keeps its shape)
    b.doc.schem.wires_mut().push_back(w);
    // Slide ja to (0,5) and read the routes from the editor (the
    // entity is at its landing, the routes' contract).
    for (ws::circsim::Junction& j : b.doc.schem.junctions_mut()) {
        if (j.id == ja.id) {
            j.pos = {0.0, 5.0};
        }
    }
    const std::vector<editor::WireRoute> routes =
        b.ed.attached_wire_routes(editor::Editor::Drag::Kind::Junction,
                                  ja.id);
    REQUIRE(routes.size() == 1u);
    REQUIRE(routes[0].wire == w.id);
    REQUIRE(routes[0].waypoints.size() == 1u);  // the fresh L
    CHECK(routes[0].waypoints[0].x == 10.0);    // horizontal-first
    CHECK(routes[0].waypoints[0].y == 5.0);
    // The command lands the route with the move. The entity goes home
    // before the push (the release flow's set_drag_pos(start_world)),
    // so the command's from is the press position, not the landing.
    for (ws::circsim::Junction& j : b.doc.schem.junctions_mut()) {
        if (j.id == ja.id) {
            j.pos = {0.0, 0.0};
        }
    }
    editor::push_move_junction(b.doc, ja.id, {0.0, 5.0}, routes);
    CHECK(b.doc.schem.find_junction(ja.id)->pos.y == 5.0);
    CHECK(b.doc.schem.find_wire(w.id)->waypoints.size() == 1u);
    (void)b.doc.undo().undo();
    CHECK(b.doc.schem.find_junction(ja.id)->pos.y == 0.0);
    CHECK(b.doc.schem.find_wire(w.id)->waypoints.empty());
    (void)b.doc.undo().redo();
    CHECK(b.doc.schem.find_junction(ja.id)->pos.y == 5.0);
    CHECK(b.doc.schem.find_wire(w.id)->waypoints.size() == 1u);
}

// --- the pin-approach route: wires land on pins tip-first ---------------------
//
// A pin's dir is its OUTWARD facing (the lead away from the body), and
// route_wire's a side reads it that way (from = a + u*lead, one lead
// OUTSIDE). The b side once computed its approach target as
// b - u*lead - one lead INSIDE the part - so the final leg ran out of
// the part's own body to the pin tip: every wire that landed on a pin
// stabbed through the part and doubled back.

TEST_CASE("route_wire: a pin landing approaches tip-first from the pin's "
          "own outside") {
    // The most ordinary wire there is: two facing pins, R1.b at (3,0)
    // facing east, R2.a at (7,0) facing west. The route is the
    // straight run between them - no corner, and never a point inside
    // either part's body span ([-3,3] and [7,13]).
    const std::vector<Pos> route =
        editor::Editor::route_wire({3.0, 0.0}, 0, {7.0, 0.0}, 180);
    for (const Pos& p : route) {
        CHECK(p.x > 3.0);
        CHECK(p.x < 7.0);
    }
    // The free-run case lands on a pin from a corner: the approach leg
    // stops one lead EAST of R1's east pin (x=4, outside the body) and
    // closes west into the tip - not at x=2, inside the zigzag.
    const std::vector<Pos> cornered =
        editor::Editor::route_wire({6.0, 3.0}, -1, {3.0, 0.0}, 0);
    REQUIRE(cornered.size() == 2u);
    CHECK(cornered.back().x == 4.0);
    CHECK(cornered.back().y == 0.0);
}

TEST_CASE("the wire tool: a pin-to-pin wire runs straight between facing "
          "pins and connects them in one net") {
    Bench b;
    ws::circsim::Part r1 =
        ws::circsim::Part::make("r", "R1", "1k", {0.0, 0.0});
    r1.id = b.doc.schem.alloc_id();
    b.doc.schem.parts_mut().push_back(r1);
    ws::circsim::Part r2 =
        ws::circsim::Part::make("r", "R2", "2k", {10.0, 0.0});
    r2.id = b.doc.schem.alloc_id();
    b.doc.schem.parts_mut().push_back(r2);
    // R1's b pin sits at (3,0); R2's a pin at (7,0) - facing tips.
    b.wire_click({3.0, 0.0}, {7.0, 0.0});
    REQUIRE(b.doc.schem.wires().size() == 1u);
    const ws::circsim::Wire& w = b.doc.schem.wires().front();
    const std::vector<Pos> path = b.ed.wire_path(w);
    // The straight run: endpoints and nothing else - the old inside-out
    // approach put a waypoint PAST the landing pin (x=8, inside R2).
    REQUIRE(path.size() == 2u);
    CHECK(path.front().x == 3.0);
    CHECK(path.front().y == 0.0);
    CHECK(path.back().x == 7.0);
    CHECK(path.back().y == 0.0);
    // Both pins ride the wire: one net, the wire in it.
    ws::circsim::NetBuild nb =
        ws::circsim::build_nets(b.doc.schem, &b.ed.library);
    const ws::circsim::Net* n1 =
        nb.nets.net_of_pin(ws::circsim::PinRef{r1.id, 1u});
    const ws::circsim::Net* n2 =
        nb.nets.net_of_pin(ws::circsim::PinRef{r2.id, 0u});
    REQUIRE(n1 != nullptr);
    REQUIRE(n2 != nullptr);
    CHECK(n1->id == n2->id);
    CHECK(n1->wires.size() == 1u);
}

TEST_CASE("the wire tool: a landing on a pin from a corner stays outside "
          "the part's body") {
    Bench b;
    ws::circsim::Part r1 =
        ws::circsim::Part::make("r", "R1", "1k", {0.0, 0.0});
    r1.id = b.doc.schem.alloc_id();
    b.doc.schem.parts_mut().push_back(r1);
    // Free start at (6,3), land on R1's b pin at (3,0): the approach
    // runs east to (4,0) then closes west into the tip - the last
    // interior vertex is OUTSIDE the body (x > 3), never x=2.
    b.wire_click({6.0, 3.0}, {3.0, 0.0});
    REQUIRE(b.doc.schem.wires().size() == 1u);
    const std::vector<Pos> path = b.ed.wire_path(b.doc.schem.wires().front());
    REQUIRE(path.size() == 4u);
    CHECK(path[0].x == 6.0);
    CHECK(path[0].y == 3.0);
    CHECK(path[1].x == 4.0);  // the corner above the pin's outside
    CHECK(path[1].y == 3.0);
    CHECK(path[2].x == 4.0);  // the approach stop, one lead outside
    CHECK(path[2].y == 0.0);
    CHECK(path[3].x == 3.0);  // the pin tip
    CHECK(path[3].y == 0.0);
}

TEST_CASE("the wire tool: a T-tap onto a wire's body joins the nets") {
    Bench b;
    b.place_wire();  // junctions (1,1)/(4,1), one wire between
    // Tap the wire's body at (3,1) and run up to free grid (3,5).
    b.wire_click({2.9, 1.0}, {3.0, 5.0});
    REQUIRE(b.doc.schem.wires().size() == 2u);
    // The tap minted a junction ON the first wire's path, the landing
    // another; the new wire runs straight (3,1) -> (3,5).
    CHECK(b.doc.schem.junctions().size() == 4u);
    const std::vector<Pos> path =
        b.ed.wire_path(b.doc.schem.wires().back());
    REQUIRE(path.size() == 2u);
    CHECK(path.front().x == 3.0);
    CHECK(path.front().y == 1.0);
    CHECK(path.back().x == 3.0);
    CHECK(path.back().y == 5.0);
    // The nets say what the drawing says: the tap rides the first wire.
    const std::uint32_t old_wire = b.doc.schem.wires().front().id;
    const std::uint32_t new_wire = b.doc.schem.wires().back().id;
    ws::circsim::NetBuild nb =
        ws::circsim::build_nets(b.doc.schem, &b.ed.library);
    const ws::circsim::Net* a = nb.nets.net_of_wire(old_wire);
    const ws::circsim::Net* c = nb.nets.net_of_wire(new_wire);
    REQUIRE(a != nullptr);
    REQUIRE(c != nullptr);
    CHECK(a->id == c->id);
}

// --- the id-mint regressions (the "vanishing wire" class) ------------------
//
// Every entity the schematic stores owns a DISTINCT id (ids start at
// 1; 0 is the "none" sentinel). The wire commit, the palette's part
// placement and the label placement all mint theirs now; the
// push_place_* seams mint as belt-and-braces. The old default-0 wires
// collided on one id, and everything keyed on Wire::id answered wrong.

TEST_CASE("the wire tool: every drawn wire owns a distinct id") {
    Bench b;
    // Two segments through the real click path: the first mints its
    // own junctions, the second chains off the landing.
    b.wire_click({1.0, 1.0}, {4.0, 1.0});
    b.click_at({7.0, 1.0});
    REQUIRE(b.doc.schem.wires().size() == 2u);
    const std::uint32_t first = b.doc.schem.wires().front().id;
    const std::uint32_t second = b.doc.schem.wires().back().id;
    CHECK(first != 0u);
    CHECK(second != 0u);
    CHECK(first != second);
}

TEST_CASE("the wire tool: deleting a wire keeps the junctions its "
          "survivors use") {
    Bench b;
    // Wire A (1,1)->(4,1); wire B chains off A's landing junction and
    // runs to (7,1) - B and A SHARE the middle junction.
    b.wire_click({1.0, 1.0}, {4.0, 1.0});
    b.click_at({7.0, 1.0});
    REQUIRE(b.doc.schem.wires().size() == 2u);
    REQUIRE(b.doc.schem.junctions().size() == 3u);
    const std::uint32_t survivor = b.doc.schem.wires().front().id;
    const std::uint32_t doomed = b.doc.schem.wires().back().id;
    editor::push_delete_wire(b.doc, doomed);
    // Exactly B went, and only ITS private junction followed it: the
    // shared one survives, so A still resolves and still draws. The
    // id-0 collision used to skip no wire in the orphan scan - the
    // shared junction was wrongly orphaned and A stopped drawing (the
    // "vanishing wire").
    REQUIRE(b.doc.schem.wires().size() == 1u);
    CHECK(b.doc.schem.wires().front().id == survivor);
    CHECK(b.doc.schem.junctions().size() == 2u);
    const std::vector<Pos> path =
        b.ed.wire_path(b.doc.schem.wires().front());
    REQUIRE(path.size() == 2u);
    CHECK(path.front().x == 1.0);
    CHECK(path.front().y == 1.0);
    CHECK(path.back().x == 4.0);
    CHECK(path.back().y == 1.0);
}

TEST_CASE("the wire tool: undo removes the segment just drawn, not the "
          "oldest wire") {
    Bench b;
    b.wire_click({1.0, 1.0}, {4.0, 1.0});
    b.click_at({7.0, 1.0});
    REQUIRE(b.doc.schem.wires().size() == 2u);
    const std::uint32_t survivor = b.doc.schem.wires().front().id;
    (void)b.doc.undo().undo();
    // The LAST segment is the one that goes (AddWireCommand::undo
    // takes wire_.id): the id-0 collision used to make take_wire(0)
    // answer the oldest wire instead - and the survivor then dangled
    // off the junction the undo really took, so it stopped drawing.
    REQUIRE(b.doc.schem.wires().size() == 1u);
    CHECK(b.doc.schem.wires().front().id == survivor);
    CHECK(b.doc.schem.junctions().size() == 2u);
    const std::vector<Pos> path =
        b.ed.wire_path(b.doc.schem.wires().front());
    REQUIRE(path.size() == 2u);
    CHECK(path.front().x == 1.0);  // A's geometry, not B's
    CHECK(path.back().x == 4.0);
}

TEST_CASE("a placed part owns a fresh id") {
    Bench b;
    b.ed.tool = editor::Tool::Place;
    b.ed.place_symbol = "r";
    b.click_at({0.0, 0.0});
    REQUIRE(b.doc.schem.parts().size() == 1u);
    const std::uint32_t first = b.doc.schem.parts().front().id;
    CHECK(first != 0u);
    b.click_at({6.0, 0.0});
    REQUIRE(b.doc.schem.parts().size() == 2u);
    CHECK(b.doc.schem.parts().back().id != 0u);
    CHECK(b.doc.schem.parts().back().id != first);
}

TEST_CASE("a placed label owns a fresh id") {
    Bench b;
    b.ed.tool = editor::Tool::Label;
    b.click_at({2.0, 3.0});  // label_text defaults to "net"
    REQUIRE(b.doc.schem.labels().size() == 1u);
    CHECK(b.doc.schem.labels().front().id != 0u);
}

TEST_CASE("push_place_* mints the id when the caller never set one") {
    Bench b;
    // The seams' belt-and-braces: 0 is the "none" sentinel, so an
    // un-minted entity gets its id at the door - no future placement
    // path can repeat the id-0 collision.
    ws::circsim::Junction a;
    a.id = b.doc.schem.alloc_id();
    a.pos = {1.0, 1.0};
    ws::circsim::Junction c;
    c.id = b.doc.schem.alloc_id();
    c.pos = {4.0, 1.0};
    ws::circsim::Wire w;  // id 0 - deliberately un-minted
    w.a = ws::circsim::JunctionRef{a.id};
    w.b = ws::circsim::JunctionRef{c.id};
    editor::push_place_wire(b.doc, w, {a, c});
    REQUIRE(b.doc.schem.wires().size() == 1u);
    CHECK(b.doc.schem.wires().front().id != 0u);

    ws::circsim::Part p =
        ws::circsim::Part::make("r", "R9", "1k", {0.0, 0.0});
    editor::push_place_part(b.doc, p);
    REQUIRE(b.doc.schem.parts().size() == 1u);
    CHECK(b.doc.schem.parts().front().id != 0u);

    ws::circsim::Label l;
    l.pos = {2.0, 2.0};
    l.name = "VCC";
    editor::push_place_label(b.doc, l);
    REQUIRE(b.doc.schem.labels().size() == 1u);
    CHECK(b.doc.schem.labels().front().id != 0u);

    // And the minted ids stay mutually distinct.
    CHECK(b.doc.schem.wires().front().id !=
          b.doc.schem.parts().front().id);
    CHECK(b.doc.schem.parts().front().id !=
          b.doc.schem.labels().front().id);
}

// --- the interactive canvas: the run paints back -----------------------------

namespace {

// FNV-1a over a name, the wheel index net_color hashes with (the
// mapping is the contract: a net's color is its name's wheel slot).
[[nodiscard]] std::uint32_t fnv1a(const std::string& s) {
    std::uint32_t h = 2166136261u;
    for (const char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }
    return h;
}

// The voltage ramp's reference (the spec the canvas paints): the same
// blue-gray-red ladder, sqrt-spread, written out so a ramp change is a
// deliberate flip of this test.
[[nodiscard]] std::uint32_t ref_voltage_color(double v, double vmax) {
    const double u = std::clamp(v / std::max(vmax, 1e-9), -1.0, 1.0);
    const double a = std::sqrt(std::fabs(u));
    const double base[3] = {122.0, 126.0, 134.0};
    const double hot[3] = {255.0, 86.0, 60.0};
    const double cold[3] = {86.0, 150.0, 255.0};
    const double* toward = u >= 0.0 ? hot : cold;
    std::uint8_t rgb[3];
    for (int i = 0; i < 3; ++i) {
        rgb[i] = static_cast<std::uint8_t>(
            std::lround(base[i] + (toward[i] - base[i]) * a));
    }
    return ws::ui::r2d::rgba(rgb[0], rgb[1], rgb[2], 242);
}

// Delivers a run the way the app's deliver_run does (the Problems
// cache's copies, the Results cache's originals, the transport parked
// at the run's end) - the editor half, minus the panel opening.
void deliver(Editor& ed, ws::circsim::RunOutcome outcome) {
    const std::uint64_t v = ed.schem()->version();
    ed.problems.nets = outcome.nets;
    ed.problems.nets_valid = true;
    ed.problems.map = outcome.map;
    ed.problems.map_valid = true;
    ed.refresh_problems(true, std::move(outcome.problems), "run", v);
    if (!outcome.run.has_value()) {
        return;
    }
    ed.results.has = true;
    ed.results.version = v;
    ed.results.run = std::move(*outcome.run);
    ed.results.nets = std::move(outcome.nets);
    ed.results.map = std::move(outcome.map);
    ed.playback = editor::Playback{};
    ed.transport_scrub = false;
    if (const ws::spice::Table* sweep = ed.canvas_sweep_table();
        sweep != nullptr) {
        const std::span<const double> axis =
            ws::circsim::overlay_sweep_axis(*sweep);
        if (!axis.empty()) {
            ed.playback.t = axis.back();
        }
    }
    ed.results.table = 0;
    // The app's delivery bump: re-keys the frame-budget caches (the
    // results panel is not drawn here, but the fake delivery mirrors
    // main.cpp's block exactly).
    ++ed.results.run_id;
    ed.results.visible_for = editor::ResultsCache::kNoTable;
    ed.results.visible.clear();
    ed.results.x_sorted_for = editor::ResultsCache::kNoTable;
    ed.results.x_runs_for = editor::ResultsCache::kNoTable;
    ed.results.hover = -1;
    ed.results.view = editor::PlotView{};
    ed.results.view_for = editor::ResultsCache::kNoTable;
    ed.results.gesture = editor::PlotGesture{};
}

// The frame's fills whose centroid is within `tol` of `at` (the glow
// discs, the transport chrome - anything round drawn at a known spot).
[[nodiscard]] std::vector<const canvas::CanvasCmd*> fills_near(
    const Bench& b, ws::math::Vec2 at, float tol) {
    std::vector<const canvas::CanvasCmd*> out;
    for (const canvas::CanvasCmd& c : b.clc.cmds) {
        if (c.kind == canvas::CanvasCmdKind::fill_polygon &&
            vec2_near(centroid(c), at, tol)) {
            out.push_back(&c);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("wire colors: the mode picks the ink") {
    SUBCASE("plain is the classic dim stroke") {
        Bench b;
        b.place_wire();
        b.ed.wire_colors = editor::WireColorMode::Plain;
        b.draw();
        const canvas::CanvasCmd* wire = wire_cmd(b);
        REQUIRE(wire != nullptr);
        CHECK(wire->color ==
              gui::dim(gui::kDefaultChrome.text, 0.85f));
    }
    SUBCASE("nets wears the wheel color of the net's name") {
        Bench b;
        b.place_wire();
        b.ed.wire_colors = editor::WireColorMode::Nets;
        b.draw();
        const canvas::CanvasCmd* wire = wire_cmd(b);
        REQUIRE(wire != nullptr);
        REQUIRE(!b.doc.schem.wires().empty());
        const ws::circsim::Net* n =
            b.ed.net_cache().net_of_wire(
                b.doc.schem.wires().front().id);
        REQUIRE(n != nullptr);
        CHECK(wire->color ==
              editor::series_color(fnv1a(n->name)));
    }
    SUBCASE("voltage without a run falls back to the plain ink") {
        Bench b;
        b.place_wire();
        b.ed.wire_colors = editor::WireColorMode::Voltage;
        b.draw();
        const canvas::CanvasCmd* wire = wire_cmd(b);
        REQUIRE(wire != nullptr);
        CHECK(wire->color ==
              gui::dim(gui::kDefaultChrome.text, 0.85f));
    }
    SUBCASE("voltage with a run rides the node-voltage ramp") {
        // The divider: 5 V at the source net, 2.5 V at "out", ground.
        Bench b;
        ws::circsim::Part* v = b.doc.schem.add_part(
            ws::circsim::Part::make("v", "V1", "5", {0.0, -4.0}));
        ws::circsim::Part* r1 = b.doc.schem.add_part(
            ws::circsim::Part::make("r", "R1", "10k", {6.0, 0.0}));
        ws::circsim::Part* r2 = b.doc.schem.add_part(
            ws::circsim::Part::make("r", "R2", "10k", {14.0, 0.0}));
        ws::circsim::Part* g = b.doc.schem.add_part(
            ws::circsim::Part::make("gnd", "", "", {7.0, -6.0}));
        b.doc.schem.add_wire(ws::circsim::Wire::endpoints(
            ws::circsim::PinRef{v->id, 0}, ws::circsim::PinRef{r1->id, 0}));
        b.doc.schem.add_wire(ws::circsim::Wire::endpoints(
            ws::circsim::PinRef{r1->id, 1}, ws::circsim::PinRef{r2->id, 0}));
        b.doc.schem.add_wire(ws::circsim::Wire::endpoints(
            ws::circsim::PinRef{r2->id, 1}, ws::circsim::PinRef{g->id, 0}));
        b.doc.schem.add_wire(ws::circsim::Wire::endpoints(
            ws::circsim::PinRef{g->id, 0}, ws::circsim::PinRef{v->id, 1}));
        ws::circsim::Label* out = b.doc.schem.add_label({});
        out->pos = Pos{10.0, 0.0};
        out->name = "out";
        b.doc.schem.analysis.entries.push_back(ws::circsim::OpSpec{});
        deliver(b.ed, ws::circsim::run_schematic(b.doc.schem));
        REQUIRE(b.ed.results.has);

        b.ed.wire_colors = editor::WireColorMode::Voltage;
        b.draw();
        // The source wire (N001, 5 V = the ramp's max) and the out wire
        // (2.5 V): each wears the ramp's answer for its own net.
        const ws::circsim::NetList& nets = b.ed.net_cache();
        for (const ws::circsim::Wire& w : b.doc.schem.wires()) {
            const ws::circsim::Net* n = nets.net_of_wire(w.id);
            REQUIRE(n != nullptr);
            // the recorded stroke of exactly this wire: match by the
            // full path the wire resolves to
            const std::vector<Pos> path = b.ed.wire_path(w);
            REQUIRE(path.size() >= 2u);
            const canvas::CanvasCmd* found = nullptr;
            for (const canvas::CanvasCmd& cmd : b.clc.cmds) {
                if (cmd.kind != canvas::CanvasCmdKind::stroke_polyline ||
                    cmd.points.size() != path.size()) {
                    continue;
                }
                bool ok = true;
                for (std::size_t i = 0; i < path.size(); ++i) {
                    if (!vec2_near(cmd.points[i],
                                   b.ed.view.w2s(path[i]), 0.01f)) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    found = &cmd;
                    break;
                }
            }
            REQUIRE(found != nullptr);
            if (n->is_ground) {
                continue;  // ground never rides the ramp (no v(0) column)
            }
            const double volts =
                n->name == "out"
                    ? 2.5
                    : 5.0;  // the divider's two named answers
            CHECK(found->color == ref_voltage_color(volts, 5.0));
        }
    }
    SUBCASE("the toggle buttons pick the mode") {
        Bench b;
        b.place_wire();
        b.draw();  // adopt the viewport
        const editor::ModeToggleLayout modes =
            editor::mode_toggle_layout(b.body);
        b.click_screen(modes.buttons[0].x + 3.0f,
                       modes.buttons[0].y + 3.0f);
        CHECK(b.ed.wire_colors == editor::WireColorMode::Plain);
        b.click_screen(modes.buttons[2].x + 3.0f,
                       modes.buttons[2].y + 3.0f);
        CHECK(b.ed.wire_colors == editor::WireColorMode::Voltage);
        b.click_screen(modes.buttons[1].x + 3.0f,
                       modes.buttons[1].y + 3.0f);
        CHECK(b.ed.wire_colors == editor::WireColorMode::Nets);
    }
}

TEST_CASE("the LED glows with the run's current") {
    Bench b;
    b.theme.anim_enabled = false;  // snap: the eased path needs real dt
    ws::circsim::Part* v = b.doc.schem.add_part(
        ws::circsim::Part::make("v", "V1", "5", {0.0, -4.0}));
    ws::circsim::Part* r = b.doc.schem.add_part(
        ws::circsim::Part::make("r", "R1", "330", {6.0, 0.0}));
    ws::circsim::Part* led = b.doc.schem.add_part(
        ws::circsim::Part::make("led", "D1", "", {14.0, 0.0}));
    led->params["model"] = "led";
    led->params["color"] = "red";
    ws::circsim::Part* g = b.doc.schem.add_part(
        ws::circsim::Part::make("gnd", "", "", {7.0, -6.0}));
    b.doc.schem.add_wire(ws::circsim::Wire::endpoints(
        ws::circsim::PinRef{v->id, 0}, ws::circsim::PinRef{r->id, 0}));
    b.doc.schem.add_wire(ws::circsim::Wire::endpoints(
        ws::circsim::PinRef{r->id, 1}, ws::circsim::PinRef{led->id, 0}));
    b.doc.schem.add_wire(ws::circsim::Wire::endpoints(
        ws::circsim::PinRef{led->id, 1}, ws::circsim::PinRef{g->id, 0}));
    b.doc.schem.add_wire(ws::circsim::Wire::endpoints(
        ws::circsim::PinRef{g->id, 0}, ws::circsim::PinRef{v->id, 1}));
    b.doc.schem.directives = ".model led D(Is=1e-16 N=2)";
    b.doc.schem.analysis.entries.push_back(ws::circsim::OpSpec{});

    // No run yet: the LED draws dark (its picture, no halo).
    b.draw();
    CHECK(fills_near(b, b.ed.view.w2s(led->pos), 3.0f).empty());

    deliver(b.ed, ws::circsim::run_schematic(b.doc.schem));
    REQUIRE(b.ed.results.has);
    // ~10 mA through the probe column: a bright halo in the LED's red.
    b.draw();
    const std::vector<const canvas::CanvasCmd*> glow =
        fills_near(b, b.ed.view.w2s(led->pos), 3.0f);
    REQUIRE(glow.size() >= 4u);  // three halos + the die dot
    for (const canvas::CanvasCmd* c : glow) {
        const std::uint32_t col = c->color;
        const std::uint32_t rr = (col >> 24) & 0xFFu;
        const std::uint32_t gg = (col >> 16) & 0xFFu;
        const std::uint32_t bb = (col >> 8) & 0xFFu;
        CHECK(rr > gg);  // the hue is red-dominant
        CHECK(rr > bb);
        CHECK((col & 0xFFu) > 0u);  // and it glows (a nonzero alpha)
    }
}

TEST_CASE("the transport scrubs and plays the transient") {
    Bench b;
    ws::circsim::Part* v = b.doc.schem.add_part(
        ws::circsim::Part::make("v", "V1", "5", {0.0, -4.0}));
    ws::circsim::Part* r = b.doc.schem.add_part(
        ws::circsim::Part::make("r", "R1", "1k", {6.0, 0.0}));
    ws::circsim::Part* g = b.doc.schem.add_part(
        ws::circsim::Part::make("gnd", "", "", {7.0, -6.0}));
    b.doc.schem.add_wire(ws::circsim::Wire::endpoints(
        ws::circsim::PinRef{v->id, 0}, ws::circsim::PinRef{r->id, 0}));
    b.doc.schem.add_wire(ws::circsim::Wire::endpoints(
        ws::circsim::PinRef{r->id, 1}, ws::circsim::PinRef{g->id, 0}));
    b.doc.schem.add_wire(ws::circsim::Wire::endpoints(
        ws::circsim::PinRef{g->id, 0}, ws::circsim::PinRef{v->id, 1}));
    b.doc.schem.analysis.entries.push_back(ws::circsim::TranSpec{
        .tstep = 1e-3, .tstop = 3e-3});
    deliver(b.ed, ws::circsim::run_schematic(b.doc.schem));
    const ws::spice::Table* tran = b.ed.canvas_sweep_table();
    REQUIRE(tran != nullptr);
    const std::span<const double> tran_axis =
        ws::circsim::overlay_sweep_axis(*tran);
    REQUIRE(!tran_axis.empty());
    const double t0 = tran_axis.front();
    const double t1 = tran_axis.back();

    // The delivery parks the playhead at the run's end - the settled
    // state an .op reader means by "the answer".
    CHECK(b.ed.playback.t == doctest::Approx(t1));
    CHECK(!b.ed.playback.playing);

    // The strip drew: chrome lands inside its own layout.
    b.draw();
    const editor::TransportStripLayout tl =
        editor::transport_strip_layout(b.body);
    const ws::math::Vec2 center{tl.strip.x + tl.strip.width * 0.5f,
                                tl.strip.y + tl.strip.height * 0.5f};
    CHECK(!fills_near(b, center, tl.strip.width * 0.5f).empty());

    // Play: the button toggles, and a playhead at the end restarts.
    b.click_screen(tl.button.x + tl.button.width * 0.5f,
                   tl.button.y + tl.button.height * 0.5f);
    CHECK(b.ed.playback.playing);
    CHECK(b.ed.playback.t == doctest::Approx(t0));
    b.click_screen(tl.button.x + tl.button.width * 0.5f,
                   tl.button.y + tl.button.height * 0.5f);
    CHECK(!b.ed.playback.playing);

    // Scrub: a press at the bar's left end parks at the start, the
    // right end at the finish (a pixel inside the bar is a hair inside
    // the window - the assertion honors the fraction it clicked).
    const double span = t1 - t0;
    b.click_screen(tl.bar.x + 1.0f, tl.bar.y + tl.bar.height * 0.5f);
    CHECK(b.ed.transport_scrub);
    CHECK(b.ed.playback.t <= t0 + span * 0.01);
    b.click_screen(tl.bar.x + tl.bar.width - 1.0f,
                   tl.bar.y + tl.bar.height * 0.5f);
    CHECK(b.ed.playback.t >= t1 - span * 0.01);

    // The playhead resolves to the sample at or before it: the canvas's
    // shown_sample is the axis walk the colors and glow ride.
    CHECK(editor::Editor::shown_sample(*tran, t1) + 1u ==
          tran->row_count());
    CHECK(editor::Editor::shown_sample(*tran, t0) == 0u);

    // The rate chip (patch 0012): one press walks ws::circsim's ladder
    // - 1x -> 2x -> 1/4x - and the chip's rect sits inside the strip
    // beside the bar, so the press never reaches the scrub. (The pace
    // math itself - rate x dt over the window, looping - is
    // overlay_step's own headless gate in libs/circsim.)
    CHECK(b.ed.playback.rate == doctest::Approx(1.0));
    b.click_screen(tl.rate.x + tl.rate.width * 0.5f,
                   tl.rate.y + tl.rate.height * 0.5f);
    CHECK(b.ed.playback.rate == doctest::Approx(2.0));
    CHECK_FALSE(b.ed.transport_scrub);  // the chip is not the track
    b.click_screen(tl.rate.x + tl.rate.width * 0.5f,
                   tl.rate.y + tl.rate.height * 0.5f);
    CHECK(b.ed.playback.rate == doctest::Approx(0.25));
}

// --- the group edit: box-select, copy/paste/duplicate, the probe -----------------
//
// The scripted pointer frames drive the panel's own gesture ladder
// (press edge, held frames, release) - the drag_world helper spells
// the three frames a box sweep or a group move costs.

// One scripted pointer frame at a screen position: `pressed` is the
// press edge, `down` the button's hold, `shift` the modifier the
// pointer rules read (the box's extend, the drag's coarse snap).
void pointer_frame(Bench& b, ws::math::Vec2 sp, bool pressed, bool down,
                   bool shift) {
    b.clc.cmds.clear();
    ws::input::InputFrame in{};
    in.mouse_x = sp.x;
    in.mouse_y = sp.y;
    in.mouse_present = true;
    in.mouse_down[0] = down;
    in.mouse_pressed[0] = pressed;
    if (shift) {
        in.key_down[ws::input::keys::kLeftShift] = true;
    }
    b.gui.begin_frame(in);
    gui::LayoutCursor layout;
    (void)layout.begin_panel(gui::Rect{0.0f, 0.0f, 640.0f, 520.0f},
                             12.0f, 8.0f);
    b.body = layout.panel_content();
    editor::draw_canvas_panel(b.ed, b.gui, layout, b.theme);
    layout.end_panel();
    b.gui.end_frame();
}

// Press at `from` (world), hold-move to `to`, release there - the
// three frames every drag gesture costs.
void drag_world(Bench& b, Pos from, Pos to, bool shift = false) {
    if (!b.ed.view.valid()) {
        b.draw();
    }
    pointer_frame(b, b.ed.view.w2s(from), true, true, shift);
    pointer_frame(b, b.ed.view.w2s(to), false, true, shift);
    pointer_frame(b, b.ed.view.w2s(to), false, false, shift);
}

TEST_CASE("the box-select: a drag over empty space selects the set") {
    Bench b;
    const ws::circsim::Part* r1 = b.doc.schem.add_part(
        ws::circsim::Part::make("r", "R1", "1k", {0.0, 0.0}));
    (void)b.doc.schem.add_part(
        ws::circsim::Part::make("r", "R2", "1k", {10.0, 0.0}));
    // The far pair sits above the first box's reach but inside the
    // default camera (the press must land on-screen to arm the box).
    const ws::circsim::Part* far_part = b.doc.schem.add_part(
        ws::circsim::Part::make("r", "R3", "1k", {6.0, 8.0}));
    (void)far_part;

    // The sweep: press in clear space, drag the box over the pair,
    // release. The far part stays out.
    drag_world(b, {-4.0, 4.0}, {14.0, -4.0});
    CHECK_FALSE(b.ed.box.active);  // the release landed the set
    const std::vector<Selection> set = b.ed.selection_set();
    REQUIRE(set.size() == 2u);
    for (const Selection& m : set) {
        CHECK(m.kind == Selection::Kind::Part);
        CHECK(m.id != far_part->id);
    }
    // The anchor is the first part the box held.
    CHECK(b.ed.sel.kind == Selection::Kind::Part);
    CHECK(b.ed.sel.id == r1->id);

    // A plain click on empty space clears the set (the degenerate box
    // holds nothing) - the old press-to-clear rule, kept.
    b.click_at({-4.0, 4.0});
    // ...the click ARMS the box; the release (the next frame without
    // the button) lands the empty set.
    pointer_frame(b, b.ed.view.w2s({-4.0, 4.0}), false, false, false);
    CHECK(b.ed.selection_set().empty());
    CHECK(b.ed.sel.kind == Selection::Kind::None);

    // Shift extends: a second box over the far part adds it.
    drag_world(b, {-4.0, 4.0}, {14.0, -4.0});
    REQUIRE(b.ed.selection_set().size() == 2u);
    drag_world(b, {2.0, 9.0}, {10.0, 7.0}, /*shift=*/true);
    CHECK(b.ed.selection_set().size() == 3u);
}

TEST_CASE("a shift-click toggles one entity into the set") {
    Bench b;
    const ws::circsim::Part* r1 = b.doc.schem.add_part(
        ws::circsim::Part::make("r", "R1", "1k", {0.0, 0.0}));
    const ws::circsim::Part* r2 = b.doc.schem.add_part(
        ws::circsim::Part::make("r", "R2", "1k", {10.0, 0.0}));

    // Shift+click r2: the anchor moves, the set gains it.
    pointer_frame(b, {}, false, false, false);  // prime the view
    const ws::math::Vec2 r2_sp = b.ed.view.w2s({10.0, 0.0});
    pointer_frame(b, r2_sp, true, true, true);
    CHECK(b.ed.sel.kind == Selection::Kind::Part);
    CHECK(b.ed.sel.id == r2->id);
    REQUIRE(b.ed.selection_set().size() == 1u);

    // Shift+click r1: two in the set, the anchor follows the join.
    pointer_frame(b, b.ed.view.w2s({0.0, 0.0}), true, true, true);
    CHECK(b.ed.selection_set().size() == 2u);
    CHECK(b.ed.sel.id == r1->id);

    // Shift+click r1 again: the toggle takes it back out, the anchor
    // falls to what remains.
    pointer_frame(b, b.ed.view.w2s({0.0, 0.0}), true, true, true);
    CHECK(b.ed.selection_set().size() == 1u);
    CHECK(b.ed.sel.id == r2->id);
}

TEST_CASE("the group drag moves the set as ONE undo step") {
    Bench b;
    const ws::circsim::Part* r1 = b.doc.schem.add_part(
        ws::circsim::Part::make("r", "R1", "1k", {0.0, 0.0}));
    const ws::circsim::Part* r2 = b.doc.schem.add_part(
        ws::circsim::Part::make("r", "R2", "1k", {10.0, 0.0}));
    drag_world(b, {-4.0, 4.0}, {14.0, -4.0});
    REQUIRE(b.ed.selection_set().size() == 2u);
    const std::size_t before = b.doc.undo().undo_count();

    // Drag the set by its first member: press on r1, move (3,3),
    // release. Both parts ride the one delta.
    drag_world(b, {0.0, 0.0}, {3.0, 3.0});
    CHECK(b.doc.undo().undo_count() == before + 1u);
    const ws::circsim::Part* moved1 = b.doc.schem.find_part(r1->id);
    const ws::circsim::Part* moved2 = b.doc.schem.find_part(r2->id);
    REQUIRE(moved1 != nullptr);
    REQUIRE(moved2 != nullptr);
    CHECK(moved1->pos.x == doctest::Approx(3.0));
    CHECK(moved1->pos.y == doctest::Approx(3.0));
    CHECK(moved2->pos.x == doctest::Approx(13.0));
    CHECK(moved2->pos.y == doctest::Approx(3.0));

    // One undo returns the whole set.
    (void)b.doc.undo().undo();
    CHECK(b.doc.schem.find_part(r1->id)->pos.x == doctest::Approx(0.0));
    CHECK(b.doc.schem.find_part(r2->id)->pos.x ==
          doctest::Approx(10.0));
    CHECK(b.doc.schem.find_part(r2->id)->pos.y == doctest::Approx(0.0));
}

TEST_CASE("copy/paste: fresh ids, renumbered refs, the geometry kept") {
    Bench b;
    const ws::circsim::Part* r1 = b.doc.schem.add_part(
        ws::circsim::Part::make("r", "R1", "1k", {0.0, 0.0}));
    const ws::circsim::Part* r2 = b.doc.schem.add_part(
        ws::circsim::Part::make("r", "R2", "2k", {10.0, 0.0}));
    b.ed.sel = {Selection::Kind::Part, r1->id};
    b.ed.multi.push_back({Selection::Kind::Part, r2->id});

    CHECK(b.ed.copy_selection());
    REQUIRE(b.ed.clipboard.parts.size() == 2u);
    CHECK(b.ed.clipboard.center.has_value());

    const std::vector<Selection> pasted = b.ed.paste_clipboard({20.0, 0.0});
    REQUIRE(pasted.size() == 2u);
    CHECK(b.doc.schem.parts().size() == 4u);

    // The pasted pair: fresh ids, renumbered refs (R3, R4 - the next
    // free numbers per prefix), values kept, relative geometry kept.
    std::vector<const ws::circsim::Part*> news;
    for (const Selection& m : pasted) {
        CHECK(m.kind == Selection::Kind::Part);
        news.push_back(b.doc.schem.find_part(m.id));
    }
    REQUIRE(news[0] != nullptr);
    REQUIRE(news[1] != nullptr);
    CHECK(news[0]->id != r1->id);
    CHECK(news[0]->id != r2->id);
    CHECK(news[0]->ref == "R3");
    CHECK(news[1]->ref == "R4");
    CHECK(news[0]->value == "1k");
    CHECK(news[1]->value == "2k");
    CHECK((news[1]->pos.x - news[0]->pos.x) == doctest::Approx(10.0));

    // The paste lands ON the lattice (the copy's own positions are
    // lattice points, the delta is grid-aligned).
    const double grid = b.doc.schem.grid;
    CHECK(std::fmod(news[0]->pos.x, grid) == doctest::Approx(0.0));
    CHECK(std::fmod(news[0]->pos.y, grid) == doctest::Approx(0.0));

    // One undo removes exactly the pasted pair; the originals stand.
    (void)b.doc.undo().undo();
    CHECK(b.doc.schem.parts().size() == 2u);
    CHECK(b.doc.schem.find_part(r1->id) != nullptr);
    CHECK(b.doc.schem.find_part(r2->id) != nullptr);
}

TEST_CASE("copy carries a boundary wire as a minted junction stub") {
    Bench b;
    const ws::circsim::Part* r1 = b.doc.schem.add_part(
        ws::circsim::Part::make("r", "R1", "1k", {0.0, 0.0}));
    ws::circsim::Junction far_end;
    far_end.id = b.doc.schem.alloc_id();
    far_end.pos = {30.0, 0.0};
    b.doc.schem.junctions_mut().push_back(far_end);
    ws::circsim::Wire w;
    w.id = b.doc.schem.alloc_id();
    w.a = ws::circsim::PinRef{r1->id, 0};
    w.b = ws::circsim::JunctionRef{far_end.id};
    b.doc.schem.wires_mut().push_back(w);

    // The box holds the part and the wire's near half; the far
    // junction stays out (the boundary case).
    drag_world(b, {-4.0, 4.0}, {4.0, -4.0});
    const std::vector<Selection> set = b.ed.selection_set();
    REQUIRE(set.size() == 2u);  // the part and the wire
    bool has_wire = false;
    bool has_part = false;
    for (const Selection& m : set) {
        has_wire = has_wire || m.kind == Selection::Kind::Wire;
        has_part = has_part || m.kind == Selection::Kind::Part;
    }
    CHECK(has_wire);
    CHECK(has_part);

    CHECK(b.ed.copy_selection());
    // The clipboard: the part, the wire, and ONE boundary junction
    // minted at the wire's far end - the drawn stub, carried.
    REQUIRE(b.ed.clipboard.parts.size() == 1u);
    REQUIRE(b.ed.clipboard.wires.size() == 1u);
    REQUIRE(b.ed.clipboard.junctions.size() == 1u);
    CHECK(b.ed.clipboard.junctions[0].pos.x == doctest::Approx(30.0));

    const std::vector<Selection> pasted = b.ed.paste_clipboard({0.0, 20.0});
    REQUIRE(pasted.size() == 3u);
    // The pasted wire's endpoints resolve through the FRESH ids: the
    // part's pin and the minted junction at the offset position.
    const double dx = 0.0 - b.ed.clipboard.center->x;
    (void)dx;  // the delta is the clipboard's own; geometry is checked
               // through the junction below.
    std::uint32_t pasted_junction = 0;
    std::uint32_t pasted_wire = 0;
    for (const Selection& m : pasted) {
        if (m.kind == Selection::Kind::Junction) {
            pasted_junction = m.id;
        } else if (m.kind == Selection::Kind::Wire) {
            pasted_wire = m.id;
        }
    }
    REQUIRE(pasted_junction != 0u);
    REQUIRE(pasted_wire != 0u);
    const ws::circsim::Junction* landed =
        b.doc.schem.find_junction(pasted_junction);
    REQUIRE(landed != nullptr);
    CHECK(landed->pos.y == doctest::Approx(20.0));  // the offset rides
    const ws::circsim::Wire* landed_wire =
        b.doc.schem.find_wire(pasted_wire);
    REQUIRE(landed_wire != nullptr);
    const ws::circsim::JunctionRef* b_end =
        std::get_if<ws::circsim::JunctionRef>(&landed_wire->b);
    REQUIRE(b_end != nullptr);
    CHECK(b_end->junction == pasted_junction);  // remapped, not stale

    // The undo removes the whole paste: part + wire + junction.
    (void)b.doc.undo().undo();
    CHECK(b.doc.schem.junctions().size() == 1u);
    CHECK(b.doc.schem.wires().size() == 1u);
    CHECK(b.doc.schem.parts().size() == 1u);
}

TEST_CASE("duplicate nudges the copy one step, as one undo step") {
    Bench b;
    const ws::circsim::Part* r1 = b.doc.schem.add_part(
        ws::circsim::Part::make("r", "R1", "1k", {0.0, 0.0}));
    b.ed.sel = {Selection::Kind::Part, r1->id};
    const std::size_t before = b.doc.undo().undo_count();

    const std::vector<Selection> pasted = b.ed.duplicate_selection();
    REQUIRE(pasted.size() == 1u);
    CHECK(b.doc.undo().undo_count() == before + 1u);
    const ws::circsim::Part* twin = b.doc.schem.find_part(pasted[0].id);
    REQUIRE(twin != nullptr);
    // The nudge: one column over, one row down (the standard EDA
    // step - the world's y is up, so -2 is one row DOWN the screen).
    CHECK(twin->pos.x == doctest::Approx(2.0));
    CHECK(twin->pos.y == doctest::Approx(-2.0));
    CHECK(twin->ref == "R2");

    (void)b.doc.undo().undo();
    CHECK(b.doc.schem.parts().size() == 1u);
}

TEST_CASE("push_delete_set: the working set dies as ONE step") {
    Bench b;
    const ws::circsim::Part* r1 = b.doc.schem.add_part(
        ws::circsim::Part::make("r", "R1", "1k", {0.0, 0.0}));
    const ws::circsim::Part* r2 = b.doc.schem.add_part(
        ws::circsim::Part::make("r", "R2", "1k", {10.0, 0.0}));
    (void)b.doc.schem.add_wire(ws::circsim::Wire::endpoints(
        ws::circsim::PinRef{r1->id, 0}, ws::circsim::PinRef{r2->id, 0}));
    REQUIRE(b.doc.schem.wires().size() == 1u);

    // Only the PARTS are named: the wire rides the part rule (a wire
    // referencing a deleted part's pin goes with it).
    CHECK(editor::push_delete_set(b.doc, {r1->id, r2->id}, {}, {}));
    CHECK(b.doc.schem.parts().empty());
    CHECK(b.doc.schem.wires().empty());
    CHECK(b.doc.undo().can_undo());

    // One undo restores the whole world - parts and the wire between.
    (void)b.doc.undo().undo();
    REQUIRE(b.doc.schem.parts().size() == 2u);
    REQUIRE(b.doc.schem.wires().size() == 1u);
    const ws::circsim::PinRef* a =
        std::get_if<ws::circsim::PinRef>(
            &b.doc.schem.wires().front().a);
    REQUIRE(a != nullptr);
    CHECK(a->part == r1->id);
}

TEST_CASE("the probe toggles a net's curve in Results") {
    Bench b;
    // The divider: V1 - R1 - ground, one .tran to have curves.
    ws::circsim::Part* v = b.doc.schem.add_part(
        ws::circsim::Part::make("v", "V1", "5", {0.0, -4.0}));
    ws::circsim::Part* r = b.doc.schem.add_part(
        ws::circsim::Part::make("r", "R1", "1k", {6.0, 0.0}));
    ws::circsim::Part* g = b.doc.schem.add_part(
        ws::circsim::Part::make("gnd", "", "", {7.0, -6.0}));
    (void)b.doc.schem.add_wire(ws::circsim::Wire::endpoints(
        ws::circsim::PinRef{v->id, 0}, ws::circsim::PinRef{r->id, 0}));
    (void)b.doc.schem.add_wire(ws::circsim::Wire::endpoints(
        ws::circsim::PinRef{r->id, 1}, ws::circsim::PinRef{g->id, 0}));
    (void)b.doc.schem.add_wire(ws::circsim::Wire::endpoints(
        ws::circsim::PinRef{g->id, 0}, ws::circsim::PinRef{v->id, 1}));
    b.doc.schem.analysis.entries.push_back(
        ws::circsim::TranSpec{.tstep = 1e-3, .tstop = 3e-3});

    // Before any run: the honest word, nothing toggled. And a
    // resistor's body answers "not in the run" even after one - the
    // honest no, R1 has no current column.
    CHECK(b.ed.probe_at({6.0, 0.0}, 0.5).find("no results") !=
          std::string::npos);

    deliver(b.ed, ws::circsim::run_schematic(b.doc.schem));
    REQUIRE(b.ed.results.has);
    REQUIRE(!b.ed.results.run.results.tables.empty());

    // Probe the V1 body: its element's current curve, i(V1), flips
    // (a branch device's current is a table column; a resistor's
    // never is - the honest "not in the run" answers that one).
    const std::string first = b.ed.probe_at({0.0, -4.0}, 0.4);
    CHECK(first == "probe: i(V1) hidden");  // all-visible flips to off
    CHECK(b.ed.results.visible.size() ==
          b.ed.results.run.results.tables[b.ed.results.table]
              .signals.size());
    std::size_t zeros = 0;
    for (std::uint8_t f : b.ed.results.visible) {
        zeros += f == 0u ? 1u : 0u;
    }
    CHECK(zeros == 1u);
    // The same probe again: shown - the toggle, both ways.
    CHECK(b.ed.probe_at({0.0, -4.0}, 0.4) == "probe: i(V1) shown");
    zeros = 0;
    for (std::uint8_t f : b.ed.results.visible) {
        zeros += f == 0u ? 1u : 0u;
    }
    CHECK(zeros == 0u);

    // A resistor's body is the honest no (probes its own element).
    CHECK(b.ed.probe_at({6.0, 0.0}, 0.4) ==
          "probe: i(R1) is not in the run");

    // Probe a wire: the net's voltage curve (v(<token>) - the deck's
    // own node spelling, whatever the auto-namer chose).
    const ws::circsim::Wire* wire = nullptr;
    for (const ws::circsim::Wire& w : b.doc.schem.wires()) {
        const std::vector<Pos> path = b.ed.wire_path(w);
        if (path.size() >= 2u) {
            wire = &w;
            break;
        }
    }
    REQUIRE(wire != nullptr);
    const std::vector<Pos> path = b.ed.wire_path(*wire);
    const Pos mid{(path.front().x + path.back().x) * 0.5,
                  (path.front().y + path.back().y) * 0.5};
    const std::string net_probe = b.ed.probe_at(mid, 0.5);
    CHECK(net_probe.find("probe: v(") == 0u);
    CHECK(net_probe.find("hidden") != std::string::npos);
}

TEST_CASE("a .dc sweep drives the transport like a transient") {
    Bench b;
    ws::circsim::Part* v = b.doc.schem.add_part(
        ws::circsim::Part::make("v", "V1", "5", {0.0, -4.0}));
    ws::circsim::Part* r = b.doc.schem.add_part(
        ws::circsim::Part::make("r", "R1", "1k", {6.0, 0.0}));
    ws::circsim::Part* g = b.doc.schem.add_part(
        ws::circsim::Part::make("gnd", "", "", {7.0, -6.0}));
    (void)b.doc.schem.add_wire(ws::circsim::Wire::endpoints(
        ws::circsim::PinRef{v->id, 0}, ws::circsim::PinRef{r->id, 0}));
    (void)b.doc.schem.add_wire(ws::circsim::Wire::endpoints(
        ws::circsim::PinRef{r->id, 1}, ws::circsim::PinRef{g->id, 0}));
    (void)b.doc.schem.add_wire(ws::circsim::Wire::endpoints(
        ws::circsim::PinRef{g->id, 0}, ws::circsim::PinRef{v->id, 1}));
    b.doc.schem.analysis.entries.push_back(ws::circsim::DcSpec{
        .source = "V1", .start = 0.0, .stop = 5.0, .step = 0.5});
    deliver(b.ed, ws::circsim::run_schematic(b.doc.schem));

    // The .dc table IS the canvas sweep table now.
    const ws::spice::Table* t = b.ed.canvas_sweep_table();
    REQUIRE(t != nullptr);
    CHECK(t->sweep == ws::spice::SweepKind::Swept);
    const std::span<const double> axis =
        ws::circsim::overlay_sweep_axis(*t);
    REQUIRE(axis.size() >= 2u);
    CHECK(axis.front() == doctest::Approx(0.0));
    CHECK(axis.back() == doctest::Approx(5.0));

    // The delivery parks the playhead at the sweep's END - the
    // settled state, in the swept source's own units.
    CHECK(b.ed.playback.t == doctest::Approx(5.0));
    CHECK_FALSE(b.ed.playback.playing);

    // The strip draws and scrubs: the bar's left end parks at the
    // sweep's start (the transport test's own click rule).
    b.draw();
    const editor::TransportStripLayout tl =
        editor::transport_strip_layout(b.body);
    b.click_screen(tl.bar.x + 1.0f, tl.bar.y + tl.bar.height * 0.5f);
    CHECK(b.ed.transport_scrub);
    CHECK(b.ed.playback.t <= 5.0 * 0.01);

    // The playhead resolves samples on the swept axis (the canvas's
    // voltage coloring rides the same walk).
    CHECK(editor::Editor::shown_sample(*t, 0.75) == 1u);
    CHECK(editor::Editor::shown_sample(*t, 4.99) ==
          axis.size() - 2u);
}

}  // namespace
