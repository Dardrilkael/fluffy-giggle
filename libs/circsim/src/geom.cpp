// ws::circsim - the schematic interaction geometry (see geom.hpp).
// Everything here was lifted verbatim from apps/circsim_editor's
// editor.cpp/document.cpp (stage 11 - the lib extraction): the math is
// the editor's own, unchanged; only the home moved. The app's Editor
// methods are thin forwarders onto these.

#include <ws/circsim/geom.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include <ws/core/assert.hpp>
#include <ws/circsim/symbols.hpp>

namespace ws::circsim {
namespace {

[[nodiscard]] double len(Pos p) {
    return std::sqrt(p.x * p.x + p.y * p.y);
}

[[nodiscard]] bool eps_eq_pos(Pos p, Pos q, double eps) {
    return std::fabs(p.x - q.x) <= eps && std::fabs(p.y - q.y) <= eps;
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
    if (corner_needed) {
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
    unsigned long highest = 0;
    for (const Part& p : s.parts()) {
        if (p.ref.size() <= sym.ref_prefix.size()) {
            continue;
        }
        if (p.ref.compare(0, sym.ref_prefix.size(), sym.ref_prefix) != 0) {
            continue;
        }
        const std::string tail = p.ref.substr(sym.ref_prefix.size());
        bool digits = true;
        for (const char c : tail) {
            if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
                digits = false;
                break;
            }
        }
        if (digits) {
            highest =
                std::max(highest, std::strtoul(tail.c_str(), nullptr, 10));
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
            const double d = dist_to_segment(a, b, world);
            if (d <= best_d) {
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

std::vector<WireRoute> attached_wire_routes(const Schematic& s,
                                            const SymbolLibrary& lib,
                                            AttachedKind kind,
                                            std::uint32_t id) {
    constexpr double kEps = 1e-9;
    std::vector<WireRoute> out;
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
        WireRoute r;
        r.wire = w.id;
        r.waypoints = std::move(mid);
        out.push_back(std::move(r));
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
