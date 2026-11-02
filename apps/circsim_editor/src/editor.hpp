#pragma once

// apps/circsim_editor stage 3 - the shared editor core: the state the
// panels and commands all drive, plus the pure helpers (view mapping,
// snapping, hit-testing, wire routing, symbol rendering) that live
// with it. One struct, `Editor`, owned by the app; the panel draw
// callbacks and the commands receive `Editor&` and nothing else.
//
// The world frame is the MODEL's: grid units, y up, rotations CCW.
// The screen frame is gui pixels, y down. EditorView is the mapping
// (a zoom/pan camera with the y flip baked in) - deliberately NOT
// ws::ui::canvas::ViewTransform2D, whose screen-space convention is
// y-down world (its own docs say so); the schematic's math must stay
// in the model's frame or pins and pictures would disagree, so the
// editor owns its own 60-line camera instead of fighting the layer's.
// Everything drawn still goes through ws::ui::canvas's Canvas2D (the
// stage-14 layer) - strokes, tessellation, dashes and text.
//
// The interaction model (v1, one pointer):
//   Select tool  press on a part/junction/wire/label selects it;
//                dragging moves the part/junction live (the undo
//                command lands at release, capturing from/to);
//                press on empty space clears the selection.
//   Place tool   armed from the Palette; the ghost follows the
//                snapped cursor; R/M rotate/mirror the ghost; each
//                click places one part (the tool stays armed); Esc or
//                a right click disarms.
//   Wire tool    click a pin or junction starts a draft; the preview
//                follows the cursor with the ortho route (the first
//                leg honors the pin's dir, the last approaches the
//                target's); clicking a pin/junction commits, clicking
//                free space commits onto a fresh junction; every
//                commit disarms (W re-arms cheaply).
//   Label tool   armed with the Palette's name field; each click
//                places one label; stays armed.
// Wheel zooms toward the cursor; middle-drag and space+left-drag pan.

#include <array>
#include <cstdint>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <ws/circsim/export.hpp>
#include <ws/circsim/library.hpp>
#include <ws/circsim/locate.hpp>
#include <ws/circsim/model.hpp>
#include <ws/circsim/nets.hpp>
#include <ws/circsim/plot.hpp>
#include <ws/math/vecmath.hpp>
#include <ws/spice/run.hpp>
#include <ws/ui/canvas/canvas2d.hpp>
#include <ws/ui/canvas/tessellation.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>

#include "document.hpp"

namespace ws::jobs {
class JobSystem;
}
namespace ws::ui::shell {
class AppShell;
}

namespace editor {

namespace gui = ws::ui::gui;
namespace canvas = ws::ui::canvas;
using ws::circsim::Bounds;
using ws::circsim::Pos;

// Epsilon compare on world coordinates (grid units).
inline bool near(double a, double b, double eps) {
    return std::fabs(a - b) <= eps;
}

// --- the camera -------------------------------------------------------------

struct EditorView {
    double zoom = 26.0;   // screen pixels per grid unit
    Pos center{0.0, 0.0};  // the world point at the viewport's center

    // The viewport, set each frame before use (screen px, the panel
    // body's rect). Invalid (w/h <= 0) until the first draw.
    float vx = 0.0f, vy = 0.0f, vw = 0.0f, vh = 0.0f;

    [[nodiscard]] bool valid() const { return vw > 1.0f && vh > 1.0f; }

    // world (y up) <-> screen (y down): x maps straight, y flips.
    [[nodiscard]] ws::math::Vec2 w2s(Pos world) const {
        return {static_cast<float>(vx + vw * 0.5f +
                                   (world.x - center.x) * zoom),
                static_cast<float>(vy + vh * 0.5f -
                                   (world.y - center.y) * zoom)};
    }
    [[nodiscard]] Pos s2w(ws::math::Vec2 screen) const {
        return {center.x + (screen.x - vx - vw * 0.5f) / zoom,
                center.y - (screen.y - vy - vh * 0.5f) / zoom};
    }

