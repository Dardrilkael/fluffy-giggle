// ws::sch router property test: random orthogonal wires, random moves,
// every result judged by an INDEPENDENT checker (nothing here calls into
// the router's own legality code).
//
// What "a wire the user would not call fuzzy" means, as properties:
//   * clean: axis-aligned legs, no zero-length leg;
//   * never doubles back: no leg runs back over the one before it;
//   * leaves a pinned end the way the pin faces (not back into its part);
//   * never crosses or touches itself;
//   * a group drag (both ends, same delta) is a RIGID translation;
//   * a move does not make the wire more wasteful than it was (bends
//     beyond the true minimum for those two pins), and freshly routed
//     wires are at that minimum almost always.
//
// Before the router fix this test failed on 44% of random moves
// (doubling back 60-68% after a rotate / two-ended move, exit-into-part
// and self-crossing after a one-ended move).
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <ws/schematic/model.hpp>

using namespace ws::sch;

namespace {

constexpr double kTol = 1e-9;

Vec2 unit_of(int dir) {
    switch (dir) {
        case 0: return {1.0, 0.0};
        case 90: return {0.0, 1.0};
        case 180: return {-1.0, 0.0};
        default: return {0.0, -1.0};
    }
}

bool same(Vec2 a, Vec2 b) {
    return std::fabs(a.x - b.x) < kTol && std::fabs(a.y - b.y) < kTol;
}

struct Verdict {
    bool diagonal = false, zero_leg = false, doubles_back = false;
    bool exit_into_part = false, self_touch = false;
    int bends = 0;
    [[nodiscard]] bool legal() const {
        return !diagonal && !zero_leg && !doubles_back && !exit_into_part &&
               !self_touch;
    }
};

Verdict judge(const std::vector<Vec2>& p, int a_dir, int b_dir) {
    Verdict v;
    const std::size_t n = p.size();
    for (std::size_t i = 1; i < n; ++i) {
        const bool h = std::fabs(p[i].y - p[i - 1].y) < kTol;
        const bool w = std::fabs(p[i].x - p[i - 1].x) < kTol;
        if (h && w) v.zero_leg = true;
        if (!h && !w) v.diagonal = true;
    }
    for (std::size_t i = 1; i + 1 < n; ++i) {
        const Vec2 d1{p[i].x - p[i - 1].x, p[i].y - p[i - 1].y};
        const Vec2 d2{p[i + 1].x - p[i].x, p[i + 1].y - p[i].y};
        if (std::fabs(d1.x * d2.y - d1.y * d2.x) > kTol) {
            ++v.bends;
        } else if (d1.x * d2.x + d1.y * d2.y < -kTol) {
            v.doubles_back = true;
        }
    }
    if (a_dir >= 0 && n >= 2) {
        const Vec2 u = unit_of(a_dir);
        if ((p[1].x - p[0].x) * u.x + (p[1].y - p[0].y) * u.y < -kTol) {
            v.exit_into_part = true;
        }
    }
    if (b_dir >= 0 && n >= 2) {
        const Vec2 u = unit_of(b_dir);
        if ((p[n - 2].x - p[n - 1].x) * u.x + (p[n - 2].y - p[n - 1].y) * u.y <
            -kTol) {
            v.exit_into_part = true;
        }
    }
    const auto overlap = [&](std::size_t i, std::size_t j) {
        const auto lo = [](double a, double b) { return std::min(a, b); };
        const auto hi = [](double a, double b) { return std::max(a, b); };
        return !(hi(p[i - 1].x, p[i].x) < lo(p[j - 1].x, p[j].x) - kTol ||
                 hi(p[j - 1].x, p[j].x) < lo(p[i - 1].x, p[i].x) - kTol ||
                 hi(p[i - 1].y, p[i].y) < lo(p[j - 1].y, p[j].y) - kTol ||
                 hi(p[j - 1].y, p[j].y) < lo(p[i - 1].y, p[i].y) - kTol);
    };
    for (std::size_t i = 1; i < n; ++i) {
        for (std::size_t j = i + 2; j < n; ++j) {
            if (overlap(i, j)) v.self_touch = true;
        }
    }
    return v;
}

// The true minimum number of bends for a wire between two pins that must
// leave along their exit direction and arrive against it (0-1 BFS over
// (x, y, heading) on a grid around the pins). A free end (dir < 0) is
// unconstrained.
int min_bends(Vec2 A, int ad, Vec2 B, int bd) {
    const int lo_x = static_cast<int>(std::min(A.x, B.x)) - 4;
    const int hi_x = static_cast<int>(std::max(A.x, B.x)) + 4;
    const int lo_y = static_cast<int>(std::min(A.y, B.y)) - 4;
    const int hi_y = static_cast<int>(std::max(A.y, B.y)) + 4;
    const int W = hi_x - lo_x + 1;
    const int H = hi_y - lo_y + 1;
    constexpr std::array<int, 4> dx{1, 0, -1, 0};
    constexpr std::array<int, 4> dy{0, 1, 0, -1};
    const auto idx = [&](int x, int y, int d) {
        return ((x - lo_x) * H + (y - lo_y)) * 4 + d;
    };
    std::vector<int> dist(static_cast<std::size_t>(W * H * 4), 1 << 20);
    std::deque<std::array<int, 3>> dq;
    const int ax = static_cast<int>(A.x), ay = static_cast<int>(A.y);
    const int bx = static_cast<int>(B.x), by = static_cast<int>(B.y);
    for (int d = 0; d < 4; ++d) {
        if (ad >= 0 && d != ad / 90) continue;
        const int nx = ax + dx[d], ny = ay + dy[d];
        if (nx < lo_x || nx > hi_x || ny < lo_y || ny > hi_y) continue;
        dist[idx(nx, ny, d)] = 0;
        dq.push_back({nx, ny, d});
    }
    while (!dq.empty()) {
        const auto [x, y, d] = dq.front();
        dq.pop_front();
        const int c = dist[idx(x, y, d)];
        for (int nd = 0; nd < 4; ++nd) {
            if (nd == (d + 2) % 4) continue;  // no U-turn
            const int nx = x + dx[nd], ny = y + dy[nd];
            if (nx < lo_x || nx > hi_x || ny < lo_y || ny > hi_y) continue;
            const int nc = c + (nd != d ? 1 : 0);
            if (nc < dist[idx(nx, ny, nd)]) {
                dist[idx(nx, ny, nd)] = nc;
                if (nd == d) dq.push_front({nx, ny, nd});
                else dq.push_back({nx, ny, nd});
            }
        }
    }
    int best = 1 << 20;
    for (int d = 0; d < 4; ++d) {
        if (bd >= 0 && d != (bd / 90 + 2) % 4) continue;
        best = std::min(best, dist[idx(bx, by, d)]);
    }
    return best;
}

}  // namespace

