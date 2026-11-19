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
// parts, the selection highlight, and the tool ghosts/preview - then
// the screen-space chrome: the wire-color mode toggle (top-right),
// the .tran transport (bottom center) and the voltage legend
// (bottom-right), the interactive canvas's own controls.
//
// The wires and the LED glow are the run's half of the drawing: the
// paint mode (Plain / Nets / Voltage - the toggle's three buttons)
// decides each wire's ink through Editor::net_cache's one build per
// edit, and an LED's halo follows its i(<element>) probe column at
// the transport playhead's sample (run_painting resolves the run's
// tables once per frame for both).
//
// The viewport is the scroll scope's content rect: the panel NEVER
// advances the flow (no layout.next()/advance() calls anywhere in this
// file), so the scroll range stays zero and the content rect the
// cursor sees is exactly the unscrolled viewport. The shell's wheel
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
#include <cstdio>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ws/input/input.hpp>
#include <ws/circsim/overlay.hpp>
#include <ws/ui/canvas/canvas2d.hpp>

namespace editor {
namespace {

using canvas::Paint;
using canvas::Point;

// The screen-space pick radius: hits are converted to world units by
// the zoom, so picks stay steady at every magnification.
constexpr float kPickPx = 9.0f;

// The snap feedback's reach: the accent dot shows while the cursor is
// within this many screen px of the grid intersection it would land
// on (and the dot's own px).
constexpr float kSnapFeedbackPx = 8.0f;
constexpr float kSnapDotPx = 5.0f;

// The panel's own shortcuts, as raw key_down[] indices (the same
// indexing the Space check below uses: 32 is Space, and the printable
// keys share ASCII/GLFW values). Edge-detected here, gated on the
// pointer being over the canvas and no text field holding focus. If a
// binding belongs in the chord loop instead, call
// Editor::zoom_to_selection() / Editor::wire_draft.flip() from there
// and delete these two lines.
constexpr int kKeyFrameSelection = 'F';  // frame the selection
constexpr int kKeyFlipCorner = '/';      // flip the wire draft's corner
constexpr int kKeyTidyWires = 'T';       // tidy the selection's wires
constexpr int kKeyAnnotate = 'A';        // Shift+A: renumber the refs

// Pin magnetism's reach for the Place tool, in grid cells.
constexpr double kMagnetCells = 1.5;

// --- the interactive canvas's paint helpers (the run paints back) ---------

// One net's wheel color: the series wheel indexed by an FNV-1a hash of
// the NAME (a labeled net keeps its color across rebuilds and renumbered
// ids; auto names shift with the topology, which is honest). Ground is
// the one fixed face - a slate that reads as structure, not signal.
[[nodiscard]] std::uint32_t net_color(const ws::circsim::Net& n) {
    if (n.is_ground) {
        return ws::ui::r2d::rgba(112, 122, 138);
    }
    std::uint32_t h = 2166136261u;  // FNV-1a over the name
    for (const char c : n.name) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }
    return series_color(h);
}

// The voltage ramp and the LED's brightness curve moved home in
// patch 0012: ws::circsim/overlay.hpp owns the math (the locked rule
// - no ui include in the library, no paint math stranded in the
// panel), so the legend below and the glow both call the headless,
// tested functions. The thin aliases keep the draw sites reading
// the same way they always did.
[[nodiscard]] std::uint32_t voltage_color(double v, double vmax) {
    return ws::circsim::overlay_voltage_color(v, vmax);
}

[[nodiscard]] double led_brightness(double amps) {
    return ws::circsim::overlay_led_brightness(amps);
}

// The LED's own hue from its `color` param (the Choice the symbol
// specs; anything unrecognized - a hand-edited file, a .csym library
// without the param - lights the classic red).
[[nodiscard]] std::uint32_t led_hue(
    const std::map<std::string, std::string>& params) {
    const auto it = params.find("color");
    const std::string& c = it != params.end() ? it->second : "";
    if (c == "yellow") {
        return ws::ui::r2d::rgba(255, 202, 44);
    }
    if (c == "green") {
        return ws::ui::r2d::rgba(84, 220, 92);
    }
    if (c == "blue") {
        return ws::ui::r2d::rgba(72, 148, 255);
    }
    if (c == "white") {
        return ws::ui::r2d::rgba(235, 238, 242);
    }
    if (c == "orange") {
        return ws::ui::r2d::rgba(255, 142, 36);
    }
    return ws::ui::r2d::rgba(255, 76, 60);  // red
}

// What the run paints back, resolved for THIS frame: the table and the
// sample the playhead sits on, every net's node voltage at that sample,
// every element's probe current (the LED glow reads i(Dn)). Built from
// the Results cache's own copy of the run's net build and back-map, so
// an ERC pass replacing the Problems cache never dims the canvas.
struct RunPainting {
    bool ok = false;       // a run with node voltages worth painting
    bool stale = false;    // the drawing moved on since the run
    const ws::spice::Table* table = nullptr;
    std::size_t sample = 0;
    std::unordered_map<std::uint32_t, double> volts;    // net id -> V
    std::unordered_map<std::uint32_t, double> amps;     // part id -> A
    double vmax = 1e-9;     // the ramp's scale (max |v|)
};

[[nodiscard]] RunPainting run_painting(const Editor& ed) {
    RunPainting rp;
    const ws::circsim::Schematic* s = ed.schem();
    if (!ed.results.has || s == nullptr ||
        ed.results.run.results.tables.empty()) {
        return rp;
    }
    // The sweep table the transport scrubs, else the run's first
    // table at its last row (an .op's single answer, a frequency
    // sweep's end).
    const ws::spice::Table* t = ed.canvas_sweep_table();
    if (t == nullptr) {
        t = &ed.results.run.results.tables.front();
    }
    rp.table = t;
    // The extraction itself is ws::circsim's (overlay.hpp - patch
    // 0012): the playhead's sample, every mapped net's voltage, every
    // probed element's current, the ramp's scale. Headless, tested,
    // and the one implementation every surface shares.
    // NOT const: std::move from a const object silently COPIES, which
    // duplicated both hash maps every frame.
    ws::circsim::OverlaySnapshot snap =
        ws::circsim::overlay_snapshot(*t, ed.results.map, ed.playback.t);
    rp.sample = snap.sample;
    rp.volts = std::move(snap.volts);
    rp.amps = std::move(snap.amps);
    rp.vmax = snap.vmax;
    rp.ok = snap.ok;
    rp.stale = ed.results.version != s->version();
    return rp;
}

// --- the screen-space overlays' geometry (the layout the header exports;
// draw + pointer + the headless suite share it) --------------------------

// The wire-color mode toggle: three square buttons, top-right.
constexpr float kModeButtonPx = 26.0f;
constexpr float kModeGapPx = 4.0f;

}  // namespace

ModeToggleLayout mode_toggle_layout(const gui::Rect& body) {
    ModeToggleLayout out;
    const float w = 3.0f * kModeButtonPx + 2.0f * kModeGapPx;
    const float x = body.x + body.width - w - 8.0f;
    const float y = body.y + 8.0f;
    for (std::size_t i = 0; i < 3; ++i) {
        out.buttons[i] = {x + static_cast<float>(i) *
                                  (kModeButtonPx + kModeGapPx),
                          y, kModeButtonPx, kModeButtonPx};
    }
    return out;
}

TransportStripLayout transport_strip_layout(const gui::Rect& body) {
    // The pill's width floor rose with the rate chip (patch 0012): the
    // play button, a scrubbable track, the pace chip and the readout
    // all need their room on a narrow canvas too.
    const float w = std::min(320.0f, std::max(220.0f, body.width * 0.5f));
    const float h = 30.0f;
    constexpr float kReadoutPx = 96.0f;  // the right end's reservation
    // ("V2 = 470mV" at the 16px body face runs ~80px; the old 62px
    // reservation let the readout's left edge run into the rate chip -
    // the overlap this layout pass closes.)
    constexpr float kRatePx = 40.0f;     // the pace chip ("1/4x")
    constexpr float kEdgePadPx = 10.0f;  // the strip's own side air
    TransportStripLayout out;
    // The strip hugs the panel's bottom edge (the hint strip sits at
    // -24px and the voltage legend at -44px right-aligned; the old
    // -34px hover rode ON TOP of the bottom row of the schematic -
    // gnd symbols sit there). Parked flush against the bottom edge,
    // it can only occlude the last few px of the sheet, which the
    // fit's margin (view.fit's world-units air) keeps clear anyway.
    out.strip = {body.x + (body.width - w) * 0.5f,
                 body.y + body.height - h - kEdgePadPx, w, h};
    out.button = {out.strip.x + 3.0f, out.strip.y + 3.0f, h - 6.0f,
                  h - 6.0f};
    out.rate = {out.strip.x + out.strip.width - kReadoutPx - kRatePx - 2.0f,
                out.strip.y + 3.0f, kRatePx, h - 6.0f};
    out.bar = {out.strip.x + h + 4.0f, out.strip.y + h * 0.5f - 3.0f,
               std::max(20.0f, out.rate.x - (out.strip.x + h + 4.0f) - 4.0f),
               6.0f};
    out.readout = {out.rate.x + out.rate.width + 2.0f, out.strip.y,
                   kReadoutPx - 4.0f, h};
    return out;
}

namespace {

// Scrubs the playhead to the bar position under `mouse_x` (the
// fraction math is ws::circsim's overlay_scrub - one rule, tested).
void scrub_to(Editor& ed, const ws::spice::Table& t, const gui::Rect& bar,
              float mouse_x) {
    if (bar.width <= 1.0f) {
        return;
    }
    const ws::circsim::OverlayWindow w = ws::circsim::overlay_window(t);
    if (!(w.t1 > w.t0)) {
        return;
    }
    const double frac = std::clamp(
        (mouse_x - bar.x) / static_cast<double>(bar.width), 0.0, 1.0);
    ed.playback.t = ws::circsim::overlay_scrub(w, frac);
}

// True when the point sits in the rect (the half-open screen rule).
[[nodiscard]] bool in_rect(const gui::Rect& r, float x, float y) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
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
    std::uint32_t wire = 0;             // the wire id for WirePoint
    // A WirePoint landing whose position carries an existing junction:
    // the dot the author clicked ON the wire - the commit re-binds it
    // (the T-tap through an existing dot).
    std::uint32_t tap_junction = 0;
};

[[nodiscard]] SnapTarget snap_wire_target(const Editor& ed, Pos world) {
    // Coincident pins resolve for the WIRE tool the way the magnet
    // mated them: when the pin hit at the cursor is the draft's own
    // start pin and ANOTHER part's pin sits at the same spot, the
    // second click names the OTHER pin - the wire the partition rule
    // says the touch is not. (hit_pin's strict < keeps the first
    // part's pin for both clicks otherwise, and the commit's
    // same-entity guard rightly refuses the self-loop.)
    std::optional<PinHit> pin = ed.hit_pin(world, pick_world(ed.view));
    if (pin.has_value() && ed.wire_draft.active) {
        const auto* start_pin =
            std::get_if<ws::circsim::PinRef>(&ed.wire_draft.a);
        if (start_pin != nullptr && *start_pin == pin->ref) {
            if (const std::optional<PinHit> other =
                    ed.hit_other_pin_at(world, pick_world(ed.view),
                                        pin->ref, pin->pos)) {
                pin = other;
            }
        }
    }
    if (pin.has_value()) {
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
        t.wire = wp->wire;
        // A junction already sitting at that wire point: the author
        // clicked the dot itself ON the wire - the click re-binds it
        // (the T-tap gesture through an existing dot). The commit
        // side adds the binding; nothing else changes.
        if (const std::uint32_t j = ed.hit_junction(
                wp->pos, pick_world(ed.view)).value_or(0)) {
            t.tap_junction = j;
        }
        return t;
    }
    SnapTarget t;
    t.pos = ed.snap(world);
    return t;
}

