// Circsim frame-cost regression tests: selection indexing, cached wire
// geometry, culling, and built-in symbol extent.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/canvas_panel.hpp"
#include "../src/editor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <ws/circsim/library.hpp>
#include <ws/circsim/model.hpp>
#include <ws/circsim/symbols.hpp>
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
using editor::SchematicDocument;
using editor::Selection;
using editor::WireGeom;
using ws::circsim::Pos;
using ws::circsim::Wire;

struct Bench {
    ws::settings::StdFileSystem fs;
    SchematicDocument doc{std::string{}, fs};
    Editor ed;
    gui::GuiContext gui;
    canvas::CommandListCanvas clc;
    gui::ChromeTheme theme = gui::kDefaultChrome;

    Bench() {
        ed.doc = &doc;
        ed.canvas_panel_canvas = &clc;
    }

    void place_wire() {
        ws::circsim::Junction a;
        a.id = doc.schem.alloc_id();
        a.pos = {1.0, 1.0};
        ws::circsim::Junction b;
        b.id = doc.schem.alloc_id();
        b.pos = {4.0, 1.0};
        doc.schem.junctions_mut().push_back(a);
        doc.schem.junctions_mut().push_back(b);
        Wire w;
        w.id = doc.schem.alloc_id();
        w.a = ws::circsim::JunctionRef{a.id};
        w.b = ws::circsim::JunctionRef{b.id};
        w.ortho = true;
        doc.schem.wires_mut().push_back(w);
        doc.schem.bump();
    }

    void draw() {
        clc.cmds.clear();
        ws::input::InputFrame in{};
        in.mouse_x = -1000.0f;
        in.mouse_y = -1000.0f;
        in.mouse_present = true;
        gui.begin_frame(in);
        gui::LayoutCursor layout;
        (void)layout.begin_panel(gui::Rect{0.0f, 0.0f, 640.0f, 520.0f},
                                 12.0f, 8.0f);
        editor::draw_canvas_panel(ed, gui, layout, theme);
        layout.end_panel();
        gui.end_frame();
    }
};

[[nodiscard]] bool near2(ws::math::Vec2 a, ws::math::Vec2 b, float tol) {
    return std::fabs(a.x - b.x) <= tol && std::fabs(a.y - b.y) <= tol;
}

[[nodiscard]] const canvas::CanvasCmd* wire_cmd(const Bench& b) {
    const ws::math::Vec2 a = b.ed.view.w2s({1.0, 1.0});
    const ws::math::Vec2 c = b.ed.view.w2s({4.0, 1.0});
    for (const canvas::CanvasCmd& cmd : b.clc.cmds) {
        if (cmd.kind != canvas::CanvasCmdKind::stroke_polyline ||
            cmd.points.size() != 2u) continue;
        if ((near2(cmd.points[0], a, 0.01f) && near2(cmd.points[1], c, 0.01f)) ||
            (near2(cmd.points[0], c, 0.01f) && near2(cmd.points[1], a, 0.01f))) {
            return &cmd;
        }
    }
    return nullptr;
}
}  // namespace

TEST_CASE("SelectionIndex is equivalent to Editor::in_selection") {
    Bench b;
    b.ed.sel = {Selection::Kind::Wire, 7};
    b.ed.multi = {{Selection::Kind::Part, 3},
                  {Selection::Kind::Junction, 3},
                  {Selection::Kind::Label, 9},
                  {Selection::Kind::None, 0},
                  {Selection::Kind::Part, 3}};
    const editor::SelectionIndex idx = b.ed.selection_index();
    for (std::uint8_t k = 0;
         k <= static_cast<std::uint8_t>(Selection::Kind::Label); ++k) {
        for (std::uint32_t id = 0; id <= 12; ++id) {
            const Selection of{static_cast<Selection::Kind>(k), id};
            CHECK(idx.contains(of) == b.ed.in_selection(of));
        }
    }
    b.ed.multi.clear();
    const editor::SelectionIndex solo = b.ed.selection_index();
    CHECK(solo.contains({Selection::Kind::Wire, 7}));
    CHECK_FALSE(solo.contains({Selection::Kind::Wire, 8}));
    CHECK_FALSE(solo.contains({Selection::Kind::None, 0}));
}

