# libs/circsim - CLAUDE.md

`ws::circsim` - the schematic side of the circsim app (model, `.csim`
format, net building, ERC, ws::spice export) plus its headless CLI
(`apps/circsim_cli`). Headless by construction: no ui include anywhere;
`ws::spice` is the ONLY simulator this lib ever talks to.

Deps: `ws::core`, `ws::math` (DVec2 positions), `ws::spice`, and
`ws::settings` (json::Object - the unknown-field carrier - plus the
FileSystem atomic-write seam). The settings link is the one deviation from
the task's "core, math, spice" list; the alternative was nlohmann fetched
privately, which would duplicate a first-party capability.

## Model (model.hpp)
- Ids are `uint32`, allocated from one `Schematic` counter and NEVER
  reused within a file: `next_id` persists, and load raises it above every
  id seen (dropped duplicates included). 0 is the "none" sentinel.
- Positions are grid units, y up, doubles with integer values (exact
  coincidence arithmetic). `rot` is CCW degrees 0/90/180/270; `mirror` is
  the x-flip, applied BEFORE rotation (symbols.hpp `pin_pos`).
- Wires end on REFERENCES (`Pin{part,pin}` / `Junction{id}`), never on
  positions. Waypoints are cosmetic. Junctions are explicit dots.
- `Part.params` is stringly on purpose: the symbol table (and stage 2's
  JSON library) owns the types; the property grid reads them there.
- `directives` is a verbatim text block (.model/.subckt/.include) that
  exports ahead of the device cards, unedited.
- Every mutation bumps `Schematic::version()` (cache staleness).
- Unknown `.csim` fields ride in each object's `unknown`
  (`ws::settings::json::Object`, insertion order) and re-emerge on save
  after the known fields.

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

## Layout
- `include/ws/circsim/`: model.hpp, symbols.hpp, diag.hpp, nets.hpp,
  format.hpp, erc.hpp, export.hpp. ws::core::Result only; no exceptions.
- `src/`: model.cpp, symbols.cpp, nets.cpp, format.cpp, erc.cpp,
  export.cpp. The built-in symbol table in symbols.cpp is stage 2's JSON
  library seed (same data shape, swapped source).
- `examples/`: divider.csim, rc_step.csim (the smoke suite's decks).
- `apps/circsim_cli`: run / erc / netlist subcommands; exit codes 0 ok,
  1 sim/ERC error, 2 usage/IO (the ws::spice CLI convention).

## Tests
```
ctest --preset linux -R circsim    # plus circsim_smoke over the CLI
```
One binary, four TUs: test_format.cpp (round-trip, unknowns, migration,
tolerant load - owns the doctest main), test_nets.cpp (the connectivity
rules), test_erc.cpp (one case per check), test_export.cpp (golden decks,
the full-path analytic sims, error mapping).