    // Zoom by `factor` toward `anchor` (screen px): the world point
    // under the anchor stays put. Clamped to the editor's range.
    void zoom_at(ws::math::Vec2 anchor, double factor) {
        if (!valid()) {
            return;
        }
        const Pos under = s2w(anchor);
        zoom *= factor;
        if (zoom < 3.0) {
            zoom = 3.0;
        }
        if (zoom > 400.0) {
            zoom = 400.0;
        }
        // Keep `under` at the anchor: center = under + (center-anchor
        // in world units at the new zoom).
        const double dx = (vx + vw * 0.5f - anchor.x) / zoom;
        const double dy = (anchor.y - vy - vh * 0.5f) / zoom;
        center = {under.x + dx, under.y + dy};
    }

    // Pan by a screen-pixel delta.
    void pan_by(ws::math::Vec2 screen_delta) {
        center.x -= screen_delta.x / zoom;
        center.y += screen_delta.y / zoom;
    }

    // Fit `b` (world) with `margin` world units of air on every side.
    // A degenerate box keeps the zoom and centers.
    void fit(const Bounds& b, double margin) {
        if (!valid()) {
            return;
        }
        center = {(b.min_x + b.max_x) * 0.5, (b.min_y + b.max_y) * 0.5};
        const double w = (b.max_x - b.min_x) + 2.0 * margin;
        const double h = (b.max_y - b.min_y) + 2.0 * margin;
        if (w <= 0.0 || h <= 0.0) {
            return;
        }
        zoom = std::min(static_cast<double>(vw) / w,
                        static_cast<double>(vh) / h);
        if (zoom < 3.0) {
            zoom = 3.0;
        }
        if (zoom > 400.0) {
            zoom = 400.0;
        }
    }
};

// --- selection & tools --------------------------------------------------------

enum class Tool : std::uint8_t { Select, Place, Wire, Label };

struct Selection {
    enum class Kind : std::uint8_t { None, Part, Wire, Junction, Label };
    Kind kind = Kind::None;
    std::uint32_t id = 0;

    [[nodiscard]] bool is(Kind k, std::uint32_t of) const {
        return kind == k && id == of;
    }
    void clear() { kind = Kind::None; }
};

// A hovered pin (the wire tool's target highlight).
struct PinHit {
    ws::circsim::PinRef ref{};
    Pos pos{0.0, 0.0};
    int dir = 0;  // the pin's pointing direction, degrees
};

// --- the caches ----------------------------------------------------------------

// The Problems panel's list: whatever produced it (ERC, a run, a
// load's repairs) plus the maps the rows locate through.
struct ProblemsCache {
    bool has = false;          // a check ran at least once
    bool from_run = false;     // a simulation's outcome (not plain ERC)
    std::uint64_t version = 0;  // the schematic version it ran against
    std::string header;        // "erc: 2 errors, 3 warnings" and friends
    std::vector<ws::circsim::Diag> diags;
    std::vector<std::string> lines;  // the rendered rows (borrowed bytes)
    ws::circsim::NetList nets;       // the check's net build
    bool nets_valid = false;
    ws::circsim::SpiceExport map;    // the run's export back-map
    bool map_valid = false;
};

struct NetlistCache {
    bool has = false;
    std::uint64_t version = 0;
    std::string text;                 // the exported deck
    std::vector<std::string> lines;   // split for the panel's rows
};

// The Results panel's cache (stage 4): the last run's output plus the
// view over it - which table is shown, which signals are visible, and
// the sample the cursor hovers (the crosshair's index, -1 when off).
// A document switch clears it (document_changed); an EDIT does not -
// the panel marks itself stale against the schematic's version, the
// same contract the Problems and Netlist panels keep.
struct ResultsCache {
    static constexpr std::size_t kNoTable = static_cast<std::size_t>(-1);

