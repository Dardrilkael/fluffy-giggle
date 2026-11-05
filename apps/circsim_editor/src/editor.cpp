// apps/circsim_editor - the shared core's implementation (see
// editor.hpp). The pure geometry lives here so the panels and the
// commands share one truth: snapping, bounds, hit-testing, the ortho
// route, the symbol renderer and the cache refreshers.

#include "editor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <ws/core/assert.hpp>
#include <ws/circsim/erc.hpp>

namespace editor {
namespace {

using canvas::Paint;
using canvas::Point;
using ws::circsim::JunctionRef;
using ws::circsim::PinRef;

[[nodiscard]] double len(Pos p) {
    return std::sqrt(p.x * p.x + p.y * p.y);
}

// The label pick's extra reach, in screen px beside the pick radius:
// the text's own footprint is wider than its anchor point, so the
// click target grows by a few pixels - never a fixed world unit
// (which at max zoom answered clicks 400px away).
constexpr double kLabelPickPx = 6.0;

// Squared distance from `p` to segment ab.
[[nodiscard]] double dist_to_segment(Pos a, Pos b, Pos p) {
    const Pos ab{b.x - a.x, b.y - a.y};
    const Pos ap{p.x - a.x, p.y - a.y};
    const double denom = ab.x * ab.x + ab.y * ab.y;
    if (denom <= 0.0) {
        return len(ap);
    }
    double t = (ap.x * ab.x + ap.y * ab.y) / denom;
    t = std::max(0.0, std::min(1.0, t));
    return len(Pos{ap.x - t * ab.x, ap.y - t * ab.y});
}

}  // namespace

const ws::circsim::SymbolDef* Editor::sym_of(
    const ws::circsim::Part& p) const {
    return library.find(p.symbol);
}

canvas::GridSpec Editor::grid_spec() const {
    // The schematic's own grid is the base; everything else (faces,
    // thresholds, cap) is the spec's stock, which the panel paints
    // over with the theme's tokens.
    canvas::GridSpec spec;
    const ws::circsim::Schematic* s = schem();
    spec.base = s != nullptr && s->grid > 0.0 ? s->grid : 1.0;
    spec.major_interval = 5;  // the x5 ladder the draw assumes
    return spec;
}

Pos Editor::snap(Pos world, bool coarse) const {
    // snap_coord reads the SAME spec the grid renderer draws (the
    // drawn dots and the snap targets can never disagree).
    const canvas::GridSpec spec = grid_spec();
    return {canvas::snap_coord(world.x, spec, coarse),
            canvas::snap_coord(world.y, spec, coarse)};
}

std::optional<Bounds> Editor::part_bounds(std::uint32_t id) const {
    const ws::circsim::Schematic* s = schem();
    const ws::circsim::Part* p = s != nullptr ? s->find_part(id) : nullptr;
    if (p == nullptr) {
        return std::nullopt;
    }
    // A new version retires the WHOLE cache, not just the entry being
    // asked for: an edit may have moved, rotated or deleted any part,
    // and an entry computed against an older version must never
    // answer for the new one. (Stamping the new version on a single
    // recomputed entry once left every OTHER part serving stale
    // boxes - the dragged part's highlight and refdes text lagged at
    // the pre-drag position while the symbol itself moved.)
    if (!bounds_valid_ || bounds_version_ != s->version()) {
        part_bounds_cache_.clear();
        bounds_version_ = s->version();
        bounds_valid_ = true;
    }
    const auto cached = part_bounds_cache_.find(id);
    if (cached != part_bounds_cache_.end()) {
        return cached->second;
    }
    // Cache miss: compute and store.
    const ws::circsim::SymbolDef* sym = sym_of(*p);
    if (sym == nullptr) {
        Bounds b{p->pos.x - 1.0, p->pos.y - 1.0, p->pos.x + 1.0,
                 p->pos.y + 1.0};
        part_bounds_cache_[id] = b;
        return b;
    }
    const Bounds local = ws::circsim::symbol_bounds(*sym);
    std::optional<Bounds> out;
    const auto grow = [&](Pos w) {
        if (!out.has_value()) {
            out = Bounds{w.x, w.y, w.x, w.y};
            return;
        }
        out->min_x = std::min(out->min_x, w.x);
        out->min_y = std::min(out->min_y, w.y);
        out->max_x = std::max(out->max_x, w.x);
        out->max_y = std::max(out->max_y, w.y);
    };
    for (const Pos corner :
         {Pos{local.min_x, local.min_y}, Pos{local.max_x, local.min_y},
          Pos{local.min_x, local.max_y}, Pos{local.max_x, local.max_y}}) {
        grow(ws::circsim::transform_pos(*p, corner));
    }
    if (out.has_value()) {
        part_bounds_cache_[id] = *out;
    }
    return out;
}

std::vector<Pos> Editor::wire_path(const ws::circsim::Wire& w) const {
    std::vector<Pos> out;
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return out;
    }
    const auto endpoint_pos = [&](const ws::circsim::WireEndpoint& end)
        -> std::optional<Pos> {
        if (const PinRef* pin = std::get_if<PinRef>(&end)) {
            const ws::circsim::Part* p = s->find_part(pin->part);
            const ws::circsim::SymbolDef* sym =
                p != nullptr ? sym_of(*p) : nullptr;
            if (p != nullptr && sym != nullptr &&
                pin->pin < sym->pins.size()) {
                return ws::circsim::pin_pos(*sym, *p, pin->pin);
            }
            return std::nullopt;
        }
        const JunctionRef* j = std::get_if<JunctionRef>(&end);
        const ws::circsim::Junction* found =
            j != nullptr ? s->find_junction(j->junction) : nullptr;
        if (found != nullptr) {
            return found->pos;
        }
        return std::nullopt;
    };
    const std::optional<Pos> a = endpoint_pos(w.a);
    const std::optional<Pos> b = endpoint_pos(w.b);
    if (!a.has_value() || !b.has_value()) {
        return out;  // dangling: the loader's tolerance, nothing to draw
    }
    out.reserve(w.waypoints.size() + 2);
    out.push_back(*a);
    for (const Pos& p : w.waypoints) {
        out.push_back(p);
    }
    out.push_back(*b);
    return out;
}