TEST_CASE("wire_geom caches polyline and bbox by schematic version") {
    Bench b;
    b.place_wire();
    const Wire& w = b.doc.schem.wires().front();
    const WireGeom& g = b.ed.wire_geom(w);
    const std::vector<Pos> direct = b.ed.wire_path(w);
    REQUIRE(g.path.size() == direct.size());
    for (std::size_t i = 0; i < direct.size(); ++i) {
        CHECK(g.path[i].x == direct[i].x);
        CHECK(g.path[i].y == direct[i].y);
    }
    CHECK(g.min_x == 1.0);
    CHECK(g.max_x == 4.0);
    CHECK(g.min_y == 1.0);
    CHECK(g.max_y == 1.0);
    CHECK(&b.ed.wire_geom(w) == &g);
    for (ws::circsim::Junction& j : b.doc.schem.junctions_mut()) {
        if (j.pos.x == 4.0) j.pos.x = 9.0;
    }
    b.doc.schem.bump();
    const WireGeom& g2 = b.ed.wire_geom(w);
    CHECK(g2.max_x == 9.0);
    CHECK(g2.min_x == 1.0);
}

TEST_CASE("wire_geom is cleared by document switch at equal versions") {
    Bench b;
    b.place_wire();
    const Wire& w = b.doc.schem.wires().front();
    CHECK(b.ed.wire_geom(w).max_x == 4.0);
    SchematicDocument other{std::string{}, b.fs};
    ws::circsim::Junction a;
    a.id = other.schem.alloc_id();
    a.pos = {0.0, 0.0};
    ws::circsim::Junction c;
    c.id = other.schem.alloc_id();
    c.pos = {20.0, 0.0};
    other.schem.junctions_mut().push_back(a);
    other.schem.junctions_mut().push_back(c);
    Wire w2;
    w2.id = w.id;
    w2.a = ws::circsim::JunctionRef{a.id};
    w2.b = ws::circsim::JunctionRef{c.id};
    w2.ortho = true;
    other.schem.wires_mut().push_back(w2);
    other.schem.bump();
    while (other.schem.version() < b.doc.schem.version()) other.schem.bump();
    while (b.doc.schem.version() < other.schem.version()) b.doc.schem.bump();
    REQUIRE(other.schem.version() == b.doc.schem.version());
    b.ed.doc = &other;
    b.ed.document_changed();
    CHECK(b.ed.wire_geom(other.schem.wires().front()).max_x == 20.0);
}

TEST_CASE("view culling skips off-screen wires and draws visible ones") {
    SUBCASE("visible") {
        Bench b;
        b.place_wire();
        b.ed.view.center = {2.5, 1.0};
        b.ed.view.zoom = 26.0;
        b.draw();
        CHECK(wire_cmd(b) != nullptr);
    }
    SUBCASE("culled") {
        Bench b;
        b.place_wire();
        b.ed.view.center = {5000.0, 5000.0};
        b.ed.view.zoom = 26.0;
        b.draw();
        CHECK(wire_cmd(b) == nullptr);
    }
    SUBCASE("selected wire keeps highlight") {
        Bench b;
        b.place_wire();
        b.ed.view.center = {2.5, 1.0};
        b.ed.view.zoom = 26.0;
        b.draw();
        const canvas::CanvasCmd* plain = wire_cmd(b);
        REQUIRE(plain != nullptr);
        const std::uint32_t plain_color = plain->color;
        b.ed.set_selection({Selection::Kind::Wire,
                            b.doc.schem.wires().front().id});
        b.draw();
        const canvas::CanvasCmd* selected = wire_cmd(b);
        REQUIRE(selected != nullptr);
        CHECK(selected->color != plain_color);
    }
}

TEST_CASE("view cull margin covers every built-in symbol") {
    for (const ws::circsim::SymbolDef& sym :
         ws::circsim::builtin_library().symbols()) {
        const ws::circsim::Bounds bb = ws::circsim::symbol_bounds(sym);
        const double reach =
            std::max({std::fabs(bb.min_x), std::fabs(bb.max_x),
                      std::fabs(bb.min_y), std::fabs(bb.max_y)});
        CHECK(reach <= editor::kViewCullMarginCells);
    }
}
