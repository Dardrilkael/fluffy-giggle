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
    // Fast path: cache hit.
    if (bounds_valid_ && s != nullptr && bounds_version_ == s->version()) {
        const auto it = part_bounds_cache_.find(id);
        if (it != part_bounds_cache_.end()) {
            return it->second;
        }
    }
    // Cache miss or invalid: compute and store.
    const ws::circsim::SymbolDef* sym = sym_of(*p);
    if (sym == nullptr) {
        Bounds b{p->pos.x - 1.0, p->pos.y - 1.0, p->pos.x + 1.0,
                  p->pos.y + 1.0};
        if (s != nullptr) {
            bounds_version_ = s->version();
            part_bounds_cache_[id] = b;
            bounds_valid_ = true;
        }
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
    if (s != nullptr && out.has_value()) {
        bounds_version_ = s->version();
        part_bounds_cache_[id] = *out;
        bounds_valid_ = true;
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

    // a -> [a's lead] -> mid... -> [b's lead] -> b
    const auto assemble = [&](const std::vector<Pos>& mid) {
        std::vector<Pos> full;
        full.push_back(a);
        if (a_dir >= 0) {
            full.push_back(from);
        }
        full.insert(full.end(), mid.begin(), mid.end());
        if (b_dir >= 0) {
            full.push_back(target);
        }
        full.push_back(b);
        return full;
    };

    // A spike is a vertex where the path doubles straight back on
    // itself (a leg that retraces the previous one). The fold below
    // erases a spike by dropping the vertex - and with it a lead - so
    // a route containing one runs back through the pin's own body.
    const auto spikes = [&](const std::vector<Pos>& path) {
        std::vector<Pos> d;
        for (const Pos& p : path) {
            if (d.empty() || !same(d.back(), p)) {
                d.push_back(p);
            }
        }
        int n = 0;
        for (std::size_t i = 2; i < d.size(); ++i) {
            const Pos u{d[i - 1].x - d[i - 2].x, d[i - 1].y - d[i - 2].y};
            const Pos v{d[i].x - d[i - 1].x, d[i].y - d[i - 1].y};
            const double cross = u.x * v.y - u.y * v.x;
            const double dot = u.x * v.x + u.y * v.y;
            if (eps_eq(cross, 0.0, kEps) && dot < 0.0) {
                ++n;
            }
        }
        return n;
    };

    // Candidate routes, most preferred first. The first one is what the
    // router always produced (an L: the leg out of `from` continues the
    // committed direction, horizontal when free; aligned endpoints need
    // no corner). The rest only win when the first one doubles back:
    //   - the other L orientation (the target sits behind a's lead:
    //     turn at once instead of retracing it),
    //   - a jog around (collinear but behind: no L exists, so step
    //     one lead aside, run past, and step back).
    std::vector<std::vector<Pos>> candidates;
    const bool corner_needed = !eps_eq(from.x, target.x, kEps) &&
                               !eps_eq(from.y, target.y, kEps);
    if (corner_needed) {
        const Pos h_first{target.x, from.y};  // along x, then y
        const Pos v_first{from.x, target.y};  // along y, then x
        const bool pref_h = a_dir < 0 || a_dir == 0 || a_dir == 180;
        candidates.push_back(assemble({pref_h ? h_first : v_first}));
        candidates.push_back(assemble({pref_h ? v_first : h_first}));
    } else {
        candidates.push_back(assemble({}));
        if (!same(from, target)) {
            const bool along_x = eps_eq(from.y, target.y, kEps);
            const Pos perp = along_x ? Pos{0.0, kLead} : Pos{kLead, 0.0};
            for (const double sign : {1.0, -1.0}) {
                const Pos j1{from.x + perp.x * sign, from.y + perp.y * sign};
                const Pos j2{target.x + perp.x * sign,
                             target.y + perp.y * sign};
                candidates.push_back(assemble({j1, j2}));
            }
        }
    }
    std::size_t pick = 0;
    int pick_spikes = spikes(candidates[0]);
    for (std::size_t i = 1; i < candidates.size() && pick_spikes > 0; ++i) {
        const int n = spikes(candidates[i]);
        if (n < pick_spikes) {
            pick = i;
            pick_spikes = n;
        }
    }
    const std::vector<Pos>& full = candidates[pick];

    // Fold duplicates and collinear middles (these become the stored
    // waypoints; noise would ride straight into the .csim). A spike
    // that no candidate could avoid still folds away here, as before.
    std::vector<Pos> folded;
    for (const Pos& p : full) {
        if (!folded.empty() && same(folded.back(), p)) {
            continue;
        }
        if (folded.size() >= 2u) {
            const Pos& p0 = folded[folded.size() - 2u];
            const Pos& p1 = folded[folded.size() - 1u];
            if ((eps_eq(p0.x, p1.x, kEps) && eps_eq(p1.x, p.x, kEps)) ||
                (eps_eq(p0.y, p1.y, kEps) && eps_eq(p1.y, p.y, kEps))) {
                folded.back() = p;  // collinear: extend the leg
                // p might equal p0 (spike straight back), creating a
                // consecutive duplicate. Remove it immediately to
                // maintain the "no consecutive duplicates" invariant.
                if (folded.size() >= 2u &&
                    same(folded[folded.size() - 2u], folded.back())) {
                    folded.pop_back();
                }
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
            const double d = len(Pos{cand.x - world.x, cand.y - world.y});
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
    // Parts first (they are the tallest objects), then pins? No: a
    // click that lands on a pin should select the PART (pins are the
    // part's), so parts-before-pins is not a question here. Junctions
    // beat wires (a dot sits on every wire end), wires beat labels.
    for (const ws::circsim::Part& p : s->parts()) {
        const std::optional<Bounds> b = part_bounds(p.id);
        if (b.has_value() && world.x >= b->min_x && world.x <= b->max_x &&
            world.y >= b->min_y && world.y <= b->max_y) {
            return {Selection::Kind::Part, p.id};
        }
    }
    if (const std::uint32_t j = hit_junction(world, radius).value_or(0)) {
        return {Selection::Kind::Junction, j};
    }
    double best_d = radius;
    Selection best_wire{};
    for (const ws::circsim::Wire& w : s->wires()) {
        const std::vector<Pos> path = wire_path(w);
        for (std::size_t i = 1; i < path.size(); ++i) {
            const double d = dist_to_segment(path[i - 1], path[i], world);
            if (d < best_d) {
                best_d = d;
                best_wire = {Selection::Kind::Wire, w.id};
            }
        }
    }
    if (best_wire.kind == Selection::Kind::Wire) {
        return best_wire;
    }
    for (const ws::circsim::Label& l : s->labels()) {
        if (len(Pos{l.pos.x - world.x, l.pos.y - world.y}) <= radius + 1.0) {
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
