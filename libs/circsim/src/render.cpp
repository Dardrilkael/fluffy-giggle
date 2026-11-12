#include <algorithm>
#include <cmath>

#include <ws/circsim/render.hpp>

namespace ws::circsim {
namespace {

// The vertex-to-pin tolerance the editor's renderer always used: a pin
// "touches" the picture when a prim VERTEX sits within a quarter unit.
[[nodiscard]] bool near(double a, double b, double eps) {
    return std::fabs(a - b) <= eps;
}

[[nodiscard]] double len(Pos p) {
    return std::sqrt(p.x * p.x + p.y * p.y);
}

}  // namespace

std::vector<SymbolStroke> render_symbol(const SymbolDef& sym,
                                        const Part& placement,
                                        double switch_closed) {
    std::vector<SymbolStroke> out;
    out.reserve(sym.draw.size() + sym.pins.size() +
                (sym.blade.has_value() ? 1u : 0u));

    // transform_pos's exact math (mirror, then rotation, then
    // translation) - the one shared transform, so the picture and the
    // pins can never disagree.
    const auto place = [&](Pos local) {
        return transform_pos(placement, local);
    };

    for (const DrawPrim& d : sym.draw) {
        switch (d.kind) {
            case DrawPrim::Kind::Line:
                if (d.pts.size() >= 2u) {
                    SymbolStroke s;
                    s.kind = SymbolStroke::Kind::Line;
                    s.pts = {place(d.pts[0]), place(d.pts[1])};
                    out.push_back(std::move(s));
                }
                break;
            case DrawPrim::Kind::Poly: {
                if (d.pts.size() < 2u) {
                    break;
                }
                SymbolStroke s;
                s.kind = SymbolStroke::Kind::Poly;
                s.pts.reserve(d.pts.size());
                for (const Pos& p : d.pts) {
                    s.pts.push_back(place(p));
                }
                // symbols.hpp's contract: Poly is an OPEN polyline -
                // a shape that means to close repeats its first vertex
                // (the diode's triangle, the transistor arrowheads).
                // The stage-2 renderer closed every poly unconditionally
                // instead, hanging a phantom segment off the resistor's
                // zigzag; the flag now answers the contract.
                s.closed = false;
                out.push_back(std::move(s));
                break;
            }
            case DrawPrim::Kind::PolyFill: {
                if (d.pts.size() < 3u) {
                    break;
                }
                SymbolStroke s;
                s.kind = SymbolStroke::Kind::PolyFill;
                s.pts.reserve(d.pts.size());
                for (const Pos& p : d.pts) {
                    s.pts.push_back(place(p));
                }
                out.push_back(std::move(s));
                break;
            }
            case DrawPrim::Kind::Rect: {
                if (d.pts.size() < 2u) {
                    break;
                }
                // A closed quad, not an axis-aligned rect call: the
                // placement may have rotated the box.
                SymbolStroke s;
                s.kind = SymbolStroke::Kind::Poly;
                s.pts = {place(d.pts[0]),
                         place(Pos{d.pts[1].x, d.pts[0].y}),
                         place(d.pts[1]),
                         place(Pos{d.pts[0].x, d.pts[1].y})};
                s.closed = true;
                out.push_back(std::move(s));
                break;
            }
            case DrawPrim::Kind::Circle:
                if (!d.pts.empty()) {
                    SymbolStroke s;
                    s.kind = SymbolStroke::Kind::Circle;
                    s.pts = {place(d.pts[0])};
                    s.r = d.r;
                    out.push_back(std::move(s));
                }
                break;
            case DrawPrim::Kind::Arc: {
                if (d.pts.empty()) {
                    break;
                }
                // Mirror flips an arc to its reflection (angles
                // negated about the y axis, sweep swapped); rotation
                // adds to both ends. Both in degrees here.
                SymbolStroke s;
                s.kind = SymbolStroke::Kind::Arc;
                s.pts = {place(d.pts[0])};
                s.r = d.r;
                s.a0 = d.a0;
                s.a1 = d.a1;
                if (placement.mirror) {
                    s.a0 = 180.0 - d.a1;
                    s.a1 = 180.0 - d.a0;
                }
                const int add = ((placement.rot % 360) + 360) % 360;
                s.a0 += static_cast<double>(add);
                s.a1 += static_cast<double>(add);
                out.push_back(std::move(s));
                break;
            }
            case DrawPrim::Kind::Dot:
                if (!d.pts.empty()) {
                    SymbolStroke s;
                    s.kind = SymbolStroke::Kind::Dot;
                    s.pts = {place(d.pts[0])};
                    s.r = d.r;
                    out.push_back(std::move(s));
                }
                break;
            case DrawPrim::Kind::Text:
                if (!d.pts.empty()) {
                    SymbolStroke s;
                    s.kind = SymbolStroke::Kind::Text;
                    s.pts = {place(d.pts[0])};
                    s.text = d.text;
                    out.push_back(std::move(s));
                }
                break;
        }
    }

    // The switch blade (SwitchBlade): the one prim the placement does
    // not own outright. The endpoints render VERBATIM - t=0 is the
    // authored open rest (bit-identical to the old static line), t=1
    // the thrown tip - and between them the tip sweeps the angle's
    // shortest rotational path and the radius about the pivot. The
    // interpolation happens in the symbol's own local space and THEN
    // rides the shared transform, so mirror/rotation fold into the
    // moving tip exactly as they do into every static vertex.
    if (sym.blade.has_value()) {
        const SwitchBlade& b = *sym.blade;
        const Pos open_off{b.open_tip.x - b.pivot.x,
                           b.open_tip.y - b.pivot.y};
        const Pos closed_off{b.closed_tip.x - b.pivot.x,
                             b.closed_tip.y - b.pivot.y};
        const double t = std::clamp(switch_closed, 0.0, 1.0);
        Pos tip = b.open_tip;
        if (t >= 1.0) {
            tip = b.closed_tip;
        } else if (t > 0.0) {
            const double a0 = std::atan2(open_off.y, open_off.x);
            const double a1 = std::atan2(closed_off.y, closed_off.x);
            double sweep = a1 - a0;
            constexpr double kTwoPi = 6.283185307179586476925287;
            while (sweep > 3.14159265358979323846) {
                sweep -= kTwoPi;  // the shortest rotational path
            }
            while (sweep < -3.14159265358979323846) {
                sweep += kTwoPi;
            }
            const double r0 = len(open_off);
            const double r1 = len(closed_off);
            const double a = a0 + sweep * t;
            const double r = r0 + (r1 - r0) * t;
            tip = Pos{b.pivot.x + r * std::cos(a),
                      b.pivot.y + r * std::sin(a)};
        }
        SymbolStroke s;
        s.kind = SymbolStroke::Kind::Line;
        s.pts = {place(b.pivot), place(tip)};
        out.push_back(std::move(s));
    }

    // Does any draw primitive reach this pin? A prim VERTEX within a
    // quarter unit, or a pin ON a curve - a circle/arc's own curve
    // (the inductor's coil: its pins sit on the outermost bump, no
    // vertex anywhere near, and the vertex-only rule drew a stub
    // straight through the coil) or inside a dot.
    const auto prim_touches = [&](const Pos& pin) {
        for (const DrawPrim& d : sym.draw) {
            for (const Pos& v : d.pts) {
                if (near(v.x, pin.x, 0.25) && near(v.y, pin.y, 0.25)) {
                    return true;
                }
            }
            const double curve_off =
                std::fabs(len(Pos{d.pts.empty() ? 0.0 : d.pts[0].x - pin.x,
                                  d.pts.empty() ? 0.0 : d.pts[0].y - pin.y}) -
                          d.r);
            switch (d.kind) {
                case DrawPrim::Kind::Circle:
                case DrawPrim::Kind::Arc:
                    if (!d.pts.empty() && curve_off <= 0.25) {
                        return true;
                    }
                    break;
                case DrawPrim::Kind::Dot:
                    if (!d.pts.empty() &&
                        len(Pos{d.pts[0].x - pin.x, d.pts[0].y - pin.y}) <=
                            d.r + 0.25) {
                        return true;
                    }
                    break;
                default:
                    break;
            }
        }
        return false;
    };

    // The pin-lead safety net: a pin nothing touches gets a stub
    // pointing into the body (opposite the pin's own direction).
    const Bounds b = symbol_bounds(sym);
    for (const PinDef& pin : sym.pins) {
        const Pos local{pin.dx, pin.dy};
        if (prim_touches(local)) {
            continue;
        }
        const Pos u = [&] {
            switch ((((pin.dir + 180) % 360) + 360) % 360) {
                case 0: return Pos{1.0, 0.0};
                case 90: return Pos{0.0, 1.0};
                case 180: return Pos{-1.0, 0.0};
                default: return Pos{0.0, -1.0};
            }
        }();
        // Until the bounds' edge (plus a little overlap), one unit
        // minimum, three maximum - a lead, not a transmission line.
        const bool outside_x =
            local.x < b.min_x || local.x > b.max_x;
        const bool outside_y =
            local.y < b.min_y || local.y > b.max_y;
        double reach = 1.0;
        if (outside_x) {
            reach = std::fabs(u.x) > 0.0
                        ? std::fabs(local.x - (u.x > 0.0 ? b.max_x : b.min_x))
                        : 1.0;
        } else if (outside_y) {
            reach = std::fabs(u.y) > 0.0
                        ? std::fabs(local.y - (u.y > 0.0 ? b.max_y : b.min_y))
                        : 1.0;
        }
        reach = std::max(1.0, std::min(3.0, reach + 0.5));
        SymbolStroke s;
        s.kind = SymbolStroke::Kind::Line;
        s.pts = {place(local),
                 place(Pos{local.x + u.x * reach,
                           local.y + u.y * reach})};
        out.push_back(std::move(s));
    }

    return out;
}

}  // namespace ws::circsim