// Where a Place-tool click / ghost lands: the snapped cursor, nudged
// so a pin of the part mates with a pin within kMagnetCells. Shift (the
// coarse snap) is an explicit "put it exactly here" and skips the
// magnet. One formula for the ghost, the click and the snap dot.
[[nodiscard]] Pos place_pos(const Editor& ed, const ws::circsim::SymbolDef& sym,
                            Pos world, bool coarse) {
    const double radius = coarse ? 0.0 : kMagnetCells * ed.grid_spec().base;
    return ed.place_snap(sym, world, ed.ghost_rot, ed.ghost_mirror, coarse,
                         radius);
}

// --- drag helpers --------------------------------------------------------------

// Where the drag in flight would land: the snap of (cursor - grab
// offset). A junction drag stays on the fine grid; parts and labels
// follow the Shift modifier. (One formula for the live move, the
// release and the snap feedback - they used to carry a copy each.)
[[nodiscard]] Pos drag_target(const Editor& ed, Pos world, bool coarse) {
    const bool use_coarse =
        coarse && ed.drag.kind != Editor::Drag::Kind::Junction;
    return ed.snap(Pos{world.x - ed.drag.grab_offset.x,
                       world.y - ed.drag.grab_offset.y},
                   use_coarse);
}

// Writes the dragged entity's position straight into the schematic (no
// undo - the command lands at release). Returns true if the position
// actually changed (and a bump was done).
bool set_drag_pos(Editor& ed, Pos at) {
    ws::circsim::Schematic* s = ed.schem();
    if (s == nullptr) {
        return false;
    }
    switch (ed.drag.kind) {
        case Editor::Drag::Kind::Part:
            if (ws::circsim::Part* p = s->find_part(ed.drag.id)) {
                if (!eps_eq(p->pos.x, at.x, 1e-9) || !eps_eq(p->pos.y, at.y, 1e-9)) {
                    // A part move is geometry-only - nothing else
                    // rides. Wires END on the part's pins by reference
                    // and rubber-band through the endpoint resolution
                    // every frame; a junction sitting exactly on a pin
                    // is a POSITION COINCIDENCE the partition rule
                    // already refuses to union (nets.cpp warns
                    // entities_coincide), so the animation must not
                    // grab it either - dragging it (and every wire
                    // ending on it) along was the "wires still get
                    // grabbed" bug.
                    p->pos = at;
                    s->bump();
                    return true;
                }
            }
            break;
        case Editor::Drag::Kind::Junction:
            for (ws::circsim::Junction& j : s->junctions_mut()) {
                if (j.id == ed.drag.id) {
                    if (!eps_eq(j.pos.x, at.x, 1e-9) || !eps_eq(j.pos.y, at.y, 1e-9)) {
                        j.pos = at;
                        s->bump();
                        return true;
                    }
                    break;
                }
            }
            break;
        case Editor::Drag::Kind::Label:
            for (ws::circsim::Label& l : s->labels_mut()) {
                if (l.id == ed.drag.id) {
                    if (!eps_eq(l.pos.x, at.x, 1e-9) || !eps_eq(l.pos.y, at.y, 1e-9)) {
                        l.pos = at;
                        s->bump();
                        return true;
                    }
                    break;
                }
            }
            break;
        case Editor::Drag::Kind::None:
            break;
    }
    return false;
}

// The wire draft's start can disappear under it (undo/redo, a delete
// from another panel) or move (an undone move). A draft is only
// committable while its start still exists where it was drawn from.
[[nodiscard]] bool wire_draft_start_valid(const Editor& ed) {
    const Editor::WireDraft& d = ed.wire_draft;
    if (d.a_free) {
        return true;  // a free grid point has no anchor to lose
    }
    if (d.a_mint) {
        // A start on a wire's body: the SAME wire must still pass
        // through the point. hit_wire_point returns the wire id,
        // so we verify it matches the original wire.
        const std::optional<Editor::WirePointSnap> wp =
            ed.hit_wire_point(d.a_pos, 1e-9);
        return wp.has_value() && wp->wire == d.a_wire;
    }
    // A pin/junction start: resolve it through wire_path (a probe wire
    // with both ends on the start - empty when the endpoint is gone,
    // otherwise its position as the model sees it NOW).
    ws::circsim::Wire probe;
    probe.a = d.a;
    probe.b = d.a;
    const std::vector<Pos> path = ed.wire_path(probe);
    return path.size() >= 2u && eps_eq(path.front().x, d.a_pos.x, 1e-9) &&
           eps_eq(path.front().y, d.a_pos.y, 1e-9);
}

// --- one pointer frame -------------------------------------------------------

// A Selection's move-command spelling (the group drag's payload
// builder): wires carry no position, so they never reach the ask.
[[nodiscard]] editor::EntityMove::Kind m_kind_of(const Selection& m) {
    switch (m.kind) {
        case Selection::Kind::Junction:
            return editor::EntityMove::Kind::Junction;
        case Selection::Kind::Label:
            return editor::EntityMove::Kind::Label;
        case Selection::Kind::Part:
        case Selection::Kind::Wire:
        case Selection::Kind::None:
            break;
    }
    return editor::EntityMove::Kind::Part;
}

// The group drag's own writers: a member's position read at press
// (the starts' source) and written live (no undo - the command lands
// at release, the single drag's own contract).
[[nodiscard]] std::optional<Pos> member_pos(const Editor& ed,
                                            const Selection& m) {
    const ws::circsim::Schematic* s = ed.schem();
    if (s == nullptr) {
        return std::nullopt;
    }
    switch (m.kind) {
        case Selection::Kind::Part:
            if (const ws::circsim::Part* p = s->find_part(m.id)) {
                return p->pos;
            }
            break;
        case Selection::Kind::Junction:
            if (const ws::circsim::Junction* j =
                    s->find_junction(m.id)) {
                return j->pos;
            }
            break;
        case Selection::Kind::Label:
            if (const ws::circsim::Label* l = s->find_label(m.id)) {
                return l->pos;
            }
            break;
        case Selection::Kind::Wire:
        case Selection::Kind::None:
            break;
    }
    return std::nullopt;
}

bool set_member_pos(Editor& ed, const Selection& m, Pos at) {
    ws::circsim::Schematic* s = ed.schem();
    if (s == nullptr) {
        return false;
    }
    const auto land = [&](double cur_x, double cur_y,
                          auto&& write) {
        if (eps_eq(cur_x, at.x, 1e-9) && eps_eq(cur_y, at.y, 1e-9)) {
            return false;
        }
        write(at);
        s->bump();
        return true;
    };
    switch (m.kind) {
        case Selection::Kind::Part:
            if (ws::circsim::Part* p = s->find_part(m.id)) {
                // Geometry-only, the single move's rule: wires end on
                // pins by reference and rubber-band; a dot exactly on
                // a pin is a coincidence the partition rule refuses
                // (entities_coincide) - it never rides the drag.
                return land(p->pos.x, p->pos.y,
                            [&](Pos v) { p->pos = v; });
            }
            break;
        case Selection::Kind::Junction:
            for (ws::circsim::Junction& j : s->junctions_mut()) {
                if (j.id == m.id) {
                    return land(j.pos.x, j.pos.y,
                                [&](Pos v) { j.pos = v; });
                }
            }
            break;
        case Selection::Kind::Label:
            for (ws::circsim::Label& l : s->labels_mut()) {
                if (l.id == m.id) {
                    return land(l.pos.x, l.pos.y,
                                [&](Pos v) { l.pos = v; });
                }
            }
            break;
        case Selection::Kind::Wire:
        case Selection::Kind::None:
            break;
    }
    return false;
}

