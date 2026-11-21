# ws::plotmath

## What this lib is

The one authoritative plotting math core (the unify-plotting merge):
every plotting decision the generic gui plot widget (`libs/ui`'s
`widgets_plot.cpp`) and the Circsim Results panel
(`apps/circsim_editor`'s `results_panel.cpp`) share - the nice-tick
ladder, the engineering-notation label, the range padding, the
view clamp/zoom/pan invariants, the nearest-sample hover lookup, the
min/max pixel-bucket decimation, the monotone-run family split, and
the navigation constants (wheel factor, strip exponential,
double-click window, margins) the two surfaces' gesture FEEL rides.

It sits BELOW both consumers on purpose: `ui` links it, `circsim`
links it, and neither reaches into the other. Pure std + core-tier
headers only - no font, no canvas, no Vulkan, no ws::spice, no
GuiContext, no editor state. Everything answers doubles, plain index
lists, and strings/char buffers.

## The two facades

Both consumers keep their own public spellings; both are thin forwards
here:

- `ws::ui::gui` (widgets.hpp): `plot_ticks` (fills the public
  fixed-32-slot `PlotTicks` from the core's vector), `plot_format_eng`
  (the caller-scratch buffer spelling), `plot_expand_range` /
  `plot_clamp_view` / `plot_zoom_range` / `plot_pan_range`
  (field-converting forwards over `PlotAxisRange`).
- `ws::circsim` (plot.hpp): `nice_ticks`, `format_eng`,
  `expand_range`, `clamp_view`, `zoom_range`, `pan_range`,
  `decimate_minmax`, `decimate_minmax_index`, `split_axis_runs` -
  same names as always, field-converting forwards over `PlotRange` /
  `AxisRun` / `AxisRuns`. `signal_y_axis` stays circsim's own (SPICE
  save-name knowledge, not plotting math).

The parity suite (`apps/circsim_editor/tests/test_plot_parity.cpp`)
feeds identical data and viewport configurations through BOTH facades
and asserts the answers byte-identical - the merge's own regression
gate: no future edit can split the spellings into two decision sets
again without it failing.

## The contracts (the merge's decisions)

- `nice_ticks`: the 1/2/5 x 10^k ladder, endpoints never forced; the
  guard set is the UNION of the two historical spellings (circsim's
  underflow/2^53 deep-zoom refusals + the widget's 32-tick cap - a
  huge target's answer stops at 32, visible not fatal).
- `format_eng` / `format_eng_buf`: one walk, two return spellings
  (string vs caller-scratch buffer) - byte-identical answers.
- `expand_range` / `clamp_view` / `zoom_range` / `pan_range`: the
  home/view invariant pair (the view never leaves home, stays a
  healthy interval) and the cursor-anchored zoom contract (the anchor
  coordinate stays under the cursor).
- `decimate_minmax` / `decimate_minmax_index`: min/max per bucket in
  occurrence order, forced endpoints, ORIGINAL sample indices (tooltips
  and markers reference the source rows), empty = draw raw.
- `split_axis_runs`: maximal monotone runs covering [0, n) exactly -
  the never-chord-independent-runs contract (.step families and
  falling .dc passes stay disconnected).
- The k* constants: the shared gesture feel - both surfaces' wheel
  notches zoom 1.15, their strip drags compound exp(0.005), their
  double-clicks are 0.35 s / 5 px.

## Files

- `include/ws/plotmath/plotmath.hpp` - the contracts (read this first).
- `src/plotmath.cpp` - the implementation (the merged bodies; the
  provenance comment in the header says whose spelling won where and
  why - the superset guard set).
- `tests/test_plotmath.cpp` - the core's own gate: axis edges (NaN,
  inf, degenerate, tiny/huge, the cap), the format walk and its two
  spellings, the view invariants, transform round trips, the hover
  lookup's two paths, decimation and the run family, the constants.

Provenance note: this TU is the merge of `libs/circsim/src/plot.cpp`
and the file-local copies in `libs/ui/src/gui/widgets_plot.cpp`, which
had themselves started as one (widgets_plot's own comment said "the
circsim spelling, kept byte-for-byte") and then drifted apart in guard
sets. This library is the end of that drift.
