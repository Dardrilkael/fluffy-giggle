#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ws/circsim/model.hpp>
#include <ws/settings/json.hpp>

// The symbol table (apps/circsim stage 1 + 2): what a Part's `symbol` key
// means - its pins (names, grid-unit offsets, pointing directions), the
// SPICE element letter, the reference-designator prefix, how the part's
// value/params reach the exported card (stage 1), the typed parameter
// specs the property grid and ERC read, and the drawing primitives the
// editor renders (stage 2). Data, not behavior: a .csym library file
// feeds the same shapes (library.hpp); net building/ERC/export consume
// them through a lookup only.
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

// Multi-card symbols: one placed part emits SEVERAL deck cards - the
// 2-winding transformer is L + L + K, the only shape today. Each entry
// is one card: the device letter (its name = letter + the part ref's
// body + `suffix`, export.cpp's sub_element_name), the pin indices it
// connects in ITS SPICE order (a subset of the symbol's pins; the
// coupling card has none), and its own param tail. `name_siblings`
// spells the OTHER cards' names as leading tokens - the coupling names
// what it couples, so the K card never goes stale when the ref changes.
// A .csym override replaces the whole symbol, this list included; the
// .csym spelling ("cards") round-trips through library.hpp.
struct CardEmit {
    char letter = 0;                   // 'L', 'K'
    std::string suffix;                // "p", "s", ""
    std::vector<std::uint32_t> pins;   // symbol pin indices, in order
    std::vector<ParamEmit> emit;       // this card's tail
    bool name_siblings = false;        // spell the sibling names first
};

// What one params key MEANS (stage 2): the type the property grid edits
// and ERC validates, the label the grid shows, the default a NEW part
// starts from (never injected at export - the schematic file is the
// truth), and the choice list for Choice params.
struct ParamSpec {
    enum class Type : std::uint8_t { Text, Number, Choice };
    Type type = Type::Text;
    std::string key;     // "model", "l", "w", "z0"...
    std::string label;   // display label; empty = the key
    std::string default_value;  // prefill for new parts ("" = none)
    std::vector<std::string> choices;  // Choice: the allowed values (ci)
};

// One drawing primitive, grid units relative to the symbol origin,
// transformed by the part's mirror-then-rotation exactly like pins
// (transform_pos). Headless consumers: symbol_bounds (the AABB) and the
// CLI's table dump; the editor is the renderer.
struct DrawPrim {
    enum class Kind : std::uint8_t {
        Line,     // pts[0] -> pts[1]
        Poly,     // open polyline through pts
        PolyFill, // filled polygon through pts (arrowheads); the fill
                  // closes the loop, no repeated vertex needed
        Rect,     // axis-aligned, pts[0]/pts[1] opposite corners
        Circle,   // pts[0] center, r radius
        Arc,      // pts[0] center, r radius, a0..a1 degrees CCW (y up)
        Dot,      // filled circle (junction dots, tap points)
        Text,     // text anchored at pts[0], editor-styled
    };
    Kind kind = Kind::Line;
    std::vector<Pos> pts;
    double r = 0.0;         // Circle / Arc / Dot
    double a0 = 0.0;        // Arc start angle
    double a1 = 360.0;      // Arc end angle
    std::string text;       // Text
};

struct SymbolDef {
    std::string key;    // "r", "npn", "gnd" (lowercase; matched ci)
    std::string name;   // display name: "Resistor"
    char spice_letter = 0;  // 'R'; 0 = never exported (power/ground)
    std::string ref_prefix;  // "R"; empty = no ref expected (power)
    std::vector<PinDef> pins;  // SPICE order; index = Part pin indices
    std::optional<std::string> fixed_net;  // power symbols: the pin's net
    std::vector<ParamEmit> emit;
    std::vector<CardEmit> card_emits;  // non-empty = multi-card (see above)
    bool needs_value = true;   // ERC: value must be non-empty
    bool needs_model = false;  // ERC: params["model"] must be non-empty
    std::vector<ParamSpec> params;  // typed specs (stage 2)
    std::vector<DrawPrim> draw;     // the symbol's picture (stage 2)
    ws::settings::json::Object unknown;  // unrecognized .csym fields ride
};

// The built-in set: r, c, l, xfmr, k, tline, v, i, e, f, g, h, b, d,
// led, npn, pnp, nmos, pmos, s, w, gnd, vcc - the classic twelve plus
// the orphan devices ws::spice always parsed (stage 24). Offsets are
// whole grid units so rotated pins stay on grid points.
[[nodiscard]] const std::vector<SymbolDef>& builtin_symbols();

// Case-insensitive lookup by key; null when unknown. BUILT-INS ONLY -
// the effective table (built-ins + .csym layers) lives in library.hpp.
[[nodiscard]] const SymbolDef* find_symbol(std::string_view key);

// A placed part's pin position: the pin offset mirrored (x-flip) then
// rotated about the part origin, plus the part position. Rotations are
// CCW with y up; `rot` values outside {0,90,180,270} fold to the nearest
// quarter turn (the loader does not reject them).
[[nodiscard]] Pos pin_pos(const SymbolDef& s, const Part& p,
                          std::uint32_t pin_index);

// The shared part transform: mirror (x-flip) first, then rotation (CCW,
// y up), then the part position. pin_pos applies it to pin offsets; the
// editor applies the same to DrawPrim points so the picture and the pins
// can never disagree.
[[nodiscard]] Pos transform_pos(const Part& p, Pos v);

// The axis-aligned bounding box of a symbol's picture AND pins, grid
// units, origin-relative: {min_x, min_y, max_x, max_y}. Arcs bound by
// their full circle (conservative); Text bounds by its anchor (no font
// metrics headlessly). Empty symbol -> all zeros.
struct Bounds {
    double min_x = 0.0, min_y = 0.0, max_x = 0.0, max_y = 0.0;

    [[nodiscard]] bool operator==(const Bounds&) const = default;
};
[[nodiscard]] Bounds symbol_bounds(const SymbolDef& s);

// The params a NEW part of this symbol starts from: every spec's
// non-empty default, in spec order. Never consulted at export or ERC
// time - what the schematic stores is what exports.
[[nodiscard]] std::map<std::string, std::string> default_params(
    const SymbolDef& s);

}  // namespace ws::circsim