std::vector<Pos> Editor::route_wire(Pos a, int a_dir, Pos b, int b_dir) {
    // The lead length: how far a route commits to a direction before
    // it may turn. One grid unit reads clean at every zoom (a pin's
    // own lead is typically three).
    constexpr double kLead = 1.0;
    constexpr double kEps = 1e-9;

    // Directions: negative = free; non-negative dirs are folded into
    // [0, 360). A negative pin dir is "free" (no forced lead-out).
    // Note: pin dirs are always non-negative cardinals (0, 90, 180, 270)
    // so negative is only used for "free" wire drafts.
    const auto norm = [](int dir) { return dir < 0 ? -1 : dir % 360; };
    a_dir = norm(a_dir);
    b_dir = norm(b_dir);

    const auto unit = [](int dir) -> Pos {
        switch (dir) {
            case 0: return {1.0, 0.0};
            case 90: return {0.0, 1.0};
            case 180: return {-1.0, 0.0};
            case 270: return {0.0, -1.0};
            default:
                WS_CORE_ASSERT(false && "route_wire: non-cardinal pin direction");
                return {0.0, -1.0};  // fallback to south (270)
        }
    };
    const auto same = [&](Pos p, Pos q) {
        return eps_eq(p.x, q.x, kEps) && eps_eq(p.y, q.y, kEps);
    };
    if (same(a, b)) {
        return {};  // a zero-length wire has no route (not a loop)
    }

    Pos from = a;  // where the route is free to turn: past a's lead
    if (a_dir >= 0) {
        const Pos u = unit(a_dir);
        from = {a.x + u.x * kLead, a.y + u.y * kLead};
    }
    Pos target = b;  // ... and the point the route aims for before b
    if (b_dir >= 0) {
        const Pos u = unit(b_dir);
        // Approach b ALONG b_dir: stop one lead short of it.
        target = {b.x - u.x * kLead, b.y - u.y * kLead};
    }

    // Full polyline a -> ... -> b; the stored waypoints are its
    // interior after folding. A corner is needed only when the free
    // point past a's lead and the approach point before b share
    // neither axis (aligned endpoints fold away).
    const bool corner_needed = !eps_eq(from.x, target.x, kEps) &&
                               !eps_eq(from.y, target.y, kEps);
    std::vector<Pos> full;
    full.push_back(a);
    if (a_dir >= 0) {
        full.push_back(from);
    }
    if (corner_needed) {
        // One corner (an L): the leg out of `from` continues the
        // committed direction (horizontal when free), the leg into
        // `target` closes. The final approach leg target -> b already
        // runs along b_dir, so one corner always suffices.
        const bool start_horizontal =
            a_dir < 0 || a_dir == 0 || a_dir == 180;
        if (start_horizontal) {
            full.push_back(Pos{target.x, from.y});
        } else {
            full.push_back(Pos{from.x, target.y});
        }
    }
    if (b_dir >= 0) {
        full.push_back(target);
    }
    full.push_back(b);

    // Fold duplicates and collinear middles (these become the stored
    // waypoints; noise would ride straight into the .csim). A fold
    // needs the leg to CONTINUE: a reversal is collinear too, but
    // folding it would drop the lead and run the wire straight back
    // through the part it just left - a pin at (0,0) facing +x with a
    // target at (-5,0) keeps its (1,0) lead-out corner.
    std::vector<Pos> folded;
    for (const Pos& p : full) {
        if (!folded.empty() && same(folded.back(), p)) {
            continue;
        }
        if (folded.size() >= 2u) {
            const Pos& p0 = folded[folded.size() - 2u];
            const Pos& p1 = folded[folded.size() - 1u];
            const bool collinear =
                (eps_eq(p0.x, p1.x, kEps) && eps_eq(p1.x, p.x, kEps)) ||
                (eps_eq(p0.y, p1.y, kEps) && eps_eq(p1.y, p.y, kEps));
            const double along = (p1.x - p0.x) * (p.x - p1.x) +
                                 (p1.y - p0.y) * (p.y - p1.y);
            if (collinear && along > 0.0) {
                folded.back() = p;  // the leg extends
                continue;
            }
        }
        folded.push_back(p);
    }
    if (folded.size() < 2u) {
        return {};
    }
    return {folded.begin() + 1, folded.end() - 1};
}

