# libs/spice - CLAUDE.md

The headless circuit simulator (`ws::spice`) + the `apps/spice` CLI. The
contract is `DESIGN.md` (this directory) - sections cited throughout the
code as `§n.m`; when code and this file disagree, DESIGN.md wins and this
file gets fixed.

## What exists (M0 + M1 done)
- `ws::math` sparse LU facade (lives in libs/math, grown for this arc):
  analyze once per topology, factorize per solve, real + complex,
  transpose solve for the future noise adjoint.
- Netlist front end: records with `{file,line,col}` provenance through
  `.include` expansion, the SPICE number grammar (every suffix trap
  pinned), the card AST, `.param` scopes, the expression language
  (flat stack program, constants folded at parse).
- Elaboration: `.subckt` flattening with `:`-scoped names, the node
  table, control (F/H) resolution, and the §6.6 graph checks
  (voltage-source loops - including parallel-to-ground - and no-DC-path
  findings) that turn solver mysteries into named, hinted errors.
- Devices R V I E F G H (all linear, stateless) through the staged
  `Device` interface (`include/ws/spice/device.hpp`).
- MNA assembly over the shared pattern (dry run IS the pattern, §3.2),
  `.op` and `.dc` drivers (incl. nested two-source sweeps, warm start,
  `temp` re-elaboration), the Results API + CSV writer, and the
  `spice run deck.cir -o out.csv` shell.

Everything else rejects at parse with file:line (working-rule 5): C/L/K
(M2), B/D/Q/M/T/S/W (M4+), `.lib` (backlog), `.ac/.tran` (M2), PULSE/
SIN/EXP/PWL sources (M2), `.options temp` (M5), `solver=klu` (M0b).

## Layout
- Public surface (`include/ws/spice/`): parse.hpp, deck.hpp (cards +
  ParamTable), expr.hpp, error.hpp, device.hpp, run.hpp (RunSettings +
  run_deck), results.hpp (Table/Results + CSV). ws::core::Result only -
  no exceptions, no exit() in lib paths (§0.6).
- `src/`: parse.cpp (records/numbers), deck.cpp (cards, params),
  expr.cpp (expression VM), elaborate.cpp (flatten, node table, graph
  checks), devices.cpp (the M1 device set + factory table), mna.cpp
  (pattern + numeric contexts), analysis.cpp (op/dc drivers),
  output.cpp (CSV). `cards.hpp`/`circuit.hpp`/`mna.hpp` are src-only.
- `apps/spice`: argv, file I/O, exit codes. Nothing else - every real
  behavior lives in the lib.

## Rules that keep biting
- **ws::math is PRIVATE** (§2.2): no ws::math type or header under
  include/; the MNA policy layer wraps, never re-exports.
- **Branch rows number AFTER the nodes** (§3.3): a BranchId is a 0-based
  counter; its MNA row is `node_count + id`. Devices never compute that
  themselves - they ask the load context (`branch_row()`), because only
  the system knows the final node count.
- **Pattern = what load touches** (§3.2): there is no "declare your
  entries" API. A numeric stamp outside the recorded pattern trips
  WS_CORE_ASSERT (programmer error, not a runtime failure).
- **Sign conventions follow ngspice**: V/E/H branch currents flow from
  N+ through the source to N- (a sourcing V shows negative current); a
  positive I/F/G output current likewise leaves N+ and enters N-
  (`I1 0 out 1m` charges `out` positive).
- **.op writes CSV only as the sole analysis** (§7.2); `results_csv`
  enforces it, Results keeps the table either way.
- **Deck-order execution** (§7.1): each analysis re-elaborates; a deck
  with no analysis still elaborates once (no silent gaps) but produces
  no tables.
- **Line 1 is ALWAYS the title** (§8.1, same as ngspice): a deck whose
  first line is a device card silently loses that device to the title.
  build_deck_ast warns when the title looks like a card (device letter +
  digit, or a leading '.') - `spice.warn.title_looks_like_card` - so the
  footgun is never silent. Ordinary English titles never warn.
- **.dc point grid**: `floor((stop-start)/step + 1e-9) + 1` points at
  `start + k*step` - never accumulated drift; first source innermost.
- **Diagnostics** (§9.2): `file:line:col: error: msg` / `error: [analysis]
  msg`, optional `hint:` line, warnings render with `warning:` and never
  fail a run. Exit codes: 0 ok, 1 deck/sim error, 2 usage/IO (§9.5).
- **CSV**: `%.12g`, comma separated, header row, sweep column(s) first;
  nested sweeps get one column each (inner first). Header field count
  MUST equal every row's (a plain comma-join - a leading empty field
  for an absent sweep column was the original shape bug). A `.op` table
  has no sweep column: the header starts at the first `v()`.
  Deterministic run-to-run (§10.5).
- No global mutable state: the factory table lives on the elaborator
  object (§3.4); two runs in one process never interact.

## Dialect notes (ngspice-canonical, §8.3)
- Accepted LTspice forms: `{expr}` values (devices and `.param`),
  `params:` sections on `.subckt`/X cards, bare `.param` names as
  device values, `.dc temp`.
- `.param` needs `name=value` as one token (spaces around `=` are not
  ngspice either); assignments are whitespace-separated.
- Ground: `0` and `gnd`, case-insensitive. Node names are arbitrary
  non-whitespace tokens. Matching is case-insensitive via the lowercase
  key; display spellings (first seen) name the output columns.

## Tests
```
ctest --preset linux -R spice    # linux preset = core;math;spice subset
```
One binary, three TUs: test_spice.cpp (error taxonomy, owns the doctest
main), test_parse.cpp (records/numbers/includes), test_run.cpp (M1 gate:
analytic .op/.dc rows at 1e-9 rel, parse-error goldens pinning
file:line:col prefixes, topology goldens, CSV shape, param scoping).
A new device or analysis does not land without its row (§10.1).