    bool has = false;
    std::uint64_t version = 0;           // the schematic version it ran against
    ws::spice::RunOutput run;            // tables + measurements
    std::size_t table = 0;               // the selected table
    std::size_t visible_for = kNoTable;  // the table `visible` belongs to
    std::vector<std::uint8_t> visible;   // per-signal flags of that table
    int hover = -1;                      // sample index under the cursor
};

// One editable field's caller-owned buffer (the Properties panel's
// editing state): the bytes the text_input edits, the model value it
// loaded from, and the epoch it loaded at - an epoch move (a selection
// change, an undo/redo landing) reloads it; a typing frame never does.
struct FieldBuf {
    std::array<char, 128> buf{};
    std::string shadow;
    std::uint64_t stamp = 0;
    bool combo_open = false;  // the Choice rows' dropdown state
};

// --- the editor ----------------------------------------------------------------

struct Editor {
    // Wiring, set once at setup (borrowed; the app owns every piece).
    ws::circsim::SymbolLibrary library;  // built-ins (a .csim layer is a later stage)
    ws::ui::shell::AppShell* shell = nullptr;   // toasts + status
    ws::jobs::JobSystem* jobs = nullptr;
    ws::ui::r2d::Font* font = nullptr;          // the UI face
    ws::ui::r2d::Font* font_mono = nullptr;     // the netlist panel's face
    gui::Toasts* toasts = nullptr;              // the shell's store
    std::function<void(std::string)> set_status;
    // The Results panel's export button: true = JSON, false = CSV.
    // The app installs it (the path policy - derive from the
    // document, write through the fs seam - is the app's to own).
    std::function<void(bool json)> export_results;

    // The active document (null = none open). The app re-points this
    // on every activation change and calls document_changed().
    SchematicDocument* doc = nullptr;

    // The interaction state.
    Tool tool = Tool::Select;
    std::string place_symbol;  // the armed symbol key
    int ghost_rot = 0;
    bool ghost_mirror = false;
    char label_text[64] = "net";
    Selection sel;

    // The move-drag (press to release). `live` writes the position
    // directly (no undo) while the pointer holds; the command lands at
    // release with the captured from/to.
    struct Drag {
        enum class Kind : std::uint8_t { None, Part, Junction, Label };
        Kind kind = Kind::None;
        std::uint32_t id = 0;
        Pos start_world{0.0, 0.0};
        Pos grab_offset{0.0, 0.0};  // world: press point - entity pos
        bool moved = false;         // past the click threshold
    } drag;

    // The wire draft (the Wire tool between its two clicks).
    struct WireDraft {
        bool active = false;
        ws::circsim::WireEndpoint a{};
        Pos a_pos{0.0, 0.0};
        int a_dir = 0;  // the start pin's direction, -1 when free
    } wire_draft;

    // True when the last frame's Properties draw saw a focused text
    // field - the chord loop then resolves at the text_editing rung so
    // the panel keys stay out of the field (one frame of settle, the
    // same cadence every gui claim rides).
    bool text_field_focused = false;

    // The camera + the caches.
    EditorView view;
    ProblemsCache problems;
    NetlistCache netlist;
    ResultsCache results;

    // The Properties panel's editing state: the field buffers keyed
    // by stable strings, the epoch that reloads them, the selection
    // they were built for, and the analysis entry being edited.
    std::map<std::string, FieldBuf> fields;
    std::uint64_t field_stamp = 1;
    int fields_sel_kind = -1;
    std::uint32_t fields_sel_id = 0;
    int analysis_index = -1;

    // Bumps the edit epoch (the app wires the undo stacks' listeners
    // here, so an undo/redo lands back into the field rows).
    void bump_edit_epoch() { ++field_stamp; }

    // The pan drag's arm state (middle / space+left).
    struct Pan {
        bool armed = false;
        ws::math::Vec2 from{0.0f, 0.0f};
    } pan;