// The attached ortho wires' fresh shapes for a move of `id` (kind =
// the entity kind being moved), read at the entity's LANDING: the
// editor computes the new routes; push_move_* captures each wire's
// CURRENT waypoints as the undo shape. Without the re-route a moved
// endpoint left the adjacent wire legs diagonal (waypoints are
// absolute, not relative).
std::vector<WireRoute> Editor::attached_wire_routes(
    Drag::Kind kind, std::uint32_t id) const {
    constexpr double kEps = 1e-9;
    std::vector<WireRoute> out;
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return out;
    }
    const auto hits = [&](const ws::circsim::WireEndpoint& e) {
        if (kind == Drag::Kind::Part) {
            const auto* pin = std::get_if<PinRef>(&e);
            return pin != nullptr && pin->part == id;
        }
        const auto* j = std::get_if<JunctionRef>(&e);
        return j != nullptr && j->junction == id;
    };
    for (const ws::circsim::Wire& w : s->wires()) {
        // A cosmetic freeform wire's shape is the author's - only the
        // ortho constraint re-routes.
        if (!w.ortho) {
            continue;
        }
        const bool a_hit = hits(w.a);
        const bool b_hit = hits(w.b);
        if (!a_hit && !b_hit) {
            continue;
        }
        const std::vector<Pos> path = wire_path(w);
        if (path.size() < 2u) {
            continue;  // dangling: nothing to keep straight
        }
        // Straight or one-bend wires (and anything with both ends on
        // the same move) re-route as a fresh horizontal-first L; a
        // multi-bend wire keeps its shape - only the adjacent waypoint
        // slides, chosen so both touching legs stay axis-aligned.
        std::vector<Pos> mid(path.begin() + 1, path.end() - 1);
        if (mid.size() <= 1u || (a_hit && b_hit)) {
            WireRoute r;
            r.wire = w.id;
            r.waypoints = route_wire(path.front(), -1, path.back(), -1);
            out.push_back(std::move(r));
            continue;
        }
        if (a_hit) {
            // The leg into mid[1] keeps its orientation: a vertical
            // continuation keeps x (the first leg turns horizontal),
            // a horizontal one keeps y (the first leg turns vertical).
            if (eps_eq(mid[0].x, mid[1].x, kEps)) {
                mid[0].y = path.front().y;
            } else {
                mid[0].x = path.front().x;
            }
        }
        if (b_hit) {
            Pos& last = mid.back();
            const Pos& prev = mid[mid.size() - 2u];
            if (eps_eq(prev.y, last.y, kEps)) {
                last.x = path.back().x;
            } else {
                last.y = path.back().y;
            }
        }
        WireRoute r;
        r.wire = w.id;
        r.waypoints = std::move(mid);
        out.push_back(std::move(r));
    }
    return out;
}

