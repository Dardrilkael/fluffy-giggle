// symbol draw invariants (stage 17g): the table-level contracts every
// symbol must hold - built-ins AND .csym library symbols alike - so a
// redesign that floats a pin, points one into its own body, or lands
// a degenerate fill fails the gate at the table, before any renderer
// or eyeball ever sees it.
//
// The three rules:
//   reachability - every pin lies ON a drawn stroke (a wire ending at
//     the pin visually meets the symbol; the render seam's safety net
//     exists for .csym stragglers, but a BUILT-IN that needs it is a
//     bug - the stage-2 v/i sources rode it and drew stubs through
//     their own bodies),
//   direction - pin.dir points OUT of the picture (half a unit along
//     it leaves the bounds) and a lead leaves the pin INTO the body
//     (a segment endpoint at the pin pointing against the dir - the
//     stage-2 gnd pointed into its own bars, the mosfet b pin down
//     its own lead),
//   closure - PolyFill carries three or more non-collinear points
//     (an arrowhead that flattens to a line is a typo, not a head).
//
// Plus the grid rule from symbols.hpp: pin offsets are whole grid
// units, so rotated pins stay on grid points and the exact-position
// net builder keeps its integer arithmetic.

#include <cmath>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include <ws/circsim/library.hpp>
#include <ws/circsim/symbols.hpp>

using ws::circsim::Bounds;
using ws::circsim::DrawPrim;
using ws::circsim::PinDef;
using ws::circsim::Pos;
using ws::circsim::SymbolDef;
using ws::circsim::SymbolLibrary;
using ws::circsim::symbol_bounds;

namespace {

constexpr double kTouch = 0.25;  // the safety net's own vertex tolerance

double len(Pos p) {
    return std::sqrt(p.x * p.x + p.y * p.y);
}

Pos unit(Pos p) {
    const double n = len(p);
    return n > 0.0 ? Pos{p.x / n, p.y / n} : Pos{0.0, 0.0};
}

// Squared distance from p to segment ab.
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

// Does this pin lie ON the drawn picture? Lines, polys (open), fills
// (their boundary), circles, arcs (conservatively their full circle -
// no current symbol lands a pin on an arc's sweep), dots.
bool pin_on_picture(const SymbolDef& s, const PinDef& pin) {
    const Pos p{pin.dx, pin.dy};
    for (const DrawPrim& d : s.draw) {
        switch (d.kind) {
            case DrawPrim::Kind::Line:
                if (d.pts.size() >= 2u &&
                    dist_to_segment(d.pts[0], d.pts[1], p) <= kTouch) {
                    return true;
                }
                break;
            case DrawPrim::Kind::Poly:
            case DrawPrim::Kind::PolyFill:
            case DrawPrim::Kind::Rect:
                for (std::size_t i = 0; i + 1 < d.pts.size(); ++i) {
                    if (dist_to_segment(d.pts[i], d.pts[i + 1], p) <=
                        kTouch) {
                        return true;
                    }
                }
                if (d.kind != DrawPrim::Kind::Poly && d.pts.size() >= 3u &&
                    dist_to_segment(d.pts.back(), d.pts.front(), p) <=
                        kTouch) {
                    return true;  // a fill's closing edge
                }
                break;
            case DrawPrim::Kind::Circle:
            case DrawPrim::Kind::Arc:
                if (!d.pts.empty() &&
                    std::fabs(len(Pos{d.pts[0].x - p.x, d.pts[0].y - p.y}) -
                              d.r) <= kTouch) {
                    return true;
                }
                break;
            case DrawPrim::Kind::Dot:
                if (!d.pts.empty() &&
                    len(Pos{d.pts[0].x - p.x, d.pts[0].y - p.y}) <=
                        d.r + kTouch) {
                    return true;
                }
                break;
            case DrawPrim::Kind::Text:
                break;  // text is never a pin's landing
        }
    }
    return false;
}

Pos dir_unit(int dir) {
    switch (((dir % 360) + 360) % 360) {
        case 0: return {1.0, 0.0};
        case 90: return {0.0, 1.0};
        case 180: return {-1.0, 0.0};
        default: return {0.0, -1.0};
    }
}

// Does a lead leave the pin INTO the body? Some prim segment endpoint
// sits on the pin and points against the pin's direction (a diagonal
// emitter lead counts while it keeps more than half its aim), or the
// pin sits ON a curve whose center lies the inward way (the inductor:
// its pins ride the outermost bump of the coil).
bool pin_has_lead(const SymbolDef& s, const PinDef& pin) {
    const Pos p{pin.dx, pin.dy};
    const Pos inward = dir_unit(pin.dir + 180);
    for (const DrawPrim& d : s.draw) {
        if (d.kind == DrawPrim::Kind::Circle || d.kind == DrawPrim::Kind::Arc) {
            if (d.pts.empty() ||
                std::fabs(len(Pos{d.pts[0].x - p.x, d.pts[0].y - p.y}) -
                          d.r) > kTouch) {
                continue;  // the pin is not on this curve
            }
            const Pos to_center = unit(Pos{d.pts[0].x - p.x, d.pts[0].y - p.y});
            if (to_center.x * inward.x + to_center.y * inward.y > 0.5) {
                return true;
            }
            continue;
        }
        const auto edge = [&](Pos a, Pos b) {
            if ((std::fabs(a.x - p.x) <= kTouch &&
                 std::fabs(a.y - p.y) <= kTouch) ||
                (std::fabs(b.x - p.x) <= kTouch &&
                 std::fabs(b.y - p.y) <= kTouch)) {
                // direction from the endpoint AT the pin into the
                // segment's other end
                const Pos from =
                    std::fabs(a.x - p.x) <= kTouch &&
                            std::fabs(a.y - p.y) <= kTouch
                        ? a
                        : b;
                const Pos to = (from.x == a.x && from.y == a.y) ? b : a;
                const Pos into = unit(Pos{to.x - from.x, to.y - from.y});
                if (into.x * inward.x + into.y * inward.y > 0.5) {
                    return true;
                }
            }
            return false;
        };
        if (d.kind == DrawPrim::Kind::Line && d.pts.size() >= 2u) {
            if (edge(d.pts[0], d.pts[1])) {
                return true;
            }
        } else if ((d.kind == DrawPrim::Kind::Poly ||
                    d.kind == DrawPrim::Kind::PolyFill) &&
                   d.pts.size() >= 2u) {
            for (std::size_t i = 0; i + 1 < d.pts.size(); ++i) {
                if (edge(d.pts[i], d.pts[i + 1])) {
                    return true;
                }
            }
        }
    }
    return false;
}

// The whole contract, with the symbol's key for the failure message.
void check_symbol(const SymbolDef& s) {
    INFO("symbol key: ", s.key);
    REQUIRE(!s.pins.empty());

    const Bounds b = symbol_bounds(s);
    for (const PinDef& pin : s.pins) {
        INFO("pin: ", pin.name);
        // Grid rule: whole-unit offsets keep rotated pins on grid.
        CHECK(std::fabs(pin.dx - std::round(pin.dx)) < 1e-9);
        CHECK(std::fabs(pin.dy - std::round(pin.dy)) < 1e-9);

        // Reachability: the pin lands on the picture.
        CHECK(pin_on_picture(s, pin));

        // Direction: half a unit along dir leaves the bounds.
        const Pos probe{pin.dx + dir_unit(pin.dir).x * 0.5,
                        pin.dy + dir_unit(pin.dir).y * 0.5};
        const bool outside = probe.x < b.min_x - 1e-9 ||
                             probe.x > b.max_x + 1e-9 ||
                             probe.y < b.min_y - 1e-9 ||
                             probe.y > b.max_y + 1e-9;
        CHECK(outside);

        // A lead leaves the pin into the body.
        CHECK(pin_has_lead(s, pin));
    }

    // Closure: fills are real triangles/polygons, not flattened lines.
    for (const DrawPrim& d : s.draw) {
        if (d.kind != DrawPrim::Kind::PolyFill || d.pts.size() < 3u) {
            continue;
        }
        double area2 = 0.0;  // the shoelace, doubled
        for (std::size_t i = 0; i < d.pts.size(); ++i) {
            const Pos& a = d.pts[i];
            const Pos& c = d.pts[(i + 1) % d.pts.size()];
            area2 += a.x * c.y - c.x * a.y;
        }
        CHECK(std::fabs(area2) > 0.01);
    }
}

}  // namespace