void handle_pointer(Editor& ed, gui::GuiContext& gui,
                     const gui::Rect& body) {
    // If the tool changed away from Wire, cancel any in-flight draft.
    // This catches tool switches via hotkeys, commands, or menu clicks.
    // (A draft whose start DIED is the commit's own check, not this
    // frame's: cancelling here would eat the next click's commit and,
    // with free starts, re-arm a fresh draft in the same press.)
    if (ed.tool != Tool::Wire && ed.wire_draft.active) {
        ed.wire_draft = Editor::WireDraft{};
    }

    const ws::input::InputFrame& in = gui.input();
    const bool inside =
        in.mouse_x >= body.x && in.mouse_x < body.x + body.width &&
        in.mouse_y >= body.y && in.mouse_y < body.y + body.height;
    const ws::math::Vec2 mouse{in.mouse_x, in.mouse_y};
    const Pos world = ed.view.s2w(mouse);
    const double pick = pick_world(ed.view);
    const bool space_down = in.key_down[32] != 0;
    // Shift = the coarse snap: placements, part drags and labels land
    // on the major lattice (base * 5, an integer multiple of the fine
    // grid - a coarse position is always a fine position too). Wires,
    // pins and junctions never read it: their snaps below always pass
    // fine, so nets.cpp's exact-grid joins keep their invariant.
    const bool coarse =
        in.key_down[ws::input::keys::kLeftShift] != 0 ||
        in.key_down[ws::input::keys::kRightShift] != 0;

    // The panel's keyboard conveniences (edge-detected; the memory rides
    // the Editor). Only while the pointer is over the canvas, the claims
    // are open and no text field has focus (Properties typing is safe).
    {
        const bool keys_ok =
            inside && gui.claims_open() && !ed.text_field_focused;
        const bool frame_down =
            keys_ok && in.key_down[kKeyFrameSelection] != 0;
        const bool flip_down = keys_ok && in.key_down[kKeyFlipCorner] != 0;
        const bool tidy_down = keys_ok && in.key_down[kKeyTidyWires] != 0;
        // Sheet-wide, so it asks for Shift (no accidental renumbering).
        const bool annotate_down =
            keys_ok && coarse && in.key_down[kKeyAnnotate] != 0;
        if (frame_down && !ed.key_frame_prev_) {
            (void)ed.zoom_to_selection();
        }
        if (flip_down && !ed.key_flip_prev_ && ed.wire_draft.active) {
            ed.wire_draft.flip();
        }
        if (tidy_down && !ed.key_tidy_prev_ && ed.tool == Tool::Select) {
            (void)ed.tidy_selection();
        }
        ed.key_frame_prev_ = frame_down;
        ed.key_flip_prev_ = flip_down;
        if (annotate_down && !ed.key_annotate_prev_ &&
            ed.tool == Tool::Select) {
            (void)ed.annotate();
        }
        ed.key_tidy_prev_ = tidy_down;
        ed.key_annotate_prev_ = annotate_down;
    }

    // Alt+Arrow nudges (C1): moves the selected part's annotation nudge
    // by one grid cell. The nudge is stored in Editor::annot_state_of
    // (editor-lifetime state, survives document switches like
    // wire_colors). Shift+Alt+Arrow nudges by 5 cells. Alt+R resets the
    // selected part's nudge to zero. Only active in Select tool with
    // claims open and no text field focused.
    {
        const bool keys_ok =
            inside && gui.claims_open() && !ed.text_field_focused;
        const bool alt_down =
            keys_ok &&
            (in.key_down[ws::input::keys::kLeftAlt] != 0 ||
             in.key_down[ws::input::keys::kRightAlt] != 0);
        if (alt_down) {
            const bool shift_down = coarse;  // Shift is already computed
            const double step = shift_down ? 5.0 : 1.0;
            // Up (0)
            if (in.key_pressed[ws::input::keys::kUp] &&
                !ed.key_nudge_prev_[0]) {
                if (ed.sel.kind == Selection::Kind::Part) {
                    auto& st = ed.annot_state_of(ed.sel.id);
                    st.nudge.y += step;
                    ed.bump_edit_epoch();
                }
            }
            ed.key_nudge_prev_[0] = in.key_down[ws::input::keys::kUp] != 0;
            // Down (1)
            if (in.key_pressed[ws::input::keys::kDown] &&
                !ed.key_nudge_prev_[1]) {
                if (ed.sel.kind == Selection::Kind::Part) {
                    auto& st = ed.annot_state_of(ed.sel.id);
                    st.nudge.y -= step;
                    ed.bump_edit_epoch();
                }
            }
            ed.key_nudge_prev_[1] = in.key_down[ws::input::keys::kDown] != 0;
            // Left (2)
            if (in.key_pressed[ws::input::keys::kLeft] &&
                !ed.key_nudge_prev_[2]) {
                if (ed.sel.kind == Selection::Kind::Part) {
                    auto& st = ed.annot_state_of(ed.sel.id);
                    st.nudge.x -= step;
                    ed.bump_edit_epoch();
                }
            }
            ed.key_nudge_prev_[2] = in.key_down[ws::input::keys::kLeft] != 0;
            // Right (3)
            if (in.key_pressed[ws::input::keys::kRight] &&
                !ed.key_nudge_prev_[3]) {
                if (ed.sel.kind == Selection::Kind::Part) {
                    auto& st = ed.annot_state_of(ed.sel.id);
                    st.nudge.x += step;
                    ed.bump_edit_epoch();
                }
            }
            ed.key_nudge_prev_[3] = in.key_down[ws::input::keys::kRight] != 0;
            // Reset (4) - Alt+Shift+R (the patch used Shift+R, but we
            // use Alt+R since R is already the rotate chord in Place
            // tool; Alt+R is free in Select).
            if (in.key_pressed['R'] && !ed.key_nudge_prev_[4]) {
                if (ed.sel.kind == Selection::Kind::Part) {
                    auto& st = ed.annot_state_of(ed.sel.id);
                    st.nudge = {0.0, 0.0};
                    ed.bump_edit_epoch();
                }
            }
            ed.key_nudge_prev_[4] = in.key_down['R'] != 0;
        } else {
            // Alt not held: clear the edge memory so next Alt press fires.
            ed.key_nudge_prev_[0] = false;
            ed.key_nudge_prev_[1] = false;
            ed.key_nudge_prev_[2] = false;
            ed.key_nudge_prev_[3] = false;
            ed.key_nudge_prev_[4] = false;
        }
    }

    // Wheel: zoom toward the cursor (the scroll panel's own claim is
    // the harmless no-op the header note explains). An open modal veil
    // freezes the canvas - the zoom with it (the header's own
    // contract; only the tools used to check the claims). One notch =
    // one kZoomStep; a trackpad's many small deltas ride the same
    // curve gently instead of jumping whole notches (the exponent
    // clamps against a wild scroll burst).
    constexpr double kZoomStep = 1.15;
    if (inside && gui.claims_open() && in.scroll_dy != 0.0f) {
        const float deltas =
            std::clamp(in.scroll_dy, -4.0f, 4.0f);
        ed.view.zoom_at(mouse, std::pow(kZoomStep, deltas));
    }

    // --- the screen-space overlays own the LEFT press before any tool:
    // the wire-color mode's three buttons (top-right) and the .tran
    // transport (bottom strip - a scrub in flight keeps the pointer
    // until release, the pan drag's own rule). Space+left stays a pan:
    // the modifier wins over every left-press claim below.
    if (gui.claims_open() && !space_down) {
        // A scrub in flight outranks everything but its own release.
        if (ed.transport_scrub) {
            const ws::spice::Table* tran = ed.canvas_sweep_table();
            if (tran == nullptr ||
                in.mouse_down[ws::input::mouse::kLeft] == 0) {
                ed.transport_scrub = false;
            } else {
                scrub_to(ed, *tran, transport_strip_layout(body).bar,
                         in.mouse_x);
                return;  // the scrub owns the pointer
            }
        }
        if (inside && in.mouse_pressed[ws::input::mouse::kLeft] != 0) {
            const ModeToggleLayout modes = mode_toggle_layout(body);
            for (std::size_t i = 0; i < 3; ++i) {
                if (in_rect(modes.buttons[i], in.mouse_x, in.mouse_y)) {
                    ed.wire_colors = static_cast<WireColorMode>(i);
                    return;  // the click never reaches the tools
                }
            }
            const ws::spice::Table* tran = ed.canvas_sweep_table();
            if (tran != nullptr) {
                const TransportStripLayout tl =
                    transport_strip_layout(body);
                if (in_rect(tl.button, in.mouse_x, in.mouse_y)) {
                    // Play from the START when the playhead sits at the
                    // end (a paused canvas shows the settled state); a
                    // resume keeps wherever the scrub parked it.
                    const std::span<const double> axis = tran->axis(0);
                    if (!ed.playback.playing && !axis.empty() &&
                        ed.playback.t >= axis.back()) {
                        ed.playback.t = axis.front();
                    }
                    ed.playback.playing = !ed.playback.playing;
                    return;
                }
                if (in_rect(tl.rate, in.mouse_x, in.mouse_y)) {
                    // One press walks the ladder (1/4 -> 1/2 -> 1 -> 2,
                    // wrapping): 2x halves the wall clock, 1/4x
                    // stretches it. The Results row rides the same
                    // field, so both chips always agree.
                    ed.playback.rate =
                        ws::circsim::overlay_rate_next(ed.playback.rate);
                    return;
                }
                if (in_rect(tl.strip, in.mouse_x, in.mouse_y)) {
                    ed.transport_scrub = true;
                    scrub_to(ed, *tran, tl.bar, in.mouse_x);
                    return;
                }
            }
        }
    }

    // Middle-drag pans; space+left-drag pans too (the gallery's rule).
    // Armed inside the panel; once armed the gesture keeps the pointer
    // until the buttons go up - a cursor that slips off the panel edge
    // mid-pan no longer drops the pan (the drags' own rule). An open
    // modal veil disarms.
    const bool pan_modifiers =
        in.mouse_down[ws::input::mouse::kMiddle] != 0 ||
        (space_down && in.mouse_down[ws::input::mouse::kLeft] != 0);
    if (pan_modifiers && gui.claims_open() && (ed.pan.armed || inside)) {
        if (!ed.pan.armed) {
            ed.pan.armed = true;
            ed.pan.from = mouse;
        }
        ed.view.pan_by({mouse.x - ed.pan.from.x, mouse.y - ed.pan.from.y});
        ed.pan.from = mouse;
        return;  // a pan frame owns the pointer
    }
    ed.pan.armed = false;

    // The box-select in flight: the drag owns the pointer until
    // release, the move-drag's own rule - no pan, no tool arms while
    // it runs. The release lands the set (Shift extends the current
    // set instead of replacing it).
    if (ed.box.active) {
        if (in.mouse_down[ws::input::mouse::kLeft] != 0) {
            ed.box.cur_world = world;
            return;
        }
        ed.box.active = false;
        if (ed.doc != nullptr) {
            ed.select_box(ed.box, coarse);
        }
        return;
    }

    // A group drag in flight: the same ownership rule as the single
    // drag. The pressed member's target rules the snap; the ONE delta
    // carries every member, so the set's relative geometry never
    // distorts (two parts half a cell apart stay half a cell apart).
    if (ed.group_drag.active) {
        if (in.mouse_down[ws::input::mouse::kLeft] != 0) {
            const Pos target = ed.snap(
                Pos{world.x - ed.group_drag.grab_offset.x,
                    world.y - ed.group_drag.grab_offset.y},
                coarse);
            const Pos& start =
                ed.group_drag.starts[ed.group_drag.pressed];
            if (!eps_eq(target.x, start.x, 1e-9) ||
                !eps_eq(target.y, start.y, 1e-9)) {
                ed.group_drag.moved = true;
            }
            if (ed.group_drag.moved) {
                const Pos delta{target.x - start.x, target.y - start.y};
                for (std::size_t i = 0; i < ed.group_drag.set.size();
                     ++i) {
                    const Pos at{
                        ed.group_drag.starts[i].x + delta.x,
                        ed.group_drag.starts[i].y + delta.y};
                    (void)set_member_pos(ed, ed.group_drag.set[i], at);
                }
            }
            return;
        }
        // Release: land ONE undo step over the whole set. The
        // single-drag release trick, widened: everyone visits the
        // landing first (the attached ortho wires re-route from the
        // resolved endpoints there), then goes back to the press
        // positions so the command captures the real "was".
        if (ed.group_drag.moved && ed.doc != nullptr) {
            const Pos target = ed.snap(
                Pos{world.x - ed.group_drag.grab_offset.x,
                    world.y - ed.group_drag.grab_offset.y},
                coarse);
            const Pos& start =
                ed.group_drag.starts[ed.group_drag.pressed];
            const Pos delta{target.x - start.x, target.y - start.y};
            // The T-tap spots read at the press geometry - the live
            // drag left the set at the landing, so the first hop below
            // brings everyone home before anything reads the pins
            // (at the landing the old paths are gone).
            for (std::size_t i = 0; i < ed.group_drag.set.size(); ++i) {
                (void)set_member_pos(ed, ed.group_drag.set[i],
                                     ed.group_drag.starts[i]);
            }
            std::vector<ws::circsim::PinnedSpot> pinned;
            for (const Selection& m : ed.group_drag.set) {
                if (m.kind != Selection::Kind::Part &&
                    m.kind != Selection::Kind::Junction) {
                    continue;
                }
                for (const ws::circsim::PinnedSpot& ps :
                     ed.wire_pin_spots(
                         m.kind == Selection::Kind::Part
                             ? Editor::Drag::Kind::Part
                             : Editor::Drag::Kind::Junction,
                         m.id)) {
                    // Deduplicate by (pos, wire) - different wires can share
                    // a position at a junction.
                    if (std::find_if(pinned.begin(), pinned.end(),
                                     [&](const ws::circsim::PinnedSpot& q) {
                                         return q.wire == ps.wire &&
                                                eps_eq(q.pos.x, ps.pos.x, 1e-9) &&
                                                eps_eq(q.pos.y, ps.pos.y, 1e-9);
                                     }) == pinned.end()) {
                        pinned.push_back(ps);
                    }
                }
            }
            // Live-move everyone to the landing (for the route read).
            for (std::size_t i = 0; i < ed.group_drag.set.size(); ++i) {
                const Pos at{
                    ed.group_drag.starts[i].x + delta.x,
                    ed.group_drag.starts[i].y + delta.y};
                (void)set_member_pos(ed, ed.group_drag.set[i], at);
            }
            // The routes, deduped by wire id (a wire bent by two
            // members would be captured twice with the same shape).
            // No contacts collected: a move never creates
            // connections (a pin left on a foreign wire's interior
            // dangles, ERC's pin_on_wire warns it).
            std::map<std::uint32_t, WireRoute> routes;
            for (const Selection& m : ed.group_drag.set) {
                if (m.kind != Selection::Kind::Part &&
                    m.kind != Selection::Kind::Junction) {
                    continue;  // wires and labels carry no attachment
                }
                const Editor::Drag::Kind kind =
                    m.kind == Selection::Kind::Part
                        ? Editor::Drag::Kind::Part
                        : Editor::Drag::Kind::Junction;
                for (WireRoute& r :
                     ed.attached_wire_routes(kind, m.id, pinned)) {
                    routes[r.wire] = std::move(r);
                }
            }
            // Back to the press positions (the command's "was").
            for (std::size_t i = 0; i < ed.group_drag.set.size(); ++i) {
                (void)set_member_pos(ed, ed.group_drag.set[i],
                                     ed.group_drag.starts[i]);
            }
            // No glue payload: a group move is geometry-only.
            // Wires end on member pins by reference (the routes above
            // keep them ortho); a dot sitting exactly on a member's pin
            // is a coincidence the partition rule refuses
            // (entities_coincide) - it and its wires stay behind.
            std::vector<editor::GluedJunctionMove> glue;
            // The payload, when anything actually moved.
            std::vector<editor::EntityMove> moves;
            bool any = false;
            for (std::size_t i = 0; i < ed.group_drag.set.size(); ++i) {
                const Pos to{ed.group_drag.starts[i].x + delta.x,
                              ed.group_drag.starts[i].y + delta.y};
                const bool moved =
                    !eps_eq(to.x, ed.group_drag.starts[i].x, 1e-9) ||
                    !eps_eq(to.y, ed.group_drag.starts[i].y, 1e-9);
                any = any || moved;
                editor::EntityMove mv;
                mv.id = ed.group_drag.set[i].id;
                mv.from = ed.group_drag.starts[i];
                mv.to = to;
                mv.kind = m_kind_of(ed.group_drag.set[i]);
                moves.push_back(mv);
            }
            if (any) {
                std::vector<WireRoute> route_list;
                route_list.reserve(routes.size());
                for (auto& [_, r] : routes) {
                    route_list.push_back(std::move(r));
                }
                editor::push_move_set(*ed.doc, std::move(moves),
                                      std::move(route_list), std::move(glue));
                // No contact junctions: the group move connects
                // nothing either (same rule as the single drag).
            } else {
                // Nothing moved: put everyone back at the landing the
                // live drag had left them at (a no-op command would
                // be an undo step that does nothing).
                for (std::size_t i = 0; i < ed.group_drag.set.size();
                     ++i) {
                    const Pos at{
                        ed.group_drag.starts[i].x + delta.x,
                        ed.group_drag.starts[i].y + delta.y};
                    (void)set_member_pos(ed, ed.group_drag.set[i], at);
                }
            }
        }
        ed.group_drag = Editor::GroupDrag{};
        return;
    }

    // A drag in flight outranks every tool arming.
    if (ed.drag.kind != Editor::Drag::Kind::None) {
        if (in.mouse_down[ws::input::mouse::kLeft] != 0) {
            const Pos at = drag_target(ed, world, coarse);
            if (!eps_eq(at.x, ed.drag.start_world.x, 1e-9) ||
                !eps_eq(at.y, ed.drag.start_world.y, 1e-9)) {
                ed.drag.moved = true;
            }
            if (ed.drag.moved) {
                // Live move: direct writes, no undo - the command lands
                // at release.
                set_drag_pos(ed, at);
            }
            return;
        }
        // Release: land the undo command when the drag actually moved.
        if (ed.drag.moved) {
            const Pos to = drag_target(ed, world, coarse);
            // The entity first visits the landing (the attached ortho
            // wires re-route from the resolved endpoints there), then
            // goes back to its press position so the command captures
            // THAT as its from - the live drag had left it at the
            // release position, and a from == to command is an undo
            // no-op (nothing would ever move back). Restore even when
            // doc is null so the entity is never left stranded.
            std::vector<WireRoute> routes;
            std::vector<editor::GluedJunctionMove> glue;
            if (ed.doc != nullptr) {
                if (ed.drag.kind == Editor::Drag::Kind::Part) {
                    // No glue: a single move is geometry-only (the
                    // group move's rule). Wires end on the part's pins
                    // by reference and the routes below keep them
                    // ortho; a dot exactly on a pin is a coincidence
                    // the partition rule refuses (entities_coincide).
                    // The T-tap spots read at the press geometry too
                    // The T-tap spots read at the press geometry too
                    // (at the landing the old paths are gone).
                    const std::vector<ws::circsim::PinnedSpot> pinned =
                        ed.wire_pin_spots(ed.drag.kind, ed.drag.id);
                    set_drag_pos(ed, to);
                    routes = ed.attached_wire_routes(ed.drag.kind,
                                                     ed.drag.id, pinned);
                    // A move never creates connections: a pin the
                    // landing leaves on another wire's interior stays
                    // a dangling touch (nets.cpp's pin_on_wire
                    // warning names it; wiring the pin is the
                    // author's gesture). The older flow minted a
                    // bound junction at each contact - the "silent
                    // pickup" - which connected nets the drawing
                    // never said to connect.
                } else {
                    // The T-tap spots read at the press geometry (the
                    // live drag left the entity at the landing; the
                    // hop home reads what the author drew).
                    set_drag_pos(ed, ed.drag.start_world);
                    const std::vector<ws::circsim::PinnedSpot> pinned =
                        ed.wire_pin_spots(ed.drag.kind, ed.drag.id);
                    set_drag_pos(ed, to);
                    routes = ed.attached_wire_routes(ed.drag.kind,
                                                     ed.drag.id, pinned);
                    // The binding changes the LANDING makes (the
                    // entity sits at `to`): a junction dragged onto a
                    // A junction move is geometry-only now (the
                    // partition rule): the dot's tap bindings ride
                    // the move unchanged - a landing on another
                    // wire's body stays a warned coincidence until
                    // the author binds it, and a dot dragged OFF its
                    // tapped wire keeps the binding (ERC's
                    // tap_detached names it) instead of silently
                    // splitting the net.

                }
            }
            set_drag_pos(ed, ed.drag.start_world);
            const bool landed =
                !eps_eq(to.x, ed.drag.start_world.x, 1e-9) ||
                !eps_eq(to.y, ed.drag.start_world.y, 1e-9);
            if (landed && ed.doc != nullptr) {  // dragged back to where it started: no edit
                if (ed.drag.kind == Editor::Drag::Kind::Part) {
                    push_move_part(*ed.doc, ed.drag.id, to,
                                   std::move(routes), std::move(glue));
                    // No contact junctions: a move never connects
                    // anything (see above) - a landing that touches
                    // a foreign wire leaves the pin dangling, warned
                    // by ERC's pin_on_wire until the author wires it.
                } else if (ed.drag.kind == Editor::Drag::Kind::Junction) {
                    push_move_junction(*ed.doc, ed.drag.id, to,
                                       std::move(routes), {}, {});
                } else if (ed.drag.kind == Editor::Drag::Kind::Label) {
                    push_move_label(*ed.doc, ed.drag.id, to);
                }
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
                if (hit.kind == Selection::Kind::None) {
                    // Empty space arms the box: a drag sweeps the
                    // rectangle, a plain click lands an empty box and
                    // CLEARS the set (the old press-to-clear rule,
                    // kept - just answered at release, the box's own
                    // grammar).
                    if (ed.doc != nullptr) {
                        ed.box = BoxSelect{};
                        ed.box.active = true;
                        ed.box.start_world = ed.snap(world);
                        ed.box.cur_world = ed.box.start_world;
                    }
                    break;
                }
                if (coarse) {
                    // Shift+click toggles the entity into the set -
                    // no drag starts from a toggle press (the set is
                    // the subject, not the pointer's cargo).
                    ed.toggle_selection(hit);
                    break;
                }
                // A press on a member of a wider set drags the SET
                // (the box's own payoff); a press outside it starts
                // fresh, the plain single click it always was.
                const std::vector<Selection> set = ed.selection_set();
                if (set.size() > 1 && ed.in_selection(hit)) {
                    Editor::GroupDrag gd;
                    gd.active = true;
                    bool pressed_found = false;
                    for (const Selection& m : set) {
                        const std::optional<Pos> at = member_pos(ed, m);
                        if (!at.has_value()) {
                            continue;
                        }
                        gd.set.push_back(m);
                        gd.starts.push_back(*at);
                        if (m.kind == hit.kind && m.id == hit.id) {
                            gd.pressed = gd.starts.size() - 1u;
                            gd.grab_offset =
                                Pos{world.x - at->x, world.y - at->y};
                            pressed_found = true;
                        }
                    }
                    if (pressed_found) {
                        ed.group_drag = std::move(gd);
                        break;
                    }
                }
                ed.set_selection(hit);
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
                    place_pos(ed, *sym, world, coarse));
                // Minted for the same reason as the wire commit's id:
                // the command layer preserves the id it is handed, and
                // the palette is the parts' only creation path (0 is
                // the "none" sentinel - every placed part used to
                // share it, so part picks and deletes answered the
                // oldest part).
                p.id = ed.schem()->alloc_id();
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
                    // Start on a connection point: a pin, a junction,
                    // another wire's body (stage 18b: the editor mints
                    // a junction on that wire at commit, the standard
                    // EDA T-tap from a bus) - or on FREE grid (stage
                    // 19f: the commit side always supported free
                    // landings; the start junction mints at commit too,
                    // so a cancel leaves nothing behind).
                    const SnapTarget t = snap_wire_target(ed, world);
                    ed.wire_draft = Editor::WireDraft{};
                    ed.wire_draft.active = true;
                    ed.wire_draft.a_pos = t.pos;
                    ed.wire_draft.a = t.end;  // unused when a_mint
                    ed.wire_draft.a_dir = t.dir;
                    ed.wire_draft.a_mint =
                        t.kind == SnapTarget::Kind::WirePoint ||
                        t.kind == SnapTarget::Kind::Free;
                    ed.wire_draft.a_free =
                        t.kind == SnapTarget::Kind::Free;
                    ed.wire_draft.a_wire = t.wire;
                    break;
                }
                // Commit: onto a pin/junction, a wire's body (a fresh
                // junction there - its id minted now, the same path the
                // free case takes), or free space (also a fresh
                // junction). The a side may also need a mint (a start on
                // a wire's body), deferred to here so a cancel leaves
                // nothing behind.
                const SnapTarget t = snap_wire_target(ed, world);
                // A click on the START ENTITY is not a commitment - a
                // wire back onto its own origin is a degenerate
                // self-loop (identical endpoints, an empty folded
                // route, and for a wire-point start two junctions
                // minted at one position). The same POSITION, however,
                // commits when it names a DIFFERENT entity: two parts'
                // pins mated at identical coordinates by the Place
                // tool's magnetism connect nothing under the partition
                // rule (entities_coincide warns and says to wire them),
                // and that wire - two coincident pins, no waypoints -
                // is exactly the author's gesture. The old guard
                // compared POSITIONS and so dropped the click; only the
                // entity is the loop. A free start keeps the position
                // rule (two free clicks at one grid point name no
                // second entity - nothing to wire).
                const auto same_entity = [&]() {
                    if (ed.wire_draft.a_free) {
                        return t.kind == SnapTarget::Kind::Free &&
                               eps_eq(t.pos.x, ed.wire_draft.a_pos.x,
                                      1e-9) &&
                               eps_eq(t.pos.y, ed.wire_draft.a_pos.y,
                                      1e-9);
                    }
                    if (ed.wire_draft.a_mint) {
                        // A start on a wire's body: the loop is the
                        // SAME wire-point (same wire; a free or pin
                        // landing at that position is a different
                        // entity - it commits).
                        return t.kind == SnapTarget::Kind::WirePoint &&
                               t.wire == ed.wire_draft.a_wire;
                    }
                    return t.kind != SnapTarget::Kind::Free &&
                           t.kind != SnapTarget::Kind::WirePoint &&
                           t.end == ed.wire_draft.a;
                };
                if (same_entity()) {
                    break;
                }
                // The start anchor may have died between the clicks
                // (an undo, a delete from another panel): the editor
                // never creates a dangling endpoint, so the draft is
                // cancelled with a word instead.
                if (!wire_draft_start_valid(ed)) {
                    ed.toast("the wire's start is gone - the draft is "
                             "cancelled",
                             gui::ToastSeverity::warning);
                    ed.wire_draft = Editor::WireDraft{};
                    break;
                }
                ws::circsim::Wire w;
                // The wire's id is minted HERE, in the same breath as
                // its junctions below: the command layer PRESERVES the
                // id it is handed (reinsert_wire -> raise_next_id only
                // raises the counter past it; the library's add_wire,
                // which mints, is not on this path). The old default
                // 0 - the "none" sentinel (ids start at 1) - landed on
                // every hand-drawn wire at once: find_wire/take_wire
                // answered the oldest one, Selection::is lit them all,
                // and the delete path's orphan scan skipped no wire,
                // so deleting any wire orphaned junctions its
                // survivors still referenced (their wires then stopped
                // drawing - the "vanishing wire").
                w.id = ed.schem()->alloc_id();
                std::vector<ws::circsim::Junction> junctions;
                if (ed.wire_draft.a_mint) {
                    ws::circsim::Junction j;
                    j.pos = ed.wire_draft.a_pos;
                    j.id = ed.schem()->alloc_id();
                    // A start on a wire's BODY: the dot taps that wire -
                    // the authored binding whose connection the net
                    // rule honors (a start on free space taps nothing;
                    // the wire being drawn ends on this dot by ref).
                    if (!ed.wire_draft.a_free &&
                        ed.wire_draft.a_wire != 0) {
                        j.taps.push_back(ed.wire_draft.a_wire);
                    }
                    junctions.push_back(j);
                    w.a = ws::circsim::JunctionRef{j.id};
                } else {
                    w.a = ed.wire_draft.a;
                }
                if (t.kind == SnapTarget::Kind::Free) {
                    ws::circsim::Junction j;
                    j.pos = t.pos;
                    j.id = ed.schem()->alloc_id();
                    junctions.push_back(j);
                    w.b = ws::circsim::JunctionRef{j.id};
                } else if (t.kind == SnapTarget::Kind::WirePoint) {
                    // The commit side's own T-tap: a landing on a
                    // wire's body. A dot already at that position is
                    // the author clicking the dot itself - re-bind it
                    // (the binding rides the same command as the
                    // wire; undo removes both); otherwise a fresh dot
                    // mints, bound the same way. The re-bind carries a
                    // SNAPSHOT of the junction (the command stores
                    // whole entities), so a junction that already
                    // taps other wires keeps them.
                    if (t.tap_junction != 0) {
                        const ws::circsim::Junction* found =
                            ed.schem()->find_junction(t.tap_junction);
                        if (found != nullptr) {
                            ws::circsim::Junction bound = *found;
                            if (std::find(bound.taps.begin(),
                                         bound.taps.end(),
                                         t.wire) == bound.taps.end()) {
                                bound.taps.push_back(t.wire);
                            }
                            junctions.push_back(bound);
                            w.b = ws::circsim::JunctionRef{bound.id};
                        } else {
                            // The dot died between the snap and the
                            // commit (an undo, another panel's delete):
                            // mint a fresh one, bound, the same path.
                            ws::circsim::Junction j;
                            j.pos = t.pos;
                            j.id = ed.schem()->alloc_id();
                            j.taps.push_back(t.wire);
                            junctions.push_back(j);
                            w.b = ws::circsim::JunctionRef{j.id};
                        }
                    } else {
                        ws::circsim::Junction j;
                        j.pos = t.pos;
                        j.id = ed.schem()->alloc_id();
                        j.taps.push_back(t.wire);
                        junctions.push_back(j);
                        w.b = ws::circsim::JunctionRef{j.id};
                    }
                } else {
                    w.b = t.end;
                }
                w.waypoints = Editor::route_wire(ed.wire_draft.a_pos,
                                                 ed.wire_draft.a_dir, t.pos,
                                                 t.dir,
                                                 ed.wire_draft.flip_route);
                w.ortho = true;
                const ws::circsim::WireEndpoint landing = w.b;
                push_place_wire(*ed.doc, std::move(w), std::move(junctions));
                // The tool stays armed and the next draft starts at the
                // landing: a multi-segment net is one run of clicks
                // (the old commit disarmed, so every segment cost a W
                // press and a re-pick). The landing endpoint exists now
                // - a minted junction included - so the chain binds to
                // it directly, no mint pending.
                ed.wire_draft = Editor::WireDraft{};
                ed.wire_draft.active = true;
                ed.wire_draft.a = landing;
                ed.wire_draft.a_pos = t.pos;
                ed.wire_draft.a_dir = t.dir;
                break;
            }
            case Tool::Label: {
                if (ed.doc == nullptr || ed.label_text[0] == '\0') {
                    break;
                }
                ws::circsim::Label l;
                // The label joins the mint: the command layer preserves
                // ids, and 0 is the "none" sentinel (same class as the
                // wire and part placements).
                l.id = ed.schem()->alloc_id();
                l.pos = ed.snap(world, coarse);
                l.name = ed.label_text;
                // The authored binding (the junction taps' rule): a
                // label placed ON a wire binds that wire by reference -
                // the connection the placement names, position
                // inherited from the click only for where it sits.
                // Moving the wire or the label later preserves the
                // binding; a label placed off every wire saves wire 0
                // and loads back on the legacy position rule.
                if (const std::optional<Editor::WirePointSnap> wp =
                        ed.hit_wire_point(l.pos, pick_world(ed.view))) {
                    l.wire = wp->wire;
                }
                push_place_label(*ed.doc, std::move(l));
                break;
            }
            case Tool::Probe: {
                // The probe stays armed (the wire tool's rule): a run
                // of clicks toggles a run of curves, Esc or a right
                // click puts the Select tool back.
                if (ed.doc == nullptr) {
                    break;
                }
                ed.status(ed.probe_at(world, pick));
                break;
            }
        }
        return;
    }

    // Right click: the staging - the in-flight box cancels first,
    // then the draft, the tool disarms, the selection clears last (a
    // stray click used to kill the draft AND the tool in one go; the
    // Esc command keeps the same ladder).
    if (in.mouse_pressed[ws::input::mouse::kRight] != 0) {
        if (ed.box.active) {
            ed.box = BoxSelect{};
        } else if (ed.wire_draft.active) {
            ed.wire_draft = Editor::WireDraft{};
        } else if (ed.tool != Tool::Select) {
            ed.tool = Tool::Select;
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
    // The fit's overlay insets: the mode toggle's band (top) and the
    // transport strip's (bottom) - the controls a fit must not park
    // content under. The strip only exists with a scrubbable run, but
    // the reservation is constant so a fit does not re-decide mid-run.
    ed.view.inset_top = 8.0f + 26.0f + 8.0f;   // toggle row + its pads
    ed.view.inset_bottom = 30.0f + 2.0f * 10.0f;  // strip + bottom air
    // Motion: the smooth camera and the locate pulse follow the theme's
    // animation switch (instant when motion is off, like every other
    // eased thing in this panel). tick() advances both by real time.
    ed.view.animate = theme.anim_enabled;
    (void)ed.tick();
    ws::circsim::Schematic& s = *ed.schem();
    s.view.zoom = ed.view.zoom;
    s.view.center = ed.view.center;

    handle_pointer(ed, gui, body);

    // The pointer facts the render halves share (screen-space inside,
    // world cursor, the Shift modifier the ghosts follow). These are
    // re-derived AFTER handle_pointer on purpose, not shared with its
    // opening reads: `inside` and `coarse` are functions of this
    // frame's input alone (identical either way), but `world` maps
    // through ed.view - and handle_pointer MOVES the view (the wheel
    // zooms toward the cursor, the pan drags it). A world cursor
    // captured before the interaction would lag one frame behind the
    // camera it belongs to, so the snap feedback and the ghosts would
    // draw against the pre-zoom geometry. The input itself never
    // changes mid-frame (gui.input() is the frame's own copy); the
    // second derivation is the one that speaks the CURRENT camera.
    const ws::input::InputFrame& in = gui.input();
    const bool inside =
        in.mouse_x >= body.x && in.mouse_x < body.x + body.width &&
        in.mouse_y >= body.y && in.mouse_y < body.y + body.height;
    const Pos world = ed.view.s2w({in.mouse_x, in.mouse_y});
    const bool coarse =
        in.key_down[ws::input::keys::kLeftShift] != 0 ||
        in.key_down[ws::input::keys::kRightShift] != 0;

    // --- the canvas floor + the dot grid (SCREEN space, pixel-snapped).
    // grid.hpp's system - ONE spec with the snap: the base spacing is
    // the schematic's own grid (Schematic::grid through
    // Editor::grid_spec), the faces the theme's tokens, the density
    // the x5 level ladder whose minors fade in and out so a level
    // change never moves or pops a mark. Every dot's centre is
    // snapped from its integer index, so panning at fractional
    // offsets is shimmer-free at any zoom. While a tool is armed or
    // a drag is in flight the lattice brightens (a ~150ms ease,
    // instant when the theme's motion is off); a view change alone
    // never triggers it (view changes are not edits - the camera
    // sync above rides the same rule).
    std::optional<Pos> ring_target;  // the wire tool's pin/junction hit

    // The frame's one drawing surface: the live renderer, or the
    // headless test's injected CommandListCanvas (the seam's comment
    // in editor.hpp). Everything below - the floor, the grid, the
    // feedback, the world - records through it.
    canvas::Canvas2D& rc = ed.schematic_draw_canvas();
    {
        canvas::GridSpec spec = ed.grid_spec();
        spec.minor_color = theme.grid_minor;
        spec.major_color = theme.grid_major;
        spec.axis_color = theme.grid_axis;
        spec.show_dots = true;
        spec.show_axis = ed.show_origin_axes;  // C4: origin crosshair

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

        const Point floor{body.x, body.y};
        rc.fill_rects(&floor, 1, body.width, body.height, theme.canvas_bg);
        canvas::draw_grid(rc, frame, spec, brightness);
    }

    // --- the snap feedback (SCREEN space, drawing only - the snap
    // itself is exactly what it always was). The wire tool's answer
    // rings pins and junctions (in world space, with the wires
    // below); a free landing spot dots the grid intersection the
    // cursor is within kSnapFeedbackPx of.
    if (inside && gui.claims_open()) {
        std::optional<Pos> free_spot;
        if (ed.tool == Tool::Wire || ed.wire_draft.active) {
            const SnapTarget t = snap_wire_target(ed, world);
            if (t.kind == SnapTarget::Kind::Free) {
                free_spot = t.pos;
            } else {
                ring_target = t.pos;
            }
        } else if (ed.drag.kind != Editor::Drag::Kind::None) {
            // The drag's own landing spot (a junction drag stays on
            // the fine grid; parts and labels follow Shift).
            free_spot = drag_target(ed, world, coarse);
        } else if (ed.tool == Tool::Place || ed.tool == Tool::Label) {
            free_spot = ed.snap(world, coarse);
            if (ed.tool == Tool::Place && !ed.place_symbol.empty()) {
                if (const ws::circsim::SymbolDef* sym =
                        ed.library.find(ed.place_symbol)) {
                    free_spot = place_pos(ed, *sym, world, coarse);
                }
            }
        }
        if (free_spot.has_value()) {
            const ws::math::Vec2 sp = ed.view.w2s(*free_spot);
            const float dx = sp.x - in.mouse_x;
            const float dy = sp.y - in.mouse_y;
            if (dx * dx + dy * dy <=
                kSnapFeedbackPx * kSnapFeedbackPx) {
                const Point dot{canvas::snap_mark(sp.x, kSnapDotPx),
                                canvas::snap_mark(sp.y, kSnapDotPx)};
                rc.fill_rects(&dot, 1, kSnapDotPx, kSnapDotPx,
                              theme.control_on);
            }
        }
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

    // --- wires, junctions, labels. The interactive canvas's first half:
    // the paint mode decides the ink. Plain keeps the classic single
    // stroke; Nets and Voltage resolve each wire through net_cache (one
    // build per edit, the auto-junction dots' own seam) - Nets wears the
    // name-hashed wheel, Voltage the last run's node-voltage ramp. A
    // wire nobody can resolve (an edit the run never saw, a net the
    // build dropped) falls back to the plain ink, so a stale run never
    // erases the drawing. The selection keeps the accent either way.
    const RunPainting rp = run_painting(ed);
    const ws::circsim::NetList* wire_nets =
        ed.wire_colors == WireColorMode::Plain ? nullptr : &ed.net_cache();

    Paint wire_paint;
    wire_paint.width = 2.0f;
    wire_paint.color = gui::dim(theme.text, 0.85f);
    Paint wire_sel = wire_paint;
    wire_sel.color = theme.control_on;
    wire_sel.width = 3.0f;

    // --- the world-space view cull. The camera is center/zoom over the
    // body rect, so the visible world rect is exact; a margin (cells)
    // keeps big symbols, their text and a wire's stroke from popping at
    // the edge. Anything fully outside skips tessellation, net lookup
    // and text layout - the cost that scaled with the WHOLE drawing
    // instead of what is on screen. A selected part is never culled.
    constexpr double kCullMarginCells = kViewCullMarginCells;
    const bool cull_on = ed.view.zoom > 1e-9;
    const double cull_hw =
        cull_on ? 0.5 * static_cast<double>(body.width) / ed.view.zoom : 0.0;
    const double cull_hh =
        cull_on ? 0.5 * static_cast<double>(body.height) / ed.view.zoom : 0.0;
    const double cull_x0 = ed.view.center.x - cull_hw - kCullMarginCells;
    const double cull_x1 = ed.view.center.x + cull_hw + kCullMarginCells;
    const double cull_y0 = ed.view.center.y - cull_hh - kCullMarginCells;
    const double cull_y1 = ed.view.center.y + cull_hh + kCullMarginCells;
    const auto point_visible = [&](double x, double y) {
        return !cull_on || (x >= cull_x0 && x <= cull_x1 && y >= cull_y0 &&
                            y <= cull_y1);
    };
    const SelectionIndex sel_index = ed.selection_index();

    // A net id -> its Net (the join dots carry ids; NetList indexes
    // names/pins/wires, so this is the one walk over the public list).
    const auto net_of_id = [wire_nets](std::uint32_t id) {
        const ws::circsim::Net* out = nullptr;
        if (wire_nets != nullptr) {
            for (const ws::circsim::Net& n : wire_nets->nets) {
                if (n.id == id) {
                    out = &n;
                    break;
                }
            }
        }
        return out;
    };
    // One entity's ink: the plain color until a net resolves, then the
    // mode's answer for it (Voltage falls back to plain for unknown
    // nets - a missing reading is not a zero reading).
    const auto entity_color = [&](const ws::circsim::Net* n) {
        if (n == nullptr || wire_nets == nullptr) {
            return wire_paint.color;
        }
        if (ed.wire_colors == WireColorMode::Voltage) {
            const auto it = rp.volts.find(n->id);
            if (it == rp.volts.end()) {
                return wire_paint.color;
            }
            return voltage_color(it->second, rp.vmax);
        }
        return net_color(*n);
    };

    // The live-drag rubber band: while a drag (single or group) is
    // moving an entity, an ortho wire ending on it resolves its
    // endpoints live but its WAYPOINTS are the author's old ones -
    // the raw path can run DIAGONAL legs (a pin dragged up-left of a
    // junction draws a slash every frame; the V-shaped tangles the
    // screenshot shows), and even tidied it still draws the STALE
    // corridor (a hold frame showed wires running down the old route
    // with dangling ends where the part used to be). The DRAW answers
    // with the PREVIEW of the landing itself (live_wire_path: the
    // same fresh-route / slid-waypoint family attached_wire_routes
    // lands at release, computed against the live geometry) so what
    // you see while holding the button is exactly what the release
    // will land. Freeform wires never ride this (their shape is the
    // author's); the document is untouched mid-drag - the waypoints
    // still land via the release's command.
    const auto wire_live_end = [&](const ws::circsim::WireEndpoint& e) {
        if (ed.drag.kind == Editor::Drag::Kind::Part) {
            const auto* pin = std::get_if<ws::circsim::PinRef>(&e);
            return pin != nullptr && pin->part == ed.drag.id;
        }
        if (ed.drag.kind == Editor::Drag::Kind::Junction) {
            const auto* j = std::get_if<ws::circsim::JunctionRef>(&e);
            return j != nullptr && j->junction == ed.drag.id;
        }
        if (ed.group_drag.active) {
            for (const Selection& m : ed.group_drag.set) {
                if (m.kind == Selection::Kind::Part) {
                    const auto* pin =
                        std::get_if<ws::circsim::PinRef>(&e);
                    if (pin != nullptr && pin->part == m.id) {
                        return true;
                    }
                } else if (m.kind == Selection::Kind::Junction) {
                    const auto* j =
                        std::get_if<ws::circsim::JunctionRef>(&e);
                    if (j != nullptr && j->junction == m.id) {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    const auto drag_attached = [&](const ws::circsim::Wire& w) {
        return w.ortho && (wire_live_end(w.a) || wire_live_end(w.b));
    };
    const bool drag_live =
        (ed.drag.kind != Editor::Drag::Kind::None && ed.drag.moved) ||
        (ed.group_drag.active && ed.group_drag.moved);
    std::vector<Point> wire_pts;  // one buffer for every wire of the frame
    for (const ws::circsim::Wire& w : s.wires()) {
        const WireGeom& geom = ed.wire_geom(w);
        if (geom.path.size() < 2u) continue;
        const std::vector<Pos>* draw_path = &geom.path;
        std::vector<Pos> previewed;  // per-wire preview landing route
        if (drag_live && drag_attached(w)) {
            previewed = ws::circsim::live_wire_path(
                s, ed.library, w, wire_live_end(w.a), wire_live_end(w.b));
            if (previewed.size() >= 2u) {
                draw_path = &previewed;
            }
        }
        if (cull_on && (geom.max_x < cull_x0 || geom.min_x > cull_x1 ||
                        geom.max_y < cull_y0 || geom.min_y > cull_y1)) {
            continue;
        }
        wire_pts.clear();
        wire_pts.reserve(draw_path->size());
        for (const Pos& p : *draw_path) {
            wire_pts.push_back(Point{static_cast<float>(p.x),
                                     static_cast<float>(p.y)});
        }
        const bool selected =
            sel_index.contains({Selection::Kind::Wire, w.id});
        Paint paint = selected ? wire_sel : wire_paint;
        if (!selected) {
            paint.color = entity_color(wire_nets != nullptr
                                           ? wire_nets->net_of_wire(w.id)
                                           : nullptr);
        }
        rc.stroke_polyline(
            wire_pts.data(), static_cast<int>(wire_pts.size()), paint);
    }
    for (const ws::circsim::Junction& j : s.junctions()) {
        if (!point_visible(j.pos.x, j.pos.y)) {
            continue;
        }
        rc.fill_circle(
            Point{static_cast<float>(j.pos.x), static_cast<float>(j.pos.y)},
            0.14f,
            sel_index.contains({Selection::Kind::Junction, j.id})
                ? theme.control_on
                : entity_color(wire_nets != nullptr
                                   ? wire_nets->net_of_junction(j.id)
                                   : nullptr));
    }
    // The silent joins' dots (stage 18): pins a wire's path picked up,
    // vertices T-riding another wire - the net rules connect them, so
    // the canvas says so, the same dot the authored junctions wear.
    // nets.cpp already suppressed every position an authored junction
    // or a wire landing covers, so what arrives here is exactly the
    // invisible set. Skipped while a drag is moving something: the
    // live writes bump the schematic version EVERY frame, and the dots'
    // cache is keyed on it, so drawing them here would run a full
    // build_nets per frame. They come back at release (the one rebuild
    // per edit).
    const bool drag_moving =
        ed.drag.kind != Editor::Drag::Kind::None && ed.drag.moved;
    if (!drag_moving) {
        for (const auto& [dot_pos, dot_net] : ed.auto_junction_dots()) {
            if (!point_visible(dot_pos.x, dot_pos.y)) {
                continue;
            }
            rc.fill_circle(
                Point{static_cast<float>(dot_pos.x),
                      static_cast<float>(dot_pos.y)},
                0.14f, entity_color(net_of_id(dot_net)));
        }
    }
    // C1-C4: Annotation placement engine - runs every frame (rides the
    // view's culling). Places part annotations (refdes + value/model) and
    // net labels (net name at centroid) avoiding collisions with part
    // bodies, wires, junctions and view edges. The engine returns placed
    // TextTiles with screen rects and world anchors; the panel draws them
    // verbatim. Per-part nudges from Editor::annot_state_of are applied
    // by the engine.
    const Editor::AnnotPlacement empty_placement{};
    const Editor::AnnotPlacement& placement =
        theme.font != nullptr && ed.show_part_values
            ? ed.place_part_annotation()
            : empty_placement;
    std::unordered_map<std::string, const Editor::TextTile*> part_tiles_by_refdes;
    part_tiles_by_refdes.reserve(placement.part_tiles.size());
    for (const auto& tile : placement.part_tiles) {
        part_tiles_by_refdes.emplace(tile.refdes, &tile);
    }

    if (theme.font != nullptr) {
        // Net labels: draw from the placement engine's net_tiles (C1).
        // The engine places the net name at the net's centroid, avoiding
        // collisions. We match by name since the engine returns the net
        // name in the tile's refdes field.
        for (const auto& tile : placement.net_tiles) {
            // The tile's screen_rect is the text bounds; draw at its top-left
            (void)rc.draw_text(tile.screen_rect.x, tile.screen_rect.y,
                               tile.refdes.c_str(), *theme.font,
                               gui::dim(theme.text, 0.9f));
        }
        // Authored labels (the Label tool's labels): keep drawing at their
        // explicit positions (they are not moved by the engine).
        for (const ws::circsim::Label& l : s.labels()) {
            if (!point_visible(l.pos.x, l.pos.y)) {
                continue;
            }
            const Point at{static_cast<float>(l.pos.x),
                           static_cast<float>(l.pos.y)};
            rc.stroke_line(at, Point{at.x + 0.35f, at.y + 0.35f},
                           wire_paint);
            (void)rc.draw_text(at.x + 0.45f, at.y - 0.1f, l.name.c_str(),
                               *theme.font,
                               sel_index.contains({Selection::Kind::Label, l.id})
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
        const bool selected =
            sel_index.contains({Selection::Kind::Part, p.id});
        if (!selected && !point_visible(p.pos.x, p.pos.y)) {
            continue;  // off screen: no glow ease, no symbol, no text
        }
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
        // The LED glow (the interactive canvas's payoff): the run's
        // i(<element>) probe maps to a brightness, and the halo discs
        // carry it UNDER the strokes - alpha falling with radius, the
        // color param's own hue, a bright die at the center. The
        // intensity eases toward the run's answer (a light turning on
        // reads as a light, not a flip); instant when the theme's
        // motion is off. No run, no probe column: the target is dark.
        if (sym != nullptr && sym->key == "led") {
            double target = 0.0;
            const auto lit = rp.amps.find(p.id);
            if (lit != rp.amps.end()) {
                target = led_brightness(lit->second);
            }
            char glow_id[32];
            std::snprintf(glow_id, sizeof glow_id, "led glow %u", p.id);
            const float glow = gui.animate_toward(
                gui.ids().internal_id(glow_id),
                static_cast<float>(target),
                theme.anim_enabled ? 0.12f : 0.0f);
            if (glow > 0.015f) {
                const std::uint32_t hue = led_hue(p.params);
                const Point center{static_cast<float>(p.pos.x),
                                   static_cast<float>(p.pos.y)};
                rc.fill_circle(center, 1.15f, gui::dim(hue, 0.50f * glow));
                rc.fill_circle(center, 1.90f, gui::dim(hue, 0.28f * glow));
                rc.fill_circle(center, 2.75f, gui::dim(hue, 0.13f * glow));
                rc.fill_circle(center, 0.22f, gui::dim(hue, glow));
            }
        }
        // The switch blade (the LED glow's mechanical sibling): the
        // run's answer at the playhead - the S/W device's own
        // hysteresis walk over the control column (overlay.hpp) - eases
        // the blade from its open rest onto the mate contact. A switch
        // reads as a switch the same ~120 ms; instant when the theme's
        // motion is off. No run, no control column, no model card: the
        // target is open - the static picture every other surface
        // (the palette, the ghost, the PNG export) draws too.
        double blade_closed = 0.0;
        if (sym != nullptr && sym->blade.has_value()) {
            double target = 0.0;
            if (rp.table != nullptr) {
                const ws::circsim::OverlaySwitch sw = ws::circsim::overlay_switch(
                    *rp.table, ed.results.map, ed.results.nets, s, p, *sym,
                    ed.playback.t);
                if (sw.known && sw.closed) {
                    target = 1.0;
                }
            }
            char blade_id[32];
            std::snprintf(blade_id, sizeof blade_id,
                          "switch blade %u", p.id);
            blade_closed = static_cast<double>(gui.animate_toward(
                gui.ids().internal_id(blade_id),
                static_cast<float>(target),
                theme.anim_enabled ? 0.12f : 0.0f));
        }
        if (sym != nullptr) {
            draw_symbol(rc, *sym, p.pos, p.rot, p.mirror, body_paint,
                        theme.text, theme.font, blade_closed);
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
        // The refdes + value: draw from the placement engine's part_tiles (C1).
        // The engine places the annotation avoiding collisions; we match
        // by refdes (which is unique per part in the schematic).
        if (theme.font != nullptr && ed.show_part_values) {
            // The per-frame index avoids scanning every annotation for
            // every visible part (quadratic matching on large sheets).
            const auto found_tile = part_tiles_by_refdes.find(p.ref);
            const Editor::TextTile* placed =
                found_tile != part_tiles_by_refdes.end()
                    ? found_tile->second
                    : nullptr;
            if (placed != nullptr) {
                // Draw refdes and value from the placed tile.
                // The tile's screen_rect is the combined text bounds
                // (refdes + " " + value). We need to split them.
                // The engine places refdes left, value right with one
                // space between. Draw refdes at screen_rect.x, value at
                // screen_rect.x + refdes_width + space_width.
                if (!placed->refdes.empty()) {
                    (void)rc.draw_text(placed->screen_rect.x,
                                       placed->screen_rect.y,
                                       placed->refdes.c_str(),
                                       *theme.font,
                                       gui::dim(theme.text, 0.75f));
                }
                if (!placed->value.empty()) {
                    const float ref_w = rc.measure_text(placed->refdes.c_str(),
                                                        *theme.font);
                    const float space_w = rc.measure_text(" ", *theme.font);
                    (void)rc.draw_text(placed->screen_rect.x + ref_w + space_w,
                                       placed->screen_rect.y,
                                       placed->value.c_str(),
                                       *theme.font,
                                       gui::dim(theme.text, 0.75f));
                }
            }
        }
    }

    // --- the pin/junction target highlight (the wire tool's cursor):
    // a ring on whatever snap_wire_target answered - pins and
    // junctions alike, the same target the preview routes to.
    if (ring_target.has_value()) {
        Paint ring;
        ring.width = 2.0f;
        ring.color = theme.control_on;
        rc.stroke_circle(Point{static_cast<float>(ring_target->x),
                               static_cast<float>(ring_target->y)},
                         0.35f, ring);
    }

    // --- the wire draft preview
    if (ed.wire_draft.active && inside) {
        const SnapTarget t = snap_wire_target(ed, world);
        if (!eps_eq(t.pos.x, ed.wire_draft.a_pos.x, 1e-9) ||
            !eps_eq(t.pos.y, ed.wire_draft.a_pos.y, 1e-9)) {
            const std::vector<Pos> route = Editor::route_wire(
                ed.wire_draft.a_pos, ed.wire_draft.a_dir, t.pos, t.dir,
                ed.wire_draft.flip_route);
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

            // The length readout: the run's routed length in grid
            // cells, riding beside the cursor target - no more
            // counting dots to see how far a wire reaches.
            if (theme.font != nullptr) {
                double length = 0.0;
                for (std::size_t i = 1; i < pts.size(); ++i) {
                    length += std::hypot(
                        static_cast<double>(pts[i].x - pts[i - 1].x),
                        static_cast<double>(pts[i].y - pts[i - 1].y));
                }
                const double cells = length / ed.grid_spec().base;
                const long whole = std::lround(cells);
                char label[32];
                if (std::fabs(cells - static_cast<double>(whole)) < 1e-6) {
                    std::snprintf(label, sizeof label, "%ld cell%s", whole,
                                  whole == 1 ? "" : "s");
                } else {
                    std::snprintf(label, sizeof label, "%.1f cells", cells);
                }
                (void)rc.draw_text(static_cast<float>(t.pos.x + 0.45),
                                   static_cast<float>(t.pos.y + 0.35), label,
                                   *theme.font, gui::dim(theme.text, 0.8f));
            }
        }
    }

    // --- the box-select rectangle: the sweep's own veil, over the
    // entities, under the chrome (the plot's band zoom wears the same
    // face - the two rubber bands in the app read as cousins).
    if (ed.box.active) {
        const auto [lo, hi] = ed.box.rect();
        const Point a = ed.view.w2s(lo);
        const Point c = ed.view.w2s(hi);
        const Point quad[4] = {{a.x, a.y},
                               {c.x, a.y},
                               {c.x, c.y},
                               {a.x, c.y}};
        const std::uint32_t accent = theme.control_on;
        rc.fill_polygon(quad, 4, gui::dim(accent, 36));
        Paint band;
        band.width = 1.0f;
        band.color = gui::dim(accent, 200);
        rc.stroke_polyline(quad, 4, band, true);
    }

    // --- the placement / label ghosts
    if (inside && gui.claims_open()) {
        if (ed.tool == Tool::Place && !ed.place_symbol.empty()) {
            if (const ws::circsim::SymbolDef* sym =
                    ed.library.find(ed.place_symbol)) {
                // Draw the preview from the same transformed Part used
                // on placement. The outline and pin markers make the
                // symbol's footprint and connection points legible
                // before committing it, including after rotate/mirror.
                ws::circsim::Part ghost = ws::circsim::Part::make(
                    sym->key, "", "", place_pos(ed, *sym, world, coarse));
                ghost.rot = ed.ghost_rot;
                ghost.mirror = ed.ghost_mirror;

                draw_symbol(rc, *sym, ghost.pos, ghost.rot, ghost.mirror,
                            ghost_paint, gui::dim(theme.text, 0.45f),
                            theme.font);

                const Bounds local = ws::circsim::symbol_bounds(*sym);
                const Point corners[4] = {
                    {static_cast<float>(ws::circsim::transform_pos(
                         ghost, Pos{local.min_x, local.min_y}).x),
                     static_cast<float>(ws::circsim::transform_pos(
                         ghost, Pos{local.min_x, local.min_y}).y)},
                    {static_cast<float>(ws::circsim::transform_pos(
                         ghost, Pos{local.max_x, local.min_y}).x),
                     static_cast<float>(ws::circsim::transform_pos(
                         ghost, Pos{local.max_x, local.min_y}).y)},
                    {static_cast<float>(ws::circsim::transform_pos(
                         ghost, Pos{local.max_x, local.max_y}).x),
                     static_cast<float>(ws::circsim::transform_pos(
                         ghost, Pos{local.max_x, local.max_y}).y)},
                    {static_cast<float>(ws::circsim::transform_pos(
                         ghost, Pos{local.min_x, local.max_y}).x),
                     static_cast<float>(ws::circsim::transform_pos(
                         ghost, Pos{local.min_x, local.max_y}).y)}};
                Paint footprint = ghost_paint;
                footprint.width = 1.0f;
                footprint.dash.count = 2;
                footprint.dash.lengths[0] = 0.22f;
                footprint.dash.lengths[1] = 0.18f;
                rc.stroke_polyline(corners, 4, footprint, true);

                for (std::size_t i = 0; i < sym->pins.size(); ++i) {
                    const Pos pin = ws::circsim::pin_pos(*sym, ghost,
                                                        static_cast<std::uint32_t>(i));
                    rc.fill_circle(Point{static_cast<float>(pin.x),
                                         static_cast<float>(pin.y)},
                                   0.12f, theme.control_on);
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

    // --- the locate pulse (world space): a ring that blooms and fades
    // where jump_to / "frame selected" sent the camera. Its radius is
    // screen px over the zoom, so it reads the same at every
    // magnification. Skipped when the theme's motion is off.
    if (ed.pulse.active && theme.anim_enabled) {
        const double t = std::min(1.0, ed.pulse.age / Editor::Pulse::kLife);
        const double ease = 1.0 - (1.0 - t) * (1.0 - t);
        Paint ring;
        ring.width = static_cast<float>(3.0 - 1.5 * t);
        ring.color = gui::dim(theme.control_on, static_cast<float>(1.0 - t));
        rc.stroke_circle(
            Point{static_cast<float>(ed.pulse.pos.x),
                  static_cast<float>(ed.pulse.pos.y)},
            static_cast<float>((12.0 + 56.0 * ease) / ed.view.zoom), ring);
    }

    rc.pop_transform();

    // --- the screen-space overlays: the mode toggle (top-right), the
    // .tran transport (bottom center) and the voltage legend
    // (bottom-right). The hint strip keeps the bottom-left corner.
    rc.set_view_scale(1.0f);

    // The wire-color mode toggle: three glyph buttons - the plain ink
    // stroke, the net wheel, the voltage ramp. The active one wears the
    // accent's fill and border; the rest stay quiet outlines.
    {
        const ModeToggleLayout modes = mode_toggle_layout(body);
        for (std::size_t i = 0; i < 3; ++i) {
            const gui::Rect& r = modes.buttons[i];
            const bool active =
                ed.wire_colors == static_cast<WireColorMode>(i);
            if (active) {
                rc.fill_rounded_rect(r.x, r.y, r.width, r.height, 5.0f,
                                     gui::dim(theme.control_on, 0.18f));
            }
            Paint box;
            box.width = active ? 1.5f : 1.0f;
            box.color = active ? theme.control_on
                               : gui::dim(theme.text, 0.40f);
            rc.stroke_rounded_rect(r.x, r.y, r.width, r.height, 5.0f, box);
            const std::uint32_t glyph =
                active ? theme.control_on : gui::dim(theme.text, 0.75f);
            switch (static_cast<WireColorMode>(i)) {
                case WireColorMode::Plain: {
                    Paint line;
                    line.width = 2.0f;
                    line.color = glyph;
                    rc.stroke_line(Point{r.x + 6.0f, r.y + r.height * 0.5f},
                                   Point{r.x + r.width - 6.0f,
                                         r.y + r.height * 0.5f},
                                   line);
                    break;
                }
                case WireColorMode::Nets: {
                    Paint line;
                    line.width = 2.0f;
                    for (int k = 0; k < 3; ++k) {
                        line.color = gui::dim(series_color(k),
                                              active ? 1.0f : 0.60f);
                        rc.stroke_line(
                            Point{r.x + 6.0f, r.y + 7.0f + 4.0f * k},
                            Point{r.x + r.width - 6.0f,
                                  r.y + 7.0f + 4.0f * k},
                            line);
                    }
                    break;
                }
                case WireColorMode::Voltage: {
                    // The mini ramp: cold, base, hot.
                    const std::uint32_t cols[3] = {
                        voltage_color(-1.0, 1.0), voltage_color(0.0, 1.0),
                        voltage_color(1.0, 1.0)};
                    const float bw = (r.width - 12.0f) / 3.0f;
                    for (int k = 0; k < 3; ++k) {
                        const Point corner{r.x + 6.0f + bw * k,
                                           r.y + r.height * 0.5f - 3.0f};
                        rc.fill_rects(&corner, 1, bw - 1.0f, 6.0f,
                                      gui::dim(cols[k],
                                               active ? 1.0f : 0.60f));
                    }
                    break;
                }
            }
        }
    }

    // C2: Wire color caption - a one-line label under the toggle
    // explaining the current mode. Rides the theme's secondary text.
    if (theme.font != nullptr) {
        const ModeToggleLayout modes = mode_toggle_layout(body);
        const gui::Rect& r = modes.buttons[static_cast<int>(ed.wire_colors)];
        const char* caption = "";
        switch (ed.wire_colors) {
            case WireColorMode::Plain:
                caption = "Plain: single ink color";
                break;
            case WireColorMode::Nets:
                caption = "Nets: each net its own color";
                break;
            case WireColorMode::Voltage:
                caption = "Voltage: node voltages as blue-gray-red";
                break;
        }
        const float cw = rc.measure_text(caption, *theme.font);
        const float cx = r.x + (r.width - cw) * 0.5f;
        const float cy = r.y + r.height + 4.0f;
        (void)rc.draw_text(cx, cy, caption, *theme.font,
                           gui::dim(theme.text, 0.55f));
    }

    // The transport: the play/pause button, the scrub track with the
    // playhead knob, and the axis readout ("1.2ms" for a .tran,
    // "V1 = 3.4V" for a .dc sweep). Drawn only when the run has a
    // scrubbable table.
    if (const ws::spice::Table* tran = ed.canvas_sweep_table();
        tran != nullptr) {
        const TransportStripLayout tl = transport_strip_layout(body);
        const std::span<const double> axis =
            ws::circsim::overlay_sweep_axis(*tran);
        const double t0 = axis.empty() ? 0.0 : axis.front();
        const double t1 = axis.empty() ? 0.0 : axis.back();
        const double span = std::max(t1 - t0, 1e-12);
        const double frac =
            std::clamp((ed.playback.t - t0) / span, 0.0, 1.0);

        rc.fill_rounded_rect(tl.strip.x, tl.strip.y, tl.strip.width,
                             tl.strip.height, tl.strip.height * 0.5f,
                             theme.control_bg);
        Paint border;
        border.width = 1.0f;
        border.color = gui::dim(theme.text, 0.35f);
        rc.stroke_rounded_rect(tl.strip.x, tl.strip.y, tl.strip.width,
                               tl.strip.height, tl.strip.height * 0.5f,
                               border);

        // Play (a triangle) / pause (two bars).
        const gui::Rect& b = tl.button;
        if (ed.playback.playing) {
            const Point bars[2] = {
                {b.x + b.width * 0.30f, b.y + b.height * 0.22f},
                {b.x + b.width * 0.62f, b.y + b.height * 0.22f}};
            rc.fill_rects(bars, 2, b.width * 0.12f, b.height * 0.56f,
                          theme.control_on);
        } else {
            const Point tri[3] = {
                {b.x + b.width * 0.32f, b.y + b.height * 0.18f},
                {b.x + b.width * 0.32f, b.y + b.height * 0.82f},
                {b.x + b.width * 0.80f, b.y + b.height * 0.50f}};
            rc.fill_polygon(tri, 3, theme.control_on);
        }

        // The track, the played span, the knob.
        const gui::Rect& bar = tl.bar;
        const float track_y = bar.y + bar.height * 0.5f - 1.0f;
        const Point track{bar.x, track_y};
        rc.fill_rects(&track, 1, bar.width, 2.0f,
                      gui::dim(theme.text, 0.30f));
        const Point prog{bar.x, track_y};
        rc.fill_rects(&prog, 1,
                      static_cast<float>(bar.width * frac), 2.0f,
                      theme.control_on);
        rc.fill_circle(
            Point{bar.x + static_cast<float>(bar.width * frac),
                  bar.y + bar.height * 0.5f},
            5.0f, theme.control_on);

        // The rate chip: the sweep's pace, one press cycles the
        // ladder. It wears the "on" face while playing at a pace other
        // than 1x, so an accidental 1/4x says so at a glance.
        const gui::Rect& rt = tl.rate;
        const bool pace_off = ed.playback.rate != 1.0;
        rc.fill_rounded_rect(rt.x, rt.y, rt.width, rt.height, 4.0f,
                             pace_off ? gui::dim(theme.control_on, 0.22f)
                                      : theme.control_bg);
        Paint chip;
        chip.width = 1.0f;
        chip.color = pace_off
                         ? gui::dim(theme.control_on, 0.80f)
                         : gui::dim(theme.text, 0.35f);
        rc.stroke_rounded_rect(rt.x, rt.y, rt.width, rt.height, 4.0f,
                               chip);
        if (theme.font != nullptr) {
            const char* rl =
                ws::circsim::overlay_rate_label(ed.playback.rate);
            const float rw = rc.measure_text(rl, *theme.font);
            (void)rc.draw_text(
                rt.x + (rt.width - rw) * 0.5f,
                rt.y + (rt.height - theme.font->line_height()) * 0.5f,
                rl, *theme.font,
                pace_off ? theme.control_on : gui::dim(theme.text, 0.85f));
        }

        // The readout (a fontless theme draws the shape alone): the
        // axis's own spelling - seconds for a .tran, the swept
        // source's name and unit for a .dc. Drawn inside the strip's
        // own reserved box (the readout rect), clipped at the pill's
        // right edge: the text can never run into the rate chip (the
        // old right-anchored spelling put "V2 = 470mV" under the
        // chip's "1x").
        if (theme.font != nullptr) {
            const std::string label =
                ws::circsim::overlay_axis_readout(*tran, ed.playback.t);
            const float w = rc.measure_text(label.c_str(), *theme.font);
            const float avail =
                std::max(0.0f, tl.readout.width);
            const std::string shown =
                w <= avail ? label
                           : label.substr(0, static_cast<std::size_t>(
                                   avail / std::max(1.0f, w) *
                                   static_cast<float>(label.size())));
            const float sw = rc.measure_text(shown.c_str(), *theme.font);
            (void)rc.draw_text(
                tl.readout.x + (tl.readout.width - sw) * 0.5f,
                tl.strip.y + tl.strip.height * 0.5f - 6.0f,
                shown.c_str(), *theme.font, gui::dim(theme.text, 0.85f));
        }
    }

    // The voltage legend: the ramp and its ends, bottom-right, only in
    // Voltage mode with a run behind it. A stale run says so.
    if (ed.wire_colors == WireColorMode::Voltage && rp.ok) {
        constexpr float kLegendW = 108.0f;
        constexpr float kLegendH = 6.0f;
        constexpr int kLegendSteps = 9;
        const float lx = body.x + body.width - kLegendW - 10.0f;
        const float ly = body.y + body.height - 44.0f;
        const float sw = kLegendW / kLegendSteps;
        for (int k = 0; k < kLegendSteps; ++k) {
            const double u =
                -1.0 + 2.0 * static_cast<double>(k) / (kLegendSteps - 1);
            const Point c{lx + sw * k, ly};
            rc.fill_rects(&c, 1, sw + 0.5f, kLegendH,
                          voltage_color(u * rp.vmax, rp.vmax));
        }
        if (theme.font != nullptr) {
            const std::string lo =
                "-" + ws::circsim::format_eng(rp.vmax) + "V";
            const std::string hi =
                "+" + ws::circsim::format_eng(rp.vmax) + "V";
            (void)rc.draw_text(lx, ly - 13.0f, lo.c_str(), *theme.font,
                               gui::dim(theme.text, 0.70f));
            const float hw = rc.measure_text(hi.c_str(), *theme.font);
            (void)rc.draw_text(lx + kLegendW - hw, ly - 13.0f, hi.c_str(),
                               *theme.font, gui::dim(theme.text, 0.70f));
            if (rp.stale) {
                (void)rc.draw_text(lx, ly + kLegendH + 4.0f,
                                   "last run - the drawing moved on",
                                   *theme.font,
                                   gui::dim(theme.text, 0.55f));
            }
        }
    }

    // --- the hint strip (SCREEN space): what the pointer and keys do
    // right now, bottom-left, only while the pointer is over the canvas.
    if (inside && gui.claims_open() && theme.font != nullptr) {
        const std::string text(ed.hint());
        if (!text.empty()) {
            (void)rc.draw_text(body.x + 10.0f,
                               body.y + body.height - 24.0f, text.c_str(),
                               *theme.font, gui::dim(theme.text, 0.55f));
        }
    }
}

}  // namespace editor