std::optional<PinHit> Editor::hit_pin(Pos world, double radius) const {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return std::nullopt;
    }
    std::optional<PinHit> best;
    double best_d = radius;
    for (const ws::circsim::Part& p : s->parts()) {
        const ws::circsim::SymbolDef* sym = sym_of(p);
        if (sym == nullptr) {
            continue;
        }
        for (std::size_t i = 0; i < sym->pins.size(); ++i) {
            const Pos at = ws::circsim::pin_pos(*sym, p,
                                                static_cast<std::uint32_t>(i));
            const double d = len(Pos{at.x - world.x, at.y - world.y});
            if (d < best_d) {
                best_d = d;
                best = PinHit{PinRef{p.id, static_cast<std::uint32_t>(i)}, at,
                              sym->pins[i].dir};
            }
        }
    }
    return best;
}

std::optional<std::uint32_t> Editor::hit_junction(Pos world,
                                                  double radius) const {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return std::nullopt;
    }
    std::optional<std::uint32_t> best;
    double best_d = radius;
    for (const ws::circsim::Junction& j : s->junctions()) {
        const double d = len(Pos{j.pos.x - world.x, j.pos.y - world.y});
        if (d < best_d) {
            best_d = d;
            best = j.id;
        }
    }
    return best;
}

// A point on an existing wire's path the cursor would land on (stage
// 18b): for every segment, project the cursor onto the segment, snap
// the projection to the fine grid, and accept the snap when it stays
// on the segment (the exact check nets.cpp will run later); otherwise
// fall back to the segment's nearest endpoint (a path vertex, always
// grid-aligned). Among all segments' candidates, return the one
// closest to the cursor. route_wire's paths are ortho-on-grid, so the
// common case lands inside the segment; the fallback is the rare
// near-endpoint click, where the vertex is the right answer anyway.
std::optional<Editor::WirePointSnap> Editor::hit_wire_point(
    Pos world, double radius) const {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return std::nullopt;
    }
    std::optional<WirePointSnap> best;
    double best_d = radius;
    const canvas::GridSpec spec = grid_spec();
    const auto snap_one = [&](double v) {
        return canvas::snap_coord(v, spec, false);
    };
    for (const ws::circsim::Wire& w : s->wires()) {
        const std::vector<Pos> path = wire_path(w);
        for (std::size_t i = 1; i < path.size(); ++i) {
            const Pos a = path[i - 1];
            const Pos b = path[i];
            const Pos ab{b.x - a.x, b.y - a.y};
            const Pos ap{world.x - a.x, world.y - a.y};
            const double denom = ab.x * ab.x + ab.y * ab.y;
            double t = denom > 0.0 ? (ap.x * ab.x + ap.y * ab.y) / denom
                                  : 0.0;
            t = std::max(0.0, std::min(1.0, t));
            const Pos proj{a.x + t * ab.x, a.y + t * ab.y};
            // Snap the projection to the fine grid; for an ortho
            // segment one coordinate is the segment's stationary
            // value (already grid-aligned) and snaps to itself, the
            // other rounds to the nearest grid line.
            Pos cand{snap_one(proj.x), snap_one(proj.y)};
            // Verify cand lies on segment ab with exact equality
            // (the same on_segment nets.cpp runs). If not, the grid
            // snap drifted off the segment - use the nearer endpoint.
            // Note: the endpoint may be a pin or junction position.
            // snap_wire_target checks pin -> junction -> wire-point in
            // that order with the same radius, so the caller is safe
            // today - but this is a fragile ordering dependency. A more
            // robust approach would explicitly skip endpoints that are
            // pins/junctions here.
            const double cross =
                (b.x - a.x) * (cand.y - a.y) - (b.y - a.y) * (cand.x - a.x);
            const bool on_seg =
                cross == 0.0 &&
                cand.x >= std::min(a.x, b.x) && cand.x <= std::max(a.x, b.x) &&
                cand.y >= std::min(a.y, b.y) && cand.y <= std::max(a.y, b.y);
            if (!on_seg) {
                const double da = len(Pos{a.x - world.x, a.y - world.y});
                const double db = len(Pos{b.x - world.x, b.y - world.y});
                cand = da <= db ? a : b;
            }
            // Accept on the SEGMENT's distance to the cursor; the
            // returned candidate is still the grid-aligned point the
            // junction will be minted at. Measuring the distance to
            // the SNAPPED candidate missed a cursor riding the wire
            // between grid intersections at high zoom - its snap sits
            // half a cell away.
            const double d = dist_to_segment(a, b, world);
            if (d <= best_d) {
                best_d = d;
                best = WirePointSnap{w.id, cand};
            }
        }
    }
    return best;
}

