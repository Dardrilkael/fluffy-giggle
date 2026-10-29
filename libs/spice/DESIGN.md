# ws::spice — design document

Draft v1 (2026-10-26, revised 2026-10-27). Nothing is implemented yet: this document and
`include/ws/spice/device.hpp` are the two artifacts awaiting approval.
Implementation (§11) starts only after sign-off. Companion living docs once
the lib lands: `libs/spice/CLAUDE.md` (rules) and `STATUS.md` (state).

## 0. Problems found in the brief (working-rule 2)

Flags raised before building, each with a proposal:

1. **Reuse workspace libs; never expose their backends.** A circuit
   matrix is extremely sparse and is refactorized thousands of times per
   run over a fixed symbolic pattern. Per your calls: the sparse matrix
   + LU land as a NEW `ws::math` component, and `ws::spice` links it
   PRIVATE — no ws::math type ever appears in `include/ws/spice/*.hpp`;
   the public surface is ws::spice-owned types plus `ws::core::Result`.
   Dependency-free is demoted to a preference, per your instruction:
   where a third-party backend is genuinely better, the choice is asked,
   not assumed. §4.6 states the one such choice (the solver backend).
2. **SPICE number parsing has a famous trap.** `M`/`m` = milli (1e-3),
   `MEG` = mega (1e6), `K` = kilo, all case-insensitively — LTspice and
   ngspice agree on this. The parser follows SPICE convention; unit tests
   pin every suffix including `MIL` (25.4 µm) and the `M` vs `MEG` prefix
   scan order (§8.2).
3. **"Every conceivable circuit" is not a spec.** Semiconductor models
   alone are unbounded (a BSIM4 is thousands of lines). §11 fixes a
   concrete, verifiable order and leaves the rest to an explicit backlog.
   Anything outside the supported set is rejected at parse with a
   line-numbered error (working-rule 5) — never a silent wrong answer.
4. **`.meas` is a small language in ngspice.** v1 implements the measured
   subset (TRIG/TARG, WHEN, AVG, RMS, MAX, MIN, PP, INTEG, FIND...WHEN/AT,
   DERIV) and rejects the rest with a line number; the full grammar is a
   backlog milestone, not a silent gap.
