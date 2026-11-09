# libs/spice - CLAUDE.md

The headless circuit simulator (`ws::spice`) + the `apps/spice` CLI. The
contract is `DESIGN.md` (this directory) - sections cited throughout the
code as `§n.m`; when code and this file disagree, DESIGN.md wins and this
file gets fixed.

## What exists (M0-M5d + the 2026-10-29 structural revision)
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
  The default save list also probes TERMINAL CURRENTS: a device without
  a branch row that implements probes_current()/terminal_current()
  (the diode - i(D1) from the Shockley equation at the accepted
  solution, the Rs path reading the series element) grows an i(name)
  column in .op/.dc/.tran; .ac opts out (a DC probe cannot answer the
  small-signal system).
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
  `Builder::bjt()`/`bjt_model()`. The Early voltages (Vaf/Var) are
  implemented: oef = 1 - Vbc/Vaf - Vbe/Var multiplies the TRANSPORT
  current only (SPICE3's base charge sans roll-off), the derivatives
  gain go = Ic/Vaf and its Vbe-side mirror; Vaf/Var=0 (default) is
  byte-identical M4a; negatives reject. Builder BjtModel: optional
  vaf/var.
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
  no Meyer caps yet. The §10.1 analytic rows landed with the PMOS rhs
  fix they caught (a double polarity negation in the Norton offset -
  every PMOS operating point was wrong; NMOS and the ngspice
  cross-check never noticed).
- **M4d - the behavioral B source** (`B name n+ n- V=expr | I=expr`): a
  nonlinear source whose value is a §8.5 expression over v(node),
  v(a,b), i(Vname), `time` and .param names, linearized per Newton
  iteration by dual-number AD - one value pass plus one directional
  pass per signal reference, all in fixed-size stack arrays (caps
  reject loudly at elaboration: 64 vars / 32 refs / 256 stack). The
  stamp is the Norton linearization: Jacobian entries at the
  referenced unknowns (the VCCS convention: +dr leaves p_, -dr at n_),
  the remainder in the rhs. Domain failures (sqrt/ln/log/pow outside
  the domain, division by zero, non-finite) are Convergence errors
  naming the source and the iterate through Device::load_error - a
  NEW interface channel the kernel's FatalErr path turns into an
  immediate ladder abort (no homotopy rescues an out-of-domain
  expression). Kink slopes (sqrt/asin/acos/pow at their edges) are
  nudged 1e-12 inside the domain - the pnjlim spirit: clamp the
  linearization, never the value; the cold start x=0 sits on sqrt's
  kink, so value-pass-only failures would make every sqrt deck
  unrunnable. .ac = the gradient at the frozen OP (ref values live in
  the state slice); behavioral sources are never a stimulus and never
  publish breakpoints. `update_state` gained a `params` tail param
  (behavioral residual needs the true gains); `set_control_branch`
  gained a `which` index (a B names one i() slot per occurrence).
  write_card round-trips plain expressions; a .param-referencing one
  fails the writer naming the device (no AST rewrite yet).
- **M4e - the lossless T-line** (`T in+ in- out+ out- Z0= TD=`, or
  `F=`/`NL=` with TD = NL/F, `IC=` 1-4 values under UIC): the method of
  characteristics over two port-current branch rows. Each port is a
  Thevenin `v_k = Z0*i_k + E_k` whose EMF is the wave the OTHER port
  launched one delay ago; DC/OP is the wire pair stamped as an ideal
  1:1 transformer over the same rows (constraint edges
  (in+,out+)/(in-,out-)); .ac is the lossless two-port
  `V1 - Z0*I1 = (V2+Z0*I2)e^{-j*w*TD}`. The history: a 2048-sample
  (t, W1, W2) ring in the state slice, pruned to the pickup bracket,
  seeded with the t=0 state over [-TD, 0], LINEAR-interpolated at the
  pickup (exact on legs; ngspice's quadratic bends across a vertex).
  Delay crossings register breakpoints: `next_event` takes the §3.5
  state slice and returns the earliest sample+TD, capping dt at TD and
  landing every delayed kink on a grid point (BE restart after). No
  line LTE (nothing charge-like to integrate; the ports' curvature is
  the attached dynamic devices' LTE). Ring overflow fails loudly
  through load_error. `Builder::tline()`.
- measure:: library (rise_time, rms, trig_targ, ...) + .meas as a thin
  parser (TRIG/TARG WHEN AVG RMS MAX MIN PP INTEG FIND...WHEN/AT
  DERIV); failures are notes (--strict-meas promotes).
- .save/.print/.probe expression columns evaluated post-hoc over the
  stored defaults (no hidden B sources); the axes are pseudo-columns.
- compat=ngspice|ltspice|strict (§8.3 rev); `spice check` lint (number
  traps + topology + unused models, nothing simulates); CLI: -o -,
  --format raw, --keep-going, --strict-meas, --werror.
- **M5a - temperature (§7.7)**: temp_k() on both load contexts (the
  circuit's elaboration temperature - every temp change re-elaborates);
  the diode Is(T)/Vt(T)/vcrit(T) (diotemp.c tlev=0), the BJT factlog +
  XTB beta exponent, the MOS level-1 pbfact chain (Phi(T)/Vto(T)/
  Kp(T)=(T/Tnom)^-1.5), R/C TC1/TC2 (instances + models), per-instance
  temp= everywhere, `.options tnom=` patched into models lacking one.
  physconst.hpp (src-only) owns k/q/Eg(T)/rc_temp_factor (CODATA 2018).
  T == Tnom is bit-identical everywhere (the factors return exactly 0/1).
- **M5b - .nodeset (§7.2)**: v(node)=expr seeds the cold-start iterate
  + a soft g=1 Norton on iteration 1 of every ladder stage (released
  after). .dc hints the first point only; .ac/.tran OPs honor it.
- **M5c - .noise (§7.5)**: the adjoint analysis - one A^T x = e_out
  solve per frequency folds every device's current PSDs (NoiseContext);
  the forward gain carries only the named input source (MODEACNOISE).
  R thermal + flicker, diode _rs/_id/_1overf, BJT _ic/_ib/_fn, MOS
  _thermal(2/3·gm)/_1overf; ninteg.c power-law integration for the
  totals; spectra + per-device summary tables. (This arc also fixed the
  MOSFET stamping NOTHING into .ac - channel_eval() is now shared by
  load_dc/load_ac/load_noise.)
- **M5d - the MOS Meyer caps (§6.2)**: TOX/CGSO/CGDO/CGBO on the
  level-1 model. meyer_half is SPICE3's DEVqmeyer verbatim (HALF-caps;
  the caller doubles and adds the overlaps - mos1load.c's small-signal
  `2*half+overlap`, pinned against the ngspice source); gate_caps does
  the reverse-mode source/drain swap and the body-effected von. State:
  42 slots when caps exist - a (V,Q,I,geq,jeq) block + 3x(t,q) LTE
  history per terminal pair over the MIRROR-frame pair voltages
  (vgs/vgd/vgb; vgb = vgs - vbs, NOT vgs+vbs). load_tran stamps the
  three charge Nortons (jeq flips with sgn for PMOS); load_ac stamps
  j*omega*C at the frozen OP; the tran charge evolves
  q += (v_new-v_old)*C(v_new); the t=0 seed is the TRANOP q = v*C.
  Caps are dynamic-only (TOX or an overlap present - a bare model keeps
  the M4c stamp byte-identical).

Everything else rejects at parse with file:line (working-rule 5):
`.lib` (backlog), `solver=klu` (M0b).

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
  devices_m2.cpp / devices_m3.cpp / devices_m4.cpp / devices_m4d.cpp /
  devices_m4e.cpp
  (the 17 kinds R/V/I/E/F/G/H/C/L/K/S/W/D/Q/M/B/T - param specs +
  state slots + write_card; _m4 = the BJT/MOSFET + the shared
  junction-cap companion, _m4d = the behavioral B source, _m4e = the
  lossless T-line), mna.cpp +
  mna.hpp (pattern + numeric contexts over
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
- **Diode model params**: `.model name D(Is=... N=... Rs= Cjo= M= Vj=
  Fc= Tnom=...)` implemented (M3 + M4b's junction cap + the series
  resistance). TT, Bv, Ibv, Xti, Eg, Kf, Af are ACCEPTED (so ngspice
  decks parse cleanly) but NOT implemented - M5 (Tnom/Xti/Eg temp,
  Bv/Ibv breakdown, Kf/Af noise). Unknown keys reject at parse naming
  the allow-list. Rs > 0 moves the junction to an internal 'name#mid'
  node (SPICE3's topology; Rs scales as Rs/area); Rs = 0 creates NO
  node - the M3 stamp is byte-identical. apply_model takes the
  NodeResolver so the model itself claims the node (models resolve
  after every factory; the graph checks run after resolve_models).
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
- **T-line DC is the wire pair** (M4e): the lossless line's steady
  state is v1 = v2, i1 = -i2, stamped as an ideal 1:1 transformer over
  the two port-current branch rows - NOT node merging (elaboration
  cannot know a tran will run) and NOT a near-zero gmin fudge. The
  constraint edges (in+,out+)/(in-,out-) mirror it for the §6.6 graph
  checks: parallel lines / V sources bridging a line pair are
  constraint loops, and a far port with no other connection still has
  its DC path through the line.
- **T-line stamps never depend on state in pattern mode** (M4e,
  §3.2): the tran Thevenin row's -Z0 is a legal structural zero at dry
  run (zeroed scratch), and load_ac stamps the full two-port at ANY
  omega - the omega==0 numeric fallback writes a POSITION SUBSET (the
  wire rows), so MnaSystem::setup's AC dry run stamps at a nominal
  omega=1 to record the superset.
- **The T-line's breakpoints come from its state** (M4e): next_event
  takes the state slice (the §3.5 explicit-slice convention) and
  returns the earliest stored-sample+TD - it both caps dt at TD
  (causality: the pickup t-TD must not extrapolate past the newest
  sample) and lands delayed kinks on grid points. Sources ignore the
  new parameter; every next_event override must keep tolerating it.
- **T-line IC= replaces the waves wholesale** (M4e, the C/L ic=
  precedent): seed_state computes W_k from the four IC values when
  given, else from the t=0 solution (OP exact; UIC without IC uses the
  UIC vector - ngspice zeroes instead, documented deviation). The
  port-current branch unknowns are NOT seeded from IC.
- **The T-line overflow latch is device-side** (M4e): load_error (the
  §8.5 channel) receives no state slice, so the ring-full flag is a
  one-shot `mutable bool` on the device - set by commit, cleared at
  seed and when it fires. The same sharing argument as the inductor's
  m_ mirror covers parallel runs.

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