Selection Editor::hit_any(Pos world, double radius) const {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return {};
    }
    // Priority walks the DRAW STACK topmost-first: a junction dot
    // sits on every wire end and crossing; a wire runs over the
    // canvas and stays selectable even where it crosses or ends
    // inside a part's box (the box is bigger than the part's ink);
    // a click on a part body with no wire under it selects the part;
    // labels ride under everything. A click that lands on a pin
    // therefore still selects whatever wire lands there first - the
    // pin itself has no separate selection (the part's Properties
    // open from the part body).
    if (const std::uint32_t j = hit_junction(world, radius).value_or(0)) {
        return {Selection::Kind::Junction, j};
    }
    // The NEAREST wire in range - the loop once returned on the first
    // segment that matched, leaving its best_d tracking dead.
    std::uint32_t best_wire = 0;
    bool wire_found = false;
    double best_d = radius;
    for (const ws::circsim::Wire& w : s->wires()) {
        const std::vector<Pos> path = wire_path(w);
        for (std::size_t i = 1; i < path.size(); ++i) {
            const double d = dist_to_segment(path[i - 1], path[i], world);
            if (d <= best_d) {
                best_d = d;
                best_wire = w.id;
                wire_found = true;
            }
        }
    }
    if (wire_found) {
        return {Selection::Kind::Wire, best_wire};
    }
    // The LAST part whose box holds the point: the parts draw in list
    // order, so the last one is the one on top (the first one used to
    // win - a stacked pair selected the buried one).
    std::uint32_t best_part = 0;
    bool part_found = false;
    for (const ws::circsim::Part& p : s->parts()) {
        const std::optional<Bounds> b = part_bounds(p.id);
        if (b.has_value() && world.x >= b->min_x && world.x <= b->max_x &&
            world.y >= b->min_y && world.y <= b->max_y) {
            best_part = p.id;  // no break: the topmost wins
            part_found = true;
        }
    }
    if (part_found) {
        return {Selection::Kind::Part, best_part};
    }
    // The label reach rides the zoom (a few screen px beside the pick
    // radius) - a fixed world-unit margin once answered clicks a full
    // grid cell (400px at max zoom) away from the text.
    const double label_reach =
        radius + (view.zoom > 0.0 ? kLabelPickPx / view.zoom : 0.0);
    for (const ws::circsim::Label& l : s->labels()) {
        if (len(Pos{l.pos.x - world.x, l.pos.y - world.y}) <= label_reach) {
            return {Selection::Kind::Label, l.id};
        }
    }
    return {};
}

std::optional<Bounds> Editor::content_bounds() const {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return std::nullopt;
    }
    std::optional<Bounds> out;
    const auto grow = [&](Pos w) {
        if (!out.has_value()) {
            out = Bounds{w.x, w.y, w.x, w.y};
            return;
        }
        out->min_x = std::min(out->min_x, w.x);
        out->min_y = std::min(out->min_y, w.y);
        out->max_x = std::max(out->max_x, w.x);
        out->max_y = std::max(out->max_y, w.y);
    };
    for (const ws::circsim::Part& p : s->parts()) {
        const std::optional<Bounds> b = part_bounds(p.id);
        if (b.has_value()) {
            grow(Pos{b->min_x, b->min_y});
            grow(Pos{b->max_x, b->max_y});
        }
    }
    for (const ws::circsim::Junction& j : s->junctions()) {
        grow(j.pos);
    }
    for (const ws::circsim::Label& l : s->labels()) {
        grow(l.pos);
    }
    for (const ws::circsim::Wire& w : s->wires()) {
        for (const Pos& p : wire_path(w)) {
            grow(p);
        }
    }
    return out;
}

