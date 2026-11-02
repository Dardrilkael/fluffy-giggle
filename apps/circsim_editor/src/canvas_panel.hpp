#pragma once

// apps/circsim_editor - the Schematic panel: the canvas the model
// draws on and edits through (editor.hpp's interaction model). One
// draw call per frame: set the camera, handle the pointer, render the
// world through ws::ui::canvas's Renderer2DCanvas.

#include "editor.hpp"

namespace editor {

// The panel body draw (the shell wraps it in its scroll scope; the
// body rect IS the viewport).
void draw_canvas_panel(Editor& ed, gui::GuiContext& gui,
                       gui::LayoutCursor& layout,
                       const gui::ChromeTheme& theme);

}  // namespace editor
