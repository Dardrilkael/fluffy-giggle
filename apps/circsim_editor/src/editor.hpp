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
//   Wire tool    click a pin, junction, wire body or free grid point
//                starts a draft (free and wire-body starts mint their
//                junction at commit - a cancel leaves nothing behind;
//                Shift at the start click flips the L's first leg to
//                vertical); the preview follows the cursor with the
//                ortho route (the first leg honors the pin's dir, the
//                last approaches the target's); every commit CHAINS -
//                the next draft starts at the landing, so a
//                multi-segment net is one run of clicks; the first
//                right click (or Esc) cancels the draft, the second
//                disarms.
//   Label tool   armed with the Palette's name field; each click
//                places one label; stays armed.
// Wheel zooms toward the cursor; middle-drag and space+left-drag pan.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <ws/circsim/export.hpp>
#include <ws/circsim/library.hpp>
#include <ws/circsim/locate.hpp>
#include <ws/circsim/model.hpp>
#include <ws/circsim/nets.hpp>
#include <ws/circsim/plot.hpp>
#include <ws/circsim/render.hpp>
#include <ws/math/vecmath.hpp>
#include <ws/spice/run.hpp>
#include <ws/ui/canvas/canvas2d.hpp>
#include <ws/ui/canvas/grid.hpp>  // GridSpec: the grid the panel draws
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

// Epsilon compare on world coordinates (grid units): a position is
// "the same" far below any drawn pixel.
inline constexpr double kEps = 1e-9;
inline bool eps_eq(double a, double b, double eps = kEps) {
    return std::fabs(a - b) <= eps;
}

// The camera's clamps and default (zoom_at and fit clamp into the
// range; a fresh Editor and document_changed adopt the default).
inline constexpr double kZoomMin = 3.0;
inline constexpr double kZoomMax = 400.0;
inline constexpr double kZoomDefault = 26.0;

// The GLFW mouse button indices the panels read (libs/input passes
// the platform's numbers straight through): 0 left, 1 right, 2
// middle. The editor's panels once read 1 and 2 with the names
// swapped - the pan rode the right button and the right-click cancel
// the middle one; these names are the GLFW truth.
inline constexpr int kMouseLeft = 0;
inline constexpr int kMouseRight = 1;
inline constexpr int kMouseMiddle = 2;

// --- the camera -------------------------------------------------------------

