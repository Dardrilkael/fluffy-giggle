# libs/spice - CLAUDE.md

The headless circuit simulator (`ws::spice`) + the `apps/spice` CLI. The
contract is `DESIGN.md` (this directory) - sections cited throughout the
code as `§n.m`; when code and this file disagree, DESIGN.md wins and this
file gets fixed.

## What exists (M0 + M1 + M2 done)
- `ws::math` sparse LU facade (lives in libs/math, grown for this arc):
  analyze once per topology, factorize per solve, real + complex,
  transpose solve for the future noise adjoint.
- Netlist front end: records with `{file,line,col}` provenance through
  `.include` expansion, the SPICE number grammar (every suffix trap
  pinned), the card AST, `.param` scopes, the expression language
  (flat stack program, constants folded at parse).
- Elaboration: `.subckt` flattening with `:`-scoped names, the node
  table, control (F/H/W) resolution, K coupling and .model resolution,
  and the §6.6 graph checks (voltage-source/inductor loops - including
  parallel-to-ground - and no-DC-path findings) that turn solver
  mysteries into named, hinted errors.
- Devices R V I E F G H C L K S W through the staged `Device` interface
  (`include/ws/spice/device.hpp`): load_dc, load_tran (companion),
  load_ac (small-signal), and the begin_step/commit/lte_ok dynamic
  state machine (§3.5/§6.2).
- MNA assembly over the shared pattern (the dry run IS the pattern,
  §3.2 - now the UNION of the dc + tran + ac stamps), the complex twin
  for .ac over the same frozen pattern, `.op`/`.dc`/`.tran`/`.ac`
  drivers (trap + BE restarts, §6.3 LTE control, §6.4 breakpoints,
  §6.5 .ic/UIC), the Results API + CSV writer, and the `spice run
  deck.cir -o out.csv` shell.

Everything else rejects at parse with file:line (working-rule 5): B/D/Q/M/T
(M3+), `.lib` (backlog), `.noise/.step/.meas/.save/.print/.probe/.nodeset`
(M5), `.options temp` (M5), `solver=klu` (M0b).

## Layout
- Public surface (`include/ws/spice/`): parse.hpp, deck.hpp (cards +
  ParamTable), expr.hpp, error.hpp, device.hpp, run.hpp (RunSettings +
  run_deck), results.hpp (Table/Results + CSV). ws::core::Result only -
  no exceptions, no exit() in lib paths (§0.6).
- `src/`: parse.cpp (records/numbers), deck.cpp (cards, params),
  expr.cpp (expression VM), waveform.cpp/.hpp (PULSE/SIN/EXP/PWL, src-
  only), elaborate.cpp (flatten, node table, graph checks), devices.cpp
  (M1 device set + waveform/AC sources + the factory table),
  devices_m2.cpp (C, L, K, S, W), mna.cpp (pattern + numeric + complex
  contexts), analysis.cpp (op/dc/tran/ac drivers), output.cpp (CSV).
  `cards.hpp`/`circuit.hpp`/`mna.hpp` are src-only.
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
  The grid is guarded before the `size_t` cast: start/stop/step must be
  finite, `stop-start` and the interval count must not overflow, and the
  materialized point count is capped at 1,000,000 (`spice.semantic.dc_range`).
- **`.dc temp` re-elaborates EVERY point** (§7.7) including the first
  (the pre-sweep elaboration ran at 27 C, not the sweep start):
  elaborate_deck takes the temperature, Circuit::temp records it. M1
  devices are temp-invariant so rows do not move, but each point gets a
  fresh kernel - and the report's lu_factorizations ACCUMULATES across
  kernel swaps (delta vs the last read, never the raw counter).
- **Nested `.dc` sweeps must be distinct**: two `temp` sweeps (they would
  share one temperature unknown) and two sweeps naming the same source
  (the outer assignment silently overwrites the inner one) are Semantic
  errors, not silent no-ops.
- **`run_deck` validates the analysis cards**: a hand-built `DeckAst`
  carrying an `AnalysisCard` with neither or both of `.op`/`.dc` is a
  Semantic error instead of a silently skipped card, and a deck with no
  analysis cards but no devices is the same `empty_circuit` error the
  analyses raise.
- **`prepare()` failures use the same wrapper as `solve()` failures**:
  pattern/symbolic-analysis errors from the ws::math facade gain the
  spice taxonomy + analysis context (`wrap_solver_error`), they do not
  leak a bare math-category error.
- **Warnings do not repeat across analyses**: each analysis re-elaborates
  the same AST, so identical elaboration warnings are deduplicated after
  every analysis; a genuinely new warning (different location, message,
  or analysis context) still lands.
- **M3 follow-up - current iterate into `load()`**: M1 devices are linear
  so `MnaSystem::load(devices)` is exact, but nonlinear devices will need
  the CURRENT iterate threaded into the stamp (§3.6). Tracked in
  `docs/SPICE_PLAN.md`; do not forget when diode/BJT/MOS land.
- **Diagnostics** (§9.2): `file:line:col: error: msg` / `error: [analysis]
  msg`, optional `hint:` line, warnings render with `warning:` and never
  fail a run. Exit codes: 0 ok, 1 deck/sim error, 2 usage/IO (§9.5).
  wrap_solver_error only re-brands FACADE errors (category "math") as
  spice.singular; kernel-classified Diagnostics (convergence, NaN)
  pass through untouched - re-wrapping re-branded Convergence as
  Singular and double-rendered the prefix.
