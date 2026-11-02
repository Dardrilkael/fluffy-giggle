# libs/circsim - CLAUDE.md

`ws::circsim` - the schematic side of the circsim app (model, `.csim`
format, net building, ERC, ws::spice export, the `.csym` symbol library,
the stage-3 locate seam) plus its headless CLI (`apps/circsim_cli`) and
its editor (`apps/circsim_editor`, the one ui-bearing consumer - the
lib itself stays headless). Headless by construction: no ui include
anywhere; `ws::spice` is the ONLY simulator this lib ever talks to.

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
- The built-in table (symbols.cpp, 12 keys) carries specs + draw for
  every symbol; `find_symbol` is the BUILT-IN-ONLY lookup.
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
- Deck: title line, directives verbatim, device cards sorted by ref (ci),
  analysis cards, `.end`. Power/ground parts emit nothing. Element name =
  the ref when valid-and-prefixed, else a deterministic mangling (ERC has
  flagged it). Node tokens: identifiers only, never leading-digit; ground
  is "0". Two nets sanitizing to one token fails the export.
- `run_schematic` = build_nets -> export -> `read_deck` ->
  `build_deck_ast` -> `run_deck` (ws::spice's own front end - never a
  second solver). Failures map back through line origins (deck line ->
  part / directives / analysis), else element/node name matching.
- Analysis specs are numbers in the model; cards print %.12g. The CLI's
  `--analysis` grammar ("tran 1u 1m", "ac dec 10 1 100k") parses with
  ws::spice's number grammar.

## Locate (locate.hpp, stage 3)
- The Problems panel's jump seam: `locate_diag` parses a Diag's
  pre-rendered `where` ("R3", "R3 pin b", "npn 7", "part 12",
  "net N004", "wire 5", "label out", "directives", "analysis setup",
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

## Layout
- `include/ws/circsim/`: model.hpp, symbols.hpp, library.hpp, diag.hpp,
  nets.hpp, format.hpp, erc.hpp, export.hpp, locate.hpp.
  ws::core::Result only; no exceptions.
- `src/`: model.cpp, symbols.cpp, library.cpp, nets.cpp, format.cpp,
  erc.cpp, export.cpp, locate.cpp. The built-in symbol table in
  symbols.cpp (with its specs and draw) is the shape the .csym library
  feeds.
- `examples/`: divider.csim, rc_step.csim (the smoke suite's decks),
  tline.csym + tline_demo.csim (the stage-2 library demo: a T-line
  symbol the built-ins do not ship, run with `-S tline.csym`).
- `apps/circsim_cli`: run / erc / netlist / symbols subcommands, `-S
  <lib.csym>` (repeatable, later wins) on run/erc/netlist; exit codes
  0 ok, 1 sim/ERC error, 2 usage/IO (the ws::spice CLI convention).
- `apps/circsim_editor` (stage 3): the ws::ui::shell editor app over
  this lib - the only ui-bearing consumer. The lib itself stays
  headless (no ui include, unchanged).

## Tests
```
ctest --preset linux -R circsim    # plus circsim_smoke over the CLI
```
One binary, six TUs: test_format.cpp (round-trip, unknowns, migration,
tolerant load - owns the doctest main), test_nets.cpp (the connectivity
rules), test_erc.cpp (one case per check), test_export.cpp (golden decks,
the full-path analytic sims, error mapping), test_symbols.cpp (the
library: load/save round-trips, tolerance, layering/overrides, param
typing, draw prims, bounds, and the tline full path), test_locate.cpp
(stage 3: every `where` spelling, the back-map paths, the focus
bounds).