void Editor::jump_to(const ws::circsim::DiagTarget& t) {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return;
    }
    switch (t.kind) {
        case ws::circsim::DiagTarget::Kind::Part:
            sel = {Selection::Kind::Part, t.part};
            break;
        case ws::circsim::DiagTarget::Kind::Wire:
            sel = {Selection::Kind::Wire, t.obj};
            break;
        case ws::circsim::DiagTarget::Kind::Label:
            sel = {Selection::Kind::Label, t.obj};
            break;
        case ws::circsim::DiagTarget::Kind::Net:
        case ws::circsim::DiagTarget::Kind::Directives:
        case ws::circsim::DiagTarget::Kind::Analysis:
        case ws::circsim::DiagTarget::Kind::None:
            break;
    }
    const std::optional<Bounds> b = ws::circsim::focus_bounds(
        t, *s, problems.nets_valid ? &problems.nets : nullptr, &library);
    if (b.has_value()) {
        view.fit(*b, 3.0);
    }
    if (t.kind == ws::circsim::DiagTarget::Kind::Net) {
        const ws::circsim::Net* n = nullptr;
        for (const ws::circsim::Net& candidate : problems.nets.nets) {
            if (candidate.id == t.net) {
                n = &candidate;
                break;
            }
        }
        if (n != nullptr) {
            status("net " + n->name);
        }
    }
}

void Editor::refresh_problems(bool from_run,
                              std::vector<ws::circsim::Diag> diags,
                              std::string header) {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return;
    }
    problems.has = true;
    problems.from_run = from_run;
    problems.version = s->version();
    problems.header = std::move(header);
    problems.diags = std::move(diags);
    problems.lines.clear();
    std::size_t errors = 0;
    std::size_t warnings = 0;
    for (const ws::circsim::Diag& d : problems.diags) {
        problems.lines.push_back(d.render());
        if (d.severity == ws::circsim::Severity::Error) {
            ++errors;
        } else if (d.severity == ws::circsim::Severity::Warning) {
            ++warnings;
        }
    }
    char tail[64];
    std::snprintf(tail, sizeof tail, " - %zu error%s, %zu warning%s",
                  errors, errors == 1 ? "" : "s", warnings,
                  warnings == 1 ? "" : "s");
    problems.header += tail;
}

void Editor::run_erc() {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return;
    }
    const ws::circsim::NetBuild build =
        ws::circsim::build_nets(*s, &library);
    problems.nets = build.nets;
    problems.nets_valid = true;
    problems.map_valid = false;
    refresh_problems(false, ws::circsim::run_erc(*s, build, &library),
                     "erc");
}

void Editor::refresh_netlist() {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return;
    }
    const ws::circsim::NetBuild build =
        ws::circsim::build_nets(*s, &library);
    const auto deck =
        ws::circsim::export_deck(*s, build.nets, nullptr, nullptr, &library);
    if (!deck.has_value()) {
        netlist.has = false;
        netlist.lines.clear();
        toast("netlist export failed: " + deck.error().message,
              gui::ToastSeverity::error);
        return;
    }
    netlist.has = true;
    netlist.version = s->version();
    netlist.text = deck->text;
    netlist.lines.clear();
    std::size_t at = 0;
    while (at <= netlist.text.size()) {
        const std::size_t nl = netlist.text.find('\n', at);
        netlist.lines.push_back(netlist.text.substr(
            at, nl == std::string::npos ? std::string::npos : nl - at));
        if (nl == std::string::npos) {
            break;
        }
        at = nl + 1;
    }
}

const std::vector<Pos>& Editor::auto_junction_dots() {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        join_dots_.clear();
        joins_valid_ = false;
        return join_dots_;
    }
    if (!joins_valid_ || joins_version_ != s->version()) {
        // The same seam ERC and the export read: one build_nets per
        // edit (millisecond work at editor scales), never per frame.
        const ws::circsim::NetBuild build =
            ws::circsim::build_nets(*s, &library);
        join_dots_.clear();
        for (const ws::circsim::SilentJoin& j : build.silent_joins) {
            join_dots_.push_back(j.pos);
        }
        joins_version_ = s->version();
        joins_valid_ = true;
    }
    return join_dots_;
}

