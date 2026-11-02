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

// Two-terminal passives share one pin layout (and one emit tail).
SymbolDef two_terminal(std::string key, std::string name, char letter,
                       std::string prefix, std::vector<ParamEmit> emit,
                       bool needs_value = true) {
    SymbolDef s;
    s.key = std::move(key);
    s.name = std::move(name);
    s.spice_letter = letter;
    s.ref_prefix = std::move(prefix);
    s.pins = {PinDef{.name = "a", .dx = -3.0, .dy = 0.0, .dir = 180},
              PinDef{.name = "b", .dx = 3.0, .dy = 0.0, .dir = 0}};
    s.emit = std::move(emit);
    s.needs_value = needs_value;
    return s;
}

const std::vector<SymbolDef>& table() {
    static const std::vector<SymbolDef> symbols = {
        two_terminal("r", "Resistor", 'R', "R", {}),
        two_terminal("c", "Capacitor", 'C', "C",
                     {ParamEmit{.key = "ic", .prefix = "IC="}}),
        two_terminal("l", "Inductor", 'L', "L",
                     {ParamEmit{.key = "ic", .prefix = "IC="}}),
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
            s.emit = {ParamEmit{.key = "model"},
                      ParamEmit{.key = "area"}};
            s.needs_value = false;
            s.needs_model = true;
            return s;
        }(),
        // BJTs: C B E pin order, y-up (collector on top).
        [] {
            SymbolDef s;
            s.key = "npn";
            s.name = "NPN transistor";
            s.spice_letter = 'Q';
            s.ref_prefix = "Q";
            s.pins = {PinDef{.name = "c", .dx = 2.0, .dy = 3.0, .dir = 90},
                      PinDef{.name = "b", .dx = -3.0, .dy = 0.0, .dir = 180},
                      PinDef{.name = "e", .dx = 2.0, .dy = -3.0, .dir = 270}};
            s.emit = {ParamEmit{.key = "model"},
                      ParamEmit{.key = "area"}};
            s.needs_value = false;
            s.needs_model = true;
            return s;
        }(),
        [] {
            SymbolDef s;
            s.key = "pnp";
            s.name = "PNP transistor";
            s.spice_letter = 'Q';
            s.ref_prefix = "Q";
            s.pins = {PinDef{.name = "c", .dx = 2.0, .dy = 3.0, .dir = 90},
                      PinDef{.name = "b", .dx = -3.0, .dy = 0.0, .dir = 180},
                      PinDef{.name = "e", .dx = 2.0, .dy = -3.0, .dir = 270}};
            s.emit = {ParamEmit{.key = "model"},
                      ParamEmit{.key = "area"}};
            s.needs_value = false;
            s.needs_model = true;
            return s;
        }(),
        // MOSFETs: D G S B pin order, y-up (drain on top).
        [] {
            SymbolDef s;
            s.key = "nmos";
            s.name = "N-channel MOSFET";
            s.spice_letter = 'M';
            s.ref_prefix = "M";
            s.pins = {PinDef{.name = "d", .dx = 2.0, .dy = 3.0, .dir = 90},
                      PinDef{.name = "g", .dx = -3.0, .dy = 0.0, .dir = 180},
                      PinDef{.name = "s", .dx = 2.0, .dy = -3.0, .dir = 270},
                      PinDef{.name = "b", .dx = -3.0, .dy = -3.0, .dir = 270}};
            s.emit = {ParamEmit{.key = "model"},
                      ParamEmit{.key = "l", .prefix = "L="},
                      ParamEmit{.key = "w", .prefix = "W="}};
            s.needs_value = false;
            s.needs_model = true;
            return s;
        }(),
        [] {
            SymbolDef s;
            s.key = "pmos";
            s.name = "P-channel MOSFET";
            s.spice_letter = 'M';
            s.ref_prefix = "M";
            s.pins = {PinDef{.name = "d", .dx = 2.0, .dy = 3.0, .dir = 90},
                      PinDef{.name = "g", .dx = -3.0, .dy = 0.0, .dir = 180},
                      PinDef{.name = "s", .dx = 2.0, .dy = -3.0, .dir = 270},
                      PinDef{.name = "b", .dx = -3.0, .dy = -3.0, .dir = 270}};
            s.emit = {ParamEmit{.key = "model"},
                      ParamEmit{.key = "l", .prefix = "L="},
                      ParamEmit{.key = "w", .prefix = "W="}};
            s.needs_value = false;
            s.needs_model = true;
            return s;
        }(),
        // Ground / power: single fixed-net pins, never exported.
        [] {
            SymbolDef s;
            s.key = "gnd";
            s.name = "Ground";
            s.pins = {PinDef{.name = "0", .dx = 0.0, .dy = 0.0, .dir = 270}};
            s.fixed_net = "0";
            s.needs_value = false;
            return s;
        }(),
        [] {
            SymbolDef s;
            s.key = "vcc";
            s.name = "Supply (VCC)";
            s.pins = {PinDef{.name = "p", .dx = 0.0, .dy = 0.0, .dir = 90}};
            s.fixed_net = "VCC";
            s.needs_value = false;
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

Pos pin_pos(const SymbolDef& s, const Part& p, std::uint32_t pin_index) {
    if (pin_index >= s.pins.size()) {
        return p.pos;  // callers validate the index before asking
    }
    double dx = s.pins[pin_index].dx;
    double dy = s.pins[pin_index].dy;
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

}  // namespace ws::circsim
