#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <ws/circsim/model.hpp>
#include <ws/circsim/symbols.hpp>

// The symbol render seam (apps/circsim stage 17a): one pure function
// flattening a placed symbol's picture into world-space strokes - the
// part's mirror-then-rotation-then-position applied exactly like
// pin_pos applies it to pin offsets, so the picture and the pins can
// never disagree. The editor's canvas paints these strokes, the CLI's
// SVG dump writes them, and the tests assert them: one geometry
// implementation, three consumers, no parallel transforms to drift
// (the stage-16c lesson, applied to the symbol path).
//
// Headless by construction: no ui, no font, no canvas - Pos doubles
// and strings only, like locate/plot beside it.

namespace ws::circsim {

// One flattened stroke of a placed symbol's picture, in WORLD space.
struct SymbolStroke {
    enum class Kind : std::uint8_t {
        Line,     // pts[0] -> pts[1]
        Poly,     // polyline through pts; `closed` closes it back to pts[0]
        PolyFill, // filled polygon through pts (the fill closes it)
        Circle,   // pts[0] center, r radius (outline)
        Arc,      // pts[0] center, r radius, a0..a1 degrees CCW (y up);
                  // the placement's mirror/rotation is already folded in
        Dot,      // filled circle at pts[0], r radius
        Text,     // `text` anchored at pts[0]
    };
    Kind kind = Kind::Line;
    std::vector<Pos> pts;
    double r = 0.0;         // Circle / Arc / Dot
    double a0 = 0.0;        // Arc start angle (degrees, world space)
    double a1 = 0.0;        // Arc end angle (degrees, world space)
    bool closed = false;    // Poly only
    std::string text;       // Text only
};

// Flattens `sym`'s drawing primitives under `placement` (only pos, rot
// and mirror are read), then appends the pin-lead safety net: a pin no
// primitive vertex touches gets a stub line pointing into the body,
// opposite the pin's own direction - the .csym safety net for symbols
// whose prims fall short of their pins. Malformed prims (a line without
// two points, a circle without a center) are skipped, not repaired.
//
// `switch_closed` interpolates the SwitchBlade (when the symbol carries
// one) from open (0.0) to closed (1.0): the endpoints render verbatim,
// between them the tip sweeps the angle (shortest rotational path) and
// radius about the pivot, then the placement's transform folds in
// exactly like every static vertex. Every static consumer (the SVG
// dump, the PNG export, the palette) passes nothing - the blade at
// rest. A symbol without a blade ignores the value.
[[nodiscard]] std::vector<SymbolStroke> render_symbol(
    const SymbolDef& sym, const Part& placement,
    double switch_closed = 0.0);

}  // namespace ws::circsim