- **CSV**: `%.12g`, comma separated, header row, sweep column(s) first
  (`time` for .tran, `frequency` for .ac); nested .dc sweeps get one
  column each (inner first). Header field count MUST equal every row's
  (a plain comma-join - a leading empty field for an absent sweep
  column was the original shape bug). A `.op` table has no sweep column;
  a `.ac` table carries re()/im() PAIRS. Deterministic run-to-run (§10.5).
- No global mutable state: the factory table lives on the elaborator
  object (§3.4); two runs in one process never interact.
- **Pattern = the UNION of dc + tran + ac stamps** (§3.2): setup() dry-
  runs load_dc, load_tran (nominal dt) and load_ac; stamp POSITIONS
  never depend on dt/omega/state values, only coefficients do. C stamps
  nothing at DC (open); L's branch row gains its diagonal only in
  tran/ac - the union covers both.
- **Transient companions** (§6.2): C is a Norton (geq = 2C/dt trap,
  C/dt BE, in parallel with jeq); L is a branch row (i - ka*v -
  kb*v_partner = rhs). Coupled pairs share Minv = [L2,-M;-M,L1]/(L1L2-M^2);
  flux linkage phi = L*i + M*i_partner is what the LTE divided
  differences track. begin_step recomputes coefficients per ATTEMPT;
  commit is the only state mutation.
- **A cold UIC start is deliberately inconsistent** (§6.5): with ic=/
  .ic and a source already on at t=0, the companion's i_prev starts at
  0 while the true cap current is not - the first step carries an O(h)
  kink. That is the documented ngspice UIC semantics ("the deck's
  choice"); NEVER write an order-accuracy test on a cold UIC start -
  step from a breakpoint with an OP before it instead.
- **dtmax defaults to TSTEP** (ngspice), not SPICE3's bare
  (tstop-tstart)/50: dtmax = min(TMAX-or-TSTEP, (tstop-tstart)/50).
  Pinned by the §10.1 RC row - with the SPICE3 default the 1-tau point
  misses the 1e-3*V gate on ordinary decks.
- **LTE** (§6.3): eps = h^3*|dd3|/2 over the last four (t, q) points
  (dd3 ~ q'''/6), tolq = trtol*(reltol*max(|q|,|q_prev|) + chgtol);
  accept iff eps <= tolq; next dt = dt*min(2,(tolq/eps)^(1/3)); a BE
  step (first step, post-breakpoint restart, 3-reject ringing heuristic)
  CLEARS the device's LTE history. fixed_dt > 0 (RunSettings, test-only)
  bypasses LTE and BE for halving studies - breakpoints still truncate.
- **Single-shot PULSE (no PER) still publishes its edges**: next_event
  enumerates {td, td+tr, td+tr+pw, td+tr+pw+tf} once when PER <= 0 - it
  used to return +inf after td and the ramp/pw/fall breakpoints were
  silently stepped over.
- **.ac conventions** (§7.4): frequency column first, then re()/im()
  PAIRS per saved quantity (the locked decision); a V source without an
  AC spec is still a short (branch row, 0 stimulus); an I source without
  one is open; i(V1) keeps the .op sign convention (negative when
  delivering) in re/im form; no AC stimulus anywhere is a Semantic
  error, never a table of zeros.
- **K is pairwise** (M2): a second K on the same inductor is a Semantic
  error (multi-winding backlog); |k| >= 1 rejected (singular); V across
  L is a constraint loop caught at elaboration ("voltage-source/
  inductor loop", or "inductor loop" when all-L).
- **Switches** (§6.2): Ron/Roff conductance model with a 1e-15 floor,
  Vt/Vh (S) or It/Ih (W) hysteresis; `.model SW(Ron=1 ...)` with glued
  parens ("SW(Ron=1") and "key value" / "key = value" spellings; state
  re-evaluates inside the Newton loop (update_state) under an itl4 cap;
  a W sense source reads POSITIVE only when current flows INTO its +
  terminal (a delivering source reads negative - same as i(V1)).
- **`.options` takes both spellings**: `reltol 1e-6` and `reltol=1e-6`;
  unknown keys warn naming the bare key ('foo', not 'foo=1').
- **.ic forces via a stiff Norton** (§6.5): gforce = 1e9*max diagonal,
  stamped during the t=0 OP only and released when the transient
  starts; .ic on ground or an unknown node is a Semantic error.

## Dialect notes (ngspice-canonical, §8.3)
- Accepted LTspice forms: `{expr}` values (devices and `.param`),
  `params:` sections on `.subckt`/X cards, bare `.param` names as
  device values, `.dc temp`, `Rser=` on L (an internal `name#mid`
  node joins them - it appears in the save list).
- Waveform sources: `[DC] value [AC mag [phase]] [PULSE(...)|SIN(...)|
  EXP(...)|PWL(...)]`; the waveform may span tokens; a waveform source
  without an explicit DC uses waveform(0) for OP/DC solves; TR/TF/PW/
  PER and SIN FREQ / EXP TAUs default from TSTEP/TSTOP at .tran time
  (ngspice defaults; EXP TD2 defaults to TD1+TSTEP - give it explicitly
  for a clean single rise).
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
main), test_parse.cpp (records/numbers/includes), test_run.cpp (the M1+M2
gate: analytic .op/.dc/.tran/.ac rows at 1e-9 rel, parse-error goldens
pinning file:line:col prefixes, topology goldens, CSV shape, param
scoping; the trap-order row rides the fixed_dt hook).
A new device or analysis does not land without its row (§10.1).
