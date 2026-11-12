// The built-in symbol table (stage 2, redrawn stage 17, widened stage
// 24, stage 25): the r, c, l, v, i, d, npn, pnp, nmos, pmos, gnd, vcc
// set plus the orphan devices ws::spice always supported but no symbol
// ever carried into a schematic - the controlled sources (e, f, g, h),
// the behavioral source (b), the switches (s, w), the lossless line
// (tline), the coupling (k) and the 2-winding transformer (xfmr, one
// part = the L + L + K card trio) - and the op amp (opamp, the X
// subckt instance whose cards live in the built-in model library).
// The semiconductor symbols declare their model_kind so the property
// grid and ERC know what the `model` param names. Pure data: the
// geometry that READS it (lookup, transform, bounds, the render seam)
// lives in symbols.cpp/render.cpp; a .csym library layers over this
// table through library.hpp.

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

DrawPrim dot(double cx, double cy, double r) {
    DrawPrim d;
    d.kind = DrawPrim::Kind::Dot;
    d.pts = {Pos{cx, cy}};
    d.r = r;
    return d;
}

DrawPrim rect(double x1, double y1, double x2, double y2) {
    DrawPrim d;
    d.kind = DrawPrim::Kind::Rect;
    d.pts = {Pos{x1, y1}, Pos{x2, y2}};
    return d;
}