struct EditorView {
    double zoom = kZoomDefault;  // screen pixels per grid unit
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
        zoom = std::clamp(zoom, kZoomMin, kZoomMax);
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
        zoom = std::clamp(zoom, kZoomMin, kZoomMax);
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

// The Results plot's manual view (stage 5): the zoom/pan window the
// wheel, the rubber band and the drags produce, in DATA units,
// riding over the automatic fit (ws::circsim::clamp_view keeps it
// inside the data's padded extent). Inactive = the auto fit, the
// stage-4 behavior exactly. Stage 6 gives the two-axis plot its
// second window: y_* is the LEFT spine's window (the volt company),
// y1_* the RIGHT one's (the amp company) - each clamped into its own
// home, so a mixed legend zooms one spine without dragging the other.
struct PlotView {
    bool active = false;
    double x_lo = 0.0, x_hi = 0.0;
    double y_lo = 0.0, y_hi = 0.0;    // the left y axis (volts)
    double y1_lo = 0.0, y1_hi = 0.0;  // the right y axis (amperes)
};

// The plot's in-flight pointer gesture (stage 5): the rubber-band box
// zoom, the two axis-drag zooms, and the pan. The press-time view is
// captured so the drag rebases from it every frame (no drift) and a
// right-click restores it exactly. The double-click clock rides the
// input frame's dt - the reset gesture's "two clicks, same spot".
// Stage 6 adds the right strip's drag (zoom_axis 1 vs 0) and the
// right spine's press-time capture (y1_*, anchor_y1) so its drag
// rebases and cancels exactly like the left one always did.
struct PlotGesture {
    enum class Kind : std::uint8_t { None, Band, ZoomX, ZoomY, Pan };
    Kind kind = Kind::None;
    float from_x = 0.0f, from_y = 0.0f;     // the press point, screen px
    double anchor_x = 0.0, anchor_y = 0.0;  // the data point under the press
    double anchor_y1 = 0.0;                 // ... on the right spine (stage 6)
    double x_lo = 0.0, x_hi = 0.0;          // the x view at press
    double y_lo = 0.0, y_hi = 0.0;          // the left y view at press
    double y1_lo = 0.0, y1_hi = 0.0;        // the right y view at press
    int zoom_axis = 0;  // the ZoomY strip: 0 = left, 1 = right (stage 6)
    bool was_active = false;                // the view at press (the cancel restore)
    // The double-click detector: the clock runs while the pointer is
    // over the plot, resets on every press there.
    float click_clock = 100.0f;
    float click_x = 0.0f, click_y = 0.0f;
};

// The Results panel's cache (stage 4): the last run's output plus the
// view over it - which table is shown, which signals are visible, and
// the sample the cursor hovers (the crosshair's index, -1 when off).
// Stage 5 adds the plot's manual view and its in-flight gesture, both
// keyed to the table they belong to. Stage 6 widens both to the two
// spines: the view carries the right axis's window beside the left's,
// the gesture its press-time capture - a mixed V/I legend zooms and
// pans each spine alone. A document switch clears it
// (document_changed); an EDIT does not -
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
    PlotView view;                       // the manual zoom/pan window
    std::size_t view_for = kNoTable;     // the table `view` belongs to
    PlotGesture gesture;                 // the in-flight plot gesture
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
    // The toast texts' stable home (toast() borrows from it; mutable
    // through const - toasting is not an edit).
    mutable std::deque<std::string> toast_text_;
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
    // The Palette panel's view state: the search box, and the groups
    // the user collapsed (open is the default, so a new group shows).
    char palette_filter[48] = "";
    std::set<std::string> palette_closed;
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
        // A start with no entity behind it (a wire's body - stage
        // 18b - or free space): the start endpoint is not a pin or
        // junction, so the commit mints a junction at `a_pos` and
        // binds the new wire's `a` to it. Deferred past a cancel, so
        // abandoning the draft leaves nothing behind. `a_free` marks
        // the free-space flavor (no anchor wire to revalidate).
        bool a_mint = false;
        bool a_free = false;
        // A free start's L flips: horizontal-first by default, the
        // Shift modifier at the start click picks vertical-first.
        // Recorded AT THE START so the preview and the commit route
        // from the same answer.
        bool a_vertical = false;
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

    // auto_junction_dots' cache (stage 18): the positions themselves,
    // the schematic version they were built against, and whether a
    // build ever ran (a fresh Editor starts invalid, not version 0).
    std::vector<Pos> join_dots_;
    std::uint64_t joins_version_ = 0;
    bool joins_valid_ = false;

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

    // The Schematic panel's draw seam: the same pattern one panel over
    // - null = &renderer (the live app), a headless test injects a
    // CommandListCanvas and asserts the camera mapping the panel
    // emits (every element must land where EditorView::w2s answers -
    // the grid, the snap feedback and the picks speak that mapping).
    canvas::Canvas2D* canvas_panel_canvas = nullptr;