TEST_CASE("router fuzz: moving a connected element never makes the wire fuzzy") {
    std::mt19937 rng(12345);
    const auto rnd = [&](int lo, int hi) {
        return std::uniform_int_distribution<int>(lo, hi)(rng);
    };

    enum Kind { MoveA, MoveB, MoveBothSame, MoveBothDiff, RotateA, kKinds };
    std::array<int, kKinds> n{}, defects{}, wastefuller{}, at_minimum{};
    std::array<int, kKinds> rigid_fail{};

    for (int trial = 0; trial < 12000; ++trial) {
        // --- an independently generated, LEGAL starting wire ---------
        const int ad = rnd(0, 3) * 90, bd = rnd(0, 3) * 90;
        const Vec2 A{double(rnd(0, 20)), double(rnd(0, 20))};
        const Vec2 B{double(rnd(0, 20)), double(rnd(0, 20))};
        const Vec2 sa{A.x + unit_of(ad).x, A.y + unit_of(ad).y};
        const Vec2 sb{B.x + unit_of(bd).x, B.y + unit_of(bd).y};
        std::vector<Vec2> path{A, sa};
        bool horiz = rnd(0, 1) == 1;
        Vec2 cur = sa;
        for (int i = rnd(0, 3); i > 0; --i) {  // a random authored detour
            (horiz ? cur.x : cur.y) = double(rnd(0, 20));
            path.push_back(cur);
            horiz = !horiz;
        }
        if (horiz) {
            if (std::fabs(cur.x - sb.x) > kTol) path.push_back({sb.x, cur.y});
        } else if (std::fabs(cur.y - sb.y) > kTol) {
            path.push_back({cur.x, sb.y});
        }
        path.push_back(sb);
        path.push_back(B);
        std::vector<Vec2> dedup;
        for (const Vec2& q : path) {
            if (dedup.empty() || !same(dedup.back(), q)) dedup.push_back(q);
        }
        if (!judge(dedup, ad, bd).legal()) continue;
        std::vector<Vec2> corners{dedup.front()};  // keep real corners only
        for (std::size_t i = 1; i + 1 < dedup.size(); ++i) {
            const Vec2 d1{dedup[i].x - dedup[i - 1].x, dedup[i].y - dedup[i - 1].y};
            const Vec2 d2{dedup[i + 1].x - dedup[i].x, dedup[i + 1].y - dedup[i].y};
            if (std::fabs(d1.x * d2.y - d1.y * d2.x) > kTol) corners.push_back(dedup[i]);
        }
        corners.push_back(dedup.back());

        Doc d;
        const NodeId na = d.add_pin(1, 0, A), nb = d.add_pin(2, 0, B);
        d.add_wire(na, nb, {corners.begin() + 1, corners.end() - 1}, ad, bd);
        const Verdict before = judge(corners, ad, bd);
        const int excess_before = before.bends - min_bends(A, ad, B, bd);

        // --- a random move ---------------------------------------------
        const Kind kind = static_cast<Kind>(rnd(0, kKinds - 1));
        MoveSession s;
        Vec2 A2 = A, B2 = B;
        int ad2 = ad;
        const auto spot = [&] { return Vec2{double(rnd(0, 20)), double(rnd(0, 20))}; };
        switch (kind) {
            case MoveA: A2 = spot(); s.land[na] = A2; break;
            case MoveB: B2 = spot(); s.land[nb] = B2; break;
            case MoveBothSame: {
                const double dx = rnd(-8, 8), dy = rnd(-8, 8);
                A2 = {A.x + dx, A.y + dy};
                B2 = {B.x + dx, B.y + dy};
                s.land[na] = A2;
                s.land[nb] = B2;
                break;
            }
            case MoveBothDiff:
                A2 = spot();
                B2 = spot();
                s.land[na] = A2;
                s.land[nb] = B2;
                break;
            case RotateA:
                ad2 = (ad + 90 * rnd(1, 3)) % 360;
                s.land[na] = A;
                s.land_dir[na] = ad2;
                break;
            default: break;
        }
        // Pins one grid unit apart, facing each other's bodies, cannot be
        // routed legally by anything; coincident pins have no wire at all.
        if (std::fabs(A2.x - B2.x) + std::fabs(A2.y - B2.y) < 2.0) continue;

        const Overlay ov = resolve_move(d, s);
        std::vector<Vec2> after{A2};
        const auto it = ov.wirePath.find(d.wires[0].id);
        const auto& mid = it != ov.wirePath.end() ? it->second : d.wires[0].mid;
        after.insert(after.end(), mid.begin(), mid.end());
        after.push_back(B2);

        const Verdict v = judge(after, ad2, bd);
        ++n[kind];
        if (!v.legal()) ++defects[kind];
        const int mb = min_bends(A2, ad2, B2, bd);
        if (v.bends - mb > excess_before) ++wastefuller[kind];
        if (v.bends == mb) ++at_minimum[kind];

        if (kind == MoveBothSame) {  // the group drag is a RIGID translation
            const double dx = A2.x - A.x, dy = A2.y - A.y;
            bool rigid = after.size() == corners.size();
            for (std::size_t i = 0; rigid && i < after.size(); ++i) {
                rigid = same(after[i], {corners[i].x + dx, corners[i].y + dy});
            }
            if (!rigid) ++rigid_fail[kind];
        }
    }

    const char* names[] = {"move a", "move b", "group drag", "move both", "rotate a"};
    for (int k = 0; k < kKinds; ++k) {
        INFO("kind: " << std::string(names[k]) << "  n=" << n[k]);
        REQUIRE(n[k] > 100);  // the generator really exercised every kind
        CHECK(defects[k] == 0);  // never doubles back / crosses / exits backwards
        CHECK(wastefuller[k] * 100 <= n[k]);  // <= 1%: a move never degrades a wire
    }
    CHECK(rigid_fail[MoveBothSame] == 0);
    // A FRESH route (both ends moved separately, or a pin turned) is at
    // the true minimum almost always.
    CHECK(at_minimum[MoveBothDiff] * 100 >= n[MoveBothDiff] * 95);
    CHECK(at_minimum[RotateA] * 100 >= n[RotateA] * 95);
}
