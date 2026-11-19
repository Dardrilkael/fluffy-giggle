// ws::sch - the router + resolve_move (phase 2). ONE routing code path:
// a deterministic pattern family (L / Z / U / slid-waypoint), chosen by
// (bends, then length), with a final collinear + zero-length cleanup.
// No A*, no Dijkstra, no judging against sheet terminals - connectivity
// never consults geometry, so the router owns only SHAPE.

#include <ws/schematic/model.hpp>

#include <algorithm>
#include <cmath>
#include <map>

namespace ws::sch {
namespace {

constexpr double kEps = 1e-9;
constexpr double kLead = 1.0;  // the stub length a pinned exit commits to

[[nodiscard]] Vec2 unit(int dir) {
    switch (dir) {
        case 0: return {1.0, 0.0};
        case 90: return {0.0, 1.0};
        case 180: return {-1.0, 0.0};
        default: return {0.0, -1.0};
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
//   * one end pinned: keep the pinned stub (out kLead along its exit),
//     route the free end to the stub's tip with the two Ls;
//   * both ends pinned: the L family against both stubs, plus the U
//     escapes when the stubs face each other's lanes head-on (an L would
//     leave a pin backwards - the only case a U wins);
//   * deterministic: candidates in a fixed order, strict better-only.
[[nodiscard]] std::vector<Vec2> route_ends(Vec2 a, int a_dir, Vec2 b,
                                            int b_dir) {
    std::vector<std::vector<Vec2>> cands;

    const auto stub = [](Vec2 p, int dir) -> Vec2 {
        const Vec2 u = unit(dir);
        return {p.x + u.x * kLead, p.y + u.y * kLead};
    };
    const bool pinned_a = a_dir >= 0;
    const bool pinned_b = b_dir >= 0;

    if (!pinned_a && !pinned_b) {
        cands.push_back(pattern_l(a, b, true));
        cands.push_back(pattern_l(a, b, false));
    } else if (pinned_a && !pinned_b) {
        const Vec2 out_a = stub(a, a_dir);
        cands.push_back(pattern_l(a, b, unit(a_dir).y == 0.0));
        cands.push_back(pattern_l(a, b, unit(a_dir).y != 0.0));
        // Out the stub then free L from the tip to b.
        std::vector<Vec2> via{a, out_a};
        const auto rest = pattern_l(out_a, b, unit(a_dir).y == 0.0);
        via.insert(via.end(), rest.begin() + 1, rest.end());
        cands.push_back(via);
        std::vector<Vec2> via2{a, out_a};
        const auto rest2 = pattern_l(out_a, b, unit(a_dir).y != 0.0);
        via2.insert(via2.end(), rest2.begin() + 1, rest2.end());
        cands.push_back(via2);
    } else if (!pinned_a && pinned_b) {
        const Vec2 out_b = stub(b, b_dir);
        cands.push_back(pattern_l(a, b, true));
        cands.push_back(pattern_l(a, b, false));
        std::vector<Vec2> via{a};
        const auto rest = pattern_l(a, out_b, unit(b_dir).y == 0.0);
        via.insert(via.end(), rest.begin() + 1, rest.end());
        via.push_back(b);
        cands.push_back(via);
        std::vector<Vec2> via2{a};
        const auto rest2 = pattern_l(a, out_b, unit(b_dir).y != 0.0);
        via2.insert(via2.end(), rest2.begin() + 1, rest2.end());
        via2.push_back(b);
        cands.push_back(via2);
    } else {
        // Both pinned: the Ls through both stubs, both orientations.
        const Vec2 out_a = stub(a, a_dir);
        const Vec2 out_b = stub(b, b_dir);
        for (const bool hfirst : {true, false}) {
            std::vector<Vec2> via{a, out_a};
            const auto rest = pattern_l(out_a, out_b, hfirst);
            via.insert(via.end(), rest.begin() + 1, rest.end());
            via.push_back(b);
            cands.push_back(std::move(via));
        }
        // The U escapes: a facing away from b's lane, or head-on.
        const Vec2 ua = unit(a_dir);
        const Vec2 ub = unit(b_dir);
        const Vec2 rel{b.x - a.x, b.y - a.y};
        const bool a_back = rel.x * ua.x + rel.y * ua.y < -kEps;
        const Vec2 rel_b{a.x - b.x, a.y - b.y};
        const bool b_back = rel_b.x * ub.x + rel_b.y * ub.y < -kEps;
        if (a_back || b_back) {
            cands.push_back(pattern_u(a, a_dir, b, b_dir, false));
            cands.push_back(pattern_u(a, a_dir, b, b_dir, true));
        }
        // The Zs: stub out, run a lane, land into the other stub - the
        // workhorse when the pins face the same axis but offset lanes.
        for (const bool lane_is_y : {true, false}) {
            const double lane = lane_is_y ? out_a.y : out_a.x;
            std::vector<Vec2> via{a, out_a};
            const auto z = pattern_z(out_a, out_b, lane, lane_is_y);
            via.insert(via.end(), z.begin() + 1, z.end());
            via.push_back(b);
            cands.push_back(std::move(via));
        }
    }

    // The exit-leg legality filter: a pinned end's FIRST/LAST leg must
    // not head backwards into its part (dot with the exit unit >= 0).
    std::vector<Vec2> best;
    Cost best_cost;
    bool have = false;
    for (const auto& cand : cands) {
        if (cand.size() < 2u) {
            continue;
        }
        if (pinned_a) {
            const Vec2 u = unit(a_dir);
            const double dot = (cand[1].x - cand[0].x) * u.x +
                               (cand[1].y - cand[0].y) * u.y;
            if (dot < -kEps) {
                continue;
            }
        }
        if (pinned_b) {
            const Vec2 u = unit(b_dir);
            const double dot =
                (cand[cand.size() - 2].x - cand.back().x) * u.x +
                (cand[cand.size() - 2].y - cand.back().y) * u.y;
            if (dot < -kEps) {
                continue;
            }
        }
        const Cost c = cost_of(cand);
        if (!have || c.better(best_cost)) {
            best = cand;
            best_cost = c;
            have = true;
        }
    }
    if (!have) {
        // No legal candidate (degenerate geometry): the plain L, the
        // honest fallback.
        best = pattern_l(a, b, true);
    }
    return best;
}

// The final cleanup: duplicate vertices, zero-length legs, collinear
// continuing middles fold away. A collinear REVERSAL keeps its corner
// (folding it would run the wire back through the part it just left).
[[nodiscard]] std::vector<Vec2> cleanup(const std::vector<Vec2>& full) {
    std::vector<Vec2> out;
    for (const Vec2& p : full) {
        if (!out.empty() && std::fabs(out.back().x - p.x) <= kEps &&
            std::fabs(out.back().y - p.y) <= kEps) {
            continue;  // duplicate / zero-length leg
        }
        if (out.size() >= 2u) {
            const Vec2& p0 = out[out.size() - 2u];
            const Vec2& p1 = out[out.size() - 1u];
            const bool collinear =
                (std::fabs(p0.x - p1.x) <= kEps &&
                 std::fabs(p1.x - p.x) <= kEps) ||
                (std::fabs(p0.y - p1.y) <= kEps &&
                 std::fabs(p1.y - p.y) <= kEps);
            const double along = (p1.x - p0.x) * (p.x - p1.x) +
                                 (p1.y - p0.y) * (p.y - p1.y);
            if (collinear && along > 0.0) {
                out.back() = p;  // the leg extends: fold
                continue;
            }
        }
        out.push_back(p);
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
        const bool moved =
            std::fabs(n->pos.x - pos.x) > kEps ||
            std::fabs(n->pos.y - pos.y) > kEps;
        if (moved) {
            ov.nodePos[id] = pos;
        }
    }

    // The ends dragged free: their wires' paths bend to the new point.
    std::map<WireId, bool> end_move;  // wire -> a side moved
    for (const auto& e : session.ends) {
        end_move[e.wire] = e.a_side;
    }

    // 2. Wires: every wire whose endpoints (or a dragged end) moved.
    for (const Wire& w : doc.wires) {
        const Node* na = doc.find_node(w.a);
        const Node* nb = doc.find_node(w.b);
        if (na == nullptr || nb == nullptr) {
            continue;  // dangling: nothing to route
        }
        const bool a_moved = ov.nodePos.count(w.a) != 0;
        const bool b_moved = ov.nodePos.count(w.b) != 0;
        const bool a_turned = session.land_dir.count(w.a) != 0;
        const bool b_turned = session.land_dir.count(w.b) != 0;
        const auto em = end_move.find(w.id);
        const bool end_moved = em != end_move.end();

        Vec2 a = na->pos;
        Vec2 b = nb->pos;
        if (a_moved) {
            a = ov.nodePos.at(w.a);
        }
        if (b_moved) {
            b = ov.nodePos.at(w.b);
        }

        // The exit directions: the stored ones, overridden by the
        // session's rotation map (a rotated pin's new face).
        int a_dir = w.a_dir;
        int b_dir = w.b_dir;
        const auto da = session.land_dir.find(w.a);
        if (da != session.land_dir.end()) {
            a_dir = da->second;
            ov.endDir[{w.id, true}] = a_dir;
        }
        const auto db = session.land_dir.find(w.b);
        if (db != session.land_dir.end()) {
            b_dir = db->second;
            ov.endDir[{w.id, false}] = b_dir;
        }
        // A junction's end is never pinned; a dragged free end is free.
        for (const auto& e : session.ends) {
            if (e.wire == w.id) {
                if (e.a_side) {
                    a_dir = kDirFree;  // dragged loose: free
                } else {
                    b_dir = kDirFree;
                }
            }
        }
        if (!na->is_pin()) {
            a_dir = kDirFree;
        }
        if (!nb->is_pin()) {
            b_dir = kDirFree;
        }

        // The stored path (waypoints may be stale after the move).
        std::vector<Vec2> path;
        path.push_back(a);
        path.insert(path.end(), w.mid.begin(), w.mid.end());
        path.push_back(b);

        std::vector<Vec2> fresh;
        const std::size_t old_bends = [&] {
            std::vector<Vec2> fp;
            fp.push_back(na->pos);
            fp.insert(fp.end(), w.mid.begin(), w.mid.end());
            fp.push_back(nb->pos);
            return cost_of(fp).bends;
        }();

        if (!a_moved && !b_moved && !end_moved && !a_turned && !b_turned) {
            continue;  // untouched: keeps its shape
        }

        // THE RULES:
        //  * both ends moved (same delta - the part/group drag): the
        //    corridor translates rigidly, else re-route;
        //  * one end moved: the fixed end's stub + exit direction kept,
        //    the nearest waypoint slides; a short wire re-routes;
        //  * an end dragged free: route to the session's point.
        if (end_moved) {
            // The dragged end's new position is the session's point.
            for (const auto& e : session.ends) {
                if (e.wire == w.id) {
                    if (e.a_side) {
                        a = e.to;
                    } else {
                        b = e.to;
                    }
                }
            }
            fresh = route_ends(a, a_dir, b, b_dir);
        } else if (a_moved && b_moved) {
            // Rigid translate: same shape offset by the a-side delta
            // when both ends moved by the same delta (the group drag's
            // own invariant); otherwise a fresh route.
            const Vec2 da2{a.x - na->pos.x, a.y - na->pos.y};
            const Vec2 db2{b.x - nb->pos.x, b.y - nb->pos.y};
            const bool same =
                std::fabs(da2.x - db2.x) <= kEps &&
                std::fabs(da2.y - db2.y) <= kEps;
            if (same) {
                fresh.clear();
                fresh.push_back(a);
                // The corridor translates rigidly: the STORED mids ride
                // the delta (they were authored at press geometry).
                for (const Vec2& m : w.mid) {
                    fresh.push_back({m.x + da2.x, m.y + da2.y});
                }
                fresh.push_back(b);
            } else {
                fresh = route_ends(a, a_dir, b, b_dir);
            }
        } else if (a_moved || b_moved || a_turned || b_turned) {
            // One end moved (or a rotation turned a pin's face): keep the
            // shape when it has bends to keep, sliding the adjacent
            // waypoint; re-route when short or when the pin's face turned
            // (the old corridor leaves the old face).
            if (old_bends >= 2 && !a_turned && !b_turned) {
                fresh = slide_one_end(path, a_moved);
            } else {
                fresh = route_ends(a, a_dir, b, b_dir);
            }
        }

        // Cleanup: collinear middles and zero-length legs fold away;
        // only CHANGED shapes ride the overlay (the wire's emitted mids
        // vs its stored ones - a straightened wire emits EMPTY mids and
        // is a real change, an untouched wire emits nothing at all).
        fresh = cleanup(fresh);
        if (fresh.size() >= 2u) {
            std::vector<Vec2> mid(fresh.begin() + 1, fresh.end() - 1);
            if (mid != w.mid) {
                ov.wirePath[w.id] = std::move(mid);
            }
        }
    }
    return ov;
}

}  // namespace ws::sch
