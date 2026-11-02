#include <algorithm>
#include <array>
#include <cctype>

#include <ws/circsim/symbols.hpp>

namespace ws::circsim {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// --- spec + primitive builders (keep the table below declarative) ---------

ParamSpec spec(ParamSpec::Type type, std::string key, std::string label,
               std::string default_value = "") {
    ParamSpec p;
    p.type = type;
    p.key = std::move(key);
    p.label = std::move(label);
    p.default_value = std::move(default_value);
    return p;
}

DrawPrim line(double x1, double y1, double x2, double y2) {
    DrawPrim d;
    d.kind = DrawPrim::Kind::Line;
    d.pts = {Pos{x1, y1}, Pos{x2, y2}};
    return d;
}

DrawPrim poly(std::vector<Pos> pts) {
    DrawPrim d;
    d.kind = DrawPrim::Kind::Poly;
    d.pts = std::move(pts);
    return d;
}

DrawPrim circle(double cx, double cy, double r) {
    DrawPrim d;
    d.kind = DrawPrim::Kind::Circle;
    d.pts = {Pos{cx, cy}};
    d.r = r;
    return d;
}

DrawPrim arc(double cx, double cy, double r, double a0, double a1) {
    DrawPrim d;
    d.kind = DrawPrim::Kind::Arc;
    d.pts = {Pos{cx, cy}};
    d.r = r;
    d.a0 = a0;
    d.a1 = a1;
    return d;
}

DrawPrim dot(double cx, double cy, double r) {
    DrawPrim d;
    d.kind = DrawPrim::Kind::Dot;
    d.pts = {Pos{cx, cy}};
    d.r = r;
    return d;
}

DrawPrim text(double x, double y, std::string s) {
    DrawPrim d;
    d.kind = DrawPrim::Kind::Text;
    d.pts = {Pos{x, y}};
    d.text = std::move(s);
    return d;
}

// Two-terminal passives share one pin layout (and one emit tail).
SymbolDef two_terminal(std::string key, std::string name, char letter,
                       std::string prefix, std::vector<ParamEmit> emit,
                       std::vector<DrawPrim> draw,
                       std::vector<ParamSpec> params = {},
                       bool needs_value = true) {
    SymbolDef s;
    s.key = std::move(key);
    s.name = std::move(name);
    s.spice_letter = letter;
    s.ref_prefix = std::move(prefix);
    s.pins = {PinDef{.name = "a", .dx = -3.0, .dy = 0.0, .dir = 180},
              PinDef{.name = "b", .dx = 3.0, .dy = 0.0, .dir = 0}};
    s.emit = std::move(emit);
    s.params = std::move(params);
    s.draw = std::move(draw);
    s.needs_value = needs_value;
    return s;
}

// The BJT / MOSFET families share everything but the arrow and the
// substrate blob; one body, small per-flavor deltas.
SymbolDef bjt(std::string key, std::string name, bool pnp) {
    SymbolDef s;
    s.key = std::move(key);
    s.name = std::move(name);
    s.spice_letter = 'Q';
    s.ref_prefix = "Q";
    s.pins = {PinDef{.name = "c", .dx = 2.0, .dy = 3.0, .dir = 90},
              PinDef{.name = "b", .dx = -3.0, .dy = 0.0, .dir = 180},
              PinDef{.name = "e", .dx = 2.0, .dy = -3.0, .dir = 270}};
    s.emit = {ParamEmit{.key = "model"}, ParamEmit{.key = "area"}};
    s.needs_value = false;
    s.needs_model = true;
    s.params = {spec(ParamSpec::Type::Text, "model", "Model"),
                spec(ParamSpec::Type::Number, "area", "Area")};
    s.draw = {// base lead + bar
              line(-3.0, 0.0, -1.0, 0.0), line(-1.0, -1.6, -1.0, 1.6),
              // collector + emitter strokes
              line(-1.0, 0.8, 2.0, 3.0), line(-1.0, -0.8, 2.0, -3.0)};
    // The emitter arrow: out along the emitter (NPN), in at the bar (PNP).
    s.draw.push_back(pnp ? dot(-0.25, -1.35, 0.35) : dot(1.25, -2.45, 0.35));
    return s;
}

SymbolDef mosfet(std::string key, std::string name, bool pmos) {
    SymbolDef s;
    s.key = std::move(key);
    s.name = std::move(name);
    s.spice_letter = 'M';
    s.ref_prefix = "M";
    s.pins = {PinDef{.name = "d", .dx = 2.0, .dy = 3.0, .dir = 90},
              PinDef{.name = "g", .dx = -3.0, .dy = 0.0, .dir = 180},
              PinDef{.name = "s", .dx = 2.0, .dy = -3.0, .dir = 270},
              PinDef{.name = "b", .dx = -3.0, .dy = -3.0, .dir = 270}};
    s.emit = {ParamEmit{.key = "model"}, ParamEmit{.key = "l", .prefix = "L="},
              ParamEmit{.key = "w", .prefix = "W="}};
    s.needs_value = false;
    s.needs_model = true;
    s.params = {spec(ParamSpec::Type::Text, "model", "Model"),
                spec(ParamSpec::Type::Number, "l", "L"),
                spec(ParamSpec::Type::Number, "w", "W")};
    s.draw = {// gate lead + gate bar + channel bar
              line(-3.0, 0.0, -1.0, 0.0), line(-1.0, -1.6, -1.0, 1.6),
              line(0.0, -1.6, 0.0, 1.6),
              // drain + source strokes
              line(0.0, 1.0, 2.0, 3.0), line(0.0, -1.0, 2.0, -3.0),
              // bulk route down and out to the b pin
              poly({Pos{0.0, -1.0}, Pos{-1.0, -1.0}, Pos{-1.0, -3.0},
                    Pos{-3.0, -3.0}})};
    // The substrate arrow: at the channel (PMOS) vs at the pin (NMOS).
    s.draw.push_back(pmos ? dot(-0.55, -1.0, 0.3) : dot(-2.45, -3.0, 0.3));
    return s;
}

const std::vector<SymbolDef>& table() {
    static const std::vector<SymbolDef> symbols = {
        two_terminal(
            "r", "Resistor", 'R', "R", {},
            {// the classic zigzag body between the pins
             poly({Pos{-3.0, 0.0}, Pos{-2.0, 0.6}, Pos{-1.0, -0.6},
                  Pos{0.0, 0.6}, Pos{1.0, -0.6}, Pos{2.0, 0.6},
                  Pos{3.0, 0.0}})}),
        two_terminal(
            "c", "Capacitor", 'C', "C", {ParamEmit{.key = "ic", .prefix = "IC="}},
            {line(-3.0, 0.0, -0.6, 0.0), line(-0.6, -1.6, -0.6, 1.6),
             line(0.6, -1.6, 0.6, 1.6), line(0.6, 0.0, 3.0, 0.0)},
            {spec(ParamSpec::Type::Number, "ic", "IC")}),
        two_terminal(
            "l", "Inductor", 'L', "L", {ParamEmit{.key = "ic", .prefix = "IC="}},
            {arc(-2.25, 0.0, 0.75, 0.0, 180.0), arc(-0.75, 0.0, 0.75, 0.0, 180.0),
             arc(0.75, 0.0, 0.75, 0.0, 180.0), arc(2.25, 0.0, 0.75, 0.0, 180.0)},
            {spec(ParamSpec::Type::Number, "ic", "IC")}),
        // Sources: p/n, value then AC then waveform (the ngspice grammar
        // order - "V1 a b 5 AC 1 PULSE(...)").
        [] {
            SymbolDef s;
            s.key = "v";
            s.name = "Voltage source";
            s.spice_letter = 'V';
            s.ref_prefix = "V";
            s.pins = {PinDef{.name = "p", .dx = -1.0, .dy = 0.0, .dir = 180},
                      PinDef{.name = "n", .dx = 1.0, .dy = 0.0, .dir = 0}};
            s.emit = {ParamEmit{.key = "ac", .prefix = "AC "},
                      ParamEmit{.key = "wave", .prefix = ""}};
            s.params = {spec(ParamSpec::Type::Text, "ac", "AC"),
                        spec(ParamSpec::Type::Text, "wave", "Waveform")};
            s.draw = {circle(0.0, 0.0, 1.0), text(-0.55, -0.15, "+")};
            return s;
        }(),
        [] {
            SymbolDef s;
            s.key = "i";
            s.name = "Current source";
            s.spice_letter = 'I';
            s.ref_prefix = "I";
            s.pins = {PinDef{.name = "p", .dx = -1.0, .dy = 0.0, .dir = 180},
                      PinDef{.name = "n", .dx = 1.0, .dy = 0.0, .dir = 0}};
            s.emit = {ParamEmit{.key = "ac", .prefix = "AC "},
                      ParamEmit{.key = "wave", .prefix = ""}};
            s.params = {spec(ParamSpec::Type::Text, "ac", "AC"),
                        spec(ParamSpec::Type::Text, "wave", "Waveform")};
            s.draw = {circle(0.0, 0.0, 1.0), line(0.0, -0.6, 0.0, 0.6),
                      line(0.0, 0.6, -0.25, 0.3), line(0.0, 0.6, 0.25, 0.3)};
            return s;
        }(),
        // Diode: anode/cathode; the model token rides where a value would.
        [] {
            SymbolDef s;
            s.key = "d";
            s.name = "Diode";
            s.spice_letter = 'D';
            s.ref_prefix = "D";
            s.pins = {PinDef{.name = "a", .dx = -3.0, .dy = 0.0, .dir = 180},
                      PinDef{.name = "k", .dx = 3.0, .dy = 0.0, .dir = 0}};
            s.emit = {ParamEmit{.key = "model"}, ParamEmit{.key = "area"}};
            s.needs_value = false;
            s.needs_model = true;
            s.params = {spec(ParamSpec::Type::Text, "model", "Model"),
                        spec(ParamSpec::Type::Number, "area", "Area")};
            s.draw = {line(-3.0, 0.0, -1.0, 0.0),
                      poly({Pos{-1.0, 1.0}, Pos{-1.0, -1.0}, Pos{1.0, 0.0},
                            Pos{-1.0, 1.0}}),
                      line(1.0, -1.0, 1.0, 1.0), line(1.0, 0.0, 3.0, 0.0)};
            return s;
        }(),
        // BJTs: C B E pin order, y-up (collector on top).
        bjt("npn", "NPN transistor", false),
        bjt("pnp", "PNP transistor", true),
        // MOSFETs: D G S B pin order, y-up (drain on top).
        mosfet("nmos", "N-channel MOSFET", false),
        mosfet("pmos", "P-channel MOSFET", true),
        // Ground / power: single fixed-net pins, never exported.
        [] {
            SymbolDef s;
            s.key = "gnd";
            s.name = "Ground";
            s.pins = {PinDef{.name = "0", .dx = 0.0, .dy = 0.0, .dir = 270}};
            s.fixed_net = "0";
            s.needs_value = false;
            s.draw = {line(0.0, 0.0, 0.0, -1.0), line(-1.5, -1.0, 1.5, -1.0),
                      line(-1.0, -2.0, 1.0, -2.0),
                      line(-0.5, -3.0, 0.5, -3.0)};
            return s;
        }(),
        [] {
            SymbolDef s;
            s.key = "vcc";
            s.name = "Supply (VCC)";
            s.pins = {PinDef{.name = "p", .dx = 0.0, .dy = 0.0, .dir = 90}};
            s.fixed_net = "VCC";
            s.needs_value = false;
            s.draw = {line(0.0, 0.0, 0.0, -1.0), line(-1.5, -1.0, 1.5, -1.0),
                      text(-1.4, -2.4, "VCC")};
            return s;
        }(),
    };
    return symbols;
}

}  // namespace

const std::vector<SymbolDef>& builtin_symbols() {
    return table();
}

const SymbolDef* find_symbol(std::string_view key) {
    const std::string lower_key = lower(key);
    for (const SymbolDef& s : table()) {
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