    // The canvas the schematic panel renders through (one per
    // editor; the canvas layer's own state is per-instance).
    canvas::Renderer2DCanvas renderer;

    // The Results panel's draw seam: null = &renderer (the live app);
    // a headless test injects a CommandListCanvas here and asserts the
    // plot geometry the panel emits (the same purity seam the gui
    // suite rides - the results panel is app code, so the injection
    // point is a member, not a link-time stub).
    canvas::Canvas2D* plot_canvas = nullptr;

    // The canvas the Results panel draws through.
    [[nodiscard]] canvas::Canvas2D& results_draw_canvas() {
        return plot_canvas != nullptr ? *plot_canvas : renderer;
    }

    // --- helpers (editor.cpp) ---

    [[nodiscard]] ws::circsim::Schematic* schem() {
        return doc != nullptr ? &doc->schem : nullptr;
    }
    [[nodiscard]] const ws::circsim::Schematic* schem() const {
        return doc != nullptr ? &doc->schem : nullptr;
    }

    [[nodiscard]] const ws::circsim::SymbolDef* sym_of(
        const ws::circsim::Part& p) const;

    // Snap to the schematic's grid.
    [[nodiscard]] Pos snap(Pos world) const;

    // The part's world AABB (the transformed symbol bounds).
    [[nodiscard]] std::optional<Bounds> part_bounds(std::uint32_t id) const;

    // The wire's world polyline (endpoints through their refs, then
    // the waypoints between).
    [[nodiscard]] std::vector<Pos> wire_path(
        const ws::circsim::Wire& w) const;

    // The ortho route for a wire from `a` (leaving along `a_dir`, -1 =
    // free) to `b` (approached along `b_dir`, -1 = free): the waypoint
    // list between the two endpoints - a leads out, an L or Z lands,
    // collinear legs folded away.
    [[nodiscard]] static std::vector<Pos> route_wire(Pos a, int a_dir,
                                                     Pos b, int b_dir);

    // Hit tests (world space). The pin hit radius is in world units
    // (screen px / zoom), so picks stay steady across zooms.
    [[nodiscard]] std::optional<PinHit> hit_pin(Pos world,
                                                double radius) const;
    [[nodiscard]] std::optional<std::uint32_t> hit_junction(
        Pos world, double radius) const;
    [[nodiscard]] Selection hit_any(Pos world, double radius) const;

    // The whole schematic's content extent (for the fit command).
    [[nodiscard]] std::optional<Bounds> content_bounds() const;

    // Select-and-zoom for a Problems row (locate.hpp's seam).
    void jump_to(const ws::circsim::DiagTarget& t);

    // Cache refreshers (the commands' act; synchronous - ERC and the
    // export are millisecond work at editor scales).
    void refresh_problems(bool from_run,
                          std::vector<ws::circsim::Diag> diags,
                          std::string header);
    void run_erc();
    void refresh_netlist();

    // Clear every per-document surface (the app calls this when the
    // active document changes).
    void document_changed();

    // The toast pair the commands and panels use.
    void toast(std::string text, gui::ToastSeverity severity) const;
    void status(std::string text) const;
};

// The shared symbol renderer: `draw` prims + pin stubs through a
// canvas, in WORLD space (the canvas transform must be the editor's).
// `pos`/`rot`/`mirror` are the placement; used for placed parts, the
// placement ghost and the palette's previews alike. The pin stubs
// connect each pin to the picture (the built-in tables' draw prims
// already carry their leads; the stubs only fire for symbols whose
// prims fall short of their pins - a .csym safety net).
void draw_symbol(canvas::Canvas2D& c, const ws::circsim::SymbolDef& sym,
                 Pos pos, int rot, bool mirror,
                 const canvas::Paint& body, std::uint32_t text_color,
                 const ws::ui::r2d::Font* font);

}  // namespace editor
