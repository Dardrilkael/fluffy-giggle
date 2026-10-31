# libs/spice - CLAUDE.md

The headless circuit simulator (`ws::spice`) + the `apps/spice` CLI. The
contract is `DESIGN.md` (this directory) - sections cited throughout the
code as `§n.m`; when code and this file disagree, DESIGN.md wins and this
file gets fixed.

## What exists (M0-M4c + the 2026-10-29 structural revision)
- `ws::math` sparse LU facade (lives in libs/math, grown for this arc):
  analyze once per topology, factorize per solve, real + complex,
  transpose solve for the future noise adjoint.
- Netlist front end: records with `{file,line,col}` provenance through
  `.include` expansion, the SPICE number grammar (every suffix trap
  pinned), the card AST, `.param` scopes (rng-threaded for Monte-Carlo),
  the expression language with SIGNAL references (`v(node)`,
  `v(a,b)`, `i(Vsrc)`) and `@dev[param]` values.
- **§3.5 rev - the arenas:** a Circuit is immutable topology + devices;
  each run binds a RunState = {bound parameter values (expression
  sources re-evaluated per run), fixed-size dynamic state slices,
  pins}. Value writes never move stamp positions - symbolic reuse is
  by construction (the §7.6 pattern-hash check is retired). Devices'
  dynamic hooks take their state slice explicitly; fold_state() folds
  bound params (and K-partner L) in per step attempt.
- **§2.1 rev - two front ends, one elaborator:** CircuitBuilder (nodes
  as handles, `10_k`/`100_n` unit literals, ExprVal scoped values)
  emits the same card stream the parser does; Simulator::create/run/
  set/set_source/release_source/snapshot/restore/rebind is the C++
  surface. write_netlist(circuit, state) emits a flattened numeric
  deck for any circuit - the ref-harness bridge; every device
  implements write_card or the writer fails naming it.
- **§7.8 rev - columnar results:** Table = sweep axes + one contiguous
  column per signal; `t["v(out)"]` is a span. row(i)/rows() materialize
  row-major for the CSV writer/tests. .raw writer: LTspice/ngspice
  binary format, direct column dump, .ac re/im pairs merge to complex.
- **§6 rev - steppable transient:** TranMarch (shared time loop) +
  TransientRun::advance_to (whole accepted steps only), StopToken
  (prompt, resumable), progress callback, set_source between steps =
  closed-loop control. run_tran is a 3-line loop over TranMarch.
- Analyses: .op/.dc/.tran/.ac over the shared kernel; the §5.2 ladder
  (newton -> gmin staircase -> source homotopy -> PTC) with the stage
  in AnalysisReport; .step families (param/temp/source, list/dec/oct,
  first card outermost) merged into one family table with step axes;
  Monte-Carlo gauss()/flat() through the per-run Rand (seeded, recorded).