TEST_CASE("every built-in symbol holds the draw invariants") {
    for (const SymbolDef& s : ws::circsim::builtin_symbols()) {
        check_symbol(s);
    }
}

TEST_CASE("library symbols hold the draw invariants too") {
    // The shipped tline example, inlined like test_symbols does
    // (tests run from the build tree) - a .csym-authored symbol, the
    // proof the invariants are library-general, not table-special.
    constexpr std::string_view kTlineLib = R"json({
        "format": "circsim-symbols", "version": 1,
        "symbols": [{
            "key": "tline", "name": "Lossless transmission line",
            "spice": "T", "ref": "T",
            "pins": [
                {"name": "a1", "pos": [-4.0, 1.0], "dir": 180},
                {"name": "a2", "pos": [-4.0, -1.0], "dir": 180},
                {"name": "b1", "pos": [4.0, 1.0], "dir": 0},
                {"name": "b2", "pos": [4.0, -1.0], "dir": 0}
            ],
            "emit": [
                {"param": "z0", "prefix": "Z0=", "optional": false},
                {"param": "td", "prefix": "TD=", "optional": false}
            ],
            "draw": [
                {"line": [[-4.0, 1.0], [-1.0, 1.0]]},
                {"line": [[-4.0, -1.0], [-1.0, -1.0]]},
                {"line": [[1.0, 1.0], [4.0, 1.0]]},
                {"line": [[1.0, -1.0], [4.0, -1.0]]},
                {"rect": [-1.0, -2.0, 1.0, 2.0]},
                {"line": [[-1.0, 2.0], [1.0, -2.0]]}
            ]
        }]
    })json";

    SymbolLibrary lib;
    std::vector<ws::circsim::Diag> diags;
    REQUIRE(lib.load_text(kTlineLib, "tline.csym", diags).has_value());
    CHECK(diags.empty());
    const SymbolDef* t = lib.find("tline");
    REQUIRE(t != nullptr);
    check_symbol(*t);

    // And a straggler FAILS: a pin the picture never touches, aimed
    // into its own body - the class of bug the suite exists to catch.
    SymbolDef bad;
    bad.key = "bad";
    bad.pins = {PinDef{.name = "a", .dx = -5.0, .dy = 0.0, .dir = 180}};
    DrawPrim body;
    body.kind = DrawPrim::Kind::Circle;
    body.pts = {Pos{0.0, 0.0}};
    body.r = 1.0;
    bad.draw.push_back(body);
    CHECK_FALSE(pin_on_picture(bad, bad.pins[0]));
    CHECK_FALSE(pin_has_lead(bad, bad.pins[0]));
    // dir 180 points away from the body: the outward probe passes but
    // the pin floats - reachability is the rule that catches this one.
    const Bounds bb = symbol_bounds(bad);
    const Pos probe{bad.pins[0].dx - 0.5, bad.pins[0].dy};
    CHECK(probe.x < bb.min_x);  // outside: dir alone is not enough
}
