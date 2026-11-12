#pragma once

// ws::circsim - the schematic interaction geometry: the pure editor
// math over the model, headless and reusable (the editor app's panels,
// any second frontend, a batch CLI). Everything here was lifted
// verbatim from apps/circsim_editor's editor.cpp/document.cpp, where it
// lived beside the Editor struct it served; the app keeps thin
// forwarders (Editor::route_wire and friends) so its own API is
// unchanged - this header is where the LOGIC lives now.
//
// The families:
//   * polylines - the ortho router (route_wire), the tidy pass
//     (tidy_route) and the shared fold (duplicate/collinear-middle
//     folding, the .csim waypoint contract);
//   * refdes - next_ref (the designator minter), is_auto_ref /
//     mentions_token (what Annotate may rename, what a rename can
//     re-aim) and annotation_order (the reading-order sort);
//   * bounds - a part's transformed world AABB (part_world_bounds), a
//     wire's resolved world polyline (wire_path) and the whole sheet's
//     extent (content_bounds);
//   * picking - hit_pin / hit_junction (the nearest entity within a
//     world radius) and hit_wire_point (the T-tap snap: a grid-aligned
//     point ON a wire's path, bit-equal to a position nets.cpp's
//     exact-double on_polyline accepts);
//   * placement - magnet_shift, the Place tool's pin magnetism (the
//     nudge that mates a ghost's nearest pin onto a real one), and
//     attached_wire_routes, the re-route shapes that keep a moved
//     part's/junction's ortho wires straight;
//   * colors - series_color, the data-color wheel every colored series
//     in the workspace shares (the Results plot's signals and the
//     canvas's per-net wire coloring), so a net and its waveform can
//     never look like strangers.
//
// No ui include anywhere - the grid the T-tap snaps to arrives as a
// spacing (the caller owns which lattice; the editor app passes its
// canvas GridSpec's base), and every position is model-world (grid
// units, y up).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ws/circsim/library.hpp>
#include <ws/circsim/model.hpp>

namespace ws::circsim {

// --- polylines -----------------------------------------------------------

// The ortho route for a wire from `a` (leaving along `a_dir`, -1 =
// free) to `b` (approached along `b_dir`, -1 = free): the waypoint
// list between the two endpoints - a leads out, an L lands, collinear
// legs folded away (a reversal keeps its lead-out corner; folding it
// would run the wire back through the part body it just left).
// `flip` bends the corner the other way (vertical-first instead of
// horizontal-first) - the Tab toggle; it only matters when the route
// needs a corner at all.
[[nodiscard]] std::vector<Pos> route_wire(Pos a, int a_dir, Pos b,
                                          int b_dir, bool flip = false);

// Re-straightens an ortho wire's full world path (endpoint, waypoints,
// endpoint): every diagonal leg gets an axis-aligned corner, duplicate
// points and collinear continuing middles fold away, the rest of the
// author's shape is kept (a gentle fix-up, not a re-route). Returns
// the cleaned INTERIOR waypoints.
[[nodiscard]] std::vector<Pos> tidy_route(const std::vector<Pos>& path);

// --- refdes ----------------------------------------------------------------

// The next free reference designator for a symbol ("R1", "R2", ...):
// the highest trailing number on the symbol's own prefix, plus one -
// no gap reuse, deterministic. Ref-less symbols (power/ground) answer
// "" (the symbol's contract).
[[nodiscard]] std::string next_ref(const Schematic& s,
                                   const SymbolDef& sym);

// prefix + digits only ("R12"): a ref Annotate may regenerate. "R_load"
// and "TP1a" are the author's and stay put.
[[nodiscard]] bool is_auto_ref(const std::string& ref,
                               const std::string& prefix);

// Is `token` named in `text` as a whole word (ASCII case-insensitive,
// like SPICE)? "R1" matches ".meas I(R1)" but not "R10" or "XR1" -
// the guard behind Annotate's renamed-ref-in-directives warning.
[[nodiscard]] bool mentions_token(const std::string& text,
                                  const std::string& token);

// The pure ordering core of Annotate: indices into `at` in reading
// order - top to bottom in rows (within `row_tol`), left to right
// within a row (world y points up).
[[nodiscard]] std::vector<std::size_t> annotation_order(
    const std::vector<Pos>& at, double row_tol);

// --- bounds -----------------------------------------------------------------

// Grows a world AABB point by point.
struct BoundsBuilder {
    std::optional<Bounds> box;

