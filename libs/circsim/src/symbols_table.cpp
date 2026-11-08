// The built-in symbol table (stage 2, redrawn stage 17): the twelve
// keys - r, c, l, v, i, d, npn, pnp, nmos, pmos, gnd, vcc - with
// their param specs, emit tails and drawing primitives. Pure data:
// the geometry that READS it (lookup, transform, bounds, the render
// seam) lives in symbols.cpp/render.cpp; a .csym library layers over
// this table through library.hpp.

#include <string>
#include <utility>
#include <vector>

#include <ws/circsim/symbols.hpp>

namespace ws::circsim {

namespace {
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

DrawPrim polyfill(std::vector<Pos> pts) {
    DrawPrim d;
    d.kind = DrawPrim::Kind::PolyFill;
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
//
// The emitter arrow is a FILLED head riding the emitter stroke: out
// toward the emitter pin (NPN), in at the bar (PNP) - the convention
// that tells the two apart at a glance. The stage-2 table marked the
// spot with a dot; a dot carries no direction.
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
    // The emitter arrowhead (computed on the emitter stroke's own
    // direction: tip at t=0.78 for the NPN, t=0.25 for the PNP; base
    // 0.19 behind it, half-width 0.28).
    s.draw.push_back(pnp
                         ? polyfill({Pos{-0.25, -1.35}, Pos{0.486, -1.542},
                                     Pos{0.154, -1.994}})
                         : polyfill({Pos{1.34, -2.516}, Pos{0.966, -1.894},
                                     Pos{0.634, -2.346}}));
    return s;
}

SymbolDef mosfet(std::string key, std::string name, bool pmos) {
    SymbolDef s;
    s.key = std::move(key);
    s.name = std::move(name);
    s.spice_letter = 'M';
    s.ref_prefix = "M";
    // The bulk pin points LEFT (dir 180): its lead arrives from the
    // right along y=-3, so a wire leaving the pin runs left, away
    // from the body (the stage-2 table said 270 - down its own lead).
    s.pins = {PinDef{.name = "d", .dx = 2.0, .dy = 3.0, .dir = 90},
              PinDef{.name = "g", .dx = -3.0, .dy = 0.0, .dir = 180},
              PinDef{.name = "s", .dx = 2.0, .dy = -3.0, .dir = 270},
              PinDef{.name = "b", .dx = -3.0, .dy = -3.0, .dir = 180}};
    s.emit = {ParamEmit{.key = "model"}, ParamEmit{.key = "l", .prefix = "L="},
              ParamEmit{.key = "w", .prefix = "W="}};
    s.needs_value = false;
    s.needs_model = true;
    s.params = {spec(ParamSpec::Type::Text, "model", "Model"),
                spec(ParamSpec::Type::Number, "l", "L"),
                spec(ParamSpec::Type::Number, "w", "W")};
    // The bulk tees off the CHANNEL BAR's bottom (0,-1.6), drops to
    // y=-3 and runs left to the pin. The stage-2 route teed at
    // (0,-1) - the source stroke's own start - reading as a
    // source-to-bulk short at every zoom.
    s.draw = {// gate lead + gate bar + channel bar
              line(-3.0, 0.0, -1.0, 0.0), line(-1.0, -1.6, -1.0, 1.6),
              line(0.0, -1.6, 0.0, 1.6),
              // drain + source strokes
              line(0.0, 1.0, 2.0, 3.0), line(0.0, -1.0, 2.0, -3.0),
              // bulk route down and out to the b pin
              poly({Pos{0.0, -1.6}, Pos{0.0, -3.0}, Pos{-3.0, -3.0}})};
    // The substrate arrow at the top of the bulk drop: INTO the
    // channel (up, NMOS - the P bulk's diode pointing at the N
    // channel) vs AWAY from it (down, PMOS).
    s.draw.push_back(pmos
                         ? polyfill({Pos{0.0, -2.15}, Pos{-0.28, -1.6},
                                     Pos{0.28, -1.6}})
                         : polyfill({Pos{0.0, -1.6}, Pos{-0.28, -2.15},
                                     Pos{0.28, -2.15}}));
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
        // order - "V1 a b 5 AC 1 PULSE(...)"). Pins at +-3 with real
        // lead prims (the safety net must never fire: a pin ON the
        // circle has no prim vertex there, and the net's stub drew a
        // diameter line straight through the body - the stage-17d fix).
        // Polarity drawn as LINES, not text: + and - read identically
        // headless, at any zoom, in any font.
        [] {
            SymbolDef s;
            s.key = "v";
            s.name = "Voltage source";
            s.spice_letter = 'V';
            s.ref_prefix = "V";
            s.pins = {PinDef{.name = "p", .dx = -3.0, .dy = 0.0, .dir = 180},
                      PinDef{.name = "n", .dx = 3.0, .dy = 0.0, .dir = 0}};
            s.emit = {ParamEmit{.key = "ac", .prefix = "AC "},
                      ParamEmit{.key = "wave", .prefix = ""}};
            s.params = {spec(ParamSpec::Type::Text, "ac", "AC"),
                        spec(ParamSpec::Type::Text, "wave", "Waveform")};
            s.draw = {// the leads: circle edge to pin, prim vertices ON
                      // the pins so the picture owns them
                      line(-3.0, 0.0, -1.0, 0.0), line(1.0, 0.0, 3.0, 0.0),
                      circle(0.0, 0.0, 1.0),
                      // + at the p side, - at the n side (line-drawn)
                      line(-0.65, 0.0, -0.05, 0.0),
                      line(-0.35, -0.3, -0.35, 0.3),
                      line(0.05, 0.0, 0.65, 0.0)};
            return s;
        }(),
        [] {
            SymbolDef s;
            s.key = "i";
            s.name = "Current source";
            s.spice_letter = 'I';
            s.ref_prefix = "I";
            s.pins = {PinDef{.name = "p", .dx = -3.0, .dy = 0.0, .dir = 180},
                      PinDef{.name = "n", .dx = 3.0, .dy = 0.0, .dir = 0}};
            s.emit = {ParamEmit{.key = "ac", .prefix = "AC "},
                      ParamEmit{.key = "wave", .prefix = ""}};
            s.params = {spec(ParamSpec::Type::Text, "ac", "AC"),
                        spec(ParamSpec::Type::Text, "wave", "Waveform")};
            s.draw = {// the leads, like the v source
                      line(-3.0, 0.0, -1.0, 0.0), line(1.0, 0.0, 3.0, 0.0),
                      circle(0.0, 0.0, 1.0),
                      // the arrow: shaft up the middle, head a filled
                      // triangle (pointing +y; the fill closes it)
                      line(0.0, -0.7, 0.0, 0.25),
                      polyfill({Pos{0.0, 0.75}, Pos{-0.3, 0.25},
                                Pos{0.3, 0.25}})};
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
        // LED: a diode that reports its light. The SPICE identity is the
        // diode's (letter D, model + area, ref Dn) - the deck cannot tell
        // them apart. The `color` Choice is the editor's alone: spec'd (so
        // ERC's unused_param never fires and the property grid offers the
        // list) but NOT emitted (the deck stays a plain diode card), and
        // the canvas paints the glow in it. The emission arrows ride the
        // same 45-degree up-right the convention draws them at; the
        // arrowheads are hand-placed triangles in the BJT's own style.
        [] {
            SymbolDef s;
            s.key = "led";
            s.name = "LED (light emitting)";
            s.spice_letter = 'D';
            s.ref_prefix = "D";
            s.pins = {PinDef{.name = "a", .dx = -3.0, .dy = 0.0, .dir = 180},
                      PinDef{.name = "k", .dx = 3.0, .dy = 0.0, .dir = 0}};
            s.emit = {ParamEmit{.key = "model"}, ParamEmit{.key = "area"}};
            s.needs_value = false;
            s.needs_model = true;
            s.params = {
                spec(ParamSpec::Type::Text, "model", "Model"),
                spec(ParamSpec::Type::Number, "area", "Area"),
                spec(ParamSpec::Type::Choice, "color", "Color", "red")};
            s.params.back().choices = {"red", "yellow", "green",
                                       "blue", "white", "orange"};
            s.draw = {// the diode body: leads, triangle, bar
                      line(-3.0, 0.0, -1.0, 0.0),
                      poly({Pos{-1.0, 1.0}, Pos{-1.0, -1.0}, Pos{1.0, 0.0},
                            Pos{-1.0, 1.0}}),
                      line(1.0, -1.0, 1.0, 1.0), line(1.0, 0.0, 3.0, 0.0),
                      // the light: two emission arrows at 45 degrees
                      // up-right, clear of the triangle's upper edge and
                      // the bar (hand-placed in the BJT arrowhead's style:
                      // head length 0.34, half-width 0.15)
                      line(0.35, 0.55, 0.81, 1.01),
                      polyfill({Pos{1.05, 1.25}, Pos{0.704, 1.116},
                                Pos{0.916, 0.904}}),
                      line(0.05, 0.90, 0.41, 1.26),
                      polyfill({Pos{0.65, 1.50}, Pos{0.304, 1.366},
                                Pos{0.516, 1.154}})};
            return s;
        }(),
        // BJTs: C B E pin order, y-up (collector on top).
        bjt("npn", "NPN transistor", false),
        bjt("pnp", "PNP transistor", true),
        // MOSFETs: D G S B pin order, y-up (drain on top).
        mosfet("nmos", "N-channel MOSFET", false),
        mosfet("pmos", "P-channel MOSFET", true),
        // Ground / power: single fixed-net pins, never exported. The
        // pin is where the wire lands; the body hangs BELOW it (the
        // bars) and the pin points UP into the circuit (dir 90 - the
        // wire-start direction; the stage-2 table said 270, straight
        // into its own bars).
        [] {
            SymbolDef s;
            s.key = "gnd";
            s.name = "Ground";
            s.pins = {PinDef{.name = "0", .dx = 0.0, .dy = 0.0, .dir = 90}};
            s.fixed_net = "0";
            s.needs_value = false;
            s.draw = {line(0.0, 0.0, 0.0, -1.0), line(-1.5, -1.0, 1.5, -1.0),
                      line(-1.0, -2.0, 1.0, -2.0),
                      line(-0.5, -3.0, 0.5, -3.0)};
            return s;
        }(),
        // The supply faces UP: the bar and label sit ABOVE the node
        // (the stage-2 table hung them below - a ground with text).
        // The pin points DOWN into the circuit below it.
        [] {
            SymbolDef s;
            s.key = "vcc";
            s.name = "Supply (VCC)";
            s.pins = {PinDef{.name = "p", .dx = 0.0, .dy = 0.0, .dir = 270}};
            s.fixed_net = "VCC";
            s.needs_value = false;
            s.draw = {line(0.0, 0.0, 0.0, 1.0), line(-1.5, 1.0, 1.5, 1.0),
                      text(-1.4, 2.4, "VCC")};
            return s;
        }(),
    };
    return symbols;
}

}  // namespace

const std::vector<SymbolDef>& builtin_symbols() {
    return table();
}

}  // namespace ws::circsim
