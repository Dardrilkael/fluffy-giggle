// ws::sch - the router + resolve_move (phase 2). ONE routing code path:
// a deterministic pattern family (L / Z / U / slid-waypoint), chosen by
// (bends, then length), with a final collinear + zero-length cleanup.
// No A*, no Dijkstra, no judging against sheet terminals - connectivity
// never consults geometry, so the router owns only SHAPE.

#include <ws/schematic/model.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <vector>

#include "geom.hpp"

namespace ws::sch {
namespace {

using detail::kEps;
constexpr double kLead = 1.0;  // the stub length a pinned exit commits to

[[nodiscard]] Vec2 unit(int dir) {
    switch (dir) {
        case 0: return {1.0, 0.0};
        case 90: return {0.0, 1.0};
        case 180: return {-1.0, 0.0};
        case 270: return {0.0, -1.0};
        default: return {0.0, 0.0};  // free / invalid: no lead
    }
}

// --- the pattern family -----------------------------------------------------

// A candidate route's cost: bends first, then length. Deterministic tie
// order by construction order (first found wins on strict <).
struct Cost {
    int bends = 0;
    double length = 0.0;
    [[nodiscard]] bool better(const Cost& o) const {
        if (bends != o.bends) {
            return bends < o.bends;
        }
        return length < o.length - kEps;
    }
};

[[nodiscard]] Cost cost_of(const std::vector<Vec2>& p) {
    Cost c;
    for (std::size_t i = 1; i < p.size(); ++i) {
        c.length += std::fabs(p[i].x - p[i - 1].x) +
                    std::fabs(p[i].y - p[i - 1].y);
        if (i + 1 < p.size()) {
            const double cross = (p[i].x - p[i - 1].x) * (p[i + 1].y - p[i].y) -
                                  (p[i].y - p[i - 1].y) * (p[i + 1].x - p[i].x);
            if (std::fabs(cross) > kEps) {
                ++c.bends;
            }
        }
    }
    return c;
}

// Drops consecutive duplicate points ONLY. Collinear points stay, so a
// reversal (the wire running back over itself) remains visible to
// legal_route; cleanup() is what folds them afterwards.
[[nodiscard]] std::vector<Vec2> dedupe(const std::vector<Vec2>& p) {
    std::vector<Vec2> out;
    for (const Vec2& q : p) {
        if (out.empty() || !detail::near(out.back(), q)) {
            out.push_back(q);
        }
    }
    return out;
}

// A route is LEGAL when it is a clean orthogonal walk:
//   * every leg is axis-aligned and has length;
//   * no collinear REVERSAL (a leg running back over the one before it -
//     the wire doubling back on itself, usually through its own part);
//   * a pinned end's first / last leg does not head backwards into its
//     part;
//   * no two non-adjacent legs touch (a wire never crosses or touches
//     itself - that would be an implicit self-connection).
// cost_of() cannot see a reversal (its cross product is 0, so it counts
// 0 bends): without this check a doubled-back path looked CHEAPER than an
// honest one and won the candidate race.
[[nodiscard]] bool legal_route(const std::vector<Vec2>& p, int a_dir,
                               int b_dir) {
    const std::size_t n = p.size();
    if (n < 2u) {
        return false;
    }
    for (std::size_t i = 1; i < n; ++i) {
        const bool h = std::fabs(p[i].y - p[i - 1].y) <= kEps;
        const bool v = std::fabs(p[i].x - p[i - 1].x) <= kEps;
        if (h == v) {
            return false;  // diagonal (neither) or zero-length (both)
        }
    }
    for (std::size_t i = 1; i + 1 < n; ++i) {
        const double dot = (p[i].x - p[i - 1].x) * (p[i + 1].x - p[i].x) +
                           (p[i].y - p[i - 1].y) * (p[i + 1].y - p[i].y);
        const double cross = (p[i].x - p[i - 1].x) * (p[i + 1].y - p[i].y) -
                             (p[i].y - p[i - 1].y) * (p[i + 1].x - p[i].x);
        if (std::fabs(cross) <= kEps && dot < 0.0) {
            return false;  // reversal
        }
    }
    if (a_dir >= 0) {
        const Vec2 u = unit(a_dir);
        if ((p[1].x - p[0].x) * u.x + (p[1].y - p[0].y) * u.y < -kEps) {
            return false;
        }
    }
    if (b_dir >= 0) {
        const Vec2 u = unit(b_dir);
        if ((p[n - 2].x - p[n - 1].x) * u.x +
                (p[n - 2].y - p[n - 1].y) * u.y < -kEps) {
            return false;
        }
    }
    // Axis-aligned legs: a closed segment IS its bounding box.
    const auto touch = [&](std::size_t i, std::size_t j) {
        return !(std::max(p[i - 1].x, p[i].x) < std::min(p[j - 1].x, p[j].x) - kEps ||
                 std::max(p[j - 1].x, p[j].x) < std::min(p[i - 1].x, p[i].x) - kEps ||
                 std::max(p[i - 1].y, p[i].y) < std::min(p[j - 1].y, p[j].y) - kEps ||
                 std::max(p[j - 1].y, p[j].y) < std::min(p[i - 1].y, p[i].y) - kEps);
    };
    for (std::size_t i = 1; i < n; ++i) {
        for (std::size_t j = i + 2; j < n; ++j) {
            if (touch(i, j)) {
                return false;
            }
        }
    }
    return true;
}

// The L: out from a (free), one corner, into b (free).
[[nodiscard]] std::vector<Vec2> pattern_l(Vec2 a, Vec2 b, bool horizontal_first) {
    std::vector<Vec2> p{a};
    if (std::fabs(a.x - b.x) > kEps && std::fabs(a.y - b.y) > kEps) {
        if (horizontal_first) {
            p.push_back({b.x, a.y});
        } else {
            p.push_back({a.x, b.y});
        }
    }
    p.push_back(b);
    return p;
}

// The Z with a lane: out from a to the lane, across, down into b. Used
// when a side's exit is pinned and the free sides need a middle lane.
[[nodiscard]] std::vector<Vec2> pattern_z(Vec2 a, Vec2 b, double lane,
                                           bool lane_is_y) {
    std::vector<Vec2> p{a};
    if (lane_is_y) {
        p.push_back({a.x, lane});
        p.push_back({b.x, lane});
    } else {
        p.push_back({lane, a.y});
        p.push_back({lane, b.y});
    }
    p.push_back(b);
    return p;
}

// The U: both ends pinned, at least one facing away from the other's
// lane. Out a's lead, one lead further on the cross axis (so both stubs
// clear), across, and back into b's lead. `flip` swaps which cross axis
// the detour runs on.
[[nodiscard]] std::vector<Vec2> pattern_u(Vec2 a, int a_dir, Vec2 b, int b_dir,
                                           bool flip) {
    std::vector<Vec2> p{a};
    const Vec2 ua = unit(a_dir);
    const Vec2 out_a{a.x + ua.x * kLead, a.y + ua.y * kLead};
    const Vec2 ub = unit(b_dir);
    const Vec2 out_b{b.x + ub.x * kLead, b.y + ub.y * kLead};
    p.push_back(out_a);
    // The U runs on the axis PERPENDICULAR to a's lead; the lane sits
    // one lead past BOTH stub tips on that axis (the sign away from the
    // other stub's tip, so the detour clears both).
    const bool a_horizontal = ua.y == 0.0;
    const bool go_horizontal = a_horizontal != flip;
    if (go_horizontal) {
        // a's lead is vertical: run horizontally at a lane past both.
        const double lane_y =
            out_b.y >= out_a.y ? out_a.y - kLead : out_a.y + kLead;
        p.push_back({out_a.x, lane_y});
        p.push_back({out_b.x, lane_y});
    } else {
        // a's lead is horizontal: run vertically.
        const double lane_x =
            out_b.x >= out_a.x ? out_a.x - kLead : out_a.x + kLead;
        p.push_back({lane_x, out_a.y});
        p.push_back({lane_x, out_b.y});
    }
    p.push_back(out_b);
    p.push_back(b);
    return p;
}

// --- the router proper --------------------------------------------------------

// Routes one wire end pair. `a_dir`/`b_dir` are the ends' exit directions
// (-1 = free). Returns the FULL polyline (a..b); the caller cleans it.
//
// The rules (the spec's own):
//   * both ends free: the two Ls, cheapest wins;
//   * one end pinned: the direct Ls, plus the route that keeps the pinned
//     stub (out kLead along its exit) and runs the two Ls from the tip;
//   * both ends pinned: the centred Z lanes and the L family through both
//     stubs, plus the U escapes when a stub faces away from the other pin;
//   * detours (C-shaped lanes) always join the race, so it never has zero
//     legal entrants;
//   * deterministic: candidates in a fixed order, strict better-only.
[[nodiscard]] std::vector<Vec2> route_ends(Vec2 a, int a_dir, Vec2 b,
                                            int b_dir) {
    std::vector<std::vector<Vec2>> cands;
    cands.reserve(16);

    const bool pinned_a = a_dir >= 0;
    const bool pinned_b = b_dir >= 0;
    const auto stub = [](Vec2 p, int dir) -> Vec2 {
        const Vec2 u = unit(dir);
        return {p.x + u.x * kLead, p.y + u.y * kLead};
    };
    // The stub tips (the pins themselves when free).
    const Vec2 ta = pinned_a ? stub(a, a_dir) : a;
    const Vec2 tb = pinned_b ? stub(b, b_dir) : b;

    // a -> [ta] -> L(ta, tb) -> [tb] -> b. Free ends collapse into their
    // neighbour (dedupe() drops the repeated point), so one builder serves
    // every pinned / free combination.
    const auto through = [&](bool hfirst) {
        std::vector<Vec2> via{a};
        if (pinned_a) {
            via.push_back(ta);
        }
        const std::vector<Vec2> rest = pattern_l(ta, tb, hfirst);
        via.insert(via.end(), rest.begin(), rest.end());
        if (pinned_b) {
            via.push_back(b);
        }
        cands.push_back(std::move(via));
    };
    const bool a_h = pinned_a && unit(a_dir).y == 0.0;
    const bool b_h = pinned_b && unit(b_dir).y == 0.0;

    if (!pinned_a && !pinned_b) {
        cands.push_back(pattern_l(a, b, true));
        cands.push_back(pattern_l(a, b, false));
    } else if (pinned_a && !pinned_b) {
        cands.push_back(pattern_l(a, b, a_h));
        cands.push_back(pattern_l(a, b, !a_h));
        through(a_h);
        through(!a_h);
    } else if (!pinned_a && pinned_b) {
        cands.push_back(pattern_l(a, b, true));
        cands.push_back(pattern_l(a, b, false));
        through(b_h);
        through(!b_h);
    } else {
        // The Zs: stub out, run a lane HALFWAY between the two stub tips
        // (rounded to the whole grid unit, as kLead is one), land into the
        // other stub. They come first so a centred lane wins a tie against
        // the L that hugs the far stub. (The previous lane was the stub's
        // own coordinate, which made every Z a copy of an L.)
        for (const bool lane_is_y : {true, false}) {
            const double lane = std::round(
                lane_is_y ? 0.5 * (ta.y + tb.y) : 0.5 * (ta.x + tb.x));
            std::vector<Vec2> via{a, ta};
            const auto z = pattern_z(ta, tb, lane, lane_is_y);
            via.insert(via.end(), z.begin() + 1, z.end());
            via.push_back(b);
            cands.push_back(std::move(via));
        }
        // The Ls through both stubs, both orientations.
        through(true);
        through(false);
        // The U escapes: a facing away from b's lane, or head-on.
        const Vec2 ua = unit(a_dir);
        const Vec2 ub = unit(b_dir);
        const bool a_back =
            (b.x - a.x) * ua.x + (b.y - a.y) * ua.y < -kEps;
        const bool b_back =
            (a.x - b.x) * ub.x + (a.y - b.y) * ub.y < -kEps;
        if (a_back || b_back) {
            cands.push_back(pattern_u(a, a_dir, b, b_dir, false));
            cands.push_back(pattern_u(a, a_dir, b, b_dir, true));
        }
    }

    // The detours: from the two stub tips run a C-shaped lane past BOTH on
    // each of the four sides. They cost two bends so they never beat a
    // real pattern, but they guarantee the race always has a legal entrant
    // when every L / Z / U would double back.
    {
        const double right = std::max(ta.x, tb.x) + kLead;
        const double left = std::min(ta.x, tb.x) - kLead;
        const double top = std::max(ta.y, tb.y) + kLead;
        const double bottom = std::min(ta.y, tb.y) - kLead;
        const auto around = [&](Vec2 m1, Vec2 m2) {
            std::vector<Vec2> via{a};
            if (pinned_a) {
                via.push_back(ta);
            }
            via.push_back(m1);
            via.push_back(m2);
            if (pinned_b) {
                via.push_back(tb);
            }
            via.push_back(b);
            cands.push_back(std::move(via));
        };
        for (const double lx : {right, left}) {
            around({lx, ta.y}, {lx, tb.y});
        }
        for (const double ly : {top, bottom}) {
            around({ta.x, ly}, {tb.x, ly});
        }
    }

    // The race: only LEGAL candidates run (see legal_route), cheapest
    // (bends, then length) wins, ties by construction order.
    std::vector<Vec2> best;
    Cost best_cost;
    bool have = false;
    for (const auto& raw : cands) {
        std::vector<Vec2> cand = dedupe(raw);
        if (!legal_route(cand, a_dir, b_dir)) {
            continue;
        }
        const Cost c = cost_of(cand);
        if (!have || c.better(best_cost)) {
            best = std::move(cand);
            best_cost = c;
            have = true;
        }
    }
    if (!have) {
        // No legal candidate (degenerate geometry, e.g. both ends on the
        // same spot): the plain L. NOT checked by legal_route - it may
        // leave a pin backwards; the detours make this practically
        // unreachable.
        best = pattern_l(a, b, true);
    }
    return best;
}

// The final cleanup: duplicate vertices, zero-length legs and collinear
// middles fold away, and a SPIKE (a leg that runs out and straight back,
// A -> B -> C collinear with C behind B) is removed. Each removal can
// expose another on the tail, so the tail is re-examined until stable.
// (It used to KEEP a collinear reversal "so the wire does not run back
// through the part it just left" - which is exactly the doubled-back wire
// users saw. Whether the cleaned wire still leaves its pin correctly is
// legal_route's question, asked by the caller.)
[[nodiscard]] std::vector<Vec2> cleanup(const std::vector<Vec2>& full) {
    std::vector<Vec2> out;
    for (const Vec2& p : full) {
        if (!out.empty() && detail::near(out.back(), p)) {
            continue;  // duplicate / zero-length leg
        }
        out.push_back(p);
        while (out.size() >= 3u) {
            const std::size_t n = out.size();
            const Vec2 p0 = out[n - 3];
            const Vec2 p1 = out[n - 2];
            const Vec2 p2 = out[n - 1];
            const bool collinear =
                (std::fabs(p0.x - p1.x) <= kEps &&
                 std::fabs(p1.x - p2.x) <= kEps) ||
                (std::fabs(p0.y - p1.y) <= kEps &&
                 std::fabs(p1.y - p2.y) <= kEps);
            if (!collinear) {
                break;
            }
            out.erase(out.end() - 2);  // fold (continuing) or spike (reversal)
            if (detail::near(out[out.size() - 2], out.back())) {
                out.erase(out.end() - 2);  // the spike returned to its start
            }
        }
    }
    return out;
}

// The slid-waypoint corridor: one end moved, the author's multi-bend
// shape kept - only the waypoint ADJACENT to the moved end slides so
// both touching legs stay axis-aligned.
[[nodiscard]] std::vector<Vec2> slide_one_end(const std::vector<Vec2>& path,
                                                bool move_a) {
    std::vector<Vec2> p = path;
    if (p.size() < 3u) {
        return p;  // straight or dangling: nothing to slide
    }
    if (move_a) {
        // The first interior corner: keep its orientation (the leg into
        // p[1] keeps its axis) and slide onto the new endpoint's lane.
        if (std::fabs(p[1].x - p[2].x) <= kEps) {
            p[1].y = p[0].y;  // p[1]->p[2] vertical: slide y
        } else {
            p[1].x = p[0].x;  // horizontal: slide x
        }
    } else {
        const std::size_t n = p.size();
        if (std::fabs(p[n - 2].y - p[n - 3].y) <= kEps) {
            p[n - 2].x = p[n - 1].x;
        } else {
            p[n - 2].y = p[n - 1].y;
        }
    }
    return p;
}

}  // namespace

Overlay resolve_move(const Doc& doc, const MoveSession& session) {
    Overlay ov;

    // 1. Node positions: every landing in the session's land map (the
    // adapter resolved part pins' new positions; the lib needs no symbol
    // knowledge).
    for (const auto& [id, pos] : session.land) {
        const Node* n = doc.find_node(id);
        if (n == nullptr) {
            continue;
        }
        if (std::fabs(n->pos.x - pos.x) > kEps ||
            std::fabs(n->pos.y - pos.y) > kEps) {
            ov.nodePos[id] = pos;
        }
    }

    // The ends dragged free, by wire (a wire may have both ends dragged;
    // for a repeated side the last entry wins).
    struct EndDrag {
        std::optional<Vec2> a_to;
        std::optional<Vec2> b_to;
    };
    std::map<WireId, EndDrag> drags;
    for (const auto& e : session.ends) {
        EndDrag& d = drags[e.wire];
        (e.a_side ? d.a_to : d.b_to) = e.to;
    }

    // 2. Wires: every wire whose endpoints (or a dragged end) moved.
    for (const Wire& w : doc.wires) {
        const bool a_moved = ov.nodePos.count(w.a) != 0;
        const bool b_moved = ov.nodePos.count(w.b) != 0;
        const bool a_turned = session.land_dir.count(w.a) != 0;
        const bool b_turned = session.land_dir.count(w.b) != 0;
        const auto dr = drags.find(w.id);
        const bool end_moved = dr != drags.end();
        if (!a_moved && !b_moved && !end_moved && !a_turned && !b_turned) {
            continue;  // untouched: keeps its shape (decided FIRST, cheaply)
        }

        const Node* na = doc.find_node(w.a);
        const Node* nb = doc.find_node(w.b);
        if (na == nullptr || nb == nullptr) {
            continue;  // dangling: nothing to route
        }

        Vec2 a = a_moved ? ov.nodePos.at(w.a) : na->pos;
        Vec2 b = b_moved ? ov.nodePos.at(w.b) : nb->pos;

        // The exit directions: the stored ones, overridden by the
        // session's rotation map (a rotated pin's new face).
        int a_dir = w.a_dir;
        int b_dir = w.b_dir;
        if (const auto it = session.land_dir.find(w.a);
            it != session.land_dir.end()) {
            a_dir = it->second;
            ov.endDir[{w.id, true}] = a_dir;
        }
        if (const auto it = session.land_dir.find(w.b);
            it != session.land_dir.end()) {
            b_dir = it->second;
            ov.endDir[{w.id, false}] = b_dir;
        }
        // A dragged free end is free; a junction's end is never pinned.
        if (end_moved) {
            if (dr->second.a_to) {
                a_dir = kDirFree;
            }
            if (dr->second.b_to) {
                b_dir = kDirFree;
            }
        }
        if (!na->is_pin()) {
            a_dir = kDirFree;
        }
        if (!nb->is_pin()) {
            b_dir = kDirFree;
        }

        // THE RULES:
        //  * an end dragged free: route to the session's point;
        //  * both ends moved (same delta - the part/group drag, no pin
        //    turned): the corridor translates rigidly, else re-route;
        //  * one end moved: the fixed end's stub + exit direction kept,
        //    the nearest waypoint slides; a short wire re-routes;
        //  * a pin's face turned: the old corridor leaves the old face,
        //    so re-route.
        std::vector<Vec2> fresh;
        if (end_moved) {
            if (dr->second.a_to) {
                a = *dr->second.a_to;
            }
            if (dr->second.b_to) {
                b = *dr->second.b_to;
            }
            fresh = route_ends(a, a_dir, b, b_dir);
        } else if (a_moved && b_moved && !a_turned && !b_turned) {
            const Vec2 da{a.x - na->pos.x, a.y - na->pos.y};
            const Vec2 db{b.x - nb->pos.x, b.y - nb->pos.y};
            if (std::fabs(da.x - db.x) <= kEps &&
                std::fabs(da.y - db.y) <= kEps) {
                // The STORED mids ride the delta (they were authored at
                // press geometry).
                fresh.reserve(w.mid.size() + 2u);
                fresh.push_back(a);
                for (const Vec2& m : w.mid) {
                    fresh.push_back({m.x + da.x, m.y + da.y});
                }
                fresh.push_back(b);
            } else {
                fresh = route_ends(a, a_dir, b, b_dir);
            }
        } else {
            // Here either exactly one end moved (no turn) or a face
            // turned. Only the first case can keep an authored shape.
            const auto bends_of = [&](Vec2 p, Vec2 q) {
                std::vector<Vec2> fp;
                fp.reserve(w.mid.size() + 2u);
                fp.push_back(p);
                fp.insert(fp.end(), w.mid.begin(), w.mid.end());
                fp.push_back(q);
                return cost_of(fp).bends;
            };
            const int old_bends = bends_of(na->pos, nb->pos);
            if (old_bends >= 2 && !a_turned && !b_turned) {
                std::vector<Vec2> path;
                path.reserve(w.mid.size() + 2u);
                path.push_back(a);
                path.insert(path.end(), w.mid.begin(), w.mid.end());
                path.push_back(b);
                fresh = slide_one_end(path, a_moved);
                // Sliding one waypoint keeps the author's shape but knows
                // nothing about the pins: the moved end may now leave its
                // part backwards, the corridor may fold over itself, or
                // the wire may cross itself. Keep the slid shape only if
                // it is a legal route; otherwise re-route from scratch.
                //
                // And keep it only if the move did not make it MORE
                // WASTEFUL than it already was ("waste" = bends beyond
                // the best route for the same ends; the pattern router is
                // within one bend of optimal in practice, so it stands in
                // for the minimum). An authored two-bend detour stays a
                // two-bend detour; a detour that was necessary before the
                // move and pointless after it is re-routed.
                const std::vector<Vec2> kept = cleanup(fresh);
                const std::vector<Vec2> best_new =
                    cleanup(route_ends(a, a_dir, b, b_dir));
                const std::vector<Vec2> best_old =
                    cleanup(route_ends(na->pos, a_dir, nb->pos, b_dir));
                const int waste_before =
                    std::max(0, old_bends - cost_of(best_old).bends);
                if (!legal_route(kept, a_dir, b_dir) ||
                    cost_of(kept).bends > cost_of(best_new).bends + waste_before) {
                    fresh = best_new;
                }
            } else {
                fresh = route_ends(a, a_dir, b, b_dir);
            }
        }

        // Cleanup: collinear middles and zero-length legs fold away; only
        // CHANGED shapes ride the overlay (a straightened wire emits EMPTY
        // mids and is a real change, an untouched wire emits nothing).
        fresh = cleanup(fresh);
        std::vector<Vec2> mid;
        if (fresh.size() >= 2u) {
            mid.assign(fresh.begin() + 1, fresh.end() - 1);
        }
        // (fewer than 2 points = both ends landed on the same spot: the
        // wire has no length, so no waypoints - never leave the stale
        // ones behind to run diagonally between the old corners.)
        if (mid != w.mid) {
            ov.wirePath[w.id] = std::move(mid);
        }
    }
    return ov;
}

}  // namespace ws::sch
