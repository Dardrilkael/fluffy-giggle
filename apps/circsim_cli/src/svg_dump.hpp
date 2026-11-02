#pragma once

#include <string>

#include <ws/circsim/library.hpp>

// The stage-17b headless render: the symbol table as SVG, drawn
// through ws::circsim's render seam - the same strokes the editor
// paints, no ui, no font system (SVG text is the viewer's job). The
// before/after comparison harness for the stage-17 symbol redesign:
// render a revision, eyeball the sheet, diff by eye.
namespace svg {

// Writes one SVG per symbol (<dir>/<key>.svg) plus a contact sheet
// (<dir>/symbols.svg), every symbol at rot 0, pins marked with a dot
// and their names. Overwrites existing files. Returns 0, or 2 on IO
// failure (the message lands on stderr).
int write_symbol_svgs(const ws::circsim::SymbolLibrary& lib,
                      const std::string& dir);

}  // namespace svg
