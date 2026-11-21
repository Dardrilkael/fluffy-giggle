#pragma once

// apps/circsim_editor stage 4 - the Results panel: the run's tables
// drawn as plots (one analysis at a time, the sweep axis horizontal,
// every signal a toggleable colored series with a hover crosshair and
// live legend readout), the .op and measurement sections as value
// rows, and the export buttons. The axis math is ws::circsim::plot
// (headless, tested); this file is only the chrome. Stage 5 gave the
// plot its hands (wheel/band/axis drags/pan/reset); stage 6 gives a
// mixed V/I legend its second spine - the currents read on a right
// axis with their own fit, ticks and manual window. Stage 7 is the
// polish pass: hover tooltip, legend sparklines, bulk visibility, a
// fit-view button, aligned value rows with units. Stage E adds:
// persistent markers (E1), locate buttons for generated names (E2),
// and scoped exports + plot PNG (E3).

#include "editor.hpp"

namespace editor {

void draw_results_panel(Editor& ed, gui::GuiContext& gui,
                        gui::LayoutCursor& layout,
                        const gui::ChromeTheme& theme);

// E3's plot image: the SHOWN table's visible series rendered headless
// through RasterCanvas - the frame, the engineering-notation grid and
// tick labels, one polyline per visible signal (full resolution, no
// decimation - a PNG is written once, not drawn at 60 fps), the
// markers, and a legend row per series. Pure over its inputs so the
// headless suite can gate the bytes' shape (the RasterCanvas pixel
// seam is the test's). `visible` parallels t.signals (nullptr = all
// visible), `x_name` labels the axis.
//
// The PNG/live consistency contract: this is draw_plot's automatic
// HOME view, one-shot - the same spine split (signal_y_axis: volts
// left, amps right, independent ranges, hidden columns excluded), the
// same x envelope (split_axis_runs' union over every pass), the same
// expand_range padding, and the same one-polyline-per-run series
// shape (NaN break vertices between runs, descending runs kept
// descending). The EXPORT CONTRACT is deliberately the full-data HOME
// view, never the interactive zoom/pan window: the call site passes
// no PlotView, and that is the documented behavior (a PNG outlives
// the view that produced it - the fit is what "the plot" means). The
// interactive decimation and geometry caches are live-only concerns
// and are not consulted here.
[[nodiscard]] std::string render_plot_png(const ws::spice::Table& t,
                                          const std::uint8_t* visible,
                                          const char* x_name,
                                          const int* markers,
                                          std::size_t marker_count);

// E2: a legend row's signal name resolved onto the schematic thing it
// names, through the run's OWN back-map - the same maps the Problems
// panel locates through (the run's export, not a rebuilt one, so the
// answer matches the names the table carries even when the schematic
// has since moved). A v(net) whose net the map knows answers the Net
// target (a generated v(N001) included - that is the point); an
// i(part) answers the Part target; anything else (re()/im() pairs,
// onoise, a name the deck never spelled) answers Kind::None and the
// row draws no locate button at all - never a dead affordance.
[[nodiscard]] ws::circsim::DiagTarget locate_signal(
    const Editor& ed, const std::string& name);

}  // namespace editor