    // The canvas the Schematic panel draws through.
    [[nodiscard]] canvas::Canvas2D& schematic_draw_canvas() {
        return canvas_panel_canvas != nullptr ? *canvas_panel_canvas
                                              : renderer;
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

    // The grid spec the canvas panel draws AND snap() reads - one
    // source of truth (ws::ui::canvas::GridSpec): the base spacing is
    // the schematic's own grid (Schematic::grid, default 1), the
    // major interval the x5 ladder. The panel paints its theme's
    // grid tokens over the faces; the spacing answers both halves.
    [[nodiscard]] canvas::GridSpec grid_spec() const;

    // Snap to the grid spec's fine lattice - the same one the drawn
    // dots mark. `coarse` snaps to the major lattice instead (base *
    // major_interval, an integer multiple: a coarse position is
    // always a fine position too, so wires, pins and junctions can
    // never leave the fine grid). The wire tool and junction drags
    // always pass false; the Shift modifier carries the coarse
    // placement/drag snap.
    [[nodiscard]] Pos snap(Pos world, bool coarse = false) const;

    // The part's world AABB (the transformed symbol bounds). The id
    // form re-finds the part; the reference form serves the loops
    // that already hold it (the draw and the hit loops - no find_part
    // inside the loop).
    [[nodiscard]] std::optional<Bounds> part_bounds(std::uint32_t id) const;
    [[nodiscard]] std::optional<Bounds> part_bounds(
        const ws::circsim::Part& p) const;

    // The wire's world polyline (endpoints through their refs, then
    // the waypoints between).
    [[nodiscard]] std::vector<Pos> wire_path(
        const ws::circsim::Wire& w) const;

    // The ortho route for a wire from `a` (leaving along `a_dir`, -1 =
    // free) to `b` (approached along `b_dir`, -1 = free): the waypoint
    // list between the two endpoints - a leads out, an L or Z lands,
    // collinear legs folded away.
    [[nodiscard]] static std::vector<Pos> route_wire(
        Pos a, int a_dir, Pos b, int b_dir, bool a_vertical = false);

    // The move-drag's wire re-route data: every ORTHO wire whose
    // endpoint references the entity, carrying the waypoints that keep
    // its legs axis-aligned at the landing position. The endpoints
    // resolve live through their refs - only the waypoints would stay
    // put and turn the adjacent leg diagonal. A straight or one-bend
    // wire gets a fresh route_wire L (and so does any wire when BOTH
    // ends ride the same move); a multi-bend wire keeps its Z and only
    // the adjacent waypoint slides. Call with the entity ALREADY at
    // its landing position. (WireRoute is the document layer's
    // command cargo - document.hpp.)
    [[nodiscard]] std::vector<WireRoute> attached_wire_routes(
        Drag::Kind kind, std::uint32_t id) const;

    // Hit tests (world space). Every radius is in world units (screen
    // px / zoom), so picks stay steady across zooms - including the
    // label's own reach, passed separately so its tag-sized margin
    // (a few px, not a world unit) scales with the magnification.
    [[nodiscard]] std::optional<PinHit> hit_pin(Pos world,
                                                double radius) const;
    [[nodiscard]] std::optional<std::uint32_t> hit_junction(
        Pos world, double radius) const;
    // A point on an existing wire's path within `radius` of `world`
    // (stage 18b): the nearest grid-aligned point lying on some wire's
    // polyline, falling back to the nearest path vertex when the grid
    // snap would drift off the segment. The returned point is bit-equal
    // to a position the wire's path passes through, so nets.cpp's
    // exact-double on_polyline accepts it when the editor mints a
    // junction there. The wire tool snaps a click near another wire's
    // body to this point and connects through that junction - the
    // standard EDA T-tap, the one thing the wire tool was missing.
    struct WirePointSnap {
        std::uint32_t wire = 0;  // the wire whose path was hit
        Pos pos{0.0, 0.0};       // grid-aligned, on the wire's path
    };
    [[nodiscard]] std::optional<WirePointSnap> hit_wire_point(
        Pos world, double radius) const;
    // Selection priority walks the draw stack topmost-first:
    // junctions, then the NEAREST wire in range (a wire stays
    // selectable where it crosses a part's box), then the LAST part
    // whose box holds the point (the drawn-on-top one), then labels.
    [[nodiscard]] Selection hit_any(Pos world, double radius,
                                    double label_radius) const;

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

    // The canvas' auto-junction dots (stage 18): the silent joins the
    // net rules make - pins a wire's path picked up, vertices T-riding
    // another wire - cached against the schematic version, one rebuild
    // per edit, not per frame.
    [[nodiscard]] const std::vector<Pos>& auto_junction_dots();

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
// placement ghost and the palette's previews alike. The geometry is
// ws::circsim::render_symbol's (the stage-17a seam shared with the
// CLI's SVG dump and the lib's tests); this is the painting half -
// strokes to canvas calls. The pin-lead safety net rides the seam: a
// pin no prim vertex touches gets a stub into the body (the .csym
// path's net for symbols whose prims fall short of their pins).
void draw_symbol(canvas::Canvas2D& c, const ws::circsim::SymbolDef& sym,
                 Pos pos, int rot, bool mirror,
                 const canvas::Paint& body, std::uint32_t text_color,
                 const ws::ui::r2d::Font* font);

}  // namespace editor