5. **CSV needs a save-list semantics.** Decks say what to save
   (`.save`/`.print`/`.probe`); without one the default is all node
   voltages plus all voltage-source currents (ngspice's default). CSV
   layout: first column time/sweep/frequency, then one column per saved
   quantity, named as the deck spells it (`v(out)`, `i(V1)`), comma
   separated with a header row.
6. **"Usable later as a shared library"** means: no global mutable state
   anywhere (already a house rule), no `exit()`/`abort()` in library
   paths, and the CLI stays a thin parse → simulate → write shell in
   `apps/spice`. Device-type registration is an explicit table on the
   elaborator object — never a process-global registry.
7. **Flatten vs hierarchy.** `.subckt` is expanded (flattened) at
   elaboration with `:`-scoped names. Xyce-style hierarchical matrices
   are rejected for v1: they buy memory on massively repeated subckts and
   cost a second matrix-mapping subsystem. Noted as a known limitation
   (§8.4).
8. **ngspice as ground truth needs the binary.** The ref harness
   (`tools/spice-refcheck.ps1`, §10.2) auto-detects ngspice on PATH and
   skips loudly when absent — analytic ground truth is the primary gate
   and always runs. Confirmed 2026-10-27: ngspice is on PATH here, so
   the harness runs for real.
9. **The working tree held uncommitted stage-13 WIP.** Settled: that arc
   landed its phases on its own (`4d2daf9` the undo stack, `59f3aa2` the
   command views) and a second agent now works in `apps/ui_gallery` /
   the demo apps concurrently. spice commits add only new paths
   (`libs/spice/**`, `apps/spice/**`, `libs/math` sparse additions,
   small build wiring) and stage only files this project touched.

## 1. Goals and non-goals

Goals (from the brief, restated as verifiable targets):

- Headless C++20 core library `libs/spice` (`ws::spice`) plus CLI
  `apps/spice` — `spice run circuit.cir -o out.csv`.
- Netlist dialect: the common LTspice/ngspice subset — devices R C L K,
  V/I (DC, AC, PULSE, SIN, EXP, PWL), E F G H, behavioral B, switches,
  diode, BJT (Gummel–Poon), MOS (level 1 first; BSIM in the backlog),
  lossless transmission line. Cards: `.op .dc .ac .tran .noise .step
  .param .meas .model .subckt .include .save .print .options .nodeset
  .ic .end`.
- Accuracy gates: the analytic suite (§10.1) at per-test tolerances, and
  ngspice agreement within stated tolerances wherever the harness can run
  (§10.2).
- Errors: every unimplemented construct rejects at parse with file:line;
  solver failures carry actionable diagnostics (§9).

Non-goals (v1):

- No GUI, no plotting, no waveform binary format — CSV plus a text report
  for noise/`.meas`. A `.raw` writer is a backlog add-on behind the same
  results API.
- No `.control` scripting, `.four`, `.disto`, `.sens`, `.pss`, IBIS,
  Verilog-A, cosimulation, or multithreading.
- No SPICE2-style special cases: a construct either works or is rejected.

## 2. Architecture

### 2.1 Pipeline

```
 deck text files ──parse──► card AST (file:line provenance kept)
        │                        │
        │                  param evaluation (.param values, .step sweep sets)
        │                        │
        │                  elaborate: .model table, .subckt flattening,
        │                        │   node table, device instances
        │                        ▼
        │                  Circuit (immutable topology + devices)
        │                        │
        │                  symbolic setup (pattern, etree, pivot order) ─ reused
        │                        ▼
        │                  analysis drivers, deck order honored
        │                        │   (op result feeds ac/tran/noise)
        │                        ▼
        │                  results (per-analysis sample vectors)
        └── CLI ───────────────► CSV / text writers
```

- Parse is a pure function (text, include paths) → AST; no simulator types.
- Elaborate is a pure function AST + device factory table (§3.4) → Circuit.
- Analyses mutate only per-analysis state; `Circuit` stays const during a
  run; devices own their dynamic state and reset between analyses.
- One `Simulator` value owns the whole run. Two `Simulator`s in one
  process never interact; the only static data is const metadata tables.

### 2.2 Components and files (libs/spice)

| component | files |
|---|---|
| records, tokenizer, SPICE number parsing | src/parse.cpp, `include/ws/spice/parse.hpp` |
| expressions + dual-number AD | src/expr.cpp, `include/ws/spice/expr.hpp` |
| card AST + semantic checks | src/deck.cpp |
| param scope, .param/.step evaluation | src/params.cpp |
| models, subckt flattening, node table | src/elaborate.cpp |
| device interface + instances | `include/ws/spice/device.hpp`, src/devices/*.cpp |
| MNA assembly + load contexts | src/mna.cpp |
| *(sparse matrix + LU: a new `ws::math` component — see §4 — consumed, not owned)* | `libs/math/.../sparse*` |
| analysis drivers (op, dc, ac, tran, noise) | src/analysis/*.cpp |
| .meas post-processing | src/measure.cpp |
| results + CSV/report writers | src/output.cpp |
| error taxonomy → ws::core::Error | src/error.cpp |

`apps/spice` (main.cpp) does argv, file IO, and nothing else.

Ownership and exposure (the backend rule): the sparse matrix + LU live
in `ws::math` — the workspace's numerics home — and `ws::spice` links
that library PRIVATE. No ws::math type appears in any
`include/ws/spice/*.hpp`; the public surface is ws::spice-owned types
plus `ws::core::Result`. This is the root workspace rule verbatim ("a
lib that needs more of a third-party library grows a thin ws-layer
wrapper"): the MNA policy layer — pattern collection from device dry
runs, pivot thresholds, when a re-analyze is owed — is `ws::spice`'s
`src/mna.cpp`, a wrapper over the solver, never a re-export of it.
What `ws::math` gains is shaped by this project's needs (a facade with
analyze/factorize reuse, real + complex) and earns its suite in the
ws::math tests (§10.3). §4.6 names the backend choice behind the
facade.

## 3. Device interface

The contract is `include/ws/spice/device.hpp` (drafted alongside this
document). Decisions behind it:

### 3.1 Staged virtual interface, not fat dispatch

Each device is a polymorphic instance with staged hooks: `setup` (claim
branch-current unknowns, once per topology), `load` (stamp the MNA system),
`load_ac`, `noise`, `nonlinear_residual`, `reset_state`. Rejected
alternatives:

- **CRTP/compile-time device list** (zero virtuals): stamping is small
  O(nnz) work and the factorization dwarfs it; ngspice and SPICE3 both use
  indirect dispatch through function pointers/virtuals at scale. Template
  machinery would cost compile time and per-device unit-test isolation for
  ~0 measured gain. Revisit only with a profile in hand (house rule: no
  premature optimization).
- **SPICE2-style monolithic switch-on-type loader**: one file owning every
  stamp; kills unit testability of devices and the device/registry split.
- **Devices as pure data (stamp tables)**: cannot express nonlinear,
  behavioral or state-dependent devices.

### 3.2 Pattern collection by dry-run — the load path is the pattern

Right after `setup`, the simulator runs `load` once in *pattern mode*: a
recording context that maps every stamp to an (row, col) pair and keeps no
values. The recorded union IS the symbolic pattern fed to the LU (§4.2).
There is no separate "declare your entries" API to drift out of sync with
the real load code — pattern = what `load` touches, by construction.
Rules: state-dependent devices (switches) stamp the union of all their
states in pattern mode; zero-valued entries are legal (they cost storage,
not flops); a numeric-mode write to an entry not in the pattern trips a
programmer-error assert (house assert policy — `WS_CORE_ASSERT`).

### 3.3 Context family

- `DcLoadContext` — real loads for op/dc/tran (`kind()` disambiguates;
  tran adds `time()`, `dtime()`, `add_breakpoint`). The pattern recorder is
  a private subclass, so pattern mode runs the same code path.
- `AcLoadContext` — complex stamps around the frozen operating point; the
  OP solution is readable through it. The symbolic pattern is the union of
  the dry runs in every mode a deck can use — DC/OP, **transient** (a
  capacitor stamps nothing at DC but a companion conductance in tran, so a
  DC-only dry run would trip the §3.2 assert mid-transient) and AC — built
  once and shared.
- `NoiseContext` — per-frequency density contributions at the OP.

Helpers (`add_conductance(a,b,g)`, `add_current(a,b,i)`, branch-row
helpers) are inline non-virtual members over the recording primitives;
devices touch raw indices only for branch rows. Node ids double as
voltage-unknown indices (identity; ground is −1 and skipped), so stamps
never go through a lookup table.

### 3.4 Model/instance split; factory table on the elaborator

`.model` lines build shared `DeviceModel` parameter sets; instance cards
build `Device` objects referencing a model. Creation dispatches per first
letter through an explicit table on the elaborator — no global registry
(the no-global-mutable-state rule; also what keeps tests injectable and
the shared-library story clean). First letters not in the table → parse
error with the line number.

### 3.5 State ownership

Devices own their dynamic state: previous charges/fluxes, junction
voltages, limiting state, source schedule cursors. The simulator never
pokes device internals; `reset_state()` clears them between analyses.
This is what makes `Circuit` reusable across `.step` points and keeps the
`Simulator` re-entrant per run.

### 3.6 Nonlinear contract

`load` must produce the exact linearization at the current iterate —
correct derivatives or Newton lies, the #1 source of "converges to
garbage". Compact models are hand-differentiated and unit-tested against
finite differences; behavioral sources ride the dual-number AD (§8.5).
`nonlinear_residual()` reports the device's worst normalized
terminal-current change since the previous load, complementing the
solver's voltage-delta convergence check.

## 4. Sparse LU with symbolic reuse (a ws::math component, PRIVATE)

The facade contract is fixed no matter the backend: `analyze` once per
topology (symbolic), `factorize`/`refactor` per Newton/time step
(numeric, pattern reused), `solve` against stored factors, real and
complex variants, `ws::math`-owned types only. The sections below state
the algorithmic policy; §4.6 records the decided backend choice.

### 4.1 System shape

MNA: `[G B; C D]·[v; i] = [z; e]`, ground row/col dropped, node voltages
first, then branch rows (V sources, inductors, E/H branches). Typical size
10³–10⁵ unknowns, nnz ≈ 4–6 per device.

### 4.2 Algorithm: left-looking Gilbert–Peierls LU, KLU-style

- **Symbolic (once per topology):** pattern from the dry run → CSC, then
  a fill-reducing ordering owned by the backend: KLU path = BTF + AMD
  (KLU's own defaults); Eigen path = COLAMD inside
  `SparseLU::analyzePattern`. A bare natural order is not used: MNA branch
  rows (V/L/E/H) have structurally zero diagonals, so natural order
  invites pivot-driven fill. The ordering runs once per topology, so its
  cost is noise next to the thousands of refactors.
- **First factor:** left-looking GP with threshold partial pivoting (row
  swaps only), `|pivot| ≥ pivrel·max|col|` (pivrel default 1e-3, SPICE's);
  pivot rows and exact L/U patterns are recorded.
- **Refactor (every Newton/time step):** numeric-only pass over the
  recorded pattern and pivot rows — no search, no allocation, O(flops).
  This is the whole point of the reuse: topology never changes inside a
  transient, so the symbolic investment is paid once per `.step` point.
  Backend note: the fixed-pivot numeric-only refactor is the KLU
  backend's fast path; under Eigen, every `factorize` re-runs numeric
  pivoting but still reuses `analyzePattern`'s symbolic work — the
  facade call is identical either way (§4.6).
- **Stability check per refactor:** failing `|u_kk|` (pivabs) escalates
  to a full re-factor with fresh pivoting; a second failure is reported
  as singular (§4.4). Escalations are rare and counted into diagnostics.

### 4.3 Storage

Column-major L and U in one array (KLU layout) over a frozen pattern;
refactor is a straight pass. Explicit zeros from the switch-state union
stay in the pattern: they cost storage, never pivots.

### 4.4 Singular means a diagnostic, not just an error

The actionable naming comes from the elaboration-time graph checks
(§6.6): floating nodes, voltage-source loops, inductor/current-source
cutsets and pure capacitor loops are reported by NAME before any solve
runs — "no DC path to ground from node 'X1:7' (common causes: floating
node, series capacitors, open switch)". The runtime singular report
adds analysis/time context on top. Working-rule 6: the user gets the
root-cause candidate, not "matrix singular". The KLU backend additionally
reports the failing column (`singular_col`), which the facade maps to its
unknown's name; the Eigen backend has no such report and leans on the
graph findings alone — the diagnostics live in the graph checks, with
the factorization as a bonus where available.

### 4.5 Complex and the adjoint system

`.ac` rebinds the value type to `std::complex<double>` over the same
symbolic pattern with its own numeric factors. `.noise` needs
`Aᵀx = e` (the adjoint/transpose system): v1 factors Aᵀ explicitly
per frequency point — noise decks are small and few-pointed, so the
extra factorization is trivial and it keeps the facade backend-agnostic.
Reusing the factors for transpose solves (KLU's tsolve trick) is a
backlog optimization under either backend.

### 4.6 Rejected alternatives

- **Dense LU (ws::math):** O(n³), dead beyond toy decks; sparse wins
  somewhere in the tens-to-~100 unknowns range. No size-based dense fast
  path for now — measure first.
- **The backend choice (decided 2026-10-27):** vendor SuiteSparse KLU
  as the primary backend AND wrap Eigen's SparseLU as a
  runtime-selectable alternative — both compiled in, selected per run
  via `.options solver=klu|eigen` and the CLI `--solver=` flag, with
  the analytic suite cross-validating one backend against the other
  (§10.3). Rejected single-backends: Eigen alone re-runs numeric
  pivoting per factorize (slower per-refactor path, no failing-pivot
  column on singularity); hand-written KLU-style numerics owned ~1000+
  lines of the most failure-silent code in the project (a subtly wrong
  LU yields plausible-looking garbage — working-rule 6's nightmare)
  and was dropped when you opted for the proven library. KLU brings a
  real third-party C dependency: KLU and BTF are LGPL-2.1+, AMD and
  SuiteSparse_config are BSD-3 (exact pinned file set fixed at vendoring
  time). **Open question for sign-off:** LGPL static-linked into a library
  meant to be reused (possibly in proprietary engines/apps) carries
  relinking obligations; either link KLU dynamically/as a separate
  component, or accept Eigen (MPL-2.0) as the only backend for such
  consumers. Decide before M0b. **Build order:** the facade lands first over the Eigen
  backend — Eigen is already in-tree and the wrap is ~100 lines, so
  M1 proceeds without waiting on vendoring — and KLU + the runtime
  switch + the cross-validation land as their own milestone (M0b,
  §11) before M2, with the default flipping to `klu` there. The
  facade is KLU-shaped, so the swap touches ws::math internals only.
- **SPICE3 sparse1.3 / Markowitz symbolic ordering:** strictly more
  bookkeeping (pivot candidates, row/col permutations preserved across
  factors) for fill gains that only matter on pathological orderings.
- **Iterative (GMRES + ILU):** nonlinear MNA iterations make convergence
  guarantees hard; direct is the industry standard for a reason.
- **Re-symbolic per time step:** unnecessary — the pattern is stable;
  the switch-flip invalidation is explicit, rare, and correct.

## 5. Nonlinear: Newton and the convergence-aid ladder

### 5.1 The loop

Assemble → refactor → solve → update → check. Converged when BOTH:

- every unknown's `|Δx| ≤ reltol·max(|x_new|, |x_old|) + tol_abs`, with
  `tol_abs` = vntol for node voltages and abstol for branch currents
  (SPICE's rule is a **sum**, not a max — a max would be looser near zero
  crossings than the SPICE the ref harness compares against), and
- every nonlinear device's `nonlinear_residual() ≤ reltol·max(|I|, |Î|) + abstol`
  (Î = the current predicted by the previous linearization).

Defaults follow SPICE: reltol 1e-3, vntol 1e-6 V, abstol 1e-12 A, gmin
1e-12; iteration caps itl1=100 (op), itl2=50 (dc sweep point),
itl4=10 (tran time point). All are `.options`.

### 5.2 The ladder, fixed order (each stage logged into diagnostics)

1. **Plain Newton with per-device limiting** (§5.3).
2. **gmin homotopy:** junction-parallel conductance stepped down a
   geometric staircase to the deck's gmin; each step warm-starts from the
   previous solve.
3. **Source homotopy:** every independent source scaled 0 → 1 over the
   staircase, warm-started likewise.

Staircase endpoints and step counts follow ngspice (`gminsteps=10`,
`srcsteps=10` equivalents); the exact constants are pinned in M3 against
the comparison harness rather than invented here (working-rule 4). Which
stage succeeded goes into the diagnostic record.

**Rejected:** pseudo-transient continuation — interacts with ICs and adds
a solver mode; gmin/source stepping covers the practical corpus (LTspice
ships exactly these two). It stays in the backlog, not the ladder.

### 5.3 Limiting, before any homotopy

pn-junction voltage limiting per SPICE3's `pnjlim` (per-iteration Δv clamp
on the junction, ~2·vt scale, with its stuck-detection retry) and FET
limiting on vds/vgs (`fetlim`/`limvds`, vdsat-based). Mechanism per ngspice; the
clamp constants live with the device code and are pinned by unit tests
against finite-difference behavior, not eyeballed.

**Rejected:** global line-search damping (Armijo) — generic but slow, and
it fights the per-device limiters; per-device limiting is the SPICE-proven
tool. **Rejected:** always-on node-to-ground gshunt — it masks the
floating-node errors §4.4 exists to surface; `.options gshunt` stays
available as an opt-in.

### 5.4 Failure diagnostics

Non-convergence answers with: the analysis, per-stage iteration counts,
and the worst K unknowns by normalized residual, with names. The ladder
is fixed; options only widen it explicitly. No silent tolerance tweaks
(working-rule 6): a deck that only converges at reltol=0.1 says so in its
own output.

## 6. Transient: integration, timestep control, breakpoints

### 6.1 Integration

Trapezoidal (default; 2nd-order, A-stable, SPICE's default). Backward
Euler at: the first step after t=0, the first step after every breakpoint
landing, and as the one-step damping response to trap ringing (detected:
LTE estimates alternating while dt is already shrinking → one BE step,
resume trap). This heuristic is ours, not a claimed ngspice behavior;
the M2 RLC/switching tests decide whether it stays. Gear(2) and TR-BDF2 are backlog; Gear
orders > 2 are rejected (validation burden vs. benefit).

### 6.2 Companion models

C and L via charge/flux-consistent companions (linear C: exact;
nonlinear caps from day one integrate on **charge**, never on a
capacitance — the interface carries Q(v) and dQ/dv so MOS caps cannot
accumulate charge error). Switches are conductance models (Ron/Roff
with tiny conductance floor), never a true open — that keeps the pattern
stable (§3.2). Lossless T-line: method of characteristics with a delay
ring buffer and interpolated pickup; delay crossings register
breakpoints.

### 6.3 Timestep control

LTE-driven per dynamic device from divided differences of its own
charge/flux history (trap: third difference; the exact formula carries a
citation to the SPICE3/ngspice source at implementation, and the M2
analytic suite pins the constants — RC/RLC errors are the ground truth).
Let `tolq = trtol·(reltol·max(|q|, |q_prev|) + chgtol)` (chgtol default
1e-14 C, trtol default 7 — both SPICE's). Accept iff per-device
`ε ≤ tolq`. Next dt: `dt·min(2, (tolq/ε)^(1/3))`, clamped to
`[dtmin, dtmax]`; dtmax = min(TMAX from the .tran card,
(tstop−tstart)/50) — a constant, not `(tstop−t)/50`, which would shrink
toward zero as t→tstop; dtmin floor with a "cannot resolve" error, not an
infinite loop. Step growth never exceeds 2×.

### 6.4 Breakpoints

Sources publish their schedules (PULSE edges, PWL corners, SIN/EXP
start/stop times) through `add_breakpoint` — an idempotent, ε-merged,
sorted set. Devices register only the next event ≥ current time as they
pass it. dt is truncated to the next breakpoint exactly; landing within ε
forces the BE restart of §6.1. The first transient step is
min((tstop−t0)/50, TMAX).

### 6.5 Initial conditions (ngspice semantics)

- No `UIC`: an OP is solved at t=0; `.ic`/`ic=` values are **enforced**
  during that OP (stiff constraint) and released when the transient
  starts — unlike `.nodeset`, which only seeds; sources evaluate at t=0.
- `UIC`: no OP — `ic=` values are the t=0 state directly and the first
  step is BE. Nodes with no `ic` under UIC start at 0 V; the consequence
  (possible initial discontinuity) is the deck's choice, documented in
  the run's report, not corrected silently.
- `.nodeset` never forces anything — it only seeds Newton everywhere
  (op/dc/tran OP alike).

### 6.6 Topological hazards — elaboration-time checks, not solver mysteries

Loops of voltage sources/inductors, cutsets of current sources and
inductors, and loops mixing capacitors with voltage sources make the MNA
system structurally singular or index-2 (a cutset of only capacitors and
current sources is the separate "no DC path to ground" case, §4.4; a
loop of capacitors alone is fine). ngspice
answers these with cryptic "singular matrix" failures. We detect them at
elaboration with graph checks and name the offending cycle explicitly
("voltage-source loop: V1, V2"; "no DC path to ground: node 'X1:7'").
The deck fix is series resistance/`rseries`-style; the error message says
so. Index-2 handling itself is a backlog item, never a silent gap.

## 7. The other analyses: op, dc, ac, noise, step

Transient is §6. This section covers the rest of the analysis drivers
(`src/analysis/*.cpp`) and how a deck with several of them runs.

### 7.1 Deck-order execution and the frozen operating point

Analysis cards run in deck order, each producing its own result table.
`.ac` and `.noise` linearize around an operating point, and they never
borrow one from an earlier analysis: each solves its own OP first, with
sources at their DC values, so deck order never changes an answer. `.tran` solves its own t=0 OP
(§6.5). The OP used by an AC/noise run is recorded in that run's
diagnostics (§9.4).

### 7.2 `.op`

One Newton solve through the full ladder (§5.2). Output: all node
voltages and voltage-source/inductor branch currents (the default
save-list, §0.5), plus a device operating-point block (region, gm, gds,
junction voltages) in the text report. `.op` never writes CSV by itself
unless it is the only analysis (one row, header + values).

### 7.3 `.dc`

`.dc src start stop step [src2 start2 stop2 step2]` — up to two nested
sweeps, first source innermost (ngspice order). Sweepable: V/I source
value, a `.param` name (`.dc param name ...`), and `temp`.

- The first point runs the full ladder; every later point is
  warm-started from the previous solution with plain Newton (itl2 = 50),
  falling back to the ladder from that point on failure.
- Warm-starting means circuits with hysteresis give path-dependent
  curves (same as ngspice); the sweep direction is therefore honored as
  written, including `stop < start` with negative step.
- Step must be nonzero and point toward `stop`; the point count is
  `floor((stop−start)/step + 1e-9) + 1` — no accumulated-float drift, each
  value computed as `start + k·step`.
- A failing point aborts with the sweep value in the error (§9.2).

### 7.4 `.ac`

`.ac dec|oct|lin N fstart fstop`. Frequency list, computed directly (not
by repeated multiplication): `dec`: `f_k = fstart·10^(k/N)` for
k = 0..⌊N·log10(fstop/fstart) + 1e-9⌋ (so the endpoint is included when it
lands on the grid); `oct` likewise with 2^(k/N); `lin`: N points inclusive
of both ends.

- Sources contribute via `AC mag [phase°]`; other sources are zero in
  the small-signal system. A deck with `.ac` and **no** AC source is an
  error ("no AC stimulus"), never a table of zeros.
- Per frequency: `load_ac` stamps into the complex matrix over the shared
  pattern (§3.3), refactor, solve. The device linearization is the one
  frozen at the OP.
- CSV columns: `frequency`, then per saved quantity either the pair
  `re(v(out))`, `im(v(out))` (default save-list) or a single column when
  the deck asks for a derived form — `.print ac vm(out) vp(out) vdb(out)
  vr(out) vi(out)` — named as spelled. Phase is `atan2` in degrees,
  **not unwrapped** (same as ngspice); unwrapping is a post-processing
  concern.

### 7.5 `.noise`

`.noise v(out[,ref]) src dec|oct|lin N fstart fstop [pts_per_summary]`.

Per frequency: factor the system, solve the adjoint `Aᵀx = e_out` once
(§4.5), then every noise source's contribution is
`|x_a − x_b|²·S_i` — one solve serves all sources. The forward gain from
the input source to the output comes from one ordinary AC solve at the
same frequency, giving the input-referred density `inoise = onoise/|H|`.

- Densities in V/√Hz (A/√Hz for current outputs); the spectra are
  `onoise_spectrum`, `inoise_spectrum`, and a per-device contribution
  table at the summary points.
- Sources: resistor thermal `4kT/R`; shot `2q·I` on junctions; flicker
  `KF·I^AF/f^EF` per model; MOS channel thermal `(8/3)kT·gm` (level 1).
  Devices report them through `NoiseContext` (§3.3). `k` and `q` are
  CODATA constants fixed in one header; temperature is the run's TEMP
  (§7.7).
- Integrated totals (`onoise_total`, `inoise_total`) integrate density²
  over frequency with **log-log (power-law) segments**, which is exact
  for white and `1/f^k` pieces, and are pinned by the kT/C test (§10.1).
- `Aᵀ` is factored explicitly per frequency (§4.5).

### 7.6 `.step`

`.step param name start stop inc`, `.step param name list v1 v2 ...`,
`.step dec|oct param name start stop N`, and `.step temp ...`, with
optional source-value form (`.step V1 ...`). Multiple `.step` cards nest,
first card outermost.

- Each step point re-evaluates `.param` scope (§8.4) and re-elaborates
  into a fresh `Circuit` (cheap relative to a transient). If the stamp
  pattern hash is identical to the previous point's, the symbolic
  analysis is reused; otherwise it is redone — correct either way, never
  assumed.
- All analyses in the deck run per step point. CSV gains a leading `step`
  column plus one column per stepped parameter, so a single file holds
  the whole family.
- **Failure policy:** abort on the first failing step point with the
  step values in the error. `--keep-going` (CLI) runs the remaining
  points and exits nonzero at the end with a per-point failure list. No
  silent skipping.

### 7.7 Temperature

`.options temp=` (default 27 °C) and `tnom=` (default 27 °C). Models carry
their own temperature-dependence equations (diode/BJT Is, Eg, XTI; R/C
TC1/TC2); elaboration re-evaluates them per temperature, so `.step temp`
and `.dc temp` re-elaborate rather than patching. Per-instance `temp=`
overrides where ngspice allows it.

### 7.8 Results API

`Results` (ws::spice-owned) holds, per analysis, a `Table`: analysis
kind, sweep column kind (none / time / frequency / swept value), column
names as spelled, and row-major samples — real or complex. Nothing in
it references ws::math. Writers in `src/output.cpp` consume `Table`
only; a future `.raw` writer is another consumer of the same type.

## 8. Netlist front end: records, numbers, scope, expressions

### 8.1 Records and provenance

- Line records: first line is the title (ignored), `*` starts a full-line
  comment, `;` and ` $` start inline comments, a leading `+` continues
  the previous record, `.end` terminates. UTF-8 input; `µ` is accepted as
  `u` in numbers.
- Matching is case-insensitive via a lowercase key; the display name is
  the first spelling seen and is what output columns use (§0.5). File
  names in `.include` stay case-exact.
- Every token keeps `{file, line, col}` through `.include` expansion
  (relative to the including file, then the CLI `-I` paths). Include depth
  is capped (16) and cycles are detected by canonical path. `.lib` is
  rejected in v1 with a line-numbered error (backlog).
- Node names are arbitrary non-whitespace tokens; `0` and `gnd`
  (case-insensitive) are ground.

### 8.2 SPICE numbers

Grammar: optional sign, digits with optional `.`, optional `e[+-]digits`,
then an optional scale suffix, then ignorable trailing letters. The
suffix scan is **longest-match, case-insensitive**:

| suffix | factor | suffix | factor |
|---|---|---|---|
| `T` | 1e12 | `U` / `µ` | 1e-6 |
| `G` | 1e9 | `N` | 1e-9 |
| `MEG` | 1e6 | `P` | 1e-12 |
| `K` | 1e3 | `F` | 1e-15 |
| `MIL` | 25.4e-6 | `M` | 1e-3 |

Traps the unit tests pin, each its own case: `1m` = 1e-3 and `1meg` = 1e6
(`MEG` and `MIL` are tried before `M`); `1F` = 1e-15, **femto, not
farad** (`1uF` = 1e-6; the trailing `F` is a unit and ignored); `5V`,
`10kOhm`, `1.5e-3A` parse by ignoring trailing letters that are not a
suffix; `.5`, `5.`, `1e3`, `-2.2u`; a suffix directly after an exponent
(`1e3k`) and any non-numeric remainder that starts a number (`1.2.3`) are
errors. `1meh` is 1e-3 (the `eh` is trailing letters) — matches ngspice.

### 8.3 Cards and dialect policy

Card handlers live in `src/deck.cpp`, one function per card; an unknown
card or instance letter is a line-numbered error (§0.3). Instance
letters: R C L K V I E F G H B S W D Q M T X. ngspice syntax is canonical;
LTspice forms are accepted where unambiguous and listed in
`libs/spice/CLAUDE.md` (curly-brace parameters, `Rser=` on L, `.step
param`, `.meas`). Ambiguous or LTspice-only constructs (`.lib` models
with encrypted content, `.model` of unsupported type, `.func` beyond
the implemented subset) are rejected, not guessed.

### 8.4 Parameters, scopes, subcircuits

- `.param` evaluates lazily with a scope chain: subckt-instance
  parameters → enclosing subckt defaults (`params:`) → global. A cycle
  (`a = b+1`, `b = a`) is an error naming the cycle. `.step` values
  override at the global level.
- `.subckt name nodes... [params: ...]` / `.ends`; `.global` nodes are
  shared across all instances.
- **Flattening** (§0.7): instance `X1` of `foo` creates `X1:`-scoped
  device names (`X1:R3`) and internal nodes (`X1:7`); nested instances
  chain (`X1:X2:7`). Port nodes map to the caller's nodes. `.model`
  inside a subckt is local to it. Nesting depth is capped (32) and direct
  or mutual recursion is detected at elaboration and reported with the
  call chain.
- Known limitation (§0.7): flattening grows the matrix with every
  instance; hierarchical solve is a backlog item, not hidden.

### 8.5 Expressions and dual-number AD

Expression language (B sources, E/G with `value=`, `.param`, device
parameters in `{}`): `+ − * / ** ^`, unary ±, comparisons and `? :`,
`&& || !`, and `abs sqrt exp ln log log10 sin cos tan asin acos atan
atan2 sinh cosh tanh pow pwr min max floor ceil int sgn limit if u
uramp`; variables `time`, `temp`, `freq`; and `v(node)`, `v(a,b)`,
`i(Vname)` where legal (behavioral sources only).

- `.param` expressions evaluate in plain `double`. Behavioral-source
  expressions parse once to a flat program; constants fold at parse time.
- **Dual numbers:** a behavioral expression's references are fixed at
  parse time (say k distinct unknowns), so evaluation carries a value
  plus a k-wide gradient in preallocated scratch — no allocation per
  Newton iteration. The B source stamps the Jacobian from the gradient
  and the Norton right-hand side `f − Σ ∂f/∂x_j · x_j`. The recorded
  stamp pattern comes from the parsed reference list, not from values,
  so it is stable (§3.2).
- Derivatives of non-smooth functions are defined, not accidental:
  `abs`/`sgn` use the active branch (0 at the kink), `min`/`max` the
  selected argument, `if`/`? :` the taken branch; `sqrt`, `ln`, `log`,
  `pow` outside their domain, division by zero, and any non-finite result
  are a Convergence error naming the B source and the iterate (§9.1)
  rather than a NaN in the matrix.
- AC: the small-signal element is the gradient at the OP. Noise: none.
- Every AD derivative is checked against finite differences in the unit
  suite (§10.4).

## 9. Errors, diagnostics, exit codes

### 9.1 Taxonomy

`ws::spice::ErrorKind`: **Parse** (lexing, grammar, numbers),
**Semantic** (unknown model/node/param, bad arity, include cycle),
**Topology** (§6.6 graph findings), **Convergence** (Newton failure, NaN
iterate), **Singular** (factorization failure, §4.4), **Timestep**
(dt below dtmin), **Measure** (`.meas` evaluation), **Io**, **Usage**
(CLI/API misuse). Each maps into a `ws::core::Error` with a stable code
string (`spice.parse.unknown_card`, `spice.topology.vsource_loop`, …);
the structured payload — location, analysis, time/sweep value, candidate
names — travels in a `ws::spice::Diagnostic` carried with the error.
Library paths never call `exit()`/`abort()` (§0.6); only programmer-error
asserts (`WS_CORE_ASSERT`) can trap.

### 9.2 Message shape

`file:line:col: error: message` for anything with a source location, and
`error: [analysis context] message` otherwise, each followed by an
indented `hint:` line when one is known:

```
amp.cir:14:1: error: unknown card '.lib' (not supported in v1)
amp.cir:22:1: error: voltage-source loop: V1, V2
  hint: add series resistance in one branch of the loop
error: .dc V1 at 3.30 V: no convergence after gmin stepping and source stepping
  worst unknowns: v(X1:7) = 4.2e+01 (residual 3.1e-02), i(L2) = ...
```

### 9.3 Warnings

Collected, never fatal: unused `.model`, parameter shadowing, ignored
`.options`, `.meas` whose trigger never fired, `exp` argument clamped,
gmin or source stepping needed to converge, and any widened tolerance
(§5.4). Printed to stderr and the report; `--werror` promotes them to
errors. A warning is a fact about the run, so it also appears in the
machine-readable report.

### 9.4 Run diagnostics record

Per analysis: convergence stage reached, Newton iterations, LU refactors
and pivot escalations (§4.2), rejected timesteps, min/max accepted dt,
breakpoint count, OP used for AC/noise (§7.1), step point values (§7.6),
wall time. Written to the text report (`--report file`) and summarized
with `-v`; the same struct is returned from the library API.

### 9.5 Exit codes and `.meas` policy

`0` success; `1` simulation or deck error (any kind in §9.1 except
Io/Usage); `2` usage or file-I/O error. A `.meas` that cannot be evaluated
prints `failed` in the report with the reason and does **not** fail the
run, matching ngspice; `--strict-meas` makes it exit `1`.

### 9.6 Partial results

On a mid-analysis failure the simulator retains the accepted samples
(`Simulator::partial_results()`). The CLI discards them by default and
writes them to `-o` only with `--keep-partial`, in which case the exit
code is still `1` and the report states where the run stopped.

## 10. Verification plan

Every claim of correctness is a test with a stated reference and
tolerance. Labels in CTest: `unit`, `analytic`, `ref`.

### 10.1 Analytic suite (the primary gate; always runs)

| circuit | quantity | reference | tolerance |
|---|---|---|---|
| resistor divider, controlled sources E F G H | `.op` voltages/currents | closed form | 1e-9 rel |
| diode + series R | `.op` current | Lambert-W solution computed in the test | 1e-7 rel at `reltol=1e-9` |
| BJT forward-active bias | Ic, Ib, Vbe | Ebers–Moll/Gummel–Poon closed form | 1e-4 rel |
| MOS level 1 | Id in triode/saturation | model equations | 1e-9 rel |
| RC step | `v(t)` | `V(1−e^{−t/RC})` | ≤ 1e-3·V at defaults; ≤ 1e-5·V at `reltol=1e-6` |
| trap order | RC error ratio on halving fixed dt | ≈ 4 (2nd order) | ratio within 5% (test-only fixed-step hook in the C++ API) |
| series RLC step, underdamped | ringing frequency, overshoot | closed form | 0.2% freq, 1e-3·V amplitude |
| undamped LC | amplitude drift over 50 periods | energy conservation | trap ≤ 1%; BE restart heuristic (§6.1) must not bleed energy faster than that |
| RC low-pass `.ac` | gain magnitude, phase | `1/√(1+(ωRC)²)`, `−atan(ωRC)` | 1e-9 rel |
| RLC `.ac` | f0, Q | closed form | 1e-9 rel |
| coupled inductors (K) | voltage ratio | `√(L2/L1)` as k→1 | 1e-3 |
| switch + RC | charge curve after closing | closed form from the closing breakpoint | 1e-3·V |
| lossless T-line, matched | step arrival, level | delay TD, V/2 | edge within dtmax, 1e-3 level |
| resistor noise | density | `√(4kTR)` | 1e-6 rel |
| RC noise | integrated output | kT/C, band to 1e4·fc (tail ≈ 6e-5) | 1e-3 rel |
| `.meas` on RC step | 10–90% rise time | `RC·ln 9` | 1e-3 rel |
| behavioral B | `.op`, and derivative | closed form; central finite difference | 1e-9; 1e-6 |
| number parser | every suffix and trap of §8.2 | table | exact |

Each row is one deck plus one assertion file; a new device or analysis
does not land without its row.

### 10.2 ngspice reference harness

`tools/spice-refcheck.ps1`: finds `ngspice` on PATH; absent → prints a
loud `SKIP (ngspice not on PATH)` and exits 77 (CTest `SKIP_RETURN_CODE`).
Present → for each deck in `tests/ref/` it generates a wrapper deck that
includes the original and appends a `.control` block (`run`, `wrdata`) —
our parser never sees `.control`, ngspice does — runs ngspice in batch
mode, runs `spice run`, and compares on a common grid:

- the grid is the deck's check list in `tests/ref/manifest.csv`; both
  results are linearly interpolated onto it, since adaptive timesteps
  differ;
- metric: `max|a−b| / max(|a|, |b|, floor)` with a per-quantity `floor`;
- tolerances per analysis class live in the manifest: `.op`/`.dc`/`.ac`
  1e-6, `.tran` 5e-3, `.noise` 1e-4 — all with identical explicit
  `.options` on both sides so defaults don't mask a difference;
- the report records the ngspice version. A disagreement where the
  analytic reference sides with us is logged in `tests/ref/KNOWN_DIFFS.md`
  and never silenced by loosening a tolerance.

Analytic results outrank ngspice agreement; ngspice is the cross-check for
circuits with no closed form (rectifiers, amplifier chains, oscillators).

### 10.3 ws::math sparse tests (owned by ws::math, driven by this project)

- Random sparse systems vs a dense reference LU: relative residual
  `‖Ax−b‖/‖b‖` ≤ 1e-10 on well-conditioned cases; condition-aware
  tolerance otherwise.
- Same-pattern refactor with changed values equals a fresh factor; a
  pivot-escalation case; structural and numerical singular detection;
  real and complex; transpose solve (`Aᵀx = e`) vs an explicit
  transposed factorization.
- **Backend cross-validation** (§4.6): the same system through KLU and
  Eigen agrees to 1e-9 rel, and the whole analytic suite is run under
  both `--solver=` settings.
- Programmer-error path: a numeric write outside the recorded pattern
  trips the §3.2 assert (death test).

### 10.4 Device and parser unit tests

- **Derivatives:** every nonlinear device's `load` Jacobian vs central
  finite differences over a bias grid (step `1e-6` scaled, tolerance 1e-5
  rel, bias points away from limiter clamps); charge/flux consistency
  checks (`dQ/dv` vs differenced Q).
- **Limiters:** `pnjlim` and FET limiting pinned against finite-difference
  behavior (§5.3).
- **Parse-error goldens:** each rejection path has a deck in
  `tests/parse_errors/` plus the expected `file:line:col: error:` prefix,
  so line numbers cannot silently drift (§1 goal).
- **Fuzz:** a byte-mutation fuzz of the parser/elaborator asserting only
  "returns an error or a valid Circuit — never crashes or hangs".

### 10.5 CI, determinism, performance

- `unit` + `analytic` must pass on every commit; `ref` runs where
  ngspice exists. ASAN/UBSAN builds run `unit`.
- A given deck + backend produces bit-identical CSV across runs
  (single-threaded, no global state, §2.1). Different backends are only
  required to agree within §10.3 tolerances.
- Performance is measured, not gated: RC-ladder decks at 10²–10⁵ nodes
  record refactor time and fill into `STATUS.md`. Any optimization needs a
  profile (house rule), and a dense fast path, AMD/BTF tuning or transpose
  reuse waits on those numbers.

## 11. Implementation plan

Starts only after sign-off on this document and `device.hpp`. Each
milestone is a sequence of small commits that stage only this project's
own paths (§0.9); a milestone is done when its gate passes, not when its
code exists.

| milestone | scope | gate |
|---|---|---|
| **M0** | `ws::math` sparse facade over **Eigen**: analyze/factorize/solve/transpose, real + complex | §10.3 sparse suite (Eigen backend) |
| **M1** | parser (§8.1–8.2, 8.3 cards so far), `.param`, expression parser (no AD yet), elaboration + `.subckt` flattening, §6.6 graph checks, error taxonomy and diagnostics (§9), devices R V I E F G H, MNA assembly + pattern dry-run, `.op`, `.dc`, CSV writer, CLI skeleton | analytic `.op`/`.dc` rows; parse-error goldens; `spice run` end to end |
| **M0b** | vendor SuiteSparse KLU (+AMD, BTF), runtime `solver=klu\|eigen`, backend cross-validation; default flips to `klu` | §10.3 cross-validation + analytic suite under both backends; licensing decision of §4.6 closed first |
| **M2** | C, L, K, sources PULSE/SIN/EXP/PWL, `.ac`, `.tran` (trap/BE, LTE control, breakpoints, `.ic`/UIC), switches | RC/RLC/LC/AC/switch rows; LTE constants pinned here (§6.3) |
| **M3** | Newton loop, per-device limiting, diode, convergence ladder, gmin/source staircase constants pinned against the ref harness (§5.2) | diode rows; ref harness runs rectifier/clipper decks within manifest tolerances |
| **M4** | BJT (Gummel–Poon), MOS level 1, behavioral B with dual-number AD (§8.5), lossless T-line | BJT/MOS/B/T-line rows; finite-difference device tests |
| **M5** | `.noise`, `.step`, `.meas` subset (§0.4), `.save`/`.print`/`.probe`, `.nodeset`, temperature (§7.7), text report | noise/`.meas` rows; step-family CSV; ref `.noise` agreement |
| **M6** | hardening: shared-library audit (no globals, no `exit`), `libs/spice/CLAUDE.md` + `STATUS.md`, perf baseline (§10.5), full ref corpus | clean sanitizer runs; two `Simulator`s interleaved in one test process |

M1 proceeds without waiting for M0b (§4.6); M0b must finish before M2.

### 11.1 Backlog (explicit, not scheduled)

BSIM3/4 and MOS levels 2/3, JFET/MESFET, lossy transmission lines,
`.lib`, `.alter`, `.tf`, `.pz`, `.four`, `.disto`, `.sens`, `.pss`,
Gear(2) and TR-BDF2, pseudo-transient continuation, index-2 DAE handling
(§6.6), hierarchical matrices (§8.4), full `.meas` grammar, `.raw` writer,
AMD/BTF tuning and transpose-factor reuse (§4.5), multithreading,
Verilog-A. Anything here is rejected at parse with a line number until it
lands.

### 11.2 Open questions for sign-off

1. **KLU licensing** (§4.6): LGPL static-link consequences for reuse in
   proprietary consumers — dynamic/separate component, or Eigen-only for
   them?
2. **AC CSV layout** (§7.4): `re()`/`im()` pairs by default, derived
   columns on request — acceptable?
3. **`.step` failure policy** (§7.6): abort by default, `--keep-going`
   opt-in.
4. **`.meas` failure exit code** (§9.5): non-fatal by default,
   `--strict-meas` opt-in.
5. **Dialect** (§8.3): ngspice-canonical with listed LTspice
   accommodations — is that the intended center of gravity?