void Editor::document_changed() {
    tool = Tool::Select;
    place_symbol.clear();
    ghost_rot = 0;
    ghost_mirror = false;
    sel.clear();
    drag = Drag{};
    wire_draft = WireDraft{};
    problems = ProblemsCache{};
    netlist = NetlistCache{};
    results = ResultsCache{};
    join_dots_.clear();
    joins_valid_ = false;
    part_bounds_cache_.clear();
    bounds_valid_ = false;
    // The Properties panel's field state rides too: a new document
    // whose first selection happens to share a kind and id with the
    // old one must never show the old document's typing.
    fields.clear();
    field_stamp = 1;
    fields_sel_kind = -1;
    fields_sel_id = 0;
    analysis_index = -1;
    const ws::circsim::Schematic* s = schem();
    if (s != nullptr) {
        view.zoom = s->view.zoom > 0.0 ? s->view.zoom : 26.0;
        view.center = s->view.center;
    } else {
        view.zoom = 26.0;
        view.center = {0.0, 0.0};
    }
}

void Editor::toast(std::string text, gui::ToastSeverity severity) const {
    if (toasts != nullptr) {
        // Toasts borrows the bytes, so the text gets its own stable
        // slot (toast_text_, see editor.hpp). The old function-static
        // holder was ONE buffer shared by every toast: two toasts in a
        // frame showed the same text, and the reassignment could free
        // the bytes an earlier, still-live toast pointed at.
        constexpr std::size_t kMaxLive = 16;
        toast_text_.push_back(std::move(text));
        while (toast_text_.size() > kMaxLive) {
            toast_text_.pop_front();
        }
        toasts->push({toast_text_.back().c_str(), severity, nullptr, 4.0f});
    }
}

void Editor::status(std::string text) const {
    if (set_status) {
        set_status(std::move(text));
    }
}

// --- the symbol renderer ------------------------------------------------------

void draw_symbol(canvas::Canvas2D& c, const ws::circsim::SymbolDef& sym,
                 Pos pos, int rot, bool mirror, const Paint& body,
                 std::uint32_t text_color, const ws::ui::r2d::Font* font) {
    // The geometry lives in ws::circsim's render seam (render_symbol):
    // one pure flattening shared with the CLI's SVG dump and the tests.
    // This is the painting half only - strokes to canvas calls.
    ws::circsim::Part placement{};
    placement.pos = pos;
    placement.rot = rot;
    placement.mirror = mirror;
    constexpr double kRad = 3.14159265358979323846 / 180.0;
    for (const ws::circsim::SymbolStroke& s :
         ws::circsim::render_symbol(sym, placement)) {
        const auto pt = [&](std::size_t i) {
            return Point{static_cast<float>(s.pts[i].x),
                         static_cast<float>(s.pts[i].y)};
        };
        switch (s.kind) {
            case ws::circsim::SymbolStroke::Kind::Line:
                c.stroke_line(pt(0), pt(1), body);
                break;
            case ws::circsim::SymbolStroke::Kind::Poly: {
                std::vector<Point> pts;
                pts.reserve(s.pts.size());
                for (std::size_t i = 0; i < s.pts.size(); ++i) {
                    pts.push_back(pt(i));
                }
                c.stroke_polyline(pts.data(),
                                  static_cast<int>(pts.size()), body,
                                  s.closed);
                break;
            }
            case ws::circsim::SymbolStroke::Kind::PolyFill: {
                std::vector<Point> pts;
                pts.reserve(s.pts.size());
                for (std::size_t i = 0; i < s.pts.size(); ++i) {
                    pts.push_back(pt(i));
                }
                if (pts.size() >= 3u) {
                    c.fill_polygon(pts.data(),
                                   static_cast<int>(pts.size()), body.color);
                }
                break;
            }
            case ws::circsim::SymbolStroke::Kind::Circle:
                c.stroke_circle(pt(0), static_cast<float>(s.r), body);
                break;
            case ws::circsim::SymbolStroke::Kind::Arc:
                c.arc(pt(0), static_cast<float>(s.r),
                      static_cast<float>(s.r),
                      static_cast<float>(s.a0 * kRad),
                      static_cast<float>(s.a1 * kRad));
                c.stroke(body);
                break;
            case ws::circsim::SymbolStroke::Kind::Dot:
                c.fill_circle(pt(0), static_cast<float>(s.r), body.color);
                break;
            case ws::circsim::SymbolStroke::Kind::Text:
                if (font != nullptr && !s.text.empty()) {
                    (void)c.draw_text(pt(0).x, pt(0).y, s.text.c_str(),
                                      *font, text_color);
                }
                break;
        }
    }
}

}  // namespace editor
