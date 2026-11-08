#pragma once

// apps/circsim_editor - the Schematic panel: the canvas the model
// draws on and edits through (editor.hpp's interaction model). One
// draw call per frame: set the camera, handle the pointer, render the
// world through the Editor's schematic canvas seam (the live
// Renderer2DCanvas; a headless test injects a CommandListCanvas).
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

// --- the screen-space overlays' layout -------------------------------------
// The draw and the pointer handler share these (one geometry, one
// truth - a click can never land where a pixel was not), and the
// headless suite asserts against them the same way.

// The wire-color mode toggle: three square buttons at the body's
// top-right, index-matched to WireColorMode's declaration order
// (Plain, Nets, Voltage).
struct ModeToggleLayout {
    gui::Rect buttons[3];
};
[[nodiscard]] ModeToggleLayout mode_toggle_layout(const gui::Rect& body);

// The .tran transport: the pill at the bottom center - the play/pause
// button at its left, the scrub track filling the middle, the time
// readout riding the right end.
struct TransportStripLayout {
    gui::Rect strip;   // the whole pill (the press target)
    gui::Rect button;  // play/pause
    gui::Rect bar;     // the scrub track
};
[[nodiscard]] TransportStripLayout transport_strip_layout(
    const gui::Rect& body);

}  // namespace editor