    void add(Pos w);
    void add(const Bounds& b);
};

// A placed part's world AABB: the symbol's local bounds through the
// placement's transform, corner by corner. A part whose symbol the
// library cannot find answers a 1x1 box at its anchor (the honest
// degrade; the drawing does the same).
[[nodiscard]] std::optional<Bounds> part_world_bounds(
    const Schematic& s, const SymbolLibrary& lib, std::uint32_t part_id);

// The wire's world polyline: endpoints through their refs (a pin
// through its part's placement, a junction through its position), then
// the waypoints between. A dangling endpoint (the loader's tolerance)
// answers EMPTY - there is nothing to draw or hit.
[[nodiscard]] std::vector<Pos> wire_path(const Schematic& s,
                                         const SymbolLibrary& lib,
                                         const Wire& w);

// The whole schematic's content extent (for a fit command): every
// part's bounds, every junction and label position, every wire vertex.
[[nodiscard]] std::optional<Bounds> content_bounds(
    const Schematic& s, const SymbolLibrary& lib);

// --- picking ----------------------------------------------------------------

// A hovered pin (the wire tool's target highlight, the magnetism's
// mate target).
struct PinHit {
    PinRef ref{};
    Pos pos{0.0, 0.0};
    int dir = 0;  // the pin's pointing direction, degrees
};

// The nearest pin within `radius` of `world` (world units), or none.
// Distance is the honest metric, so picks stay steady across zooms
// when the caller converts its pixel radius (screen px / zoom).
[[nodiscard]] std::optional<PinHit> hit_pin(const Schematic& s,
                                            const SymbolLibrary& lib,
                                            Pos world, double radius);

// The nearest junction within `radius` of `world`, or none.
[[nodiscard]] std::optional<std::uint32_t> hit_junction(
    const Schematic& s, Pos world, double radius);

// A point on an existing wire's path the cursor would land on: for
// every segment, project the cursor, snap the projection to the FINE
// grid (`grid` - the caller's lattice spacing), and accept the snap
// when it stays on the segment; otherwise fall back to the segment's
// nearer endpoint (a path vertex, always grid-aligned). Among all
// candidates, the segment closest to the cursor wins. The returned
// point is bit-equal to a position the wire's path passes through, so
// nets.cpp's exact-double on_polyline accepts the junction minted
// there - the standard EDA T-tap.
struct WirePointSnap {
    std::uint32_t wire = 0;  // the wire whose path was hit
    Pos pos{0.0, 0.0};       // grid-aligned, on the wire's path
};
[[nodiscard]] std::optional<WirePointSnap> hit_wire_point(
    const Schematic& s, const SymbolLibrary& lib, Pos world,
    double radius, double grid);

// Distance (not squared) from `p` to segment ab - the wire pick's
// metric, shared with anything that walks a polyline.
[[nodiscard]] double dist_to_segment(Pos a, Pos b, Pos p);

// --- placement ----------------------------------------------------------------

// The Place tool's pin magnetism: where a ghost of `sym` at the
// ALREADY-SNAPPED `base` (rot/mirror applied) actually lands - nudged
// whole so its nearest pin coincides with a real pin within
// `magnet_radius` (world units). A part dropped near another one mates
// pin-to-pin instead of a cell off; no pin in range answers `base`
// unchanged. The snap itself is the caller's (it owns the lattice).
[[nodiscard]] Pos magnet_shift(const Schematic& s,
                               const SymbolLibrary& lib,
                               const SymbolDef& sym, Pos base, int rot,
                               bool mirror, double magnet_radius);

// The attached ortho wires' fresh shapes for a move of `id` (kind =
// the entity kind being moved), read at the entity's LANDING:
// straight or one-bend wires (and anything with both ends on the same
// move) re-route as a fresh horizontal-first L; a multi-bend wire
// keeps its shape - only the adjacent waypoint slides, chosen so both
// touching legs stay axis-aligned. Freeform (non-ortho) wires are the
// author's and are left alone. The undo commands capture each wire's
// CURRENT waypoints as the undo shape beside these.
struct WireRoute {
    std::uint32_t wire = 0;
    std::vector<Pos> waypoints;
};
enum class AttachedKind : std::uint8_t { Part, Junction };
[[nodiscard]] std::vector<WireRoute> attached_wire_routes(
    const Schematic& s, const SymbolLibrary& lib, AttachedKind kind,
    std::uint32_t id);

// --- colors ----------------------------------------------------------------

// The data-color wheel every colored series in the workspace shares
// (i mod N, the scope's rule): the Results plot's signals and the
// canvas's per-NET wire coloring both ride it, so a net and its
// waveform can never look like strangers. Pure data - no canvas, no
// theme. 0xRRGGBBAA (the r2d packing).
[[nodiscard]] std::uint32_t series_color(std::size_t i);

}  // namespace ws::circsim
