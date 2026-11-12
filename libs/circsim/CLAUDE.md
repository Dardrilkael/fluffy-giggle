# libs/circsim - CLAUDE.md

`ws::circsim` - the schematic side of the circsim app (model, `.csim`
format, net building, ERC, ws::spice export, the `.csym` symbol library,
the stage-3 locate seam, the raster PNG export) plus its headless CLI
(`apps/circsim_cli`) and its editor (`apps/circsim_editor`, the one
ui-bearing consumer - the lib itself stays headless). Headless by
construction: no ui include anywhere; `ws::spice` is the ONLY simulator
this lib ever talks to.

Deps: `ws::core`, `ws::math` (DVec2 positions), `ws::spice`, and
`ws::settings` (json::Object - the unknown-field carrier - plus the
FileSystem atomic-write seam). The settings link is the one deviation
from the task's "core, math, spice" list; the alternative was nlohmann
fetched privately, which would duplicate a first-party capability.

## Model (model.hpp)
- Ids are `uint32`, allocated from one `Schematic` counter and NEVER
  reused within a file: `next_id` persists, and load raises it above every
  id seen (dropped duplicates included). 0 is the "none" sentinel.
- Positions are grid units, y up, doubles with integer values (exact
  coincidence arithmetic). `rot` is CCW degrees 0/90/180/270; `mirror` is
  the x-flip, applied BEFORE rotation (symbols.hpp `pin_pos`).
- Wires end on REFERENCES (`Pin{part,pin}` / `Junction{id}`), never on
  positions. Waypoints are cosmetic. Junctions are explicit dots.
- `Part.params` is stringly on purpose: the symbol table owns the types
  (ParamSpec), the property grid reads them there, and ERC validates
  against them; export stays stringly end to end.
- `directives` is a verbatim text block (.model/.subckt/.include) that
  exports ahead of the device cards, unedited.
- Every mutation bumps `Schematic::version()` (cache staleness).
- Unknown `.csim` fields ride in each object's `unknown`
  (`ws::settings::json::Object`, insertion order) and re-emerge on save
  after the known fields.

## Symbols (symbols.hpp) and the library (library.hpp)
- `SymbolDef`: key (ci), name, SPICE letter (0 = never exported:
  power/ground), ref prefix, pins (array order IS SPICE pin order),
  fixed_net, the emit tail, needs_value/needs_model, the typed
  ParamSpecs, the DrawPrim picture, and its own `unknown`.
