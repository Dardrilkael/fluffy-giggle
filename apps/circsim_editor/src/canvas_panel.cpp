// apps/circsim_editor - the Schematic panel (see canvas_panel.hpp).
//
// One frame, in order: adopt the viewport, sync the camera with the
// document's stored view, handle the pointer (the interaction model
// from editor.hpp, gated on the gui's claims so an open modal veil
// freezes the canvas), then render the world - the canvas floor and
// the dot grid (ws::ui::canvas's grid system, pixel-snapped in screen
// space - one spec with the snap, its brightness eased up while a
// tool is engaged), the snap feedback (a ring on the wire tool's
// pin/junction target, an accent dot on the grid intersection a
// placement or drag is about to land on), wires, junctions, labels,
// parts, the selection highlight, and the tool ghosts/preview.
//
// The viewport is the scroll scope's content rect: the panel NEVER
// advances the flow (the one exception: the no-document early-out
// prints a single text row - nothing else runs in that frame), so the
// scroll range stays zero and the content rect the cursor sees is
// exactly the unscrolled viewport. The shell's wheel
// claim on the panel is then a harmless no-op and the canvas reads the
// same InputFrame for its zoom.
//
// The canvas transform is the camera with the y flip baked in
// (translate to the viewport's center, scale by zoom, negate y,
// translate by the negated camera center) - the exact inverse of
// EditorView::s2w, so every draw below speaks the model's y-up grid
// units from the camera's own eye while strokes and text stay
// screen-sized (the canvas layer's own convention). The grid and the
// snap feedback are the two exceptions: they draw in SCREEN space
// before the transform so their 1px marks land on device pixels at
// any pan/zoom.

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

// The label pick's own reach beyond the pick radius: a label's tag is
// screen-sized text, so its margin rides the zoom too (a fixed world
// unit once made the pick area 400px wide at max zoom).
constexpr float kLabelPickPx = 6.0f;

// The snap feedback's reach: the accent dot shows while the cursor is
// within this many screen px of the grid intersection it would land
// on (and the dot's own px).
constexpr float kSnapFeedbackPx = 8.0f;
constexpr float kSnapDotPx = 5.0f;

// The wheel zoom's step per scroll notch (pow reads the notch count,
// so fractional deltas zoom fractionally).
constexpr float kZoomStep = 1.15f;

// The junction dots' screen floor: a dot is 0.14 world units at the
// working zooms, but below ~zoom 14 that is under 2px on screen -
// the radius never shrinks past this many px (the ring and the pick
// agree by construction: the ring IS the pick radius in world units).
constexpr float kDotFloorPx = 2.0f;

[[nodiscard]] float dot_world(const EditorView& view) {
    return view.zoom > 0.0f
               ? std::max(0.14f,
                          static_cast<float>(kDotFloorPx / view.zoom))
               : 0.14f;
}

[[nodiscard]] double pick_world(const EditorView& view) {
    return view.valid() ? static_cast<double>(kPickPx) / view.zoom : 0.35;
}

// The wire tool's snap: pins first, then junctions, then a point on
// an existing wire's body (stage 18b - the editor mints a junction
// there at commit, the standard EDA T-tap), then the grid.
struct SnapTarget {
    enum class Kind : std::uint8_t { Free, Pin, Junction, WirePoint };
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
    if (const std::optional<Editor::WirePointSnap> wp =
            ed.hit_wire_point(world, pick_world(ed.view))) {
        SnapTarget t;
        t.kind = SnapTarget::Kind::WirePoint;
        t.pos = wp->pos;
        t.dir = -1;  // no approach constraint - the new wire leaves freely
        return t;
    }
    SnapTarget t;
    t.pos = ed.snap(world);
    return t;
}

// The drag's landing spot - one helper for the three former copies
// (the live frame, the release, the feedback dot): the entity follows
// the cursor minus its grab offset, snapped. A junction drag always
// stays on the fine grid; parts and labels follow the Shift modifier.
[[nodiscard]] Pos drag_landing(const Editor& ed,
                               const ws::input::InputFrame& in, Pos world) {
    const bool coarse =
        in.key_down[ws::input::keys::kLeftShift] != 0 ||
        in.key_down[ws::input::keys::kRightShift] != 0;
    return ed.snap(Pos{world.x - ed.drag.grab_offset.x,
                       world.y - ed.drag.grab_offset.y},
                   coarse && ed.drag.kind != Editor::Drag::Kind::Junction);
}

