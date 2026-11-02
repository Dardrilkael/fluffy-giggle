#pragma once

// apps/circsim_editor stage 4 - the Results panel: the run's tables
// drawn as plots (one analysis at a time, the sweep axis horizontal,
// every signal a toggleable colored series with a hover crosshair and
// live legend readout), the .op and measurement sections as value
// rows, and the export buttons. The axis math is ws::circsim::plot
// (headless, tested); this file is only the chrome.

#include "editor.hpp"

namespace editor {

void draw_results_panel(Editor& ed, gui::GuiContext& gui,
                        gui::LayoutCursor& layout,
                        const gui::ChromeTheme& theme);

}  // namespace editor