- `ParamSpec` (stage 2) is what a params key MEANS: Text / Number
  (validated with ws::spice's own number grammar) / Choice (ci list),
  plus the label and the default. `default_params(sym)` is the NEW-PART
  prefill only - never injected at export; what the file stores is what
  exports. A non-optional ParamEmit entry marks a REQUIRED param
  (ERC: missing_param).
- `DrawPrim` (stage 2): line/poly/rect/circle/arc/dot/text, grid units,
  origin-relative, transformed by the part's mirror-then-rotation
  exactly like pins (`transform_pos` - the one shared transform, so the
  picture and the pins can never disagree). `symbol_bounds` is the
  conservative AABB (arcs bound by full circles, text by its anchor).
- The built-in table (symbols.cpp, 23 keys) carries specs + draw for
  every symbol; `find_symbol` is the BUILT-IN-ONLY lookup. The `led`
  key is the diode plus a display-only `color` Choice (spec'd, never
  emitted - the deck cannot tell an LED from a diode) and two emission
  arrowheads; the editor's glow reads the color and ws::spice's i(Dn)
  probe column for the intensity.
- Stage 24 added the orphan devices the engine always parsed but no
  symbol carried: `e`/`f`/`g`/`h` (the controlled sources - E/G four
  pins in OUT+ OUT- IN+ IN- card order, F/H two pins plus the
  controlling V source's NAME as params, the grammar spelling the
  source before the gain), `b` (behavioral: exactly one of
  `vexpr`/`iexpr`, the deck rejects the bad combos loudly), `s`/`w`
  (switches, needs_model), `tline` (promoted from tline.csym verbatim)
  and `k` (the coupling: DELIBERATELY PINLESS - it names inductors by
  ref; every net/ERC/export loop only iterates pins, so a pinless part
  flows through untouched).
- `CardEmit` (stage 24, symbols.hpp) is the multi-card layout, the
  transformer its only user: one part decks `L<ref>p` + `L<ref>s` + the
  `K` that names its siblings (`name_siblings` - the coupling can never
  name an inductor that is not there). Export's `sub_element_name`
  derives every card name from the part's ref under `element_name`'s
  own rules; all names share the used-name dedup and the element
  back-map. The `.csym` spelling is `"cards"` (parse + save both in
  library.cpp); a .csym override replaces a multi-card symbol
  wholesale. ERC reads a CardEmit's non-optional emits as required
  params.
- Two engine contracts the examples teach: an ideal V directly across
  an L rejects as a V-L loop (carry a series Rs on transformer
  primaries), and the W reads a DEDICATED 0 V sense source's branch
  current (a delivering source's own i() is negative).
- `SymbolLibrary` (library.hpp) is the effective table: built-ins at
  construction, `.csym` files layered on by `load_text` - same key (ci)
  REPLACES in place, later files win, every redefinition warns
  (`circsim.symbols.override`, naming what it replaced). Null `symbols`
  parameters on build_nets/run_erc/export_deck/run_schematic mean
  built-ins alone; `builtin_library()` is the process default.
- The `.csym` format: `{"format":"circsim-symbols","version":1,...}`,
  tolerant load in the .csim sense (wrong-typed fields repair, malformed
  symbols skip - all Diag warnings; only not-JSON and wrong-format
  fail), a migration chain with the v0->v1 step, unknown fields
  preserved at ROOT and SYMBOL level (silently - the extension
  surface), unknown fields inside pins/emit/params/draw dropped WITH a
  warning (tight objects; a typo there is news, not noise). `save()`
  round-trips the user side (by SOURCE, not position - a built-in
  override keeps the built-in's slot but is library data);
  `save(true)` dumps the full table (the CLI's `symbols --json`).
- JSON "default" accepts a bare number (50 -> "50"); numbers re-emit as
  text so the model stays stringly.

## Net building (nets.hpp) - the rules that keep biting
- Reference edges first: a wire unions its two endpoints.
- Exact-position coincidence: pins/junctions sharing a grid point union;
  a wire vertex (endpoint or waypoint) on another wire's polyline unions
  the wires (T-junctions); a pin or junction on a wire's path unions with
  that wire. Two wires CROSSING where neither has a vertex NEVER connect.
- Those coincidence pickups that drew nothing are reported: NetBuild
  carries `silent_joins` - a pin on a wire's path interior (PinOnWire,
  named part/pin + wire) or a wire vertex on another wire's interior
  (WireOnWire) - one entry per position, a pin entry over a wire entry;
  an authored junction at the position suppresses it (its own dot), and
  pin-to-pin touches stay out (overlapping leads are their own mark -
  the tline deck's grounds ride them on purpose). The editor paints the
  positions as auto-junction dots; landings (wire ends on named
  entities) and crossings never report.
- Labels attach at their position (pin/junction/vertex, else the unique
  wire whose interior carries them - a crossing attaches to nothing and
  warns). Same-name labels merge, case-insensitively, ALWAYS - including
  unattached ones.
- Power/ground pins carry fixed net names and merge with every pin of the
  same name, wires or not; every ground spelling ("0" pins, labels "0"/
  "gnd") is ONE global ground.
- Names: ground "0" > lowest-id label > lowest-id power fixed name > auto
  `N001`... Auto names order nets by smallest member id (ids are unique
  across object kinds - one allocator) and skip spellings already taken.
- Ground merges mean a "gnd"-labeled net and a gnd-symbol net are the same
  net; a label spelling "gnd" is ground, not a net named gnd.
- An override with different pin offsets moves coincidence-built
  connections (reference edges survive; the pin-to-pin touch does not) -
  test_symbols.cpp pins that semantic.

## Format (format.hpp)
- `{"format":"circsim","version":1,...}`; arrays sorted by id on save for
  stable diffs. Version 0 = "no version field" and migrates through the
  chain in format.cpp (`kMigrations`); a future version loads with a
  warning. Tolerant load: duplicate ids keep the first, dangling wires are
  dropped, wrong-typed fields repair to defaults - all as LoadReport
  diagnostics, never failures. Only "not JSON" and "wrong format" fail.
- Saves go through `ws::settings::FileSystem::write` (atomic by contract).

## ERC (erc.hpp)
- floating_pin (net holds nothing else), one_pin_net (ground exempt,
  fixed-net pins exempt), empty_net, missing_ground, duplicate refs (ci),
  missing/bad ref (letter prefix, identifier chars), missing value/model,
  unknown symbol, V/L loops (multigraph over nets, parallel edges and
  self-loops included, BFS recovers the members), I cutsets (a current
  source bridging components of the no-current-source graph).
- Stage 2 param checks: a Number param that is not a SPICE number
  (bad_param, error), a Choice param outside its list (bad_param,
  warning, ci match passes), a required (non-optional emit) param that is
  missing (missing_param, error), and a params key the symbol neither
  specs nor emits (unused_param, warning - it can never reach the deck).
- Warnings never block; errors do. `run_erc(s)` = build_nets + ERC.

## Export (export.hpp)
- Deck: title line, the run temperature (`.options temp=` when the
  schematic's optional `temp` is set - emitted after the directives so
  a user's own `.options temp=` there still wins), directives verbatim,
  device cards sorted by ref (ci), analysis cards, `.end`. Power/ground
  parts emit nothing. Element name =
  the ref when valid-and-prefixed, else a deterministic mangling (ERC has
  flagged it). Node tokens: identifiers only, never leading-digit; ground
  is "0". Two nets sanitizing to one token fails the export.
- `run_schematic` = build_nets -> export -> `read_deck` ->
  `build_deck_ast` -> `run_deck` (ws::spice's own front end - never a
  second solver). Failures map back through line origins (deck line ->
  part / directives / analysis), else element/node name matching.
- Analysis specs are numbers in the model; cards print %.12g. The kinds:
  op, dc, tran, ac, and noise (`v(out[,ref]) src dec|oct|lin n fstart
  fstop [pts]`, ws::spice §7.5). The CLI's `--analysis` grammar
  ("tran 1u 1m", "ac dec 10 1 100k", "noise v(out) V1 dec 10 1 100k")
  parses with ws::spice's number grammar.

## Locate (locate.hpp, stage 3)
- The Problems panel's jump seam: `locate_diag` parses a Diag's
  pre-rendered `where` ("R3", "R3 pin b", "npn 7", "part 12",
  "net N004", "wire 5", "label out", "directives", "analysis setup",
  "simulation setup" (the temp card's origin; the Analysis block),
  comma lists) back into a `DiagTarget` (Part+optional pin / Net id /
  Wire id / Label id / the two no-geometry blocks / None).
- Element names resolve through the export back-map FIRST (the mangled
  included), then the schematic's own refs (ci), then "symbol id",
  then the raw "part id". Net names go through NetList::by_name then
  node_net. Nothing guesses: unresolvable answers None and the panel
  shows the row without a jump target.
- `focus_bounds` is the zoom answer: a part's transformed
  symbol_bounds, a net's member extent (pins/wires/junctions/labels),
  a wire's polyline, a label's anchor grown one unit; the caller's
  NetList saves a build for the Net case.

## Plot (plot.hpp, stage 4-5)
- The axis math every plot surface shares (the editor's Results
  panel today; a raster exporter later): `nice_ticks` (the 1/2/5 x 10^k
  ladder, endpoints never forced), `format_eng` (SI engineering
  notation, ASCII "u" for micro, m/M case matters, renormalizes a
  mantissa that rounds up through its decade, plain %g outside f..T),
  `expand_range` (5% padding, a degenerate span opens around its
  magnitude, non-finite answers {0,1}), `decimate_minmax` (buckets on
  a strictly-increasing x, keeps each bucket's y-min and y-max SAMPLE
  in occurrence order plus the exact ends - spikes and envelope
  survive any budget; non-increasing axes answer EMPTY = draw raw,
  the nested-.dc rule).
- The manual view (stage 5): `clamp_view` (a window slides back
  inside home; wider-than-home, degenerate or non-finite answers
  home), `zoom_range` (anchor clamped into the view first, floored at
  home's span x 1e-6, clamped into home - a wheel spun out forever
  settles on the data, never past it), `pan_range` (slides and parks
  flush at the edge). "home" is always the padded auto fit - the
  outermost view; the editor's gestures all ride these three.
- Pure doubles and strings: no ws::spice, no ui, no font. The panel
  consumes it; the tests gate it.

## Render (render.hpp, stage 17a)
- `render_symbol(sym, placement)` - the ONE geometry flattening of a
  placed symbol's picture: draw prims + the pin-lead safety net,
  world-space `SymbolStroke`s (Line/Poly/Circle/Arc/Dot/Text) with the
  part's mirror-then-rotation-then-position applied exactly like
  pin_pos applies it to pins. The editor's `draw_symbol` paints them,
  the CLI's SVG dump writes them, the tests assert them - no parallel
  transforms to drift (the stage-16c lesson, applied to symbols).
- The safety net: a pin no prim VERTEX touches and no curve carries
  (circle/arc curve within a quarter unit, dot interior; the geometric
  rule is stage 17g's - the vertex-only one drew stubs through the
  v/i sources AND the inductor's coil) gets a stub line into the
  body, opposite the pin's dir. The reach math's outside-bounds
  branches are dead code (bounds include the pins) - every stub is
  the 1.5-unit default. A stub is a LEAD, never a connection: net
  building never reads it.
- Rect prims flatten to closed quads (the placement may rotate the
  box); arcs fold mirror (reflect + sweep swap) and rotation into
  their ANGLES while the center rides the transform. Malformed prims
  are skipped, not repaired.
- test_symbol_render.cpp CHARACTERIZES the stage-2 renderer the seam
  moved verbatim - including the known warts (every Poly closes; the
  sources ride the safety net) - so the stage-17 redesign commits
  flip exactly these expectations, one diff per fix.

## Png (png.hpp)
- The raster surface, plot.hpp's promised "raster exporter later":
  `RasterCanvas` (pixel-space, 4x4-supersampled coverage for the
  analytic shapes - one hit set per call, so a joint inside one call
  never double-darkens; 1 bpp blits for the embedded font),
  `encode_png()` (8-bit RGB, filter-0 scanlines, fixed-Huffman deflate
  with greedy LZ77 - the 3-gram index is 2^20 flat slots,
  most-recent-wins, always re-checked against the 32K window; a
  distance-1 match collapses every run, white space is 2-3% of raw)
  and `render_schematic_png(s, symbols, opts)`.
- The composition rides the stage-17a seam (`render_symbol` - one
  geometry, four surfaces now: the editor canvas, the SVG dump, these
  pixels, the tests), plus one `build_nets` pass for the silent joins
  (the editor paints them as auto-junction dots; so does the sheet).
  Palette: svg_dump's ink `#1f2430` for symbols/wires/dots, `#7a2600`
  for ref/value annotations (one line, above the origin, power and
  ground symbols never annotate), `#b05600` for net labels (under the
  anchor). ppu clamps to [4, 96] with a 16384-px side cap that halves
  ppu (floor 4) rather than refusing to render.
- The one "font" in a headless lib is DATA, not a service: a generated
  bitmap table (DejaVu Sans at 12 px, ASCII 32..126, 16-row cells,
  row 12 the baseline; tools/gen_png_font.py regenerates it).
  Non-ASCII bytes draw nothing and add no advance. Determinism is a
  feature: byte-identical sheets across runs and builds, so diffs are
  meaningful.
- Unknown symbols draw nothing (erc reports them), wires ending on
  unresolvable references draw nothing (the build_nets rule), the
  empty schematic renders a blank 1x1-unit sheet. `view` is ignored -
  the sheet always fits its content.

## Layout
- `include/ws/circsim/`: model.hpp, symbols.hpp, library.hpp, diag.hpp,
  nets.hpp, format.hpp, erc.hpp, export.hpp, locate.hpp, plot.hpp,
  render.hpp (stage 17a - the symbol render seam), png.hpp (the raster
  surface + the PNG writer + the schematic composition).
  ws::core::Result only; no exceptions.
- `src/`: model.cpp, symbols.cpp (the geometry half: lookup,
  transform_pos/pin_pos, symbol_bounds, default_params),
  symbols_table.cpp (the built-in table + its prim builders - pure
  data), library.cpp, nets.cpp, format.cpp, erc.cpp, export.cpp,
  locate.cpp, plot.cpp, render.cpp (the stage-17a seam's flattening),
  png.cpp (the raster/writer/composition), png_font.cpp (the embedded
  font - pure data, generated by tools/gen_png_font.py; the
  src-local png_font.hpp is its only seam, like json_helpers.hpp).
- `examples/`: divider.csim, rc_step.csim (the smoke suite's decks),
  led_demo.csim (three LEDs over 220/680/2.2k sharing a PULSE source -
  the glow demo: 15/5/1.6 mA of red/yellow/green), tline.csym +
  tline_demo.csim (the stage-2 library demo: a T-line
  symbol the built-ins do not ship, run with `-S tline.csym`),
  scr_trigger_vfmm.csim (the VFMM 2026 SCR trigger stage: the
  PC817A opto and the Qpt41c SCR drawn from built-ins - D9/R12/C3/
  G1/Q1 and S3/D10 - with .step param ch1/ch2/pot1/load1 sweeps over
  a 1.1 s .tran in the directives block; editor-ready, no .csym
  needed).
- `apps/circsim_cli`: run / erc / netlist / png / symbols subcommands,
  `-S <lib.csym>` (repeatable, later wins) on run/erc/netlist/png;
  `png` takes `-o <out.png|->` (required) and `--ppu 4..96` (24;
  stdout is switched to binary mode under Windows). Exit codes
  0 ok, 1 sim/ERC error, 2 usage/IO (the ws::spice CLI convention).
- `apps/circsim_editor` (stages 3-4): the ws::ui::shell editor app over
  this lib - the only ui-bearing consumer. The lib itself stays
  headless (no ui include, unchanged); the editor's Results panel
  draws through plot.hpp and is gated headless by its own
  tests/test_results_panel.cpp over the Editor's plot_canvas seam.

## Tests
```
ctest --preset linux -R circsim    # plus circsim_smoke over the CLI
```
One binary, ten TUs: test_format.cpp (round-trip, unknowns, migration,
tolerant load - owns the doctest main), test_nets.cpp (the connectivity
rules), test_erc.cpp (one case per check), test_export.cpp (golden decks,
the full-path analytic sims, error mapping), test_symbols.cpp (the
library: load/save round-trips, tolerance, layering/overrides, param
typing, draw prims, bounds, and the tline full path), test_locate.cpp
(stage 3: every `where` spelling, the back-map paths, the focus
bounds), test_plot.cpp (stage 4-5: the tick ladder, the prefix walk,
the decimation contract - the exact-envelope sine among them - and
the manual-view math: clamp/zoom/pan against home, the mush floor,
the flush park), test_symbol_render.cpp (stage 17a: the render seam's
characterization - the transform, the arc angle folding, the safety
net, and the stage-2 warts the stage-17 redesign commits flip one by
one), test_symbol_draw.cpp (stage 17g: the table-level invariants -
pin reachability, pin direction with its lead, non-degenerate fills,
whole-grid pin offsets - over every built-in AND the .csym-authored
tline; the suite caught the inductor's phantom stubs on its first
run), test_render_png.cpp (the raster surface: the PNG structure and
CRCs, the Adler32 over exactly the filter-0 scanlines, the coverage
math - line rows, the cap's 13-of-16 edge, the half-blended disc
edge, the pentagram's even-odd hole - the font's advance walk, and
the composition's contract; the Huffman bits inside IDAT are verified
end to end by the smoke suite's zlib round trip, the one decoder
available). The
editor's own suite lives in the app: test_circsim_editor_panel (the
Results panel's draw through the plot_canvas seam, on the ui tree).