- **M3 - the diode + the §3.6 current iterate:** `DcLoadContext` now
  exposes `node_v()`/`branch_i()`/`gmin()` so nonlinear devices
  linearize against the current iterate (the pattern recorder returns
  0 - positions only, §3.2); `AcLoadContext` gained `state_` so the
  diode's `load_ac` reads its frozen OP `Vd_prev`. `Device` gained
  `NonlinResid nonlinear_residual(st)` and `update_state` gained an
  `x_prev` param (the pre-solve iterate, for pnjlim reference slots).
  The kernel's Newton loop now checks BOTH the voltage-delta AND the
  per-device nonlinear residual (§5.1's two-gate convergence). The
  diode: Shockley `I = Is*(exp(Vd/(n*Vt))-1) + gmin*Vd`, hand-
  differentiated, pnjlim (SPICE3 devlim, §5.3) clamps the per-iteration
  junction-voltage jump past vcrit. `solve_tran` is now a real Newton
  loop (was switch-only when every device was linear).
- **M4a - the BJT** (Ebers-Moll transport model): direct terminal-current
  stamp (Ic/Ib/Ie + derivatives into G, Norton offsets into rhs), two
  pnjlim slots (Vbe/Vbc), `.model NPN|PNP(Is= Bf= Br= Nf= Nr=)`; NPN and
  PNP share polarity-independent G entries - only the RHS flips (sgn).
  `Builder::bjt()`/`bjt_model()`.
- **M4b - junction caps**: the nonlinear charge companion (SPICE3
  depletion Q/C + the Fc*Vj linear extension), shared by the diode
  (Cjo/M/Vj/Fc; state 3 -> 16 slots) and both BJT junctions
  (Cje/Cjc/Mje/Mjc/Vje/Vjc/Fc; state 4 -> 30 slots) - the
  q_dep_jn/c_dep_jn helpers live in devices_m4.cpp. begin_step freezes
  geq/jeq at the last COMMITTED V; commit stores Q(V_new) + the LTE
  (t,Q) history; seed_state takes the OP. A device with caps is dynamic
  (is_dynamic flips true; Cjo=0 keeps the M3 diode byte-identical).
  .ac adds j*omega*Cj(V_op).
- **M4c - the MOSFET** (level 1 Shichman-Hodges + body effect): triode
  and saturation Id with (1+lambda*Vds) channel-length modulation,
  Vth = Vto + gamma*(sqrt(2*phi-Vbs) - sqrt(2*phi)); `M name d g s b
  model [L= W=]`, `.model NMOS|PMOS(Vto= Kp= Lambda= Gamma= Phi=)`. No
  fetlim by design (quadratic, not exponential - converges plain) and
  no Meyer caps yet. NOTE: no §10.1 analytic test row yet - validation
  is the ngspice 39 cross-check + the mos_* decks; the test row is the
  first gap to close.
- measure:: library (rise_time, rms, trig_targ, ...) + .meas as a thin
  parser (TRIG/TARG WHEN AVG RMS MAX MIN PP INTEG FIND...WHEN/AT
  DERIV); failures are notes (--strict-meas promotes).
- .save/.print/.probe expression columns evaluated post-hoc over the
  stored defaults (no hidden B sources); the axes are pseudo-columns.
- compat=ngspice|ltspice|strict (§8.3 rev); `spice check` lint (number
  traps + topology + unused models, nothing simulates); CLI: -o -,
  --format raw, --keep-going, --strict-meas, --werror.

Everything else rejects at parse with file:line (working-rule 5): B
(M4d), T (M4e), `.lib` (backlog), `.noise/.nodeset` (M5), `solver=klu`
(M0b).

## Layout
- Public surface (`include/ws/spice/`): parse.hpp, deck.hpp (cards +
  ParamTable), expr.hpp, error.hpp, device.hpp (§3 contract v2),
  circuit.hpp (the elaboration output - public for the builder),
  state.hpp (RunState + Rand), analysis.hpp (Op/Dc/Tran/Ac aggregates),
  builder.hpp (+ units.hpp literals), simulator.hpp, transient.hpp
  (StopToken/TransientRun), results.hpp (columnar), measure.hpp,
  raw.hpp, netlist.hpp, lint.hpp, run.hpp (run_deck + RunSettings),
  results writers. ws::core::Result only - no exceptions, no exit() in
  lib paths (§0.6); no ws::math type ever appears here (§2.2).
- `src/`: parse.cpp, deck.cpp (cards/params), expr.cpp (VM + signal/@
  refs), error.cpp (the taxonomy), elaborate.cpp (flatten, node table,
  graph checks, subckt-local value inlining), devices.cpp /
  devices_m2.cpp / devices_m3.cpp / devices_m4.cpp (the 15 kinds
  R/V/I/E/F/G/H/C/L/K/S/W/D/Q/M - param specs + state slots +
  write_card; _m4 = the BJT/MOSFET + the shared junction-cap
  companion), mna.cpp + mna.hpp (pattern + numeric contexts over
  (Circuit, RunState, x, gmin)), kernel.hpp (shared DcKernel + the
  ladder + NonlinResid convergence), analysis.cpp (drivers + .step
  family + save-expression evaluator + compat=strict), transient.cpp
  (TranMarch/TransientRun), simulator.cpp, builder.cpp, state.cpp,
  circuit.cpp (finalize + .options), waveform.cpp (PULSE/SIN/EXP/PWL,
  §6.2/§6.4), measure.cpp, raw.cpp, netlist.cpp, lint.cpp, output.cpp.
  cards.hpp/drivers.hpp/waveform.hpp are src-only.
- `apps/spice`: argv, file I/O, exit codes - run + check subcommands.
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
- **M3 landed - the §3.6 current iterate is threaded**: `MnaSystem::load`
  and `load_tran` take `(devices, rs, x, gmin)` where `x` is the current
  iterate (nullptr in pattern mode / measure-only loads). Nonlinear
  devices read `cx.node_v(n)` / `cx.branch_i(br)` to linearize against
  it. The kernel passes `&x` (the pre-solve iterate) and `settings_.gmin`
  every Newton iteration; the initial "measure max_diagonal" load passes
  nullptr (the diode at Vd=0 produces finite Gd = Is/(n*Vt) + gmin).
  `update_state` takes an `x_prev` param (the pre-solve iterate) so
  nonlinear devices can advance their pnjlim/reference slots - the
  diode stores `Vd_lim = pnjlim(Vd(x_prev), vold, ...)` as `vold` for
  the next iteration (NOT the raw Vd - that's the bug that stuck the
  diode at Vd=100V from the first iterate; see STATUS.md).
- **§5.1 two-gate convergence** (M3): the Newton loop accepts only when
  BOTH (a) every unknown's `|Δx| ≤ reltol*max(|x_new|,|x_old|) + tol_abs`
  AND (b) every device's `nonlinear_residual().delta ≤
  reltol*ref + abstol`. Linear devices return `{0,0}` and never gate.
  The diode returns `{|I_cur - I_prev|, max(|I_cur|,|I_prev|)}` tracked
  through update_state. Both gates must pass - a stable Vd with a still-
  changing I (exponential regime) keeps iterating.
- **pnjlim** (§5.3, M3): SPICE3's `pnjlim` simplified (the `icheck`
  first-time flag folded into the always-directional form - the
  difference only matters on the first clamp of a sequence, where vold
  is typically 0 so both forms step up by 2*Vt). Clamps `|ΔVd|` to `2*Vt`
  when `vnew > vcrit` (≈ 0.7V for defaults). vcrit = `N*Vt*ln(N*Vt/(√2*Is))`
  (SPICE3 formula). The clamp is essential: without it, the first Newton
  iterate from a cold start puts Vd ≈ Vs (the diode looks like a near-
  open at Vd=0), and `exp(Vs/Vt)` overflows for Vs > ~1V. Since the
  M4c fix, `update_state` returns TRUE while the limiter is actively
  clamping (Vd_lim != Vd_load; both BJT junctions likewise) - otherwise
  a stiff cold start falsely converges at a stale linearization point
  (the device current sits below abstol, so the residual gate passes).
- **Diode state** (3 slots, M3; grows to 16 when Cjo > 0): `[0] Vd_prev`
  (the clamped Vd from the last iteration, for pnjlim's vold), `[1]
  I_cur`, `[2] I_prev` (for nonlinear_residual), then the M4b companion
  slots (V_comp/Q_comp/I_comp/geq/jeq + the LTE history + folded
  Cjo*area). All zeroed at bind. `update_state` stores the CLAMPED
  `Vd_lim` as `Vd_prev` (not the raw iterate), recomputing
  `pnjlim(Vd(x_prev), vold, ...)` identically to `load_dc` - both use
  the same vold, so they agree. Dynamic ONLY when Cjo > 0
  (`is_dynamic()`); with no caps `load_tran` defaults to `load_dc`.
- **Diode temp** (M3 limitation): `Vt` is frozen at `Tnom` (default 27°C)
  at `apply_model` time. `.options temp` does NOT re-revaluate the
  diode's thermal voltage (temp-dependent models are M5). The Lambert-W
  gate uses the default 27°C; `.dc temp` over a diode gives wrong
  currents at non-27°C points. M5 threads `temp` through the load context.
- **Diode model params**: `.model name D(Is=... N=... Cjo= M= Vj= Fc=
  Tnom=...)` implemented (M3 + M4b's junction cap). Rs, TT, Bv, Ibv,
  Xti, Eg, Kf, Af are ACCEPTED (so ngspice decks parse cleanly) but NOT
  implemented - M5 (Tnom/Xti/Eg temp, Bv/Ibv breakdown, Kf/Af noise).
  Unknown keys reject at parse naming the allow-list.
- **`.options gshunt`/`gminsteps`/`srcsteps`** (M3): `gshunt` (default 0,
  off) is the opt-in per-node conductance to ground (§5.3) - masks §4.4's
  floating-node errors, so off by default; a positive value shunts every
  node diagonal in every Newton load. `gminsteps`/`srcsteps` (default 10,
  ngspice) control the §5.2 ladder's stage 2/3 staircase counts.
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

## Rules that keep biting (2026-10-29 additions)
- **The arenas are the whole design**: a Circuit never changes after
  elaborate_deck/finalize_circuit; everything mutable rides RunState.
  If you find yourself wanting a setter on a Device, you want a
  parameter slot (§3.5a) or a state slot (§3.5b) instead.
- **fold_state() before begin_step()**: dynamic hooks read bound params
  through their state slice's folded mirrors (kCParam/kLFold/...). The
  driver folds once per STEP ATTEMPT - never inside the Newton loop
  (params cannot change mid-solve). A new dynamic device must fold
  every value its hooks read, or it silently uses zeros.
- **Stamp positions never depend on values** (§3.2, now load-bearing):
  the pattern dry-run stamps against zeroed scratch slices. The
  §7.6-rev reuse guarantee IS this invariant - breaking it breaks .step,
  Monte-Carlo and set() all at once.
- **Subckt-local params inline at flatten time** (elaborate.cpp's
  inline_local_values): a card inside a subckt referencing a subckt
  param becomes a number token; GLOBAL references stay live so .step/
  Monte-Carlo rebinds reach inside instances. knows_local() is the
  predicate.
- **One .param name = one draw per bind** (the ParamTable resolve
  cache): never clear it inside a run - a plain run's .op and .tran
  must see the SAME gauss draw (test pins this). .step points hand
  bind a FRESH scope, so points draw anew by construction.
- **advance_to never splits a step**: TransientRun marches whole
  accepted steps to the target; breakpoints/LTE/BE semantics are
  identical to a one-shot run (pinned bit-for-bit). If you "need" to
  land exactly on t, you want a breakpoint, not a split.
- **Save expressions run per .step point** (while that point's state is
  live): @dev[param] reads the point's bound values. The axes are
  pseudo-columns; a bare axis name dedupes to the axis itself.
- **.raw determinism**: no Date/Offset lines unless supplied - a deck
  produces byte-identical .raw output (§10.5). Same for the netlist
  writer (to_chars shortest round-trip spellings).
- **strict compat walks the AST, not the parser**: the LTspice forms
  parse everywhere (they must - the default dialect accepts them) and
  reject post-hoc in run_deck when compat=strict. Detection is on raw
  tokens ('{' prefix, 'Rser=', 'params:'/'='), so it needs no parser
  threading.

## Tests
```
ctest --preset linux -R spice    # linux preset = core;math;spice subset
```
One binary, four TUs: test_spice.cpp (error taxonomy, owns the doctest
main), test_parse.cpp (records/numbers/includes), test_run.cpp (the
gate: analytic .op/.dc/.tran/.ac rows, parse-error goldens, topology
goldens, CSV shape, param scoping, the arenas' isolation, the ladder,
.step/Monte-Carlo, .meas, save expressions, .raw, lint, compat), and
test_builder.cpp (the C++ front end: the review's RC example, builder
vs netlist equivalence, the round-trip writer, Simulator handles,
snapshot/restore, the steppable/cancellable transient, units).
A new device or analysis does not land without its row (§10.1).

The Windows-facing CLI smoke is `tools/smoke-spice.ps1`; keep any new
CLI cases (--format raw, check, --strict-meas) in sync there when the
Windows box is next at hand.
