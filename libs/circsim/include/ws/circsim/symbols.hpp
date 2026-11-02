#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ws/circsim/model.hpp>

// The symbol table (apps/circsim stage 1): what a Part's `symbol` key
// means - its pins (names, grid-unit offsets, pointing directions), the
// SPICE element letter, the reference-designator prefix, and how the part's
// value/params reach the exported card. Data, not behavior: stage 2's
// JSON symbol library feeds the same shapes (plus drawing primitives),
// and net building/ERC/export consume the table through find_symbol only.
//
// Pin list order IS the SPICE pin order (R: a,b; Q: c,b,e; M: d,g,s,b).

namespace ws::circsim {

struct PinDef {
    std::string name;   // "a"/"b", "c"/"b"/"e", "p"/"n"
    double dx = 0.0;    // offset from the symbol origin, grid units
    double dy = 0.0;
    int dir = 180;      // where the pin points (degrees, 0 = +x): the
                        // editor's wire-start direction; net building
                        // does not read it
};

// How one token after the pin list reaches the exported card. The part's
// primary `value` is emitted first (when the symbol wants one), then these
// in order.
struct ParamEmit {
    enum class Source { Value, Param };
    Source source = Source::Param;
    std::string key;      // params key: "model", "ac", "wave", "l", "w", "ic"
    std::string prefix;   // "AC ", "L=", "IC=", "" for a bare token
    bool optional = true; // skip when the value is empty
};

struct SymbolDef {
    std::string key;    // "r", "npn", "gnd" (lowercase; matched ci)
    std::string name;   // display name: "Resistor"
    char spice_letter = 0;  // 'R'; 0 = never exported (power/ground)
    std::string ref_prefix;  // "R"; empty = no ref expected (power)
    std::vector<PinDef> pins;  // SPICE order; index = Part pin indices
    std::optional<std::string> fixed_net;  // power symbols: the pin's net
    std::vector<ParamEmit> emit;
    bool needs_value = true;   // ERC: value must be non-empty
    bool needs_model = false;  // ERC: params["model"] must be non-empty
};

// The stage-1 built-in set: r, c, l, v, i, d, npn, pnp, nmos, pmos, gnd,
// vcc. Offsets are whole grid units so rotated pins stay on grid points.
[[nodiscard]] const std::vector<SymbolDef>& builtin_symbols();

// Case-insensitive lookup by key; null when unknown.
[[nodiscard]] const SymbolDef* find_symbol(std::string_view key);

// A placed part's pin position: the pin offset mirrored (x-flip) then
// rotated about the part origin, plus the part position. Rotations are
// CCW with y up; `rot` values outside {0,90,180,270} fold to the nearest
// quarter turn (the loader does not reject them).
[[nodiscard]] Pos pin_pos(const SymbolDef& s, const Part& p,
                          std::uint32_t pin_index);

}  // namespace ws::circsim
