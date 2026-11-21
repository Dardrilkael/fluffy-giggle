# ws::plotmath - the shared plotting math core (the unify-plotting merge):
# the nice-tick ladder, engineering-notation labels, range/view
# invariants (expand/clamp/zoom/pan), the nearest-sample hover lookup,
# min/max pixel-bucket decimation, and the monotone-run family split -
# the one implementation both the generic gui plot widget (ws::ui) and
# the Circsim Results panel (ws::circsim's plot.hpp facade) call. Pure
# std + core-tier headers only: no font, no canvas, no ws::spice, no
# ui - so both tiers may depend on it (ui must not reach circsim, and
# circsim must not reach ui; this tier sits below both).
ws_add_library(plotmath DEPENDS ws::core)

# The core's own headless gate: every decision the two plotting
# surfaces render, plus the parity contracts (the buffer and string
# format spellings byte-identical, the tick cap visible, the round
# trips through the transforms).
ws_add_test(plotmath
  SOURCES tests/test_plotmath.cpp
  DEPENDS ws::plotmath)
