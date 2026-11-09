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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
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

// Epsilon compare on world coordinates (grid units).
inline bool eps_eq(double a, double b, double eps) {
    return std::fabs(a - b) <= eps;
}

// The "Tidy Wires" command's seam (document.cpp): lands the given
// shapes as ONE undo step, skipping wires that would not change, and
// returns how many wires actually changed (0 = nothing pushed). Declared
// here because document.hpp was not part of the change; moving this
// declaration into document.hpp is the intended home (a redeclaration
// in both places is harmless).
std::size_t push_tidy_wires(SchematicDocument& doc,
                            std::vector<WireRoute> routes);

// Rotate/mirror WITH the attached wires' fresh shapes, as one undo step
// (push_transform_part stays for callers with no routes). The Editor's
// transform_part() computes the routes and calls this.
void push_transform_part_routed(SchematicDocument& doc, std::uint32_t part_id,
                                int rot, bool mirror, std::string name,
                                std::vector<WireRoute> wire_routes);

// A whole-sheet renumbering as one undo step: (part id, new ref) pairs;
// entries that would not change a ref are dropped. Returns how many refs
// changed (0 = nothing pushed). Same declaration note as push_tidy_wires.
std::size_t push_annotate(
    SchematicDocument& doc,
    std::vector<std::pair<std::uint32_t, std::string>> changes);

// --- the camera -------------------------------------------------------------

struct EditorView {
    // The zoom range and the default, in screen pixels per grid unit.
    // One source of truth: zoom_at, fit, fly_to and the document load
    // all clamp through clamp_zoom (the limits used to be magic numbers
    // repeated at every call site).
    static constexpr double kMinZoom = 3.0;
    static constexpr double kMaxZoom = 400.0;
    static constexpr double kDefaultZoom = 26.0;
    [[nodiscard]] static double clamp_zoom(double z) {
        return std::clamp(z, kMinZoom, kMaxZoom);
    }

    double zoom = kDefaultZoom;  // screen pixels per grid unit
    Pos center{0.0, 0.0};        // the world point at the viewport's center

    // The viewport, set each frame before use (screen px, the panel
    // body's rect). Invalid (w/h <= 0) until the first draw.
    float vx = 0.0f, vy = 0.0f, vw = 0.0f, vh = 0.0f;

    // --- the smooth camera (opt-in) ---------------------------------------
    // fly_to() glides the camera to a target (an ease-in-out in log-zoom,
    // so a big zoom change feels even). It is OFF by default: with
    // `animate == false` fly_to() snaps, exactly the old behavior, so
    // nothing changes until the app opts in. When on, the app calls
    // step(dt) once per frame (dt in seconds) and keeps redrawing while
    // it returns true. Any manual zoom/pan cancels a glide in flight -
    // the user's hands always win over the camera's.
    struct Glide {
        bool active = false;
        Pos from_center{0.0, 0.0};
        Pos to_center{0.0, 0.0};
        double from_zoom = kDefaultZoom;
        double to_zoom = kDefaultZoom;
        double t = 0.0;         // seconds elapsed
        double duration = 0.35;  // seconds
    };
    bool animate = false;
    Glide glide;

    void cancel_glide() { glide.active = false; }

    void fly_to(Pos target, double target_zoom, double seconds = 0.35) {
        target_zoom = clamp_zoom(target_zoom);
        if (!animate || seconds <= 0.0 || !valid()) {
            center = target;
            zoom = target_zoom;
            glide.active = false;
            return;
        }
        glide.active = true;
        glide.from_center = center;
        glide.to_center = target;
        glide.from_zoom = zoom;
        glide.to_zoom = target_zoom;
        glide.t = 0.0;
        glide.duration = seconds;
    }

