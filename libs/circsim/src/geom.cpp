// ws::circsim - the schematic interaction geometry (see geom.hpp).
// Everything here was lifted verbatim from apps/circsim_editor's
// editor.cpp/document.cpp (stage 11 - the lib extraction): the math is
// the editor's own, unchanged; only the home moved. The app's Editor
// methods are thin forwarders onto these.

#include <ws/circsim/geom.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <ws/core/assert.hpp>
#include <ws/circsim/symbols.hpp>

namespace ws::circsim {
namespace {

[[nodiscard]] double len(Pos p) {
    return std::sqrt(p.x * p.x + p.y * p.y);
}

// The same point-on-segment test nets.cpp runs (collinear and inside
// the bbox): a junction "on a wire's path" means exactly this.
[[nodiscard]] bool on_seg(Pos a, Pos b, Pos p) {
    const double cross =
        (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    if (cross != 0.0) {
        return false;
    }
    return p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x) &&
           p.y >= std::min(a.y, b.y) && p.y <= std::max(a.y, b.y);
}

[[nodiscard]] bool eps_eq_pos(Pos p, Pos q, double eps) {
    return std::fabs(p.x - q.x) <= eps && std::fabs(p.y - q.y) <= eps;
}

// Exact point-on-segment test (collinear + inside bbox): the same logic
// as on_seg but the name matches the settle_route contract.
[[nodiscard]] bool point_on_segment_exact(Pos a, Pos b, Pos p) {
    return on_seg(a, b, p);
}

// Exact point-on-polyline test: true when p lies on any segment of the
// path (which is endpoint, waypoints..., endpoint).
[[nodiscard]] bool point_on_path_exact(const std::vector<Pos>& path, Pos p) {
    for (std::size_t i = 1; i < path.size(); ++i) {
        if (on_seg(path[i - 1], path[i], p)) {
            return true;
        }
    }
    return false;
}

// Folds duplicate points and collinear CONTINUING middles out of a full
// polyline (endpoint, corners, endpoint) and returns the INTERIOR - the
// waypoints the .csim stores. A fold needs the leg to continue: a
// reversal is collinear too, but folding it would run the wire straight
// back through the part it just left. Shared by route_wire (fresh
// routes) and tidy_route (existing wires).
[[nodiscard]] std::vector<Pos> fold_interior(const std::vector<Pos>& full) {
    constexpr double kEps = 1e-9;
    std::vector<Pos> folded;
    for (const Pos& p : full) {
        if (!folded.empty() && eps_eq_pos(folded.back(), p, kEps)) {
            continue;
        }
        if (folded.size() >= 2u) {
            const Pos& p0 = folded[folded.size() - 2u];
            const Pos& p1 = folded[folded.size() - 1u];
            const bool collinear =
                (std::fabs(p0.x - p1.x) <= kEps &&
                 std::fabs(p1.x - p.x) <= kEps) ||
                (std::fabs(p0.y - p1.y) <= kEps &&
                 std::fabs(p1.y - p.y) <= kEps);
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

}  // namespace

// --- polylines -----------------------------------------------------------

std::vector<Pos> route_wire(Pos a, int a_dir, Pos b, int b_dir,
                            bool flip) {
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
    if (eps_eq_pos(a, b, kEps)) {
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
        // Approach b AGAINST b_dir: a pin's dir faces outward (away
        // from the part's body - the a side's from = a + u*lead reads
        // it exactly that way), so a wire lands tip-first, traveling
        // the reverse of the facing from the pin's own outside. The
        // stop is one lead short of the tip ON that outside
        // (b + u*lead). The old b - u*lead sat the stop inside the
        // part: the final leg ran out through the body and doubled
        // back to the pin - every pin landing stabbed its part.
        target = {b.x + u.x * kLead, b.y + u.y * kLead};
    }

    // Behind-the-lead escape (the U-shape): when the target lies
    // BEHIND the committed lead-out - dot(unit(a_dir), target - from)
    // < 0 - the stock L would double back over its own start pin and
    // across the part body (a=(10,0) facing +x, b=(3,0) facing -x
    // routed (10,0)->(11,0)->(2,0)->(3,0), the leg (11,0)->(2,0)
    // running straight over pin b and through the part). The escape
    // steps PERPENDICULAR first, past the pin's lead, then closes: a
    // U around the part instead of an L through it. The mirrored
    // check applies on the b side (the approach leg heading back
    // into the target's own lead). Free ends (dir < 0) never need
    // it: nothing is behind a lead they never committed.
    const auto behind = [&](const Pos& lead_from, int lead_dir,
                            const Pos& aim) {
        if (lead_dir < 0) {
            return false;
        }
        const Pos u = unit(lead_dir);
        return (aim.x - lead_from.x) * u.x + (aim.y - lead_from.y) * u.y <
               -kEps;
    };
    const bool a_behind = behind(from, a_dir, target);
    const bool b_behind = behind(target, b_dir, from);

    // Full polyline a -> ... -> b; the stored waypoints are its
    // interior after folding. A corner is needed only when the free
    // point past a's lead and the approach point before b share
    // neither axis (aligned endpoints fold away).
    const bool corner_needed =
        !(std::fabs(from.x - target.x) <= kEps) &&
        !(std::fabs(from.y - target.y) <= kEps);
    std::vector<Pos> full;
    full.push_back(a);
    if (a_dir >= 0) {
        full.push_back(from);
    }
    if (a_behind || b_behind) {
        // The U: out along the (say horizontal) lead, one step
        // perpendicular to clear the part, across to the target's
        // lane, back down onto the approach lane, and in. The
        // perpendicular step rides the SAME side the stock L would
        // have turned (flip stays the Tab toggle); with both ends
        // behind (the head-on case) the lane is whichever axis the
        // leads share - they are parallel there, so either lane
        // closes both.
        const bool start_horizontal =
            (a_dir < 0 || a_dir == 0 || a_dir == 180) != flip;
        if (start_horizontal) {
            const double perp_y = from.y + (target.y >= from.y ? kLead : -kLead);
            full.push_back(Pos{from.x, perp_y});
            full.push_back(Pos{target.x, perp_y});
        } else {
            const double perp_x = from.x + (target.x >= from.x ? kLead : -kLead);
            full.push_back(Pos{perp_x, from.y});
            full.push_back(Pos{perp_x, target.y});
        }
    } else if (corner_needed) {
        // One corner (an L): the leg out of `from` continues the
        // committed direction (horizontal when free), the leg into
        // `target` closes. The final approach leg target -> b already
        // runs along b_dir, so one corner always suffices.
        // `flip` swaps the stock choice (the Tab toggle): the same
        // two endpoints, the corner on the other diagonal.
        const bool start_horizontal =
            (a_dir < 0 || a_dir == 0 || a_dir == 180) != flip;
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
    return fold_interior(full);
}

std::vector<Pos> tidy_route(const std::vector<Pos>& path) {
    constexpr double kEps = 1e-9;
    if (path.size() < 2u) {
        return {};
    }
    // Every diagonal leg gets one axis-aligned corner (horizontal
    // first - the stock bend); axis-aligned legs pass through. The fold
    // then merges the corner into a neighbouring collinear leg where it
    // can, so a wire that only had a stray diagonal comes out with the
    // fewest bends that keep the rest of its shape.
    std::vector<Pos> full;
    full.reserve(path.size() * 2u);
    full.push_back(path.front());
    for (std::size_t i = 1; i < path.size(); ++i) {
        const Pos p = full.back();
        const Pos q = path[i];
        if (!(std::fabs(p.x - q.x) <= kEps) &&
            !(std::fabs(p.y - q.y) <= kEps)) {
            full.push_back(Pos{q.x, p.y});
        }
        full.push_back(q);
    }
    return fold_interior(full);
}

// --- refdes ----------------------------------------------------------------

std::string next_ref(const Schematic& s, const SymbolDef& sym) {
    if (sym.ref_prefix.empty()) {
        return "";
    }
    // Saturating parse: strtoul wraps to ULONG_MAX (32-bit on Windows)
    // on a long digit run, and highest + 1 then wrapped to "R0".
    constexpr std::uint64_t kCap = 999'999'999'999ULL;
    std::uint64_t highest = 0;
    for (const Part& p : s.parts()) {
        if (p.ref.size() <= sym.ref_prefix.size()) {
            continue;
        }
        if (p.ref.compare(0, sym.ref_prefix.size(), sym.ref_prefix) != 0) {
            continue;
        }
        const std::string tail = p.ref.substr(sym.ref_prefix.size());
        bool digits = true;
        std::uint64_t value = 0;
        for (const char c : tail) {
            if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
                digits = false;
                break;
            }
            value = std::min(kCap, value * 10u +
                                       static_cast<std::uint64_t>(c - '0'));
        }
        if (digits) {
            highest = std::max(highest, value);
        }
    }
    return sym.ref_prefix + std::to_string(highest + 1);
}

bool is_auto_ref(const std::string& ref, const std::string& prefix) {
    if (ref.size() <= prefix.size() ||
        ref.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    for (std::size_t i = prefix.size(); i < ref.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(ref[i])) == 0) {
            return false;
        }
    }
    return true;
}

bool mentions_token(const std::string& text, const std::string& token) {
    if (token.empty()) {
        return false;
    }
    const auto lower = [](std::string str) {
        for (char& ch : str) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return str;
    };
    const auto word = [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
    };
    const std::string hay = lower(text);
    const std::string needle = lower(token);
    for (std::size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + 1)) {
        const std::size_t end = at + needle.size();
        const bool left = at == 0 || !word(hay[at - 1]);
        const bool right = end >= hay.size() || !word(hay[end]);
        if (left && right) {
            return true;
        }
    }
    return false;
}

std::vector<std::size_t> annotation_order(const std::vector<Pos>& at,
                                          double row_tol) {
    std::vector<std::size_t> idx(at.size());
    for (std::size_t i = 0; i < idx.size(); ++i) {
        idx[i] = i;
    }
    // Top first (world y points up); equal heights keep list order.
    std::stable_sort(idx.begin(), idx.end(),
                     [&](std::size_t a, std::size_t b) {
                         return at[a].y > at[b].y;
                     });
    // Sweep into rows anchored at each row's top part, then left to
    // right inside the row (stable: identical x keeps the order above).
    std::size_t i = 0;
    while (i < idx.size()) {
        const double anchor = at[idx[i]].y;
        std::size_t j = i + 1;
        while (j < idx.size() && anchor - at[idx[j]].y <= row_tol) {
            ++j;
        }
        std::stable_sort(idx.begin() + static_cast<std::ptrdiff_t>(i),
                         idx.begin() + static_cast<std::ptrdiff_t>(j),
                         [&](std::size_t a, std::size_t b) {
                             return at[a].x < at[b].x;
                         });
        i = j;
    }
    return idx;
}

// --- bounds -----------------------------------------------------------------

void BoundsBuilder::add(Pos w) {
    if (!box.has_value()) {
        box = Bounds{w.x, w.y, w.x, w.y};
        return;
    }
    box->min_x = std::min(box->min_x, w.x);
    box->min_y = std::min(box->min_y, w.y);
    box->max_x = std::max(box->max_x, w.x);
    box->max_y = std::max(box->max_y, w.y);
}

void BoundsBuilder::add(const Bounds& b) {
    add(Pos{b.min_x, b.min_y});
    add(Pos{b.max_x, b.max_y});
}

std::optional<Bounds> part_world_bounds(const Schematic& s,
                                        const SymbolLibrary& lib,
                                        std::uint32_t part_id) {
    const Part* p = s.find_part(part_id);
    if (p == nullptr) {
        return std::nullopt;
    }
    const SymbolDef* sym = lib.find(p->symbol);
    if (sym == nullptr) {
        return Bounds{p->pos.x - 1.0, p->pos.y - 1.0, p->pos.x + 1.0,
                      p->pos.y + 1.0};
    }
    const Bounds local = symbol_bounds(*sym);
    BoundsBuilder out;
    for (const Pos corner :
         {Pos{local.min_x, local.min_y}, Pos{local.max_x, local.min_y},
          Pos{local.min_x, local.max_y}, Pos{local.max_x, local.max_y}}) {
        out.add(transform_pos(*p, corner));
    }
    return out.box;
}

std::vector<Pos> wire_path(const Schematic& s, const SymbolLibrary& lib,
                           const Wire& w) {
    std::vector<Pos> out;
    const auto endpoint_pos = [&](const WireEndpoint& end)
        -> std::optional<Pos> {
        if (const PinRef* pin = std::get_if<PinRef>(&end)) {
            const Part* p = s.find_part(pin->part);
            const SymbolDef* sym = p != nullptr ? lib.find(p->symbol)
                                                : nullptr;
            if (p != nullptr && sym != nullptr &&
                pin->pin < sym->pins.size()) {
                return pin_pos(*sym, *p, pin->pin);
            }
            return std::nullopt;
        }
        const JunctionRef* j = std::get_if<JunctionRef>(&end);
        const Junction* found =
            j != nullptr ? s.find_junction(j->junction) : nullptr;
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

std::optional<Bounds> content_bounds(const Schematic& s,
                                     const SymbolLibrary& lib) {
    BoundsBuilder out;
    for (const Part& p : s.parts()) {
        if (const std::optional<Bounds> b = part_world_bounds(s, lib, p.id)) {
            out.add(*b);
        }
    }
    for (const Junction& j : s.junctions()) {
        out.add(j.pos);
    }
    for (const Label& l : s.labels()) {
        out.add(l.pos);
    }
    for (const Wire& w : s.wires()) {
        for (const Pos& p : wire_path(s, lib, w)) {
            out.add(p);
        }
    }
    return out.box;
}

// --- picking ----------------------------------------------------------------

double dist_to_segment(Pos a, Pos b, Pos p) {
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

std::optional<PinHit> hit_pin(const Schematic& s,
                              const SymbolLibrary& lib, Pos world,
                              double radius) {
    std::optional<PinHit> best;
    double best_d = radius;
    for (const Part& p : s.parts()) {
        const SymbolDef* sym = lib.find(p.symbol);
        if (sym == nullptr) {
            continue;
        }
        for (std::size_t i = 0; i < sym->pins.size(); ++i) {
            const Pos at = pin_pos(*sym, p, static_cast<std::uint32_t>(i));
            const double d = len(Pos{at.x - world.x, at.y - world.y});
            if (d < best_d) {
                best_d = d;
                best = PinHit{PinRef{p.id, static_cast<std::uint32_t>(i)},
                              at, sym->pins[i].dir};
            }
        }
    }
    return best;
}

std::optional<PinHit> hit_other_pin_at(
    const Schematic& s, const SymbolLibrary& lib, Pos world,
    double radius, const PinRef& exclude, Pos at) {
    // Same spot, different pin: iterate the way hit_pin does (parts
    // in insertion order, pins ascending) and keep the first pin that
    // is not the excluded one and sits exactly at `at` (within eps)
    // while still within the pick radius of the cursor.
    constexpr double kEps = 1e-9;
    for (const Part& p : s.parts()) {
        if (p.id == exclude.part) {
            continue;
        }
        const SymbolDef* sym = lib.find(p.symbol);
        if (sym == nullptr) {
            continue;
        }
        for (std::size_t i = 0; i < sym->pins.size(); ++i) {
            const Pos pos = pin_pos(*sym, p, static_cast<std::uint32_t>(i));
            if (!eps_eq_pos(pos, at, kEps)) {
                continue;
            }
            const double d = len(Pos{pos.x - world.x, pos.y - world.y});
            if (d <= radius) {
                return PinHit{PinRef{p.id, static_cast<std::uint32_t>(i)},
                              pos, sym->pins[i].dir};
            }
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> hit_junction(const Schematic& s, Pos world,
                                          double radius) {
    std::optional<std::uint32_t> best;
    double best_d = radius;
    for (const Junction& j : s.junctions()) {
        const double d = len(Pos{j.pos.x - world.x, j.pos.y - world.y});
        if (d < best_d) {
            best_d = d;
            best = j.id;
        }
    }
    return best;
}

std::optional<WirePointSnap> hit_wire_point(const Schematic& s,
                                            const SymbolLibrary& lib,
                                            Pos world, double radius,
                                            double grid) {
    std::optional<WirePointSnap> best;
    double best_d = radius;
    const auto snap_one = [&](double v) {
        // The FINE lattice, the canvas grid's own expression
        // (round(v / base) * base) - the same arithmetic
        // ws::ui::canvas::snap_coord spells, so a snap here is
        // bit-equal to the one the editor's own snapping produces.
        return grid > 0.0 ? std::round(v / grid) * grid : v;
    };
    for (const Wire& w : s.wires()) {
        const std::vector<Pos> path = wire_path(s, lib, w);
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
            // Snap the projection to the fine grid. For an ortho
            // segment the stationary coordinate is the wire's own and
            // is kept EXACT (it may be off the lattice - an off-grid
            // pin, a grid change - and snapping it moved the candidate
            // off the wire, forcing the endpoint fallback below); only
            // the running coordinate rounds to a grid line.
            Pos cand;
            if (ab.x == 0.0) {
                cand = Pos{a.x, snap_one(proj.y)};
            } else if (ab.y == 0.0) {
                cand = Pos{snap_one(proj.x), a.y};
            } else {
                cand = Pos{snap_one(proj.x), snap_one(proj.y)};
            }
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
                cand.x >= std::min(a.x, b.x) &&
                cand.x <= std::max(a.x, b.x) &&
                cand.y >= std::min(a.y, b.y) &&
                cand.y <= std::max(a.y, b.y);
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
            // Strict < keeps the FIRST wire on an exact tie (two
            // wires crossing at the cursor): the later wire used to
            // win, and a draft that started on the earlier one read
            // its own start as gone (the spurious cancel) - the
            // first wire is the one the author drew first.
            const double d = dist_to_segment(a, b, world);
            if (d < best_d) {
                best_d = d;
                best = WirePointSnap{w.id, cand};
            }
        }
    }
    return best;
}

// --- placement ----------------------------------------------------------------

Pos magnet_shift(const Schematic& s, const SymbolLibrary& lib,
                 const SymbolDef& sym, Pos base, int rot, bool mirror,
                 double magnet_radius) {
    // Where would each ghost pin land at the snapped position? If one
    // is close to a real pin, shift the whole ghost so they coincide.
    Part probe{};
    probe.pos = base;
    probe.rot = rot;
    probe.mirror = mirror;
    std::optional<Pos> best_shift;
    double best_d = magnet_radius;
    for (std::size_t i = 0; i < sym.pins.size(); ++i) {
        const Pos at = pin_pos(sym, probe, static_cast<std::uint32_t>(i));
        const std::optional<PinHit> hit = hit_pin(s, lib, at, best_d);
        if (!hit.has_value()) {
            continue;
        }
        const Pos shift{hit->pos.x - at.x, hit->pos.y - at.y};
        const double d = len(shift);
        if (d < best_d || !best_shift.has_value()) {
            best_d = d;
            best_shift = shift;
        }
    }
    if (!best_shift.has_value()) {
        return base;
    }
    return {base.x + best_shift->x, base.y + best_shift->y};
}


namespace {

// How far a route commits to a pin's facing before it may turn (the
// same one grid unit route_wire's own lead uses).
constexpr double kRouteLead = 1.0;
// How far (grid units) a candidate may slide one interior leg sideways
// to get clear of terminals it would otherwise touch.
constexpr int kMaxLegShift = 6;

// The world direction (0/90/180/270) a pin faces once the part's mirror
// and rotation apply (PinDef::dir is part-local); -1 when unknown.
[[nodiscard]] int pin_world_dir(const SymbolDef& sym, const Part& part,
                                std::uint32_t pin) {
    if (pin >= sym.pins.size()) {
        return -1;
    }
    const int local = ((sym.pins[pin].dir % 360) + 360) % 360;
    Pos v{0.0, 0.0};
    switch (local) {
        case 0: v = {1.0, 0.0}; break;
        case 90: v = {0.0, 1.0}; break;
        case 180: v = {-1.0, 0.0}; break;
        case 270: v = {0.0, -1.0}; break;
        default: return -1;
    }
    Part probe{};
    probe.rot = part.rot;
    probe.mirror = part.mirror;
    const Pos w = transform_pos(probe, v);
    if (w.x > 0.5) return 0;
    if (w.y > 0.5) return 90;
    if (w.x < -0.5) return 180;
    return 270;
}

[[nodiscard]] Pos dir_unit(int dir) {
    switch (dir) {
        case 0: return {1.0, 0.0};
        case 90: return {0.0, 1.0};
        case 180: return {-1.0, 0.0};
        default: return {0.0, -1.0};
    }
}

// Judges a candidate wire route against the sheet. Connectivity here is
// geometric - a pin or junction that lies on a wire's polyline joins that
// wire's net - so a route that merely PASSES OVER a terminal it was not
// attached to has silently wired it in. Two defects are counted:
//  * contacts: terminals (at their landing positions) on the route that
//    are neither the wire's own endpoints nor attachments the route was
//    asked to keep;
//  * lead violations: a first/last leg that heads back into the part
//    body a pin faces away from (it runs over the part's other pins).
struct RouteJudge {
    const std::vector<Pos>* terminals = nullptr;
    const std::vector<Pos>* keep = nullptr;
    int a_dir = -1;
    int b_dir = -1;

    [[nodiscard]] int contacts(const std::vector<Pos>& full) const {
        int n = 0;
        for (const Pos& t : *terminals) {
            if (eps_eq_pos(t, full.front(), 1e-9) ||
                eps_eq_pos(t, full.back(), 1e-9)) {
                continue;
            }
            bool wanted = false;
            for (const Pos& k : *keep) {
                if (eps_eq_pos(t, k, 1e-9)) {
                    wanted = true;
                    break;
                }
            }
            if (!wanted && point_on_path_exact(full, t)) {
                ++n;
            }
        }
        return n;
    }

    [[nodiscard]] int lead_violations(const std::vector<Pos>& full) const {
        int bad = 0;
        const std::size_t n = full.size();
        if (n < 2u) {
            return 0;
        }
        if (a_dir >= 0) {
            const Pos u = dir_unit(a_dir);
            const double dot = (full[1].x - full[0].x) * u.x +
                               (full[1].y - full[0].y) * u.y;
            if (dot < -1e-9) ++bad;
        }
        if (b_dir >= 0) {
            const Pos u = dir_unit(b_dir);
            const double dot = (full[n - 2u].x - full[n - 1u].x) * u.x +
                               (full[n - 2u].y - full[n - 1u].y) * u.y;
            if (dot < -1e-9) ++bad;
        }
        return bad;
    }
};

// An orthogonal route from `a` to `b` on the integer lattice anchored at
// `a` that steps around every `blocked` terminal: a small Dijkstra over
// (cell, last direction) with a bend penalty (fewer corners read
// cleaner) and the pin leads honored - the first step never heads back
// into the part `a_dir` faces away from, nor the last into the part
// `b_dir` faces away from. Returns the INTERIOR waypoints, or nullopt
// when the endpoints are off-lattice, the search box is huge, or no
// route exists. Terminals off the lattice are ignored (symbols live on
// the grid).
[[nodiscard]] std::optional<std::vector<Pos>> route_around(
    Pos a, int a_dir, Pos b, int b_dir, const std::vector<Pos>& blocked) {
    constexpr int kMargin = 8;
    constexpr int kBend = 3;
    constexpr double kLatticeEps = 1e-6;
    const auto on_lattice = [&](double v) {
        return std::fabs(v - std::round(v)) <= kLatticeEps;
    };
    if (!on_lattice(b.x - a.x) || !on_lattice(b.y - a.y)) {
        return std::nullopt;
    }
    const int bx = static_cast<int>(std::lround(b.x - a.x));
    const int by = static_cast<int>(std::lround(b.y - a.y));
    if (bx == 0 && by == 0) {
        return std::nullopt;
    }
    const int x0 = std::min(0, bx) - kMargin;
    const int x1 = std::max(0, bx) + kMargin;
    const int y0 = std::min(0, by) - kMargin;
    const int y1 = std::max(0, by) + kMargin;
    const int w = x1 - x0 + 1;
    const int h = y1 - y0 + 1;
    if (static_cast<long long>(w) * h > 60000LL) {
        return std::nullopt;
    }
    std::vector<char> wall(static_cast<std::size_t>(w) * h, 0);
    for (const Pos& t : blocked) {
        if (!on_lattice(t.x - a.x) || !on_lattice(t.y - a.y)) {
            continue;
        }
        const int ix = static_cast<int>(std::lround(t.x - a.x));
        const int iy = static_cast<int>(std::lround(t.y - a.y));
        if (ix < x0 || ix > x1 || iy < y0 || iy > y1) {
            continue;
        }
        if ((ix == 0 && iy == 0) || (ix == bx && iy == by)) {
            continue;
        }
        wall[static_cast<std::size_t>(iy - y0) * w + (ix - x0)] = 1;
    }

    // Directions: 0 +x, 1 +y, 2 -x, 3 -y; state direction 4 = start.
    constexpr int kDx[4] = {1, 0, -1, 0};
    constexpr int kDy[4] = {0, 1, 0, -1};
    const int a_idx = a_dir >= 0 ? (a_dir / 90) % 4 : -1;
    const int b_idx = b_dir >= 0 ? (b_dir / 90) % 4 : -1;
    const auto state_of = [&](int ix, int iy, int d) {
        return (static_cast<std::size_t>(iy - y0) * w + (ix - x0)) * 5u +
               static_cast<std::size_t>(d);
    };
    const std::size_t n_states = static_cast<std::size_t>(w) * h * 5u;
    std::vector<int> dist(n_states, INT_MAX);
    std::vector<std::size_t> parent(n_states, n_states);
    using Item = std::pair<int, std::size_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    const std::size_t start = state_of(0, 0, 4);
    dist[start] = 0;
    open.push({0, start});
    std::size_t goal = n_states;
    while (!open.empty()) {
        const auto [cost, st] = open.top();
        open.pop();
        if (cost > dist[st]) {
            continue;
        }
        const int d = static_cast<int>(st % 5u);
        const std::size_t cell = st / 5u;
        const int ix = static_cast<int>(cell % static_cast<std::size_t>(w)) + x0;
        const int iy = static_cast<int>(cell / static_cast<std::size_t>(w)) + y0;
        if (ix == bx && iy == by) {
            goal = st;
            break;
        }
        for (int nd = 0; nd < 4; ++nd) {
            int step = 1;
            if (d == 4) {
                if (a_idx >= 0 && nd == (a_idx + 2) % 4) {
                    continue;  // never back into the part body
                }
                if (a_idx >= 0 && nd != a_idx) {
                    step += 2;  // leaving sideways: legal, less tidy
                }
            } else {
                if (nd == (d + 2) % 4) {
                    continue;  // no U-turn
                }
                if (nd != d) {
                    step += kBend;
                }
            }
            const int nx = ix + kDx[nd];
            const int ny = iy + kDy[nd];
            if (nx < x0 || nx > x1 || ny < y0 || ny > y1 ||
                wall[static_cast<std::size_t>(ny - y0) * w + (nx - x0)] != 0) {
                continue;
            }
            if (nx == bx && ny == by && b_idx >= 0) {
                // Arriving along nd: the cell we came from must not sit
                // on b's body side; arriving against b_dir is the clean
                // tip-first landing, sideways costs a little.
                if (nd == b_idx) {
                    continue;
                }
                if (nd != (b_idx + 2) % 4) {
                    step += 2;
                }
            }
            const int next_cost = cost + step;
            const std::size_t next = state_of(nx, ny, nd);
            if (next_cost < dist[next]) {
                dist[next] = next_cost;
                parent[next] = st;
                open.push({next_cost, next});
            }
        }
    }
    if (goal == n_states) {
        return std::nullopt;
    }
    std::vector<Pos> cells;
    for (std::size_t st = goal; st != n_states; st = parent[st]) {
        const std::size_t cell = st / 5u;
        const int ix = static_cast<int>(cell % static_cast<std::size_t>(w)) + x0;
        const int iy = static_cast<int>(cell / static_cast<std::size_t>(w)) + y0;
        cells.push_back(Pos{a.x + ix, a.y + iy});
    }
    std::reverse(cells.begin(), cells.end());
    cells.front() = a;
    cells.back() = b;
    return fold_interior(cells);
}

// Picks the best route among the base one and its alternatives. A clean
// base (no contacts, no lead violation) is returned untouched, so only
// routes that would have caused trouble change. Alternatives: a fresh
// route that honors the pin directions (when nothing must be kept), a
// head/tail re-lead around the kept attachments (when something must),
// and each base/alternative with one interior leg slid sideways.
[[nodiscard]] std::vector<Pos> settle_route(
    const RouteJudge& judge, Pos front, Pos back, bool ortho,
    std::vector<Pos> base) {
    if (!ortho || eps_eq_pos(front, back, 1e-9)) {
        return base;
    }
    const std::vector<Pos>& keep = *judge.keep;
    const auto compose = [&](const std::vector<Pos>& wp) {
        std::vector<Pos> full;
        full.reserve(wp.size() + 2u);
        full.push_back(front);
        full.insert(full.end(), wp.begin(), wp.end());
        full.push_back(back);
        return full;
    };
    const auto total = [&](const std::vector<Pos>& wp, int shift) {
        const std::vector<Pos> full = compose(wp);
        return judge.contacts(full) * 1000 +
               judge.lead_violations(full) * 500 + shift * 2 +
               static_cast<int>(wp.size());
    };
    const auto defects = [&](const std::vector<Pos>& wp) {
        const std::vector<Pos> full = compose(wp);
        return judge.contacts(full) + judge.lead_violations(full);
    };
    if (defects(base) == 0) {
        return base;
    }

    // Terminals a fresh route must step around: everything except the
    // wire's own endpoints and the attachments it has to run through.
    std::vector<Pos> blocked;
    for (const Pos& t : *judge.terminals) {
        bool exempt = eps_eq_pos(t, front, 1e-9) || eps_eq_pos(t, back, 1e-9);
        for (const Pos& k : keep) {
            exempt = exempt || eps_eq_pos(t, k, 1e-9);
        }
        if (!exempt) {
            blocked.push_back(t);
        }
    }
    std::vector<std::vector<Pos>> seeds;
    seeds.push_back(base);
    if (keep.empty()) {
        for (const bool flip : {false, true}) {
            seeds.push_back(route_wire(front, judge.a_dir, back,
                                       judge.b_dir, flip));
        }
        if (auto around = route_around(front, judge.a_dir, back,
                                       judge.b_dir, blocked)) {
            seeds.push_back(std::move(*around));
        }
    } else {
        // Walk the route anchor to anchor (front, every kept point, back).
        // A stretch that touches something it should not, or leaves a pin
        // backwards, is re-routed around the terminals; a clean stretch
        // keeps the shape the author drew.
        const std::vector<Pos> full = compose(base);
        std::vector<std::size_t> anchors{0u};
        for (std::size_t i = 1; i + 1u < full.size(); ++i) {
            for (const Pos& k : keep) {
                if (eps_eq_pos(full[i], k, 1e-9)) {
                    anchors.push_back(i);
                    break;
                }
            }
        }
        anchors.push_back(full.size() - 1u);
        if (anchors.size() > 2u) {
            std::vector<Pos> assembled{full.front()};
            for (std::size_t k = 0; k + 1u < anchors.size(); ++k) {
                const std::size_t i = anchors[k];
                const std::size_t j = anchors[k + 1u];
                const bool head = k == 0u;
                const bool tail = k + 2u == anchors.size();
                RouteJudge sub_judge = judge;
                sub_judge.a_dir = head ? judge.a_dir : -1;
                sub_judge.b_dir = tail ? judge.b_dir : -1;
                const std::vector<Pos> sub(full.begin() + i,
                                           full.begin() + j + 1u);
                if (sub_judge.contacts(sub) + sub_judge.lead_violations(sub) ==
                    0) {
                    assembled.insert(assembled.end(), sub.begin() + 1,
                                     sub.end());
                    continue;
                }
                std::vector<Pos> leg;
                if (auto around = route_around(full[i], sub_judge.a_dir,
                                               full[j], sub_judge.b_dir,
                                               blocked)) {
                    leg = std::move(*around);
                } else {
                    leg = route_wire(full[i], sub_judge.a_dir, full[j],
                                     sub_judge.b_dir);
                }
                assembled.insert(assembled.end(), leg.begin(), leg.end());
                assembled.push_back(full[j]);
            }
            seeds.push_back(fold_interior(assembled));
        }
    }

    std::vector<Pos> best = base;
    int best_total = total(base, 0);
    const auto consider = [&](const std::vector<Pos>& wp, int shift) {
        const std::vector<Pos> full = compose(wp);
        for (const Pos& k : keep) {
            if (!point_on_path_exact(full, k)) {
                return;  // a candidate must still run through the keeps
            }
        }
        const int t = total(wp, shift);
        if (t < best_total) {
            best_total = t;
            best = wp;
        }
    };
    for (const std::vector<Pos>& seed : seeds) {
        consider(seed, 0);
        const std::vector<Pos> full = compose(seed);
        for (std::size_t i = 1; i + 2u < full.size(); ++i) {
            const Pos p = full[i];
            const Pos q = full[i + 1u];
            const bool horizontal =
                std::fabs(p.y - q.y) <= 1e-9 && std::fabs(p.x - q.x) > 1e-9;
            const bool vertical =
                std::fabs(p.x - q.x) <= 1e-9 && std::fabs(p.y - q.y) > 1e-9;
            if (!horizontal && !vertical) {
                continue;
            }
            // The legs either side must be perpendicular so the slide
            // only lengthens/shortens them (no diagonal appears).
            const Pos before = full[i - 1u];
            const Pos after = full[i + 2u];
            if (horizontal && !(std::fabs(before.x - p.x) <= 1e-9 &&
                                std::fabs(after.x - q.x) <= 1e-9)) {
                continue;
            }
            if (vertical && !(std::fabs(before.y - p.y) <= 1e-9 &&
                              std::fabs(after.y - q.y) <= 1e-9)) {
                continue;
            }
            bool pinned = false;
            for (const Pos& k : keep) {
                if (point_on_segment_exact(p, q, k)) {
                    pinned = true;
                    break;
                }
            }
            if (pinned) {
                continue;
            }
            for (int d = 1; d <= kMaxLegShift; ++d) {
                for (const int sign : {1, -1}) {
                    const double off = static_cast<double>(sign * d);
                    std::vector<Pos> moved = full;
                    if (horizontal) {
                        moved[i].y += off;
                        moved[i + 1u].y += off;
                    } else {
                        moved[i].x += off;
                        moved[i + 1u].x += off;
                    }
                    consider(fold_interior(moved), d);
                }
            }
        }
    }
    return best;
}

}  // namespace

std::vector<WireRoute> attached_wire_routes(
    const Schematic& s, const SymbolLibrary& lib, AttachedKind kind,
    std::uint32_t id, const std::vector<PinnedSpot>& pinned) {
    constexpr double kEps = 1e-9;
    std::vector<WireRoute> out;

    // Collect all terminals at their landing positions: what a new route
    // must not accidentally touch. Connectivity is geometric (a pin or
    // junction on a wire's polyline joins its net), so a reroute that
    // ran over a foreign pin would silently connect it.
    struct TerminalMove {
        Pos to;
    };
    std::vector<TerminalMove> terminals;
    // Pins
    for (const Part& p : s.parts()) {
        const SymbolDef* sym = lib.find(p.symbol);
        if (sym == nullptr) continue;
        for (std::uint32_t i = 0; i < sym->pins.size(); ++i) {
            terminals.push_back(TerminalMove{pin_pos(*sym, p, i)});
        }
    }
    // Junctions
    for (const Junction& j : s.junctions()) {
        terminals.push_back(TerminalMove{j.pos});
    }

    // Every pin/junction at its landing position: what a new route must
    // not accidentally touch.
    std::vector<Pos> terminal_points;
    terminal_points.reserve(terminals.size());
    for (const TerminalMove& terminal : terminals) {
        terminal_points.push_back(terminal.to);
    }
    const auto end_dir = [&](const WireEndpoint& endpoint) -> int {
        const auto* pin = std::get_if<PinRef>(&endpoint);
        if (pin == nullptr) {
            return -1;
        }
        const Part* part = s.find_part(pin->part);
        const SymbolDef* symbol =
            part != nullptr ? lib.find(part->symbol) : nullptr;
        return (part != nullptr && symbol != nullptr)
                   ? pin_world_dir(*symbol, *part, pin->pin)
                   : -1;
    };

    // The pinned spots arrive from the CALLER, captured at the press
    // geometry (before the entity moved): every junction the affected
    // wires' paths carried on their interior - the T-taps the author
    // drew - each naming its OWNING wire. At the landing the old path
    // is already gone (the endpoints resolve live), so only the caller
    // can know what the drawing carried. Each wire's fresh route runs
    // through ITS OWN spots only: a spot owned by a different wire
    // never rides this route (the older bare-position contract wove
    // every spot into every affected wire - a move could route one
    // net's wire through another net's tap dot and silently join
    // them).
    const auto hits = [&](const WireEndpoint& e) {
        if (kind == AttachedKind::Part) {
            const auto* pin = std::get_if<PinRef>(&e);
            return pin != nullptr && pin->part == id;
        }
        const auto* j = std::get_if<JunctionRef>(&e);
        return j != nullptr && j->junction == id;
    };
    for (const Wire& w : s.wires()) {
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
        const std::vector<Pos> path = wire_path(s, lib, w);
        if (path.size() < 2u) {
            continue;  // dangling: nothing to keep straight
        }
        // THIS wire's pinned spots: the taps its own press-geometry
        // path carried. The judge and the insert loop below read this
        // list, never the caller's whole set.
        std::vector<Pos> my_spots;
        for (const PinnedSpot& p : pinned) {
            if (p.wire == w.id) {
                my_spots.push_back(p.pos);
            }
        }
        // Straight or one-bend wires (and anything with both ends on
        // the same move) re-route as a fresh horizontal-first L; a
        // multi-bend wire keeps its shape - only the adjacent waypoint
        // slides, chosen so both touching legs stay axis-aligned.
        std::vector<Pos> mid(path.begin() + 1, path.end() - 1);
        if (mid.size() <= 1u || (a_hit && b_hit)) {
            WireRoute route;
            route.wire = w.id;
            std::vector<Pos> base = route_wire(path.front(), -1, path.back(), -1);
            // Insert THIS wire's pinned spots (T-taps it carried at
            // the press geometry) into the base route so they are
            // preserved by settle_route. The route runs THROUGH each
            // spot (endpoint -> spot -> endpoint).
            for (const Pos& spot : my_spots) {
                if (eps_eq_pos(spot, path.front(), kEps) ||
                    eps_eq_pos(spot, path.back(), kEps)) {
                    continue;  // an endpoint's own position - no pin
                }
                bool on_new = false;
                for (std::size_t k = 0; k + 1u < base.size(); ++k) {
                    if (on_seg(base[k], base[k + 1u], spot)) {
                        on_new = true;
                        break;
                    }
                }
                if (!on_new) {
                    const std::vector<Pos> head =
                        route_wire(path.front(), -1, spot, -1);
                    const std::vector<Pos> tail =
                        route_wire(spot, -1, path.back(), -1);
                    base = head;
                    base.push_back(spot);
                    base.insert(base.end(), tail.begin(), tail.end());
                }
            }
            const RouteJudge judge{&terminal_points, &my_spots,
                                   end_dir(w.a), end_dir(w.b)};
            route.waypoints = settle_route(judge, path.front(), path.back(),
                                           w.ortho, std::move(base));
            out.push_back(std::move(route));
            continue;
        }
        if (a_hit) {
            // The leg into mid[1] keeps its orientation: a vertical
            // continuation keeps x (the first leg turns horizontal),
            // a horizontal one keeps y (the first leg turns vertical).
            if (std::fabs(mid[0].x - mid[1].x) <= kEps) {
                mid[0].y = path.front().y;
            } else {
                mid[0].x = path.front().x;
            }
        }
        if (b_hit) {
            Pos& last = mid.back();
            const Pos& prev = mid[mid.size() - 2u];
            if (std::fabs(prev.y - last.y) <= kEps) {
                last.x = path.back().x;
            } else {
                last.y = path.back().y;
            }
        }
        // The slide can leave a zero-length or collinear waypoint
        // (a part dropped exactly in line); fold those out so they
        // never ride into the .csim.
        std::vector<Pos> full;
        full.reserve(mid.size() + 2u);
        full.push_back(path.front());
        full.insert(full.end(), mid.begin(), mid.end());
        full.push_back(path.back());
        WireRoute route;
        route.wire = w.id;
        const RouteJudge judge{&terminal_points, &my_spots,
                               end_dir(w.a), end_dir(w.b)};
        route.waypoints = settle_route(judge, path.front(), path.back(),
                                       true, fold_interior(full));
        out.push_back(std::move(route));
    }
    return out;
}

std::vector<PinnedSpot> wire_pin_spots(const Schematic& s,
                                      const SymbolLibrary& lib,
                                      AttachedKind kind, std::uint32_t id) {
    // The affected wires (the endpoint-reference scan the re-route
    // runs), read at the CURRENT geometry - the caller runs this
    // BEFORE the entity moves, so the paths still carry the author's
    // taps. Interior junctions only: the wire's own endpoint
    // positions ride their refs and never pin. Each spot names its
    // OWNING wire: only that wire's fresh route has to run through it
    // (a junction on wire W's body is W's tap - a bare position could
    // be woven into a DIFFERENT wire's route, joining nets that never
    // touched).
    std::vector<PinnedSpot> out;
    const auto hits = [&](const WireEndpoint& e) {
        if (kind == AttachedKind::Part) {
            const auto* pin = std::get_if<PinRef>(&e);
            return pin != nullptr && pin->part == id;
        }
        const auto* j = std::get_if<JunctionRef>(&e);
        return j != nullptr && j->junction == id;
    };
    for (const Wire& w : s.wires()) {
        if (!w.ortho) {
            continue;  // the author's own shape never carries the rule
        }
        if (!hits(w.a) && !hits(w.b)) {
            continue;
        }
        const std::vector<Pos> path = wire_path(s, lib, w);
        if (path.size() < 2u) {
            continue;
        }
        for (const Junction& j : s.junctions()) {
            if (eps_eq_pos(j.pos, path.front(), 1e-9) ||
                eps_eq_pos(j.pos, path.back(), 1e-9)) {
                continue;
            }
            for (std::size_t k = 0; k + 1u < path.size(); ++k) {
                if (on_seg(path[k], path[k + 1u], j.pos)) {
                    // Deduped per (position, wire): two affected wires
                    // sharing one tap pin it once EACH - the pair is
                    // the identity the re-route filters by.
                    if (std::find_if(out.begin(), out.end(),
                                     [&](const PinnedSpot& p) {
                                         return p.wire == w.id &&
                                                eps_eq_pos(p.pos, j.pos,
                                                           1e-9);
                                     }) == out.end()) {
                        out.push_back(PinnedSpot{j.pos, w.id});
                    }
                    break;
                }
            }
        }
    }
    return out;
}

JunctionTapLanding junction_tap_landing(const Schematic& s,
                                       const SymbolLibrary& lib,
                                       std::uint32_t junction_id) {
    JunctionTapLanding out;
    const Junction* j = s.find_junction(junction_id);
    if (j == nullptr) {
        return out;
    }
    // The same point-on-polyline test nets.cpp runs (collinear and
    // inside the segment's bbox), spelled once here.
    const auto on_polyline_local = [](const std::vector<Pos>& path,
                                      Pos p) {
        for (std::size_t i = 1; i < path.size(); ++i) {
            const Pos a = path[i - 1];
            const Pos b = path[i];
            const double cross =
                (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
            if (cross != 0.0) {
                continue;
            }
            if (p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x) &&
                p.y >= std::min(a.y, b.y) && p.y <= std::max(a.y, b.y)) {
                return true;
            }
        }
        return false;
    };
    for (const Wire& w : s.wires()) {
        // The dot's own wires: their endpoint refs already carry the
        // authored gesture; a "tap" on them would be noise (and the
        // re-route keeps them passing through the dot anyway).
        const auto* ea = std::get_if<JunctionRef>(&w.a);
        const auto* eb = std::get_if<JunctionRef>(&w.b);
        if ((ea != nullptr && ea->junction == junction_id) ||
            (eb != nullptr && eb->junction == junction_id)) {
            continue;
        }
        const bool on = on_polyline_local(wire_path(s, lib, w), j->pos);
        const bool tapped =
            std::find(j->taps.begin(), j->taps.end(), w.id) != j->taps.end();
        if (on && !tapped) {
            out.add.push_back(w.id);
        } else if (!on && tapped) {
            out.remove.push_back(w.id);
        }
    }
    return out;
}


// --- colors ----------------------------------------------------------------

std::uint32_t series_color(std::size_t i) {
    // The same eight the Results plot has always painted its signals
    // in, now the canvas's nets wear them too. Spelled as packed
    // 0xRRGGBBAA literals (the r2d packing) - the lib owns no ui
    // include; the app's editor.hpp re-exports the name, and the
    // values are byte-identical to the rgba(r, g, b) spelling the
    // editor app used (0xE05A4EFF == rgba(224, 90, 78), ...).
    static constexpr std::uint32_t kSeries[] = {
        0xE05A4EFFu,  // red
        0x4EA1E0FFu,  // blue
        0x58B55CFFu,  // green
        0xD39A2AFFu,  // amber
        0x9A6FD3FFu,  // purple
        0x4EC9C0FFu,  // teal
        0xD35A9EFFu,  // pink
        0x8F9A4EFFu,  // olive
    };
    return kSeries[i % (sizeof kSeries / sizeof kSeries[0])];
}

}  // namespace ws::circsim