// Slides the dragged entity to `at`. Used twice at release: the
// entity visits the landing so the attached-wire routes read the
// resolved endpoint positions, then goes home so the command captures
// the press position as its from. The live frames write their own
// positions inline (they need the bump per frame too).
void set_drag_pos(Editor& ed, Pos at) {
    if (ed.schem() == nullptr) {
        return;
    }
    switch (ed.drag.kind) {
        case Editor::Drag::Kind::Part: {
            if (ws::circsim::Part* p =
                    ed.schem()->find_part(ed.drag.id)) {
                p->pos = at;
            }
            break;
        }
        case Editor::Drag::Kind::Junction: {
            for (ws::circsim::Junction& j : ed.schem()->junctions_mut()) {
                if (j.id == ed.drag.id) {
                    j.pos = at;
                    break;
                }
            }
            break;
        }
        case Editor::Drag::Kind::Label: {
            for (ws::circsim::Label& l : ed.schem()->labels_mut()) {
                if (l.id == ed.drag.id) {
                    l.pos = at;
                    break;
                }
            }
            break;
        }
        case Editor::Drag::Kind::None:
            break;
    }
}

// The draft's start anchor may be gone by the commit (an undo, a
// delete, any edit between the two clicks): the document layer's rule
// is that the editor never creates a dangling endpoint, so the commit
// revalidates before it builds. A free start has no anchor - a
// position is always its own anchor; a wire-point start needs its
// anchor wire still passing through a_pos (the commit mints the
// junction there); a pin or junction start needs its entity.
[[nodiscard]] bool pos_on_path(const std::vector<Pos>& path, Pos p) {
    for (std::size_t i = 1; i < path.size(); ++i) {
        const Pos& a = path[i - 1];
        const Pos& b = path[i];
        const double cross =
            (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (cross == 0.0 && p.x >= std::min(a.x, b.x) &&
            p.x <= std::max(a.x, b.x) && p.y >= std::min(a.y, b.y) &&
            p.y <= std::max(a.y, b.y)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool draft_start_valid(const Editor& ed) {
    const ws::circsim::Schematic& s = *ed.schem();
    const Editor::WireDraft& d = ed.wire_draft;
    if (d.a_free) {
        return true;
    }
    if (d.a_mint) {
        for (const ws::circsim::Wire& w : s.wires()) {
            if (pos_on_path(ed.wire_path(w), d.a_pos)) {
                return true;
            }
        }
        return false;
    }
    if (const auto* pin = std::get_if<ws::circsim::PinRef>(&d.a)) {
        const ws::circsim::Part* p = s.find_part(pin->part);
        const ws::circsim::SymbolDef* sym =
            p != nullptr ? ed.sym_of(*p) : nullptr;
        return p != nullptr && sym != nullptr &&
               pin->pin < sym->pins.size();
    }
    if (const auto* j = std::get_if<ws::circsim::JunctionRef>(&d.a)) {
        return s.find_junction(j->junction) != nullptr;
    }
    return false;
}

// --- one pointer frame -------------------------------------------------------
//
// The pointer frame splits by concern: the wheel and the pan first
// (they own the pointer when they act), a drag in flight second, the
// tools' press edges third, the right-click staging last. Every piece
// is its own function below; handle_pointer is the ~40-line
// orchestrator.

// The wheel zoom and the pan gesture. Returns true when the frame
// belonged to a pan (the pointer is consumed; the tools must not read
// it this frame).
bool pointer_zoom_pan(Editor& ed, gui::GuiContext& gui,
                      const ws::input::InputFrame& in,
                      ws::math::Vec2 mouse, bool inside,
                      bool space_down) {
    // Wheel: zoom toward the cursor (the scroll panel's own claim is
    // the harmless no-op the header note explains). An open modal
    // veil freezes the canvas - the zoom with it (the header's own
    // contract; only the tools used to check the claims).
    if (inside && gui.claims_open() && in.scroll_dy != 0.0f) {
        // One notch = one kZoomStep; a trackpad's many small deltas
        // ride the same curve gently instead of jumping whole notches
        // (the exponent clamps against a wild scroll burst).
        const float deltas = std::clamp(in.scroll_dy, -4.0f, 4.0f);
        ed.view.zoom_at(mouse, std::pow(kZoomStep, deltas));
    }

    // Middle-drag pans; space+left-drag pans too (the gallery's rule).
    // Armed inside the panel; once armed the gesture keeps the pointer
    // until the buttons go up - a cursor that slips off the panel edge
    // mid-pan no longer drops the pan (the drags' own rule). An open
    // modal veil disarms.
    const bool pan_modifiers =
        in.mouse_down[kMouseMiddle] != 0 ||
        (space_down && in.mouse_down[kMouseLeft] != 0);
    if (pan_modifiers && gui.claims_open() &&
        (ed.pan.armed || inside)) {
        if (!ed.pan.armed) {
            ed.pan.armed = true;
            ed.pan.from = mouse;
        }
        ed.view.pan_by({mouse.x - ed.pan.from.x, mouse.y - ed.pan.from.y});
        ed.pan.from = mouse;
        return true;  // a pan frame owns the pointer
    }
    ed.pan.armed = false;
    return false;
}

// One held frame of a move-drag: the entity follows the cursor (live,
// direct writes, no undo - the command lands at release).
void pointer_drag_live(Editor& ed, const ws::input::InputFrame& in,
                       Pos world) {
    const Pos at = drag_landing(ed, in, world);
    if (!eps_eq(at.x, ed.drag.start_world.x) ||
        !eps_eq(at.y, ed.drag.start_world.y)) {
        ed.drag.moved = true;
    }
    if (!ed.drag.moved || ed.schem() == nullptr) {
        return;
    }
    if (ed.drag.kind == Editor::Drag::Kind::Part) {
        if (ws::circsim::Part* p = ed.schem()->find_part(ed.drag.id)) {
            p->pos = at;
            ed.schem()->bump();
        }
    } else if (ed.drag.kind == Editor::Drag::Kind::Junction) {
        for (ws::circsim::Junction& j : ed.schem()->junctions_mut()) {
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

// The drag's release: land the undo command when the drag actually
// moved. The entity first visits the landing (the attached ortho
// wires re-route from the resolved endpoints there), then goes back
// to its press position, so the command captures THAT as its from -
// the live drag had left it at the release position, and a from ==
// to command is an undo no-op (nothing would ever move back).
void pointer_drag_release(Editor& ed, const ws::input::InputFrame& in,
                          Pos world) {
    if (!ed.drag.moved || ed.doc == nullptr) {
        return;
    }
    const Pos to = drag_landing(ed, in, world);
    set_drag_pos(ed, to);
    std::vector<WireRoute> routes =
        ed.attached_wire_routes(ed.drag.kind, ed.drag.id);
    set_drag_pos(ed, ed.drag.start_world);
    if (ed.drag.kind == Editor::Drag::Kind::Part) {
        push_move_part(*ed.doc, ed.drag.id, to, std::move(routes));
    } else if (ed.drag.kind == Editor::Drag::Kind::Junction) {
        push_move_junction(*ed.doc, ed.drag.id, to, std::move(routes));
    } else if (ed.drag.kind == Editor::Drag::Kind::Label) {
        push_move_label(*ed.doc, ed.drag.id, to);
    }
}

// The Select tool's press: pick the topmost thing, arm a move-drag
// when the pick is draggable.
void pointer_select_press(Editor& ed, Pos world, double pick) {
    const double label_pick =
        pick + (ed.view.zoom > 0.0
                    ? static_cast<double>(kLabelPickPx) / ed.view.zoom
                    : 0.0);
    const Selection hit = ed.hit_any(world, pick, label_pick);
    ed.sel = hit;
    const auto arm = [&](const auto* entity, Editor::Drag::Kind kind) {
        ed.drag.kind = kind;
        ed.drag.id = hit.id;
        ed.drag.start_world = entity->pos;
        ed.drag.grab_offset =
            Pos{world.x - entity->pos.x, world.y - entity->pos.y};
    };
    if (hit.kind == Selection::Kind::Part) {
        if (const ws::circsim::Part* p = ed.schem()->find_part(hit.id)) {
            arm(p, Editor::Drag::Kind::Part);
        }
    } else if (hit.kind == Selection::Kind::Junction) {
        if (const ws::circsim::Junction* j =
                ed.schem()->find_junction(hit.id)) {
            arm(j, Editor::Drag::Kind::Junction);
        }
    } else if (hit.kind == Selection::Kind::Label) {
        if (const ws::circsim::Label* l = ed.schem()->find_label(hit.id)) {
            arm(l, Editor::Drag::Kind::Label);
        }
    }
}

// The Wire tool's first click: start the draft on a connection point
// (a pin, a junction, another wire's body - stage 18b's T-tap) or on
// free grid. `pre` is the frame's pre-computed target (null = compute
// here).
void wire_start_draft(Editor& ed, Pos world, bool coarse,
                      const SnapTarget* pre) {
    const SnapTarget t =
        pre != nullptr ? *pre : snap_wire_target(ed, world);
    ed.wire_draft = Editor::WireDraft{};
    ed.wire_draft.active = true;
    ed.wire_draft.a_pos = t.pos;
    ed.wire_draft.a_vertical = coarse;
    if (t.kind == SnapTarget::Kind::Free) {
        // The commit side always supported free landings, so a free
        // START anchors the draft to the grid point and mints its
        // junction at commit (a cancel leaves nothing behind).
        ed.wire_draft.a_dir = -1;
        ed.wire_draft.a_mint = true;
        ed.wire_draft.a_free = true;
    } else {
        ed.wire_draft.a = t.end;
        ed.wire_draft.a_dir = t.dir;
        ed.wire_draft.a_mint = t.kind == SnapTarget::Kind::WirePoint;
    }
}

// The Wire tool's second click: commit. `pre` is the frame's
// pre-computed target (null = compute here).
void wire_commit_draft(Editor& ed, Pos world, const SnapTarget* pre) {
    // Onto a pin/junction, a wire's body (a fresh junction there -
    // its id minted now, the same path the free case takes), or free
    // space (also a fresh junction). The a side may also need a mint
    // (a start on a wire's body or free space), deferred to here so a
    // cancel leaves nothing behind.
    const SnapTarget t =
        pre != nullptr ? *pre : snap_wire_target(ed, world);
    // A zero-length wire is not a commitment - whatever the landing
    // kind. The old guard fired for Free landings only, so a second
    // click on the START entity (the same pin, the same junction, the
    // same wire-point) committed a degenerate self-loop: identical
    // endpoints, an empty folded route, and for a wire-point start
    // two junctions minted at one position. The preview already drew
    // nothing for the case - the commit now agrees. Nothing is lost:
    // the same position is already one net (the coincidence rules
    // unite coincident pins and junctions, and a wire-point landing
    // sits on its own wire).
    if (eps_eq(t.pos.x, ed.wire_draft.a_pos.x) &&
        eps_eq(t.pos.y, ed.wire_draft.a_pos.y)) {
        return;
    }
    // The start anchor may have died between the clicks (an undo, a
    // delete): the editor never creates a dangling endpoint, so the
    // draft is cancelled with a word instead.
    if (!draft_start_valid(ed)) {
        ed.toast("the wire's start is gone - the draft is cancelled",
                 gui::ToastSeverity::warning);
        ed.wire_draft = Editor::WireDraft{};
        return;
    }
    ws::circsim::Wire w;
    std::vector<ws::circsim::Junction> junctions;
    if (ed.wire_draft.a_mint) {
        ws::circsim::Junction j;
        j.pos = ed.wire_draft.a_pos;
        j.id = ed.schem()->alloc_id();
        junctions.push_back(j);
        w.a = ws::circsim::JunctionRef{j.id};
    } else {
        w.a = ed.wire_draft.a;
    }
    if (t.kind == SnapTarget::Kind::Free ||
        t.kind == SnapTarget::Kind::WirePoint) {
        ws::circsim::Junction j;
        j.pos = t.pos;
        j.id = ed.schem()->alloc_id();
        junctions.push_back(j);
        w.b = ws::circsim::JunctionRef{j.id};
    } else {
        w.b = t.end;
    }
    w.waypoints = Editor::route_wire(ed.wire_draft.a_pos,
                                     ed.wire_draft.a_dir, t.pos, t.dir,
                                     ed.wire_draft.a_vertical);
    w.ortho = true;
    const ws::circsim::WireEndpoint landing = w.b;
    push_place_wire(*ed.doc, std::move(w), std::move(junctions));
    // The tool stays armed and the next draft starts at the landing:
    // a multi-segment net is one run of clicks (the old commit
    // disarmed, so every segment cost a W press). The landing
    // endpoint exists now - a minted junction included - so the chain
    // binds to it directly, no mint pending.
    ed.wire_draft = Editor::WireDraft{};
    ed.wire_draft.active = true;
    ed.wire_draft.a = landing;
    ed.wire_draft.a_pos = t.pos;
    ed.wire_draft.a_dir = t.dir;
}

// The Wire tool's press (start or commit).
void pointer_wire_press(Editor& ed, Pos world, bool coarse,
                        const SnapTarget* pre) {
    if (ed.doc == nullptr) {
        return;
    }
    if (!ed.wire_draft.active) {
        wire_start_draft(ed, world, coarse, pre);
    } else {
        wire_commit_draft(ed, world, pre);
    }
}

// The tools' press edge: the switch dispatches per tool.
void pointer_tool_press(Editor& ed, const ws::input::InputFrame& in,
                        Pos world, bool coarse, double pick,
                        const SnapTarget* wire_snap) {
    if (in.mouse_pressed[kMouseLeft] == 0) {
        return;
    }
    switch (ed.tool) {
        case Tool::Select:
            pointer_select_press(ed, world, pick);
            break;
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
                ed.snap(world, coarse));
            p.rot = ed.ghost_rot;
            p.mirror = ed.ghost_mirror;
            p.params = ws::circsim::default_params(*sym);
            push_place_part(*ed.doc, std::move(p));
            break;
        }
        case Tool::Wire:
            pointer_wire_press(ed, world, coarse, wire_snap);
            break;
        case Tool::Label: {
            if (ed.doc == nullptr || ed.label_text[0] == '\0') {
                break;
            }
            ws::circsim::Label l;
            l.pos = ed.snap(world, coarse);
            l.name = ed.label_text;
            push_place_label(*ed.doc, std::move(l));
            break;
        }
    }
}

// The right click's staging: cancel the in-flight draft first (a
// stray click used to kill the draft AND the tool in one go), then
// disarm the tool, then clear the selection.
void pointer_right_click(Editor& ed,
                         const ws::input::InputFrame& in) {
    if (in.mouse_pressed[kMouseRight] == 0) {
        return;
    }
    if (ed.wire_draft.active) {
        ed.wire_draft = Editor::WireDraft{};
    } else if (ed.tool != Tool::Select) {
        ed.tool = Tool::Select;
    } else {
        ed.sel.clear();
    }
}

void handle_pointer(Editor& ed, gui::GuiContext& gui,
                    const gui::Rect& body, const SnapTarget* wire_snap) {
    const ws::input::InputFrame& in = gui.input();
    const bool inside =
        in.mouse_x >= body.x && in.mouse_x < body.x + body.width &&
        in.mouse_y >= body.y && in.mouse_y < body.y + body.height;
    const ws::math::Vec2 mouse{in.mouse_x, in.mouse_y};
    const Pos world = ed.view.s2w(mouse);
    const double pick = pick_world(ed.view);
    const bool space_down =
        in.key_down[ws::input::keys::kSpace] != 0;
    // Shift = the coarse snap: placements, part drags and labels land
    // on the major lattice (base * 5, an integer multiple of the fine
    // grid - a coarse position is always a fine position too). Wires,
    // pins and junctions never read it: their snaps below always pass
    // fine, so nets.cpp's exact-grid joins keep their invariant.
    const bool coarse =
        in.key_down[ws::input::keys::kLeftShift] != 0 ||
        in.key_down[ws::input::keys::kRightShift] != 0;

    if (pointer_zoom_pan(ed, gui, in, mouse, inside, space_down)) {
        return;
    }

    // A drag in flight outranks every tool arming.
    if (ed.drag.kind != Editor::Drag::Kind::None) {
        if (in.mouse_down[kMouseLeft] != 0) {
            pointer_drag_live(ed, in, world);
        } else {
            pointer_drag_release(ed, in, world);
            ed.drag = Editor::Drag{};
        }
        return;
    }

    // The tools take the LEFT button on press edges, inside the body,
    // while the gui's claims are open (a modal veil freezes the
    // canvas) and space is not held (that is a pan).
    if (!(inside && gui.claims_open() && !space_down)) {
        return;
    }
    pointer_tool_press(ed, in, world, coarse, pick, wire_snap);
    pointer_right_click(ed, in);
}

// --- the render halves --------------------------------------------------------
//
// The draw splits per layer: the grid, the snap feedback, the wires
// (with their junctions, auto dots and labels), the parts, the
// target ring and the wire preview, the ghosts. draw_canvas_panel is
// the ~50-line orchestrator.

// A world position as a canvas point (the y flip rides the
// transform, not the points).
[[nodiscard]] Point world_pt(const Pos& p) {
    return Point{static_cast<float>(p.x), static_cast<float>(p.y)};
}

// The four corners of a world-space box in stroke order (the closed
// polyline the selection highlight and the ghost footprint share).
[[nodiscard]] std::array<Point, 4> box_quad(const Bounds& b) {
    return {world_pt(Pos{b.min_x, b.min_y}),
            world_pt(Pos{b.max_x, b.min_y}),
            world_pt(Pos{b.max_x, b.max_y}),
            world_pt(Pos{b.min_x, b.max_y})};
}

// The canvas floor and the dot grid (SCREEN space, pixel-snapped).
// grid.hpp's system - ONE spec with the snap: the base spacing is
// the schematic's own grid (Schematic::grid through
// Editor::grid_spec), the faces the theme's tokens, the density the
// x5 level ladder whose minors fade in and out so a level change
// never moves or pops a mark. Every dot's centre is snapped from its
// integer index, so panning at fractional offsets is shimmer-free at
// any zoom. While a tool is armed or a drag is in flight the lattice
// brightens (a ~150ms ease, instant when the theme's motion is off);
// a view change alone never triggers it (view changes are not edits
// - the camera sync rides the same rule).
void draw_grid_layer(Editor& ed, gui::GuiContext& gui,
                     const gui::Rect& body,
                     const gui::ChromeTheme& theme,
                     canvas::Canvas2D& rc) {
    canvas::GridSpec spec = ed.grid_spec();
    spec.minor_color = theme.grid_minor;
    spec.major_color = theme.grid_major;
    spec.axis_color = theme.grid_axis;
    spec.show_dots = true;

    canvas::GridFrame frame;
    frame.center_x = ed.view.center.x;
    frame.center_y = ed.view.center.y;
    frame.zoom = ed.view.zoom;
    frame.ox = body.x;
    frame.oy = body.y;
    frame.view_w = body.width;
    frame.view_h = body.height;
    frame.y_flip = true;  // the model's y-up world

    const bool tool_engaged =
        ed.tool != Tool::Select ||
        ed.drag.kind != Editor::Drag::Kind::None || ed.wire_draft.active;
    const float ease = theme.anim_enabled ? 0.15f : 0.0f;
    const float travel = gui.animate_toward(
        gui.ids().internal_id("schematic grid"),
        tool_engaged ? 1.0f : 0.0f, ease);
    const float brightness = 0.85f + 0.15f * travel;

    const Point origin{body.x, body.y};
    rc.fill_rects(&origin, 1, body.width, body.height, theme.canvas_bg);
    canvas::draw_grid(rc, frame, spec, brightness);
}

// What the snap feedback shows this frame: the wire tool's ring (a
// pin/junction/wire-point target) or the free landing's grid spot -
// the drag's landing dot, or the placement's. `wire_snap` is the
// frame's pre-computed wire target when it is still fresh (a press
// frame recomputes: the click may have changed the schematic).
struct SnapFeedback {
    std::optional<Pos> ring;
    std::optional<Pos> free_spot;
};

[[nodiscard]] SnapFeedback snap_feedback_target(
    Editor& ed, const ws::input::InputFrame& in, Pos world, bool coarse,
    const SnapTarget* wire_snap) {
    SnapFeedback out;
    if (ed.tool == Tool::Wire || ed.wire_draft.active) {
        const SnapTarget t = wire_snap != nullptr
                                 ? *wire_snap
                                 : snap_wire_target(ed, world);
        if (t.kind == SnapTarget::Kind::Free) {
            out.free_spot = t.pos;
        } else {
            out.ring = t.pos;
        }
    } else if (ed.drag.kind != Editor::Drag::Kind::None) {
        // The drag's own landing spot (a junction drag stays on the
        // fine grid; parts and labels follow Shift) - the same helper
        // the live frame and the release read.
        out.free_spot = drag_landing(ed, in, world);
    } else if (ed.tool == Tool::Place || ed.tool == Tool::Label) {
        out.free_spot = ed.snap(world, coarse);
    }
    return out;
}

// The snap feedback (SCREEN space, drawing only - the snap itself is
// exactly what it always was): the accent dot when the cursor is
// within kSnapFeedbackPx of the spot it would land on.
void draw_free_spot(Editor& ed, const ws::input::InputFrame& in,
                    std::optional<Pos> free_spot,
                    const gui::ChromeTheme& theme, canvas::Canvas2D& rc) {
    if (!free_spot.has_value()) {
        return;
    }
    const ws::math::Vec2 sp = ed.view.w2s(*free_spot);
    const float dx = sp.x - in.mouse_x;
    const float dy = sp.y - in.mouse_y;
    if (dx * dx + dy * dy <= kSnapFeedbackPx * kSnapFeedbackPx) {
        const Point dot{canvas::snap_mark(sp.x, kSnapDotPx),
                        canvas::snap_mark(sp.y, kSnapDotPx)};
        rc.fill_rects(&dot, 1, kSnapDotPx, kSnapDotPx,
                      theme.control_on);
    }
}

// The wires, their junctions, the auto-junction dots (stage 18) and
// the labels - the drawing's connection layer.
void draw_wires_layer(Editor& ed, const ws::circsim::Schematic& s,
                      const gui::ChromeTheme& theme,
                      canvas::Canvas2D& rc) {
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
            pts.push_back(world_pt(p));
        }
        rc.stroke_polyline(
            pts.data(), static_cast<int>(pts.size()),
            ed.sel.is(Selection::Kind::Wire, w.id) ? wire_sel : wire_paint);
    }
    for (const ws::circsim::Junction& j : s.junctions()) {
        rc.fill_circle(world_pt(j.pos), dot_world(ed.view),
                       ed.sel.is(Selection::Kind::Junction, j.id)
                           ? theme.control_on
                           : gui::dim(theme.text, 0.85f));
    }
    // The silent joins' dots (stage 18): pins a wire's path picked
    // up, vertices T-riding another wire - the net rules connect
    // them, so the canvas says so, the same dot the authored junctions
    // wear. nets.cpp already suppressed every position an authored
    // junction or a wire landing covers, so what arrives here is
    // exactly the invisible set.
    for (const Pos& p : ed.auto_junction_dots()) {
        rc.fill_circle(world_pt(p), dot_world(ed.view),
                       gui::dim(theme.text, 0.85f));
    }
    if (theme.font != nullptr) {
        for (const ws::circsim::Label& l : s.labels()) {
            const Point at = world_pt(l.pos);
            rc.stroke_line(at, Point{at.x + 0.35f, at.y + 0.35f},
                           wire_paint);
            (void)rc.draw_text(at.x + 0.45f, at.y - 0.1f, l.name.c_str(),
                               *theme.font,
                               ed.sel.is(Selection::Kind::Label, l.id)
                                   ? theme.control_on
                                   : gui::dim(theme.text, 0.9f));
        }
    }
}

// The parts: the selection highlight under the symbol, the symbol (or
// the unknown-symbol cross), the refdes above and the value below.
void draw_parts_layer(Editor& ed, const ws::circsim::Schematic& s,
                      const gui::ChromeTheme& theme,
                      canvas::Canvas2D& rc) {
    Paint body_paint;
    body_paint.width = 2.0f;
    body_paint.color = theme.text;

    for (const ws::circsim::Part& p : s.parts()) {
        const bool selected = ed.sel.is(Selection::Kind::Part, p.id);
        if (selected) {
            if (const std::optional<Bounds> b = ed.part_bounds(p)) {
                Paint sel_paint;
                sel_paint.width = 1.5f;
                sel_paint.color = theme.control_on;
                sel_paint.dash.count = 2;
                sel_paint.dash.lengths[0] = 0.45f;
                sel_paint.dash.lengths[1] = 0.3f;
                const std::array<Point, 4> quad = box_quad(*b);
                rc.stroke_polyline(quad.data(), 4, sel_paint, true);
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
            const std::optional<Bounds> b = ed.part_bounds(p);
            const double top = b.has_value() ? b->max_y : p.pos.y;
            const double bottom = b.has_value() ? b->min_y : p.pos.y;
            if (!p.ref.empty()) {
                (void)rc.draw_text(static_cast<float>(p.pos.x - 0.4),
                                   static_cast<float>(top + 0.35),
                                   p.ref.c_str(), *theme.font,
                                   gui::dim(theme.text, 0.75f));
            }
            if (!p.value.empty()) {
                (void)rc.draw_text(static_cast<float>(p.pos.x - 0.4),
                                   static_cast<float>(bottom - 0.9),
                                   p.value.c_str(), *theme.font,
                                   gui::dim(theme.text, 0.75f));
            }
        }
    }
}

// The pin/junction target highlight (the wire tool's cursor): a ring
// on whatever snap_wire_target answered - pins and junctions alike,
// the same target the preview routes to.
void draw_target_ring(Editor& ed, std::optional<Pos> ring_target,
                      const gui::ChromeTheme& theme,
                      canvas::Canvas2D& rc) {
    if (!ring_target.has_value()) {
        return;
    }
    Paint ring;
    ring.width = 2.0f;
    ring.color = theme.control_on;
    // The ring IS the pick area: the pick radius in world units, so
    // what you see ringed is exactly what a click would catch at
    // every zoom (a fixed 0.35 world ring once agreed with the 9px
    // pick only near zoom 26 - 35px wide at zoom 100, and the 0.14
    // dots under 1px at zoom 3).
    const float ring_r =
        ed.view.zoom > 0.0f ? kPickPx / static_cast<float>(ed.view.zoom)
                            : 0.35f;
    rc.stroke_circle(world_pt(*ring_target), ring_r, ring);
}

// The wire draft's preview: the ortho route the commit would store,
// dashed. `wire_snap` follows snap_feedback_target's freshness rule.
void draw_wire_preview(Editor& ed, Pos world,
                       const SnapTarget* wire_snap,
                       const gui::ChromeTheme& theme,
                       canvas::Canvas2D& rc) {
    if (!ed.wire_draft.active) {
        return;
    }
    const SnapTarget t =
        wire_snap != nullptr ? *wire_snap : snap_wire_target(ed, world);
    if (eps_eq(t.pos.x, ed.wire_draft.a_pos.x) &&
        eps_eq(t.pos.y, ed.wire_draft.a_pos.y)) {
        return;
    }
    const std::vector<Pos> route =
        Editor::route_wire(ed.wire_draft.a_pos, ed.wire_draft.a_dir,
                           t.pos, t.dir, ed.wire_draft.a_vertical);
    std::vector<Point> pts;
    pts.push_back(world_pt(ed.wire_draft.a_pos));
    for (const Pos& p : route) {
        pts.push_back(world_pt(p));
    }
    pts.push_back(world_pt(t.pos));
    Paint preview;
    preview.width = 2.0f;
    preview.color = theme.control_on;
    preview.dash.count = 2;
    preview.dash.lengths[0] = 0.5f;
    preview.dash.lengths[1] = 0.35f;
    rc.stroke_polyline(pts.data(), static_cast<int>(pts.size()),
                       preview);
}

// The placement / label ghosts (what the next click would put down).
void draw_ghost_layer(Editor& ed, Pos world, bool coarse,
                      const gui::ChromeTheme& theme,
                      canvas::Canvas2D& rc) {
    Paint ghost_paint;
    ghost_paint.width = 2.0f;
    ghost_paint.color = gui::dim(theme.text, 0.45f);

    if (ed.tool == Tool::Place && !ed.place_symbol.empty()) {
        if (const ws::circsim::SymbolDef* sym =
                ed.library.find(ed.place_symbol)) {
            // Draw the preview from the same transformed Part used on
            // placement. The outline and pin markers make the symbol's
            // footprint and connection points legible before
            // committing it, including after rotate/mirror.
            ws::circsim::Part ghost = ws::circsim::Part::make(
                sym->key, "", "", ed.snap(world, coarse));
            ghost.rot = ed.ghost_rot;
            ghost.mirror = ed.ghost_mirror;

            draw_symbol(rc, *sym, ghost.pos, ghost.rot, ghost.mirror,
                        ghost_paint, gui::dim(theme.text, 0.45f),
                        theme.font);

            // The footprint: each local corner transformed exactly
            // once (the quad stroke shares box_quad's corner order).
            const Bounds local = ws::circsim::symbol_bounds(*sym);
            const Pos local_corners[4] = {
                {local.min_x, local.min_y}, {local.max_x, local.min_y},
                {local.max_x, local.max_y}, {local.min_x, local.max_y}};
            std::array<Point, 4> corners;
            for (int i = 0; i < 4; ++i) {
                corners[i] = world_pt(
                    ws::circsim::transform_pos(ghost, local_corners[i]));
            }
            Paint footprint = ghost_paint;
            footprint.width = 1.0f;
            footprint.dash.count = 2;
            footprint.dash.lengths[0] = 0.22f;
            footprint.dash.lengths[1] = 0.18f;
            rc.stroke_polyline(corners.data(), 4, footprint, true);

            for (std::size_t i = 0; i < sym->pins.size(); ++i) {
                const Pos pin = ws::circsim::pin_pos(*sym, ghost, i);
                rc.fill_circle(world_pt(pin), 0.12f, theme.control_on);
            }
        }
    } else if (ed.tool == Tool::Label && ed.label_text[0] != '\0' &&
               theme.font != nullptr) {
        const Pos at = ed.snap(world, coarse);
        (void)rc.draw_text(static_cast<float>(at.x + 0.45f),
                           static_cast<float>(at.y - 0.1f),
                           ed.label_text, *theme.font,
                           gui::dim(theme.text, 0.45f));
    }
}

}  // namespace

void draw_canvas_panel(Editor& ed, gui::GuiContext& gui,
                       gui::LayoutCursor& layout,
                       const gui::ChromeTheme& theme) {
    // The scroll scope's content rect IS the viewport (this panel
    // never advances the flow - the file header's invariant; the
    // no-document early-out below is the stated exception).
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

    // The frame's pointer facts.
    const ws::input::InputFrame& in = gui.input();
    const bool inside =
        in.mouse_x >= body.x && in.mouse_x < body.x + body.width &&
        in.mouse_y >= body.y && in.mouse_y < body.y + body.height;
    const Pos world = ed.view.s2w({in.mouse_x, in.mouse_y});
    const bool coarse =
        in.key_down[ws::input::keys::kLeftShift] != 0 ||
        in.key_down[ws::input::keys::kRightShift] != 0;

    // The wire tool's target, computed ONCE per frame: the pointer
    // handler reads it on press edges, and - when no click landed
    // this frame (a click may change the schematic) - the feedback
    // and the preview reuse it too. Idle frames pay one
    // snap_wire_target, not three.
    std::optional<SnapTarget> wire_snap;
    if ((ed.tool == Tool::Wire || ed.wire_draft.active) && inside &&
        gui.claims_open()) {
        wire_snap = snap_wire_target(ed, world);
    }
    const bool clicked = in.mouse_pressed[kMouseLeft] != 0;
    const SnapTarget* snap_ptr =
        !clicked && wire_snap.has_value() ? &*wire_snap : nullptr;

    handle_pointer(ed, gui, body, snap_ptr);

    // The frame's one drawing surface: the live renderer, or the
    // headless test's injected CommandListCanvas (the seam's comment
    // in editor.hpp). Everything below - the floor, the grid, the
    // feedback, the world - records through it.
    canvas::Canvas2D& rc = ed.schematic_draw_canvas();
    draw_grid_layer(ed, gui, body, theme, rc);

    // The snap feedback (SCREEN space, drawing only - the snap itself
    // is exactly what it always was): the wire tool's ring and the
    // free landing's accent dot, gated like the pointer's own snap.
    std::optional<Pos> ring_target;
    if (inside && gui.claims_open()) {
        const SnapFeedback fb =
            snap_feedback_target(ed, in, world, coarse, snap_ptr);
        draw_free_spot(ed, in, fb.free_spot, theme, rc);
        ring_target = fb.ring;
    }

    // The canvas: the camera transform with the y flip, then the
    // world. Composed exactly as EditorView::w2s answers it - viewport
    // center, zoom with the y flip, then -center in the scaled space -
    // so a world point's drawn pixel IS its w2s pixel (what the grid
    // marks, the snap feedback and the picks already assume). Dropping
    // the last translate once drew every element offset by center *
    // zoom from the grid the moment the camera left the origin -
    // test_canvas_panel.cpp pins the agreement.
    rc.set_view_scale(static_cast<float>(ed.view.zoom));
    rc.push_transform();
    rc.translate(body.x + body.width * 0.5f, body.y + body.height * 0.5f);
    rc.scale(static_cast<float>(ed.view.zoom),
             -static_cast<float>(ed.view.zoom));
    rc.translate(-static_cast<float>(ed.view.center.x),
                 -static_cast<float>(ed.view.center.y));

    draw_wires_layer(ed, s, theme, rc);
    draw_parts_layer(ed, s, theme, rc);
    draw_target_ring(ed, ring_target, theme, rc);
    if (inside) {
        draw_wire_preview(ed, world, snap_ptr, theme, rc);
    }
    if (inside && gui.claims_open()) {
        draw_ghost_layer(ed, world, coarse, theme, rc);
    }

    rc.pop_transform();
    // The camera's scale comes off with the transform: the Results
    // panel draws through the same canvas and sets 1.0 itself, but
    // the reset here keeps the renderer's default honest for whoever
    // draws next.
    rc.set_view_scale(1.0f);
}

}  // namespace editor