    // Advance a glide by `dt` seconds; true while it is still moving.
    bool step(double dt) {
        if (!glide.active) {
            return false;
        }
        glide.t += dt;
        const double u = std::min(1.0, glide.t / glide.duration);
        const double e = u * u * (3.0 - 2.0 * u);  // smoothstep
        center = {glide.from_center.x +
                      (glide.to_center.x - glide.from_center.x) * e,
                  glide.from_center.y +
                      (glide.to_center.y - glide.from_center.y) * e};
        const double lz0 = std::log(glide.from_zoom);
        const double lz1 = std::log(glide.to_zoom);
        zoom = clamp_zoom(std::exp(lz0 + (lz1 - lz0) * e));
        if (u >= 1.0) {
            center = glide.to_center;
            zoom = glide.to_zoom;
            glide.active = false;
        }
        return glide.active;
    }

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
        glide.active = false;  // the wheel beats a glide in flight
        const Pos under = s2w(anchor);
        zoom = clamp_zoom(zoom * factor);
        // Keep `under` at the anchor: center = under + (center-anchor
        // in world units at the new zoom).
        const double dx = (vx + vw * 0.5f - anchor.x) / zoom;
        const double dy = (anchor.y - vy - vh * 0.5f) / zoom;
        center = {under.x + dx, under.y + dy};
    }

    // Pan by a screen-pixel delta.
    void pan_by(ws::math::Vec2 screen_delta) {
        glide.active = false;  // a drag beats a glide in flight
        center.x -= screen_delta.x / zoom;
        center.y += screen_delta.y / zoom;
    }

    // Fit `b` (world) with `margin` world units of air on every side.
    // A degenerate box keeps the zoom and centers.
    void fit(const Bounds& b, double margin) {
        if (!valid()) {
            return;
        }
        const Pos target{(b.min_x + b.max_x) * 0.5, (b.min_y + b.max_y) * 0.5};
        const double w = (b.max_x - b.min_x) + 2.0 * margin;
        const double h = (b.max_y - b.min_y) + 2.0 * margin;
        if (w <= 0.0 || h <= 0.0) {
            fly_to(target, zoom);  // degenerate: center, keep the zoom
            return;
        }
        // fly_to clamps the zoom, and glides when `animate` is on.
        fly_to(target, std::min(static_cast<double>(vw) / w,
                                static_cast<double>(vh) / h));
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
    // The run's OWN net build and export back-map (deliver_run moves the
    // outcome's originals here; the Problems cache holds a copy for its
    // locate seam, and an ERC pass may replace that copy without ever
    // touching these). The canvas's voltage coloring and the LED glow
    // read THEM: node_net answers net id -> node token (the v(name)
    // column), element_part answers part id -> deck element (the i(name)
    // probe column).
    ws::circsim::NetList nets;
    ws::circsim::SpiceExport map;
    std::size_t table = 0;               // the selected table
    std::size_t visible_for = kNoTable;  // the table `visible` belongs to
    std::vector<std::uint8_t> visible;   // per-signal flags of that table
    int hover = -1;                      // sample index under the cursor
    PlotView view;                       // the manual zoom/pan window
    std::size_t view_for = kNoTable;     // the table `view` belongs to
    PlotGesture gesture;                 // the in-flight plot gesture
    // The shown table's x axis is strictly increasing - a per-table
    // constant of the run (the axis data is immutable once delivered),
    // cached with the same key shape as `visible`/`view` so the plot
    // does not re-walk every sample every frame. Reset with the other
    // per-run keys when a delivery lands (a same-indexed table of a
    // NEW run is a different axis).
    bool x_sorted = false;
    std::size_t x_sorted_for = kNoTable;  // the table `x_sorted` belongs to
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

// --- the interactive canvas: how the run paints back -------------------------

// The wires' paint mode. Plain is the classic single-ink look; Nets
// gives every net its own wheel color (the drawing reads back the
// connectivity - which pins share a net is visible at a glance);
// Voltage rides the LAST run's node voltages as a blue-gray-red ramp
// (needs results; unknown nets fall back to the plain ink). Editor view
// state, not document state: it survives a document switch like the
// palette's search does, because "how this user reads a canvas" is the
// same question in the next schematic.
enum class WireColorMode : std::uint8_t { Plain, Nets, Voltage };

// The .tran playback: the transport strip's playhead, in SECONDS. A
// paused canvas shows the sample at playback.t (a delivery parks it at
// the run's END - the settled state, what an .op reader means by "the
// answer"); playing sweeps t across the table's window at a pace that
// fits the whole run into ~4 s of wall clock (the rate below stretches
// or shrinks that), looping. The voltage coloring and the LED glow
// both read the playhead's sample, so a playing transient animates the
// canvas - and the Results plot wears the same playhead as a swept
// cursor line (one clock: the canvas strip and the plot's transport
// row scrub the same field, so no two surfaces can disagree).
struct Playback {
    static constexpr double kLoopSeconds = 4.0;  // wall clock per sweep
    bool playing = false;
    double t = 0.0;
    // The sweep's pace, off ws::circsim's rate ladder (1/4, 1/2, 1, 2
    // - overlay.hpp): 1x fits the run into kLoopSeconds, 2x halves
    // the wall clock, 1/4x stretches it. The transport strip's rate
    // chip and the Results row's cycle the same field.
    double rate = 1.0;
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
    // Backing store for toast text. Toasts BORROWS the bytes, so every
    // toast needs a string of its own whose address stays put until the
    // toast expires (a deque never moves its elements on push/pop at
    // the ends). Bounded: the oldest entries are long expired by the
    // time the cap recycles them (4s toasts, 16 slots).
    mutable std::deque<std::string> toast_text_;
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
    // The Palette's layout: false = the compact picture-tile grid, true =
    // one named row per symbol (names visible without hovering).
    bool palette_list_view = false;
    // The Palette's recently-armed symbols, newest first, capped at
    // six. Editor-lifetime convenience state, not document state: a
    // document switch keeps it ("what did I place last?" is the same
    // question in the next schematic), and each Editor owns its own -
    // the file-static list it replaces was process-wide, so two
    // editors (the app and a test bench) shared one history.
    std::vector<std::string> palette_recent;
    Selection sel;

    // The interactive canvas's view state: the wire paint mode and the
    // .tran playback (see WireColorMode / Playback above). The
    // transport scrub's drag arm rides beside the playback - a scrub in
    // flight owns the pointer the way the pan drag does.
    WireColorMode wire_colors = WireColorMode::Nets;
    Playback playback;
    bool transport_scrub = false;
    // The Results panel's transport row scrub, the strip's own rule: a
    // press in the row's track arms it, the drag owns the pointer
    // until release, and both surfaces write the one playback above -
    // a scrub on the plot animates the canvas, and back.
    bool results_scrub = false;
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
        // 18b - or free space - stage 19f): the start endpoint is not
        // a pin or junction, so the commit mints a junction at `a_pos`
        // and binds the new wire's `a` to it - the same path the
        // commit-side wire-point takes, just deferred past a cancel.
        // a_wire is the id of the wire that was hit when the draft
        // started; the commit validates that the same wire still
        // exists at that position. `a_free` marks the free-space
        // flavor: no anchor wire to revalidate (the point is just a
        // grid position; it cannot go stale).
        bool a_mint = false;
        bool a_free = false;
        std::uint32_t a_wire = 0;
        // Which way the route's corner bends: false = horizontal
        // first (the stock L), true = vertical first. The panel flips
        // it on Tab while a draft is live and passes it to route_wire
        // for BOTH the preview and the commit, so what you see is what
        // lands. Cleared with the draft.
        bool flip_route = false;
        void flip() { flip_route = !flip_route; }
    } wire_draft;

    // True when the last frame's Properties draw saw a focused text
    // field - the chord loop then resolves at the text_editing rung so
    // the panel keys stay out of the field (one frame of settle, the
    // same cadence every gui claim rides).
    bool text_field_focused = false;

    // The camera + the caches.
    EditorView view;
    ProblemsCache problems;
    // The Problems panel's cursor: the diag index the row highlight
    // stands on and the Prev/Next walk steps from (-1 = none). It
    // indexes problems.diags and resets wherever that list is replaced
    // (refresh_problems, document_changed) - a cursor left over from a
    // replaced check would highlight a stranger's row.
    int problems_cursor = -1;
    // The severity filter's three toggles, all on by default: the walk
    // and the row draw both skip a hidden severity, while the header's
    // counts stay the full counts whatever is hidden.
    bool problems_show_error = true;
    bool problems_show_warning = true;
    bool problems_show_note = true;
    NetlistCache netlist;
    ResultsCache results;

    // auto_junction_dots' cache (stage 18): the positions themselves
    // paired with the net each join landed on (the dots wear the net's
    // color in Nets/Voltage mode), the schematic version they were built
    // against, and whether a build ever ran (a fresh Editor starts
    // invalid, not version 0). The net build the dots came from is the
    // net_cache below - one build_nets per edit feeds both.
    std::vector<std::pair<Pos, std::uint32_t>> join_dots_;
    std::uint64_t joins_version_ = 0;
    bool joins_valid_ = false;

    // The canvas's own net build: one build_nets per edit (the same seam
    // ERC and the export read), cached against the schematic version.
    // The wire coloring and the auto-junction dots both read it.
    ws::circsim::NetList nets_cache_;
    std::uint64_t nets_version_ = 0;
    bool nets_valid_ = false;

    // part_bounds cache: per-part world AABBs keyed by part id,
    // invalidated when the schematic version changes.
    mutable std::map<std::uint32_t, Bounds> part_bounds_cache_;
    mutable std::uint64_t bounds_version_ = 0;
    mutable bool bounds_valid_ = false;

    // The Properties panel's editing state: the field buffers keyed
    // by stable strings, the epoch that reloads them, the selection
    // they were built for, and the analysis entry being edited. The
    // tab strip's selection rides beside them (0 Properties, 1
    // Netlist, 2 Simulation) - caller-owned state the tab_bar writes,
    // surviving frames and document switches like every panel view.
    std::map<std::string, FieldBuf> fields;
    std::uint64_t field_stamp = 1;
    int fields_sel_kind = -1;
    std::uint32_t fields_sel_id = 0;
    int analysis_index = -1;
    int properties_tab = 0;

    // Bumps the edit epoch (the app wires the undo stacks' listeners
    // here, so an undo/redo lands back into the field rows).
    void bump_edit_epoch() { ++field_stamp; }

    // The pan drag's arm state (middle / space+left).
    struct Pan {
        bool armed = false;
        ws::math::Vec2 from{0.0f, 0.0f};
    } pan;

    // --- the frame tick, the locate pulse, the panel's key edges ---------
    // The Schematic panel calls tick() once per frame: it advances the
    // smooth camera and the pulse by the REAL elapsed time (steady
    // clock, clamped to 100 ms so a stalled frame never teleports the
    // view). It returns true while something is still moving - the app
    // may use that to keep requesting frames.
    [[nodiscard]] bool tick();

    // The locate pulse: a ring that blooms at a world point and fades
    // (jump_to and zoom_to_selection fire it, so "where did the view
    // just go?" answers itself). Drawn by the panel, skipped when the
    // theme's motion is off.
    struct Pulse {
        static constexpr double kLife = 0.9;  // seconds
        bool active = false;
        Pos pos{0.0, 0.0};
        double age = 0.0;
    } pulse;
    void pulse_at(Pos p) {
        pulse.active = true;
        pulse.pos = p;
        pulse.age = 0.0;
    }

    // Key-edge memory for the panel's own shortcuts (F frames the
    // selection, / flips the wire corner): "was down last frame".
    bool key_frame_prev_ = false;
    bool key_flip_prev_ = false;
    bool key_tidy_prev_ = false;
    bool key_annotate_prev_ = false;

    // Quiet toasts: an identical toast pushed while its twin is still
    // on screen is dropped instead of stacking (a command mashed ten
    // times used to paint ten copies). Off switch for tests.
    bool dedupe_toasts = true;
    mutable std::string last_toast_;
    mutable std::chrono::steady_clock::time_point last_toast_until_{};
    std::chrono::steady_clock::time_point last_tick_{};
    bool tick_started_ = false;

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

    // The Palette panel's draw seam: the same pattern a third time.
    // The palette's tiles draw the symbols' pictures directly, so the
    // seam exists for the same reason the other two do - null =
    // &renderer (the live app); a headless test injects a
    // CommandListCanvas here and asserts the tile grid's geometry
    // (fills, outlines, the fitted symbol strokes) without a renderer.
    canvas::Canvas2D* palette_canvas = nullptr;

    // The canvas the Palette panel draws through.
    [[nodiscard]] canvas::Canvas2D& palette_draw_canvas() {
        return palette_canvas != nullptr ? *palette_canvas : renderer;
    }

    // The Properties panel's draw seam: the same pattern a fourth
    // time. The source editor's waveform preview draws through here -
    // null = &renderer (the live app); a headless test injects a
    // CommandListCanvas and asserts the preview's geometry (the well,
    // the frame, the zero line, the series polyline) the same way the
    // palette and results suites assert theirs.
    canvas::Canvas2D* properties_canvas = nullptr;

    // The canvas the Properties panel draws through.
    [[nodiscard]] canvas::Canvas2D& properties_draw_canvas() {
        return properties_canvas != nullptr ? *properties_canvas : renderer;
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

    // The part's world AABB (the transformed symbol bounds).
    [[nodiscard]] std::optional<Bounds> part_bounds(std::uint32_t id) const;

    // The wire's world polyline (endpoints through their refs, then
    // the waypoints between).
    [[nodiscard]] std::vector<Pos> wire_path(
        const ws::circsim::Wire& w) const;

    // The ortho route for a wire from `a` (leaving along `a_dir`, -1 =
    // free) to `b` (approached along `b_dir`, -1 = free): the waypoint
    // list between the two endpoints - a leads out, an L lands,
    // collinear legs folded away (a reversal keeps its lead-out
    // corner; folding it would run the wire back through the part
    // body it just left). `flip` bends the corner the other way
    // (vertical-first instead of horizontal-first) - the Tab toggle;
    // it only matters when the route needs a corner at all.
    [[nodiscard]] static std::vector<Pos> route_wire(Pos a, int a_dir,
                                                     Pos b, int b_dir,
                                                     bool flip = false);

    // The attached ortho wires' fresh shapes for a move of `id` (the
    // drag-release re-route: a moved endpoint would otherwise leave
    // the adjacent legs diagonal). Read at the entity's LANDING - the
    // caller puts the entity there first, then restores the press
    // position and hands the routes to push_move_*.
    [[nodiscard]] std::vector<WireRoute> attached_wire_routes(
        Drag::Kind kind, std::uint32_t id) const;

    // Hit tests (world space). The pin hit radius is in world units
    // (screen px / zoom), so picks stay steady across zooms.
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
    [[nodiscard]] Selection hit_any(Pos world, double radius) const;

    // The whole schematic's content extent (for the fit command).
    [[nodiscard]] std::optional<Bounds> content_bounds() const;

    // Tidy: re-straightens ortho wires. tidy_route() is the pure core -
    // given a wire's full world path (endpoint, waypoints, endpoint) it
    // returns the cleaned INTERIOR waypoints: every diagonal leg gets an
    // axis-aligned corner, duplicate points and collinear continuing
    // middles fold away, the rest of the author's shape is kept (a
    // gentle fix-up, not a re-route). tidy_selection() applies it, as
    // ONE undo step, to the selected wire - or to every wire on the
    // selected part or junction - and returns how many changed. Freeform
    // (non-ortho) wires are the author's and are left alone.
    [[nodiscard]] static std::vector<Pos> tidy_route(
        const std::vector<Pos>& path);
    std::size_t tidy_selection();

    // Rotate / mirror a placed part to the GIVEN orientation (the caller
    // owns the rot/mirror semantics - this does not guess the units) and
    // keep its ortho wires straight, all as ONE undo step. The part
    // visits the new orientation, the attached wires re-route from the
    // pins' resolved positions there, then it goes back so the command
    // captures the real "was" (the drag-release trick). False when the
    // part is gone or there is no document.
    bool transform_part(std::uint32_t id, int rot, bool mirror,
                        std::string name);

    // Annotate: renumbers every part that has a reference prefix
    // (R, C, U...) 1..N per prefix in reading order - top to bottom in
    // rows, left to right within a row - as ONE undo step. Parts whose
    // ref is custom (not prefix+digits, e.g. "R_load") are left alone
    // and never collide, since generated refs are always prefix+digits.
    // Rows: parts within `row_tol_cells` grid cells of the row's top
    // part share a row (hand-placed symbols are rarely pixel-exact).
    // Warns when a renamed ref is named in the directives. Returns how
    // many refs changed.
    std::size_t annotate(double row_tol_cells = 2.0);
    // The pure ordering core: indices into `at` in reading order.
    [[nodiscard]] static std::vector<std::size_t> annotation_order(
        const std::vector<Pos>& at, double row_tol);

    // The current selection's world extent (nullopt = nothing selected
    // or the selection's entity is gone), and the "frame selected"
    // command built on it: fits the selection with `margin` grid units
    // of air (a lone junction/label just centers, keeping the zoom).
    // Returns false (with a status hint) when there is nothing to frame.
    [[nodiscard]] std::optional<Bounds> selection_bounds() const;
    bool zoom_to_selection(double margin = 3.0);

    // The Place tool's "pin magnetism": the snapped position for a
    // ghost of `sym` at `world`, nudged so the ghost's nearest pin
    // lands exactly on an existing pin within `magnet_radius` (world
    // units; <= 0 turns it off and this is just snap()). A part dropped
    // near another one mates pin-to-pin instead of a cell off.
    [[nodiscard]] Pos place_snap(const ws::circsim::SymbolDef& sym, Pos world,
                                 int rot, bool mirror, bool coarse,
                                 double magnet_radius) const;

    // A one-line, context-sensitive hint for the status bar - what the
    // pointer and keys do RIGHT NOW (tool, draft, drag). Static text,
    // safe to call every frame; the app feeds it to its status line.
    [[nodiscard]] std::string_view hint() const;

    // Select-and-zoom for a Problems row (locate.hpp's seam).
    void jump_to(const ws::circsim::DiagTarget& t);

    // Cache refreshers (the commands' act; synchronous - ERC and the
    // export are millisecond work at editor scales). `version` is the
    // schematic version the check ran AGAINST: the synchronous ERC
    // path passes the live version; a delivered run passes the version
    // its job snapshot carried at submit time, so an edit that landed
    // mid-run leaves the Problems panel honestly stale instead of
    // re-stamping itself fresh.
    void refresh_problems(bool from_run,
                          std::vector<ws::circsim::Diag> diags,
                          std::string header, std::uint64_t version);
    void run_erc();
    void refresh_netlist();

    // The canvas' auto-junction dots (stage 18): the silent joins the
    // net rules make - pins a wire's path picked up, vertices T-riding
    // another wire - cached against the schematic version, one rebuild
    // per edit, not per frame. Each dot pairs with the NET it landed on
    // (the id net_cache answers), so the canvas can paint it in the
    // net's color.
    [[nodiscard]] const std::vector<std::pair<Pos, std::uint32_t>>&
    auto_junction_dots();

    // The canvas's net build (the auto-junction dots' own seam, shared):
    // one build_nets per edit, cached against the schematic version.
    // The wire coloring reads it; ERC and the export keep their own
    // builds (the same millisecond work, independent staleness).
    [[nodiscard]] const ws::circsim::NetList& net_cache();

    // The canvas's .tran table: the Results panel's selected table when
    // it IS a time sweep, else the run's first - what the transport
    // strip scrubs and the playback animates. Null when the run has no
    // transient (or no run at all).
    [[nodiscard]] const ws::spice::Table* canvas_tran_table() const;

    // The table's sample the canvas should show: the playback playhead
    // resolved onto the time axis (a .tran axis is strictly increasing;
    // the LAST sample at or before t, clamped to the ends). For a
    // non-time table the answer is its last row - the sweep's end, the
    // state an .op reader means by "the answer".
    [[nodiscard]] static std::size_t shown_sample(
        const ws::spice::Table& t, double playhead);

    // Clear every per-document surface (the app calls this when the
    // active document changes).
    void document_changed();

    // The toast pair the commands and panels use. `seconds` is the
    // toast's lifetime (the stock 4s; the chord loop's command errors
    // ask for 5) - the text rides toast_text_'s stable slots either
    // way.
    void toast(std::string text, gui::ToastSeverity severity,
               float seconds = 4.0f) const;
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

// The data-color wheel every colored series in the app shares (i mod N,
// the scope's rule): the Results plot's signals and the canvas's per-NET
// wire coloring both ride it, so a net and its waveform can never look
// like strangers. Pure data - no canvas, no theme.
[[nodiscard]] std::uint32_t series_color(std::size_t i);

}  // namespace editor
