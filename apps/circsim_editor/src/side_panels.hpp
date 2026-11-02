#pragma once

// apps/circsim_editor - the side/bottom panels: the symbol Palette
// (placement arming + the wire/label tools), the Properties grid (the
// selection's fields over ParamSpecs; the schematic's analysis setup
// and directives when nothing is selected), the Problems list (ERC /
// run / load diags, click to select-and-zoom through locate.hpp) and
// the Netlist view (the exported deck).

#include "editor.hpp"

namespace editor {

void draw_palette_panel(Editor& ed, gui::GuiContext& gui,
                        gui::LayoutCursor& layout,
                        const gui::ChromeTheme& theme);
void draw_properties_panel(Editor& ed, gui::GuiContext& gui,
                           gui::LayoutCursor& layout,
                           const gui::ChromeTheme& theme);
void draw_problems_panel(Editor& ed, gui::GuiContext& gui,
                         gui::LayoutCursor& layout,
                         const gui::ChromeTheme& theme);
void draw_netlist_panel(Editor& ed, gui::GuiContext& gui,
                        gui::LayoutCursor& layout,
                        const gui::ChromeTheme& theme);

}  // namespace editor