// A closed diamond centred on the origin: the IEEE outline of a
// dependent (controlled) source.
DrawPrim diamond(double hw, double hh) {
    return poly({Pos{-hw, 0.0}, Pos{0.0, hh}, Pos{hw, 0.0}, Pos{0.0, -hh},
                 Pos{-hw, 0.0}});
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
    s.model_kind = pnp ? "pnp" : "npn";
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
    s.model_kind = pmos ? "pmos" : "nmos";
    s.params = {spec(ParamSpec::Type::Text, "model", "Model"),
                spec(ParamSpec::Type::Number, "l", "L"),
                spec(ParamSpec::Type::Number, "w", "W")};
    // The bulk tees off the CHANNEL BAR's bottom (0,-1.6), drops to
    // y=-3 and runs left to the pin. The stage-2 route teed at
    // (0,-1) - the source stroke's own start - reading as a
    // source-to-bulk short at every zoom.
    s.draw = {// gate lead + gate bar + channel bar
              line(-3.0, 0.0, -1.0, 0.0), line(-1.0, -1.6, -1.0, 1.6),
              // the channel as three segments (the enhancement-mode
              // convention: no channel until the gate pulls one in).
              // The drain stroke lands on the top segment (y=1.0), the
              // source stroke on the bottom one (y=-1.0), and the bulk
              // drop below still tees off the bottom segment's end.
              line(0.0, 1.6, 0.0, 0.55), line(0.0, 0.3, 0.0, -0.3),
              line(0.0, -0.55, 0.0, -1.6),
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

// The JFETs share the BJT's three-pin layout (D G S in card order,
// drain on top) with the MOSFET's bar-and-arrow language: one vertical
// channel bar (the gate lead enters it), the drain/source strokes off
// its ends, and the TYPE arrow on the gate lead - INTO the channel
// (n-channel: the P-gate's arrow points at the N channel, the BJT
// emitter-arrow convention) vs back out at the gate pin (p-channel).
SymbolDef jfet(std::string key, std::string name, bool pjf) {
    SymbolDef s;
    s.key = std::move(key);
    s.name = std::move(name);
    s.spice_letter = 'J';
    s.ref_prefix = "J";
    s.pins = {PinDef{.name = "d", .dx = 2.0, .dy = 3.0, .dir = 90},
              PinDef{.name = "g", .dx = -3.0, .dy = 0.0, .dir = 180},
              PinDef{.name = "s", .dx = 2.0, .dy = -3.0, .dir = 270}};
    s.emit = {ParamEmit{.key = "model"}, ParamEmit{.key = "area"}};
    s.needs_value = false;
    s.needs_model = true;
    s.model_kind = pjf ? "pjf" : "njf";
    s.params = {spec(ParamSpec::Type::Text, "model", "Model"),
                spec(ParamSpec::Type::Number, "area", "Area")};
    s.draw = {// gate lead + the channel bar it enters
              line(-3.0, 0.0, -1.0, 0.0), line(-1.0, -1.6, -1.0, 1.6),
              // drain + source strokes off the bar's ends
              line(-1.0, 0.8, 2.0, 3.0), line(-1.0, -0.8, 2.0, -3.0)};
    // The type arrowhead riding the gate lead: tip AT the channel bar
    // (n-channel, pointing in) vs tip back at the lead (p-channel,
    // pointing out at the gate pin). Head length 0.8, half-width 0.28
    // (the BJT arrowhead's proportions).
    s.draw.push_back(pjf
                         ? polyfill({Pos{-2.2, 0.0}, Pos{-1.4, 0.28},
                                     Pos{-1.4, -0.28}})
                         : polyfill({Pos{-1.0, 0.0}, Pos{-1.8, 0.28},
                                     Pos{-1.8, -0.28}}));
    return s;
}

const std::vector<SymbolDef>& table() {
    static const std::vector<SymbolDef> symbols = {
        two_terminal(
            "r", "Resistor", 'R', "R", {},
            {// short flat leads, then the classic zigzag. Every slope
             // is identical (0.375 run : 0.7 rise at the half-strokes,
             // 0.75 : 1.4 in the body), so the teeth read evenly at any
             // zoom and the shape is its own 180-degree rotation.
             poly({Pos{-3.0, 0.0}, Pos{-2.25, 0.0}, Pos{-1.875, 0.7},
                   Pos{-1.125, -0.7}, Pos{-0.375, 0.7}, Pos{0.375, -0.7},
                   Pos{1.125, 0.7}, Pos{1.875, -0.7}, Pos{2.25, 0.0},
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
        // Transformer (stage 24): ONE part, THREE cards - the multi-card
        // shape's only user today. The ref is the coupling's (K): ERC
        // enforces the K prefix, and export derives the two inductor
        // names from it (ref K3 -> cards L3p, L3s and K3), so the deck's
        // coupling can never name an inductor that is not there. The
        // picture: two 4-bump coils facing the laminated core, the
        // polarity dots marking the same-named ends (p+ over s+).
        [] {
            SymbolDef s;
            s.key = "xfmr";
            s.name = "Transformer (2-winding)";
            s.spice_letter = 'K';
            s.ref_prefix = "K";
            s.pins = {PinDef{.name = "p+", .dx = -5.0, .dy = 2.0, .dir = 180},
                      PinDef{.name = "p-", .dx = -5.0, .dy = -2.0, .dir = 180},
                      PinDef{.name = "s+", .dx = 5.0, .dy = 2.0, .dir = 0},
                      PinDef{.name = "s-", .dx = 5.0, .dy = -2.0, .dir = 0}};
            s.needs_value = false;  // the three params carry everything
            s.params = {
                spec(ParamSpec::Type::Number, "lp", "Primary L", "1m"),
                spec(ParamSpec::Type::Number, "ls", "Secondary L", "1m"),
                spec(ParamSpec::Type::Number, "k", "Coupling (k)", "0.98")};
            s.card_emits = {
                CardEmit{'L', "p", {0, 1},
                         {ParamEmit{.key = "lp", .prefix = "", .optional = false}}},
                CardEmit{'L', "s", {2, 3},
                         {ParamEmit{.key = "ls", .prefix = "", .optional = false}}},
                CardEmit{'K', "", {},
                         {ParamEmit{.key = "k", .prefix = "", .optional = false}},
                         true}};
            s.draw = {// the primary coil: four right-facing bumps stacked
                      // from y=-2 to y=2, so the winding's ends sit
                      // exactly on the +/-2 pin rows and the leads land
                      // on them
                      arc(-1.4, 1.5, 0.5, -90.0, 90.0),
                      arc(-1.4, 0.5, 0.5, -90.0, 90.0),
                      arc(-1.4, -0.5, 0.5, -90.0, 90.0),
                      arc(-1.4, -1.5, 0.5, -90.0, 90.0),
                      // the secondary, mirrored to face back
                      arc(1.4, 1.5, 0.5, 90.0, 270.0),
                      arc(1.4, 0.5, 0.5, 90.0, 270.0),
                      arc(1.4, -0.5, 0.5, 90.0, 270.0),
                      arc(1.4, -1.5, 0.5, 90.0, 270.0),
                      // the laminated core between them, a hair taller
                      // than the windings
                      line(-0.25, -2.1, -0.25, 2.1), line(0.25, -2.1, 0.25, 2.1),
                      // the leads
                      line(-5.0, 2.0, -1.4, 2.0), line(-5.0, -2.0, -1.4, -2.0),
                      line(1.4, 2.0, 5.0, 2.0), line(1.4, -2.0, 5.0, -2.0),
                      // the polarity dots beside the top winding ends:
                      // p+ and s+ are the same end
                      dot(-2.2, 1.45, 0.13), dot(2.2, 1.45, 0.13)};
            return s;
        }(),
        // Coupling (stage 24): the K card on the canvas - no pins, no
        // nets; it names two inductors (by ref) and the coupling factor.
        // A part that touches no net still flows through nets/ERC/export
        // untouched: the loops only ever iterate pins.
        [] {
            SymbolDef s;
            s.key = "k";
            s.name = "Coupling (K)";
            s.spice_letter = 'K';
            s.ref_prefix = "K";
            s.needs_value = false;  // all three ride as params, in card order
            s.params = {
                spec(ParamSpec::Type::Text, "l1", "First inductor"),
                spec(ParamSpec::Type::Text, "l2", "Second inductor"),
                spec(ParamSpec::Type::Number, "k", "Coupling (k)", "0.98")};
            s.emit = {ParamEmit{.key = "l1", .prefix = "", .optional = false},
                      ParamEmit{.key = "l2", .prefix = "", .optional = false},
                      ParamEmit{.key = "k", .prefix = "", .optional = false}};
            // The bare two-bar core: the same core the transformer
            // draws, standing alone where the user drops it.
            s.draw = {line(-0.7, -2.0, -0.7, 2.0), line(0.7, -2.0, 0.7, 2.0)};
            return s;
        }(),
        // The lossless transmission line, promoted from the tline.csym
        // example to a built-in (stage 24): same geometry, same
        // Z0=/TD= params, so tline_demo.csim runs without -S and a
        // .csym override keeps working (it replaces this wholesale).
        [] {
            SymbolDef s;
            s.key = "tline";
            s.name = "Lossless transmission line";
            s.spice_letter = 'T';
            s.ref_prefix = "T";
            s.pins = {PinDef{.name = "in+", .dx = -4.0, .dy = 1.0, .dir = 180},
                      PinDef{.name = "in-", .dx = -4.0, .dy = -1.0, .dir = 180},
                      PinDef{.name = "out+", .dx = 4.0, .dy = 1.0, .dir = 0},
                      PinDef{.name = "out-", .dx = 4.0, .dy = -1.0, .dir = 0}};
            s.needs_value = false;
            s.emit = {ParamEmit{.key = "z0", .prefix = "Z0=", .optional = false},
                      ParamEmit{.key = "td", .prefix = "TD=", .optional = false}};
            s.params = {
                spec(ParamSpec::Type::Number, "z0", "Z0 (ohms)", "50"),
                spec(ParamSpec::Type::Number, "td", "TD (seconds)", "1n")};
            s.draw = {line(-4.0, 1.0, -1.0, 1.0), line(-4.0, -1.0, -1.0, -1.0),
                      line(1.0, 1.0, 4.0, 1.0), line(1.0, -1.0, 4.0, -1.0),
                      rect(-1.0, -2.0, 1.0, 2.0), line(-1.0, 2.0, 1.0, -2.0)};
            return s;
        }(),
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
        // The controlled sources (stage 24), ws::spice's M1 set that no
        // built-in ever carried. Card order is the house grammar's:
        // E/G take four nodes OUT+ OUT- IN+ IN- (the E is a voltage
        // SOURCE - a +/- pair inside the diamond; the G is a current
        // source - an arrow instead), drawn as the IEEE dependent-source
        // diamond, the sense pair stacked on the left and the output
        // pair on the right. F/H: two-terminal (out+ out-) plus the NAME of the
        // V source whose branch current controls them - the grammar
        // spells the source before the gain, so both ride as params
        // (the card order forbids the value-first spelling).
        [] {
            SymbolDef s;
            s.key = "e";
            s.name = "VCVS (voltage-controlled voltage source)";
            s.spice_letter = 'E';
            s.ref_prefix = "E";
            s.pins = {PinDef{.name = "out+", .dx = 5.0, .dy = 1.0, .dir = 0},
                      PinDef{.name = "out-", .dx = 5.0, .dy = -1.0, .dir = 0},
                      PinDef{.name = "in+", .dx = -5.0, .dy = 1.0, .dir = 180},
                      PinDef{.name = "in-", .dx = -5.0, .dy = -1.0, .dir = 180}};
            s.draw = {diamond(2.5, 2.5),
                      // the four leads, each ending on the diamond's
                      // edge (x = +-1.5 at y = +-1)
                      line(-5.0, 1.0, -1.5, 1.0), line(-5.0, -1.0, -1.5, -1.0),
                      line(1.5, 1.0, 5.0, 1.0), line(1.5, -1.0, 5.0, -1.0),
                      // the output is a voltage: +/- inside the diamond,
                      // + on the out+ (top) side
                      line(-0.3, 1.3, 0.3, 1.3), line(0.0, 1.0, 0.0, 1.6),
                      line(-0.3, -1.3, 0.3, -1.3),
                      // the sense pair's polarity, beside its leads
                      line(-3.85, 1.55, -3.25, 1.55),
                      line(-3.55, 1.25, -3.55, 1.85),
                      line(-3.85, -1.55, -3.25, -1.55)};
            return s;
        }(),
        [] {
            SymbolDef s;
            s.key = "g";
            s.name = "VCCS (voltage-controlled current source)";
            s.spice_letter = 'G';
            s.ref_prefix = "G";
            s.pins = {PinDef{.name = "out+", .dx = 5.0, .dy = 1.0, .dir = 0},
                      PinDef{.name = "out-", .dx = 5.0, .dy = -1.0, .dir = 0},
                      PinDef{.name = "in+", .dx = -5.0, .dy = 1.0, .dir = 180},
                      PinDef{.name = "in-", .dx = -5.0, .dy = -1.0, .dir = 180}};
            s.draw = {diamond(2.5, 2.5),
                      line(-5.0, 1.0, -1.5, 1.0), line(-5.0, -1.0, -1.5, -1.0),
                      line(1.5, 1.0, 5.0, 1.0), line(1.5, -1.0, 5.0, -1.0),
                      // the sense pair is a voltage: +/-
                      line(-3.85, 1.55, -3.25, 1.55),
                      line(-3.55, 1.25, -3.55, 1.85),
                      line(-3.85, -1.55, -3.25, -1.55),
                      // the output is a current: the i source's arrow
                      // inside the diamond, pointing at out+ (top)
                      line(0.0, -1.3, 0.0, 0.6),
                      polyfill({Pos{0.0, 1.4}, Pos{-0.4, 0.6}, Pos{0.4, 0.6}})};
            return s;
        }(),
        [] {
            SymbolDef s;
            s.key = "f";
            s.name = "CCCS (current-controlled current source)";
            s.spice_letter = 'F';
            s.ref_prefix = "F";
            s.pins = {PinDef{.name = "p", .dx = -3.0, .dy = 0.0, .dir = 180},
                      PinDef{.name = "n", .dx = 3.0, .dy = 0.0, .dir = 0}};
            s.needs_value = false;  // the grammar wants Vname BEFORE gain
            s.params = {spec(ParamSpec::Type::Text, "vs", "Controlling V source"),
                        spec(ParamSpec::Type::Number, "gain", "Gain")};
            s.emit = {ParamEmit{.key = "vs", .prefix = "", .optional = false},
                      ParamEmit{.key = "gain", .prefix = "", .optional = false}};
            // The i source's arrow (a current output) inside the
            // dependent-source diamond.
            s.draw = {line(-3.0, 0.0, -1.6, 0.0), line(1.6, 0.0, 3.0, 0.0),
                      diamond(1.6, 1.6), line(0.0, -0.85, 0.0, 0.3),
                      polyfill({Pos{0.0, 0.95}, Pos{-0.3, 0.3}, Pos{0.3, 0.3}})};
            return s;
        }(),
        [] {
            SymbolDef s;
            s.key = "h";
            s.name = "CCVS (current-controlled voltage source)";
            s.spice_letter = 'H';
            s.ref_prefix = "H";
            s.pins = {PinDef{.name = "p", .dx = -3.0, .dy = 0.0, .dir = 180},
                      PinDef{.name = "n", .dx = 3.0, .dy = 0.0, .dir = 0}};
            s.needs_value = false;  // the grammar wants Vname BEFORE r
            s.params = {spec(ParamSpec::Type::Text, "vs", "Controlling V source"),
                        spec(ParamSpec::Type::Number, "r", "R (ohms)")};
            s.emit = {ParamEmit{.key = "vs", .prefix = "", .optional = false},
                      ParamEmit{.key = "r", .prefix = "", .optional = false}};
            // The v source's +/- marks (a voltage output) inside the
            // dependent-source diamond, + on the p side.
            s.draw = {line(-3.0, 0.0, -1.6, 0.0), line(1.6, 0.0, 3.0, 0.0),
                      diamond(1.6, 1.6), line(-0.95, 0.0, -0.35, 0.0),
                      line(-0.65, -0.3, -0.65, 0.3), line(0.35, 0.0, 0.95, 0.0)};
            return s;
        }(),
        // The behavioral source (stage 24): the B card is V= or I= plus
        // an expression - two optional params of which exactly one is
        // meant (both empty or both filled rejects loudly at parse and
        // maps back onto the part through the export back-map).
        [] {
            SymbolDef s;
            s.key = "b";
            s.name = "Behavioral source (V= or I= expression)";
            s.spice_letter = 'B';
            s.ref_prefix = "B";
            s.pins = {PinDef{.name = "p", .dx = -3.0, .dy = 0.0, .dir = 180},
                      PinDef{.name = "n", .dx = 3.0, .dy = 0.0, .dir = 0}};
            s.needs_value = false;
            s.params = {spec(ParamSpec::Type::Text, "vexpr", "V expression"),
                        spec(ParamSpec::Type::Text, "iexpr", "I expression")};
            s.emit = {ParamEmit{.key = "vexpr", .prefix = "V="},
                      ParamEmit{.key = "iexpr", .prefix = "I="}};
            s.draw = {line(-3.0, 0.0, -1.0, 0.0), line(1.0, 0.0, 3.0, 0.0),
                      circle(0.0, 0.0, 1.0), text(1.15, 1.15, "B")};
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
            s.model_kind = "d";
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
            s.model_kind = "d";
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
        // JFETs: D G S pin order, y-up (drain on top) - the last classic
        // device letter joins the palette.
        jfet("njf", "N-channel JFET", false),
        jfet("pjf", "P-channel JFET", true),
        // The op amp (stage 25): the X subckt instance. One symbol, one
        // `model` param - the subckt NAME it decks as `X<ref> out inp
        // inn vp vm <model>`; the built-in model library's opamp cards
        // (linear macromodels) are what the property grid's picker
        // offers for it, and a hand-written .subckt of one's own rides
        // the directives block exactly like a custom .model does. The
        // supplies are the macromodels' sense pins: connect them (a
        // real op amp has them, ERC wants every pin placed) and the
        // linear card simply does not read them.
        //
        // The picture: the triangle with + and - marking the inputs,
        // and the supply stubs. Every pin lands at a whole-grid point
        // that is a SEGMENT ENDPOINT with its lead pointing into the
        // body (the invariants' rule): the inputs ride their own
        // horizontal leads into the left edge, out sits at the apex,
        // and vp/vm ride their stubs rising from the slanted edges at
        // x=0 (y = +/-20/9 = +/-2.222).
        [] {
            SymbolDef s;
            s.key = "opamp";
            s.name = "Op amp";
            s.spice_letter = 'X';
            s.ref_prefix = "X";
            s.pins = {PinDef{.name = "out", .dx = 5.0, .dy = 0.0, .dir = 0},
                      PinDef{.name = "in+", .dx = -5.0, .dy = 2.0, .dir = 180},
                      PinDef{.name = "in-", .dx = -5.0, .dy = -2.0, .dir = 180},
                      PinDef{.name = "v+", .dx = 0.0, .dy = 5.0, .dir = 90},
                      PinDef{.name = "v-", .dx = 0.0, .dy = -5.0, .dir = 270}};
            s.emit = {ParamEmit{.key = "model", .optional = false}};
            s.needs_value = false;
            s.needs_model = true;
            s.model_kind = "opamp";
            s.params = {spec(ParamSpec::Type::Text, "model", "Model")};
            s.draw = {// the input leads, entering the triangle's left
                      // edge (x=-4) on the +/-2 pin rows
                      line(-5.0, 2.0, -4.0, 2.0), line(-5.0, -2.0, -4.0, -2.0),
                      // the triangle itself: left edge, apex at out
                      poly({Pos{-4.0, 4.0}, Pos{-4.0, -4.0}, Pos{5.0, 0.0},
                            Pos{-4.0, 4.0}}),
                      // the input polarity marks, inside the edge
                      text(-3.55, 2.7, "+"), text(-3.55, -2.95, "-"),
                      // the supply stubs: v+ rises from the top edge
                      // (at x=0 the edge sits at y=+20/9), v- mirrors
                      line(0.0, 2.222, 0.0, 5.0),
                      line(0.0, -2.222, 0.0, -5.0)};
            return s;
        }(),
        // The switches (stage 24). Both need a .model of type SW
        // (Ron/Roff and the threshold pair) - the directives block is
        // where it lives, the params carry only the model's NAME. The
        // S card is `S p n pc nc model`: the switched pair runs
        // horizontally through the contact, the control pair hangs off
        // the bottom like a relay's coil terminals. The W card is
        // `W p n Vname model`: the current through the NAMED V source
        // throws the contact.
        [] {
            SymbolDef s;
            s.key = "s";
            s.name = "Switch (voltage controlled)";
            s.spice_letter = 'S';
            s.ref_prefix = "S";
            s.pins = {PinDef{.name = "p", .dx = -5.0, .dy = 0.0, .dir = 180},
                      PinDef{.name = "n", .dx = 5.0, .dy = 0.0, .dir = 0},
                      PinDef{.name = "c+", .dx = 2.0, .dy = -4.0, .dir = 270},
                      PinDef{.name = "c-", .dx = -2.0, .dy = -4.0, .dir = 270}};
            s.needs_value = false;
            s.needs_model = true;
            s.model_kind = "sw";
            s.params = {spec(ParamSpec::Type::Text, "model", "Model")};
            s.emit = {ParamEmit{.key = "model", .optional = false}};
            // The blade is the animated half (SwitchBlade): the pivot
            // and mate contacts stay static dots, the swinging stroke
            // eases open->closed with the run's answer (render.hpp).
            s.blade = SwitchBlade{.pivot = Pos{-1.2, 0.0},
                                  .open_tip = Pos{0.9, 0.95},
                                  .closed_tip = Pos{1.2, 0.0}};
            s.draw = {// the body the contact sits in
                      rect(-2.5, -2.0, 2.5, 2.0),
                      // the switched path: lead, contact dot, the
                      // blade (SwitchBlade), contact dot, lead
                      line(-5.0, 0.0, -1.2, 0.0), dot(-1.2, 0.0, 0.14),
                      dot(1.2, 0.0, 0.14),
                      line(1.2, 0.0, 5.0, 0.0),
                      // the control pair rises into the box from below
                      line(2.0, -4.0, 2.0, -2.0), line(-2.0, -4.0, -2.0, -2.0),
                      // and carries its own polarity
                      line(2.65, -3.35, 3.25, -3.35),
                      line(2.95, -3.65, 2.95, -3.05),
                      line(-3.25, -3.35, -2.65, -3.35)};
            return s;
        }(),
        [] {
            SymbolDef s;
            s.key = "w";
            s.name = "Switch (current controlled)";
            s.spice_letter = 'W';
            s.ref_prefix = "W";
            s.pins = {PinDef{.name = "p", .dx = -3.0, .dy = 0.0, .dir = 180},
                      PinDef{.name = "n", .dx = 3.0, .dy = 0.0, .dir = 0}};
            s.needs_value = false;
            s.needs_model = true;
            s.model_kind = "sw";
            s.params = {spec(ParamSpec::Type::Text, "vs", "Controlling V source"),
                        spec(ParamSpec::Type::Text, "model", "Model")};
            s.emit = {ParamEmit{.key = "vs", .prefix = "", .optional = false},
                      ParamEmit{.key = "model", .optional = false}};
            // The same blade as the S switch, compact (SwitchBlade).
            s.blade = SwitchBlade{.pivot = Pos{-0.9, 0.0},
                                  .open_tip = Pos{0.6, 0.8},
                                  .closed_tip = Pos{0.9, 0.0}};
            s.draw = {// the same contact as the S switch, compact
                      line(-3.0, 0.0, -0.9, 0.0), dot(-0.9, 0.0, 0.14),
                      dot(0.9, 0.0, 0.14),
                      line(0.9, 0.0, 3.0, 0.0), text(0.55, 1.5, "W")};
            return s;
        }(),
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
