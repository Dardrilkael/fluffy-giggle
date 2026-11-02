#pragma once

// apps/circsim_editor - the Schematic panel: the canvas the model
// draws on and edits through (editor.hpp's interaction model). One
// draw call per frame: set the camera, handle the pointer, render the
// world through ws::ui::canvas's Renderer2DCanvas.
//
// The floor and the grid (stage 16) are ws::ui::canvas's grid system:
// one GridSpec (Editor::grid_spec - Schematic::grid as the base) feeds
// both the pixel-snapped dot lattice the panel draws and the snap the
// pointer lands on, the lattice brightens ~150ms while a tool is
// engaged, and the snap feedback (the wire tool's ring, the accent dot
// near a landing intersection) draws over it. Shift = the coarse
// (major-lattice) snap for placements, part drags and labels; wires,
// pins and junctions always stay on the fine grid.

#include "editor.hpp"

namespace editor {

// The panel body draw (the shell wraps it in its scroll scope; the
// body rect IS the viewport).
void draw_canvas_panel(Editor& ed, gui::GuiContext& gui,
                       gui::LayoutCursor& layout,
                       const gui::ChromeTheme& theme);

}  // namespace editor
