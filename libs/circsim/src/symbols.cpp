// The symbol geometry half of the stage-2 surface (see symbols.hpp):
// the ci lookup, the shared part transform (pin_pos's mirror-then-
// rotation), the conservative AABB and the new-part prefill. The
// built-in TABLE lives in symbols_table.cpp; .csym libraries feed
// the same shapes through library.hpp.

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include <ws/circsim/symbols.hpp>

namespace ws::circsim {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

const SymbolDef* find_symbol(std::string_view key) {
    const std::string lower_key = lower(key);
    for (const SymbolDef& s : builtin_symbols()) {
        if (lower(s.key) == lower_key) {
            return &s;
        }
    }
    return nullptr;
}

Pos transform_pos(const Part& p, Pos v) {
    double dx = v.x;
    double dy = v.y;
    if (p.mirror) {
        dx = -dx;  // x-flip first
    }
    // Then rotation, CCW, y up.
    int rot = p.rot % 360;
    if (rot < 0) {
        rot += 360;
    }
    switch (rot) {
        case 90: std::swap(dx, dy); dx = -dx; break;
        case 180: dx = -dx; dy = -dy; break;
        case 270: std::swap(dx, dy); dy = -dy; break;
        default: break;  // 0 (and anything off-grid folds to 0)
    }
    return Pos{p.pos.x + dx, p.pos.y + dy};
}

Pos pin_pos(const SymbolDef& s, const Part& p, std::uint32_t pin_index) {
    if (pin_index >= s.pins.size()) {
        return p.pos;  // callers validate the index before asking
    }
    return transform_pos(p, Pos{s.pins[pin_index].dx, s.pins[pin_index].dy});
}

Bounds symbol_bounds(const SymbolDef& s) {
    Bounds b{0.0, 0.0, 0.0, 0.0};
    bool first = true;
    const auto grow = [&](double x, double y) {
        if (first) {
            b = {x, y, x, y};
            first = false;
            return;
        }
        b.min_x = std::min(b.min_x, x);
        b.min_y = std::min(b.min_y, y);
        b.max_x = std::max(b.max_x, x);
        b.max_y = std::max(b.max_y, y);
    };
    for (const PinDef& pin : s.pins) {
        grow(pin.dx, pin.dy);
    }
    for (const DrawPrim& d : s.draw) {
        switch (d.kind) {
            case DrawPrim::Kind::Circle:
            case DrawPrim::Kind::Arc:  // bounded by the full circle
            case DrawPrim::Kind::Dot: {
                if (d.pts.empty()) {
                    break;
                }
                const Pos& c = d.pts[0];
                grow(c.x - d.r, c.y - d.r);
                grow(c.x + d.r, c.y + d.r);
                break;
            }
            case DrawPrim::Kind::Text:
                if (!d.pts.empty()) {
                    grow(d.pts[0].x, d.pts[0].y);  // anchor only
                }
                break;
            default:
                for (const Pos& p : d.pts) {
                    grow(p.x, p.y);
                }
                break;
        }
    }
    // The blade's swing (SwitchBlade): both endpoints ride the bounds -
    // the tip's whole arc lives inside them, so a selection marquee or
    // hit test never loses the thrown blade to the AABB. The built-in
    // s/w blades sit inside their bodies' boxes, but a .csym blade may
    // reach past anything the static prims draw.
    if (s.blade.has_value()) {
        grow(s.blade->pivot.x, s.blade->pivot.y);
        grow(s.blade->open_tip.x, s.blade->open_tip.y);
        grow(s.blade->closed_tip.x, s.blade->closed_tip.y);
    }
    return b;
}

std::map<std::string, std::string> default_params(const SymbolDef& s) {
    std::map<std::string, std::string> out;
    for (const ParamSpec& ps : s.params) {
        if (!ps.default_value.empty()) {
            out[ps.key] = ps.default_value;
        }
    }
    return out;
}

}  // namespace ws::circsim
