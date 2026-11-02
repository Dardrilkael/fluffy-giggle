// apps/circsim_editor - the Schematic panel (see canvas_panel.hpp).
//
// One frame, in order: adopt the viewport, sync the camera with the
// document's stored view, handle the pointer (the interaction model
// from editor.hpp, gated on the gui's claims so an open modal veil
// freezes the canvas), then render the world - grid, wires, junctions,
// labels, parts, the selection highlight, and the tool ghosts/preview.
//
// The viewport is the scroll scope's content rect: the panel NEVER
// advances the flow (no layout.next()/advance() calls anywhere in this
// file), so the scroll range stays zero and the content rect the
// cursor sees is exactly the unscrolled viewport. The shell's wheel
// claim on the panel is then a harmless no-op and the canvas reads the
// same InputFrame for its zoom.
//
// The canvas transform is the camera with the y flip baked in
// (translate to the viewport's center, scale by zoom, negate y), so
// every draw below speaks the model's y-up grid units while strokes
// and text stay screen-sized (the canvas layer's own convention).

#include "canvas_panel.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include <ws/input/input.hpp>
#include <ws/ui/canvas/canvas2d.hpp>

namespace editor {
namespace {

using canvas::Paint;
using canvas::Point;

// The screen-space pick radius: hits are converted to world units by
// the zoom, so picks stay steady at every magnification.
constexpr float kPickPx = 9.0f;

[[nodiscard]] double pick_world(const EditorView& view) {
    return view.valid() ? static_cast<double>(kPickPx) / view.zoom : 0.35;
}

// The wire tool's snap: pins first, then junctions, then the grid.
struct SnapTarget {
    enum class Kind : std::uint8_t { Free, Pin, Junction };
    Kind kind = Kind::Free;
    ws::circsim::WireEndpoint end{};  // valid for Pin / Junction
    Pos pos{0.0, 0.0};
    int dir = -1;  // the approach direction, -1 when free
};

[[nodiscard]] SnapTarget snap_wire_target(const Editor& ed, Pos world) {
    if (const std::optional<PinHit> pin =
            ed.hit_pin(world, pick_world(ed.view))) {
        SnapTarget t;
        t.kind = SnapTarget::Kind::Pin;
        t.end = pin->ref;
        t.pos = pin->pos;
        t.dir = pin->dir;
        return t;
    }
    if (const std::uint32_t j =
            ed.hit_junction(world, pick_world(ed.view)).value_or(0)) {
        SnapTarget t;
        t.kind = SnapTarget::Kind::Junction;
        t.end = ws::circsim::JunctionRef{j};
        const ws::circsim::Junction* found = ed.schem()->find_junction(j);
        t.pos = found != nullptr ? found->pos : world;
        return t;
    }
    SnapTarget t;
    t.pos = ed.snap(world);
    return t;
}

// --- one pointer frame -------------------------------------------------------

void handle_pointer(Editor& ed, gui::GuiContext& gui,
                     const gui::Rect& body) {
    const ws::input::InputFrame& in = gui.input();
    const bool inside =
        in.mouse_x >= body.x && in.mouse_x < body.x + body.width &&
        in.mouse_y >= body.y && in.mouse_y < body.y + body.height;
    const ws::math::Vec2 mouse{in.mouse_x, in.mouse_y};
    const Pos world = ed.view.s2w(mouse);
    const double pick = pick_world(ed.view);
    const bool space_down = in.key_down[32] != 0;

    // Wheel: zoom toward the cursor (the scroll panel's own claim is
    // the harmless no-op the header note explains).
    if (inside && in.scroll_dy != 0.0f) {
        ed.view.zoom_at(mouse, in.scroll_dy > 0.0f ? 1.15 : 1.0 / 1.15);
    }

    // Middle-drag pans; space+left-drag pans too (the gallery's rule).
    const bool pan_modifiers =
        in.mouse_down[1] != 0 || (space_down && in.mouse_down[0] != 0);
    if (pan_modifiers && inside) {
        if (!ed.pan.armed) {
            ed.pan.armed = true;
            ed.pan.from = mouse;
        }
        ed.view.pan_by({mouse.x - ed.pan.from.x, mouse.y - ed.pan.from.y});
        ed.pan.from = mouse;
        return;  // a pan frame owns the pointer
    }
    ed.pan.armed = false;

    // A drag in flight outranks every tool arming.
    if (ed.drag.kind != Editor::Drag::Kind::None) {
        if (in.mouse_down[0] != 0) {
            const Pos at = ed.snap(
                Pos{world.x - ed.drag.grab_offset.x,
                    world.y - ed.drag.grab_offset.y});
            if (!near(at.x, ed.drag.start_world.x, 1e-9) ||
                !near(at.y, ed.drag.start_world.y, 1e-9)) {
                ed.drag.moved = true;
            }
            if (ed.drag.moved && ed.schem() != nullptr) {
                // Live move: direct writes, no undo - the command lands
                // at release with the from/to pair (push IS the edit,
                // and apply() re-landing the same position is exact).
                if (ed.drag.kind == Editor::Drag::Kind::Part) {
                    if (ws::circsim::Part* p =
                            ed.schem()->find_part(ed.drag.id)) {
                        p->pos = at;
                        ed.schem()->bump();
                    }
                } else if (ed.drag.kind == Editor::Drag::Kind::Junction) {
                    for (ws::circsim::Junction& j :
                         ed.schem()->junctions_mut()) {
                        if (j.id == ed.drag.id) {
                            j.pos = at;
                            ed.schem()->bump();
                            break;
                        }
                    }
                } else if (ed.drag.kind == Editor::Drag::Kind::Label) {
                    for (ws::circsim::Label& l : ed.schem()->labels_mut()) {
                        if (l.id == ed.drag.id) {
                            l.pos = at;
                            ed.schem()->bump();
                            break;
                        }
                    }
                }
            }
            return;
        }
        // Release: land the undo command when the drag actually moved.
        if (ed.drag.moved && ed.doc != nullptr) {
            const Pos to = ed.snap(
                Pos{world.x - ed.drag.grab_offset.x,
                    world.y - ed.drag.grab_offset.y});
            if (ed.drag.kind == Editor::Drag::Kind::Part) {
                push_move_part(*ed.doc, ed.drag.id, to);
            } else if (ed.drag.kind == Editor::Drag::Kind::Junction) {
                push_move_junction(*ed.doc, ed.drag.id, to);
            } else if (ed.drag.kind == Editor::Drag::Kind::Label) {
                push_move_label(*ed.doc, ed.drag.id, to);
            }
        }
        ed.drag = Editor::Drag{};
        return;
    }

    // The tools take the LEFT button on press edges, inside the body,
    // while the gui's claims are open (a modal veil freezes the
    // canvas) and space is not held (that is a pan).
    if (!(inside && gui.claims_open() && !space_down)) {
        return;
    }

    if (in.mouse_pressed[0] != 0) {
        switch (ed.tool) {
            case Tool::Select: {
                const Selection hit = ed.hit_any(world, pick);
                ed.sel = hit;
                if (hit.kind == Selection::Kind::Part) {
                    if (const ws::circsim::Part* p =
                            ed.schem()->find_part(hit.id)) {
                        ed.drag.kind = Editor::Drag::Kind::Part;
                        ed.drag.id = hit.id;
                        ed.drag.start_world = p->pos;
                        ed.drag.grab_offset = Pos{world.x - p->pos.x,
                                                  world.y - p->pos.y};
                    }
                } else if (hit.kind == Selection::Kind::Junction) {
                    if (const ws::circsim::Junction* j =
                            ed.schem()->find_junction(hit.id)) {
                        ed.drag.kind = Editor::Drag::Kind::Junction;
                        ed.drag.id = hit.id;
                        ed.drag.start_world = j->pos;
                        ed.drag.grab_offset = Pos{world.x - j->pos.x,
                                                  world.y - j->pos.y};
                    }
                } else if (hit.kind == Selection::Kind::Label) {
                    if (const ws::circsim::Label* l =
                            ed.schem()->find_label(hit.id)) {
                        ed.drag.kind = Editor::Drag::Kind::Label;
                        ed.drag.id = hit.id;
                        ed.drag.start_world = l->pos;
                        ed.drag.grab_offset = Pos{world.x - l->pos.x,
                                                  world.y - l->pos.y};
                    }
                }
                break;
            }
            case Tool::Place: {
                if (ed.doc == nullptr || ed.place_symbol.empty()) {
                    break;
                }
                const ws::circsim::SymbolDef* sym =
                    ed.library.find(ed.place_symbol);
                if (sym == nullptr) {
                    break;
                }
                ws::circsim::Part p = ws::circsim::Part::make(
                    sym->key, next_ref(*ed.schem(), *sym), "",
                    ed.snap(world));
                p.rot = ed.ghost_rot;
                p.mirror = ed.ghost_mirror;
                p.params = ws::circsim::default_params(*sym);
                push_place_part(*ed.doc, std::move(p));
                break;
            }
            case Tool::Wire: {
                if (ed.doc == nullptr) {
                    break;
                }
                if (!ed.wire_draft.active) {
                    // Start only on a connection point.
                    const SnapTarget t = snap_wire_target(ed, world);
                    if (t.kind != SnapTarget::Kind::Free) {
                        ed.wire_draft.active = true;
                        ed.wire_draft.a = t.end;
                        ed.wire_draft.a_pos = t.pos;
                        ed.wire_draft.a_dir = t.dir;
                    }
                    break;
                }
                // Commit: onto a pin/junction, or free space (a fresh
                // junction there - its id minted now, the one path that
                // can, so the wire's endpoint names it).
                const SnapTarget t = snap_wire_target(ed, world);
                if (t.kind == SnapTarget::Kind::Free &&
                    near(t.pos.x, ed.wire_draft.a_pos.x, 1e-9) &&
                    near(t.pos.y, ed.wire_draft.a_pos.y, 1e-9)) {
                    break;  // a zero-length wire is not a commitment
                }
                ws::circsim::Wire w;
                std::vector<ws::circsim::Junction> junctions;
                if (t.kind == SnapTarget::Kind::Free) {
                    ws::circsim::Junction j;
                    j.pos = t.pos;
                    j.id = ed.schem()->alloc_id();
                    junctions.push_back(j);
                    w.b = ws::circsim::JunctionRef{j.id};
                } else {
                    w.b = t.end;
                }
                w.a = ed.wire_draft.a;
                w.waypoints = Editor::route_wire(ed.wire_draft.a_pos,
                                                 ed.wire_draft.a_dir, t.pos,
                                                 t.dir);
                w.ortho = true;
                push_place_wire(*ed.doc, std::move(w), std::move(junctions));
                ed.wire_draft = Editor::WireDraft{};
                ed.tool = Tool::Select;  // every commit disarms (W re-arms)
                break;
            }
            case Tool::Label: {
                if (ed.doc == nullptr || ed.label_text[0] == '\0') {
                    break;
                }
                ws::circsim::Label l;
                l.pos = ed.snap(world);
                l.name = ed.label_text;
                push_place_label(*ed.doc, std::move(l));
                break;
            }
        }
        return;
    }

    // Right click: cancel the tool / clear the selection.
    if (in.mouse_pressed[2] != 0) {
        if (ed.tool != Tool::Select) {
            ed.tool = Tool::Select;
            ed.wire_draft = Editor::WireDraft{};
        } else {
            ed.sel.clear();
        }
    }
}

}  // namespace

// --- the render ---------------------------------------------------------------

void draw_canvas_panel(Editor& ed, gui::GuiContext& gui,
                       gui::LayoutCursor& layout,
                       const gui::ChromeTheme& theme) {
    // The scroll scope's content rect IS the viewport (this panel
    // never advances the flow - the file header's invariant).
    const gui::Rect body = layout.panel_content();
    if (body.width <= 8.0f || body.height <= 8.0f) {
        return;
    }
    if (ed.schem() == nullptr) {
        gui::text(layout, theme, "no document - File > New");
        return;
    }

    // The camera adopts the viewport; the document's stored view state
    // rides back out every frame (view changes are not edits - no
    // bump(), no undo).
    ed.view.vx = body.x;
    ed.view.vy = body.y;
    ed.view.vw = body.width;
    ed.view.vh = body.height;
    ws::circsim::Schematic& s = *ed.schem();
    s.view.zoom = ed.view.zoom;
    s.view.center = ed.view.center;

    handle_pointer(ed, gui, body);

    // --- the grid, in SCREEN space and pixel-snapped.
    // Why not world space: a 1px stroke centred between two pixel rows
    // splits its coverage over both, so with a fractional pan/zoom some
    // lines come out crisp and others half-intensity, and the whole
    // grid shimmers while panning. Here every line's centre is placed
    // on a pixel centre (floor + 0.5) so each one covers exactly one
    // pixel column/row at full alpha.
    // Spacing follows the schematic's snap grid (s.grid), in levels of
    // x5: the next level's minors ARE this level's majors, so a level
    // change never moves a visible line - minors just fade in/out.
    {
        const double zoom = ed.view.zoom;
        const double base = s.grid > 0.0 ? s.grid : 1.0;
        constexpr double kMinCellPx = 10.0;   // densest minor cell
        constexpr double kFullCellPx = 24.0;  // minors at full alpha
        double step = base;
        for (int guard = 0; step * zoom < kMinCellPx && guard < 32; ++guard) {
            step *= 5.0;
        }
        const double cell = step * zoom;
        double fade = (cell - kMinCellPx) / (kFullCellPx - kMinCellPx);
        fade = std::clamp(fade, 0.0, 1.0);
        fade = fade * fade * (3.0 - 2.0 * fade);  // smoothstep

        const float left = body.x;
        const float top = body.y;
        const float right = body.x + body.width;
        const float bottom = body.y + body.height;
        const double cx = static_cast<double>(body.x) + body.width * 0.5;
        const double cy = static_cast<double>(body.y) + body.height * 0.5;
        const double half_w = static_cast<double>(body.width) * 0.5 / zoom;
        const double half_h = static_cast<double>(body.height) * 0.5 / zoom;

        Paint line;
        line.width = 1.0f;
        const auto paint_for = [&](long long i) -> Paint {
            Paint p = line;
            if (i == 0) {
                p.color = gui::dim(theme.text, 0.34f);
            } else if (i % 5 == 0) {
                p.color = gui::dim(theme.text, 0.20f);
            } else {
                p.color = gui::dim(theme.text, static_cast<float>(0.10 * fade));
            }
            return p;
        };

        const long long ix0 = static_cast<long long>(
            std::ceil((ed.view.center.x - half_w) / step));
        const long long ix1 = static_cast<long long>(
            std::floor((ed.view.center.x + half_w) / step));
        const long long iy0 = static_cast<long long>(
            std::ceil((ed.view.center.y - half_h) / step));
        const long long iy1 = static_cast<long long>(
            std::floor((ed.view.center.y + half_h) / step));
        if (ix1 - ix0 < 4000 && iy1 - iy0 < 4000) {
            for (long long i = ix0; i <= ix1; ++i) {
                const double sx =
                    cx + (static_cast<double>(i) * step - ed.view.center.x) * zoom;
                const float px = std::floor(static_cast<float>(sx)) + 0.5f;
                ed.renderer.stroke_line(Point{px, top}, Point{px, bottom}, paint_for(i));
            }
            for (long long i = iy0; i <= iy1; ++i) {
                // y is flipped: model y-up -> screen y-down
                const double sy =
                    cy - (static_cast<double>(i) * step - ed.view.center.y) * zoom;
                const float py = std::floor(static_cast<float>(sy)) + 0.5f;
                ed.renderer.stroke_line(Point{left, py}, Point{right, py}, paint_for(i));
            }
        }
    }

    // The canvas: the camera transform with the y flip, then the world.
    canvas::Renderer2DCanvas& rc = ed.renderer;
    rc.set_view_scale(static_cast<float>(ed.view.zoom));
    rc.push_transform();
    rc.translate(body.x + body.width * 0.5f, body.y + body.height * 0.5f);
    rc.scale(static_cast<float>(ed.view.zoom),
             -static_cast<float>(ed.view.zoom));

    const ws::input::InputFrame& in = gui.input();
    const bool inside =
        in.mouse_x >= body.x && in.mouse_x < body.x + body.width &&
        in.mouse_y >= body.y && in.mouse_y < body.y + body.height;
    const Pos world = ed.view.s2w({in.mouse_x, in.mouse_y});

    // --- wires, junctions, labels
    Paint wire_paint;
    wire_paint.width = 2.0f;
    wire_paint.color = gui::dim(theme.text, 0.85f);
    Paint wire_sel = wire_paint;
    wire_sel.color = theme.control_on;
    wire_sel.width = 3.0f;

    for (const ws::circsim::Wire& w : s.wires()) {
        const std::vector<Pos> path = ed.wire_path(w);
        if (path.size() < 2u) {
            continue;
        }
        std::vector<Point> pts;
        pts.reserve(path.size());
        for (const Pos& p : path) {
            pts.push_back(Point{static_cast<float>(p.x),
                                static_cast<float>(p.y)});
        }
        rc.stroke_polyline(
            pts.data(), static_cast<int>(pts.size()),
            ed.sel.is(Selection::Kind::Wire, w.id) ? wire_sel : wire_paint);
    }
    for (const ws::circsim::Junction& j : s.junctions()) {
        rc.fill_circle(
            Point{static_cast<float>(j.pos.x), static_cast<float>(j.pos.y)},
            0.14f, ed.sel.is(Selection::Kind::Junction, j.id)
                       ? theme.control_on
                       : gui::dim(theme.text, 0.85f));
    }
    if (theme.font != nullptr) {
        for (const ws::circsim::Label& l : s.labels()) {
            const Point at{static_cast<float>(l.pos.x),
                           static_cast<float>(l.pos.y)};
            rc.stroke_line(at, Point{at.x + 0.35f, at.y + 0.35f},
                           wire_paint);
            (void)rc.draw_text(at.x + 0.45f, at.y - 0.1f, l.name.c_str(),
                               *theme.font,
                               ed.sel.is(Selection::Kind::Label, l.id)
                                   ? theme.control_on
                                   : gui::dim(theme.text, 0.9f));
        }
    }

    // --- parts: the selection highlight under the symbol
    Paint body_paint;
    body_paint.width = 2.0f;
    body_paint.color = theme.text;
    Paint ghost_paint = body_paint;
    ghost_paint.color = gui::dim(theme.text, 0.45f);

    for (const ws::circsim::Part& p : s.parts()) {
        const bool selected = ed.sel.is(Selection::Kind::Part, p.id);
        if (selected) {
            if (const std::optional<Bounds> b = ed.part_bounds(p.id)) {
                Paint sel_paint;
                sel_paint.width = 1.5f;
                sel_paint.color = theme.control_on;
                sel_paint.dash.count = 2;
                sel_paint.dash.lengths[0] = 0.45f;
                sel_paint.dash.lengths[1] = 0.3f;
                const Point a{static_cast<float>(b->min_x),
                              static_cast<float>(b->min_y)};
                const Point c{static_cast<float>(b->max_x),
                              static_cast<float>(b->max_y)};
                const Point quad[4] = {a, {c.x, a.y}, c, {a.x, c.y}};
                rc.stroke_polyline(quad, 4, sel_paint, true);
            }
        }
        const ws::circsim::SymbolDef* sym = ed.sym_of(p);
        if (sym != nullptr) {
            draw_symbol(rc, *sym, p.pos, p.rot, p.mirror, body_paint,
                        theme.text, theme.font);
        } else {
            // An unknown symbol: the origin cross, so the object is
            // still visible and selectable (ERC has flagged it).
            rc.stroke_line(
                Point{static_cast<float>(p.pos.x - 0.5),
                      static_cast<float>(p.pos.y)},
                Point{static_cast<float>(p.pos.x + 0.5),
                      static_cast<float>(p.pos.y)},
                body_paint);
            rc.stroke_line(
                Point{static_cast<float>(p.pos.x),
                      static_cast<float>(p.pos.y - 0.5)},
                Point{static_cast<float>(p.pos.x),
                      static_cast<float>(p.pos.y + 0.5)},
                body_paint);
        }
        // The refdes above, the value below (power symbols carry no
        // ref; the value line only when there is one).
        if (theme.font != nullptr) {
            const std::optional<Bounds> b = ed.part_bounds(p.id);
            const double top = b.has_value() ? b->max_y : p.pos.y;
            const double bottom = b.has_value() ? b->min_y : p.pos.y;
            if (!p.ref.empty()) {
                (void)rc.draw_text(static_cast<float>(p.pos.x - 0.4f),
                                   static_cast<float>(top + 0.35f),
                                   p.ref.c_str(), *theme.font,
                                   gui::dim(theme.text, 0.75f));
            }
            if (!p.value.empty()) {
                (void)rc.draw_text(static_cast<float>(p.pos.x - 0.4f),
                                   static_cast<float>(bottom - 0.9f),
                                   p.value.c_str(), *theme.font,
                                   gui::dim(theme.text, 0.75f));
            }
        }
    }

    // --- the pin target highlight (the wire tool's cursor)
    if (ed.tool == Tool::Wire && inside && gui.claims_open()) {
        if (const std::optional<PinHit> pin =
                ed.hit_pin(world, pick_world(ed.view))) {
            Paint pin_paint;
            pin_paint.width = 2.0f;
            pin_paint.color = theme.control_on;
            rc.stroke_circle(Point{static_cast<float>(pin->pos.x),
                                   static_cast<float>(pin->pos.y)},
                             0.35f, pin_paint);
        }
    }

    // --- the wire draft preview
    if (ed.wire_draft.active && inside) {
        const SnapTarget t = snap_wire_target(ed, world);
        if (!near(t.pos.x, ed.wire_draft.a_pos.x, 1e-9) ||
            !near(t.pos.y, ed.wire_draft.a_pos.y, 1e-9)) {
            const std::vector<Pos> route = Editor::route_wire(
                ed.wire_draft.a_pos, ed.wire_draft.a_dir, t.pos, t.dir);
            std::vector<Point> pts;
            pts.push_back(Point{static_cast<float>(ed.wire_draft.a_pos.x),
                                static_cast<float>(ed.wire_draft.a_pos.y)});
            for (const Pos& p : route) {
                pts.push_back(
                    Point{static_cast<float>(p.x), static_cast<float>(p.y)});
            }
            pts.push_back(
                Point{static_cast<float>(t.pos.x), static_cast<float>(t.pos.y)});
            Paint preview;
            preview.width = 2.0f;
            preview.color = theme.control_on;
            preview.dash.count = 2;
            preview.dash.lengths[0] = 0.5f;
            preview.dash.lengths[1] = 0.35f;
            rc.stroke_polyline(pts.data(), static_cast<int>(pts.size()),
                               preview);
        }
    }

    // --- the placement / label ghosts
    if (inside && gui.claims_open()) {
        if (ed.tool == Tool::Place && !ed.place_symbol.empty()) {
            if (const ws::circsim::SymbolDef* sym =
                    ed.library.find(ed.place_symbol)) {
                draw_symbol(rc, *sym, ed.snap(world), ed.ghost_rot,
                            ed.ghost_mirror, ghost_paint,
                            gui::dim(theme.text, 0.45f), theme.font);
            }
        } else if (ed.tool == Tool::Label && ed.label_text[0] != '\0' &&
                   theme.font != nullptr) {
            const Pos at = ed.snap(world);
            (void)rc.draw_text(static_cast<float>(at.x + 0.45f),
                               static_cast<float>(at.y - 0.1f),
                               ed.label_text, *theme.font,
                               gui::dim(theme.text, 0.45f));
        }
    }

    rc.pop_transform();
}

}  // namespace editor
