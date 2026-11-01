# ws::spice — STATUS

State of the arc at a glance. The contract is `DESIGN.md` (Revision 2,
2026-10-29); the rules are `CLAUDE.md`. Where this file and DESIGN.md
disagree, DESIGN.md wins and this file gets fixed.

## Where we are

M0 + M1 + M2 + **M3 (the diode)** + **M4 complete through M4e** — BJT
(M4a), junction caps (M4b), MOSFET (M4c), behavioral B source (M4d) and
the lossless T-line (M4e, this commit) — plus the 2026-10-29 post-review
revision (four structural changes + six usage wins), all committed on
`main`. M4 is now CLOSED: every device of §11's M4 row exists.
Latest recorded gate (post-M4e Early voltages + diode Rs): **2851
assertions, 154 test cases, 0 failures** (Linux preset: GCC 14, `-Wall -Wextra -Wpedantic -Werror`).
Windows parity: the MSVC `/W4 /WX` build is clean (the one M4a shadow,
C4456, fixed in `523774b`; the earlier 97+6-case Windows smoke ran
green after `70e7e84` + `b265ae7`); the Linux-side parity checks live
in `tools/audit_*.py` / `tools/build_*.py` (documented in
`docs/SPICE_PLAN.md`).

## Landed (commit order)

| Commit | What |
|---|---|
| `8373c6e` | §3.5 rev — the param/state arenas: immutable Circuit, RunState (bound values + state slices + pins), all 12 devices on the new contract, subckt-local value inlining, Rand + gauss()/flat() opcodes, ParamTable resolve cache. |
| `5f2c8e7` | §7.8 rev — columnar results: Table = axes + signal columns, `res["v(out)"]` spans, row()/rows() views, CSV byte-identical. |
| `d8a7da0` | §2.1 rev — CircuitBuilder + unit literals + Simulator (set/set_source/release, snapshot/restore, rebind) + write_netlist round-trip + equivalence tests. Found and fixed the Rser-to-ground §6.6 false positive. |
| `8ab9534` | §6 rev — TranMarch + TransientRun: advance_to, StopToken, progress, closed-loop set_source; run_tran is a loop over the march. |
| `025e91d` | §5.2 rev — the convergence ladder with PTC as stage 4 (promoted from the backlog); node diagonals in the pattern; stage in AnalysisReport. |
| `01d5c56` | §7.6/§8.5 rev — .step families (param/temp/source; lin/list/dec/oct; first card outermost) merged into family tables; Monte-Carlo with recorded seeds; keep-going. |
| `1c0145b` | §0.4 rev — measure:: library + .meas parser; failures are notes; --strict-meas. Fixed the crossing-time interpolation bug the analytic tests caught. |
| `705cfd3` | §0.5 rev — .save/.print expressions + @dev[param], post-hoc over stored columns; v(a,b) differential; axes as pseudo-columns. |
| `f9e794a` | §7.8/§9.3/§8.3 — .raw writer (complex merging for .ac); `spice check` lint (M/milli, F/femto traps); compat=ngspice\|ltspice\|strict. |
| `2126db1` | docs — DESIGN.md Revision 2, this file, CLAUDE.md. |
| `ec349ec` | regression fix — .meas over .step families now measures each point (was measuring the last). |
| `70e7e84` | MSVC /W4 /WX portability — `struct Circuit` forward decl (C4099) and six shadowing renames (C4456/C4458). |
| `b265ae7` | .dc crash on MSVC debug — `run_dc` assigned through empty `std::optional`s (`WS_TRY_ASSIGN(*state, ...)`); silent on GCC, exit-3 abort on Windows. Fixed with emplace; caught via the ASan build helper. |
| `578d821` | §3.6 current-iterate threading (`DcLoadContext::node_v/branch_i/gmin`, `AcLoadContext::state_`), `NonlinResid` + the §5.1 nonlinear-residual convergence check, the diode (Shockley + pnjlim + 3 state slots + `.model D`), `solve_tran` upgraded to a real Newton loop, `gminsteps`/`srcsteps`/`gshunt` options, `Builder::diode()`/`diode_model()`. Gate: Lambert-W .op (1e-7 rel), .dc sweep, pnjlim stiff-bias convergence, rectifier .tran sanity, Jacobian vs central finite differences, builder .op equivalence. |
| `a756afe` | M4a — the BJT (Ebers-Moll transport model): direct terminal-current stamp (Ic/Ib/Ie + derivatives into G, Norton offsets into rhs), two pnjlim slots (Vbe/Vbc), `.model NPN\|PNP(Is= Bf= Br= Nf= Nr=)`, NPN+PNP via sgn (G entries polarity-independent, RHS flips), `Builder::bjt()`/`bjt_model()`. Gate: forward-active .op (1e-4 rel vs Ebers-Moll), cutoff, saturation, .dc Vbe sweep (exp I-V), stiff cold-start convergence, PNP polarity. |
| `3644e21` | M4b — the diode gains Cjo junction capacitance, the first nonlinear charge model: SPICE3 depletion Q/C + the Fc·Vj linear extension, trap/BE Norton (geq/jeq) frozen at the last committed V, LTE on the charge history, seeded from the OP; state 3 → 16 slots when Cjo > 0 (`is_dynamic` flips; Cjo = 0 keeps the M3 diode exactly), `.ac` adds j·ω·Cj(V_op). `.model D(Is= N= Cjo= M= Vj= Fc= Tnom=)`. Gate: Cjo .ac RC cutoff (fc ≈ 16 MHz, 0.707 at fc). |
| `48e050d` | M4b-part-2 — BJT junction caps (Cje/Cjc/Mje/Mjc/Vje/Vjc/Fc): the same charge companion per junction (shared `q_dep_jn`/`c_dep_jn` helpers), state 4 → 30 slots; `.op` unchanged by caps — verified against ngspice 39 to 5 sig figs. |
| `2a94c32` | 13 example decks (M3 diode + M4a BJT, `examples/README.md`) + the pnjlim false-convergence fix: `update_state` returns true while the limiter actively clamps (Vd_lim/Vbe_lim/Vbc_lim ≠ raw) — a stiff cold start no longer declares convergence at a stale linearization point (device current below abstol). |
| `117d647` | M4c — the MOSFET (level 1 Shichman-Hodges + body effect): triode/saturation Id with (1+λ·Vds), `M name d g s b model [L= W=]`, `.model NMOS\|PMOS(Vto= Kp= Lambda= Gamma= Phi=)`; no fetlim by design (quadratic, not exponential), no Meyer caps yet. 3 mos_* decks; ngspice 39 exact (Id = 11.0 µA). **No §10.1 analytic test row yet** (known gap). |
| `523774b` | MSVC C4456 — shadowed `area` in BJT `load_ac`. |
| `4514d91` | M4c gap closed — the MOSFET §10.1 analytic rows (triode/saturation .op, the Id(Vds) knee .dc, cutoff), which caught the PMOS Norton-offset double negation: every PMOS operating point was wrong (NMOS + the ngspice cross-check never noticed — the fix is one sign, the rows are what made it visible). |
| `3a675b1` | M4d — the behavioral B source (`B n+ n- V=expr \| I=expr`): §8.5 expressions over `v(node)`/`v(a,b)`/`i(Vname)`/`time`/.param names, linearized per Newton iteration by **dual-number AD** (one value pass + one directional pass per signal reference, fixed-size stack arrays — 64 vars/32 refs/256 stack caps that reject loudly at elaboration); the Norton stamp (Jacobian entries at the referenced unknowns, remainder in rhs); domain failures (sqrt/ln/log/pow out of domain, /0, non-finite) as **Convergence errors through the new `Device::load_error` channel** — the ladder aborts immediately, no homotopy rescues an out-of-domain expression; kink slopes nudged 1e-12 inside the domain (the pnjlim spirit: clamp the linearization, never the value); `.ac` = the gradient at the frozen OP; `update_state` gained a `params` tail, `set_control_branch` a `which` index; 3 b_* example decks. Gate: 2x multiplier .dc (exact), exp-as-diode .op vs Lambert-W, tanh soft-limiter .tran over `time`, domain-error goldens, AD Jacobian vs central finite differences. |
| (this commit) | M4e — the lossless T-line (`T in+ in- out+ out- Z0= TD=`, or `F=`/`NL=` with TD = NL/F; `IC=` takes 1–4 values, UIC): the method of characteristics over two port-current branch rows — Thevenin companions `v_k = Z0*i_k + E_k` with `E1`/`E2` the delayed waves the OTHER port launched; DC/OP is the wire pair (an ideal 1:1 transformer over the same rows — the constraint edges (in+,out+)/(in-,out-) make parallel lines and V-bridged pairs the §6.6 findings); `.ac` is the lossless two-port `V1 - Z0*I1 = (V2 + Z0*I2)e^{-j*w*TD}` (omega==0 falls back to the wire rows, and the AC pattern dry-run now stamps at a nominal omega=1 so the richest shape is recorded, §3.2); the history is a 2048-sample (t, W1, W2) ring in the state arena, pruned to the pickup bracket, seeded with the t=0 state over [-TD, 0], picked up by LINEAR interpolation (exact on legs, no quadratic corner overshoot — ngspice's 3-point Lagrange bends across a vertex); **delay crossings register breakpoints**: `next_event` grew the §3.5 state-slice parameter and returns the earliest sample+TD, capping dt at TD (the pickup never extrapolates) and landing every delayed kink on a grid point + BE restart; ring overflow is the loud `spice.timestep.tline_history` Timestep error through the load_error channel (a one-shot device-side latch — the one run-evolution datum outside the arena, because load_error has no state access); `Builder::tline()` + 4 t_* example decks. Gate: matched ramp delayed exactly (1e-9), open-end reflection ladder closed form, quarter/half-wave `.ac`, `.op`/`.dc` wire, UIC+IC seed, the overflow golden, the netlist round-trip, `.step` Z0 rebind, builder-vs-netlist equivalence. Also fixed: `device_shapes()` was missing the M4d B source (its node tokens never port-mapped inside subckts). |
| (this commit) | BJT Early voltages (Vaf/Var — the first deferred-device-physics pull-in): the SPICE3 base-charge form without roll-off, oef = 1 - Vbc/Vaf - Vbe/Var multiplying the TRANSPORT current only (the base diode currents stay unscaled, as in SPICE3); the derivatives pick up the go = Ic/Vaf output conductance and its Vbe-side mirror (dic_dvc = gm_r*oef + T/Vaf etc.), polarity-independent like every other G entry. Vaf/Var = 0 (the default) disables each term — oef == 1.0 exactly, the stamp is byte-identical to M4a. update_state's residual tracker multiplies by the same oef at the clamped point; load_ac stamps the Early derivatives at the frozen OP. Negative Vaf/Var reject loudly (they flip go's sign). Builder BjtModel grew optional vaf/var. Gate: the .dc output characteristic is the exact Early ramp (12 digits), Var scales the forward Ic by (1 - Vbe/Var), saturation combines both terms exactly, .ac exposes go = Ic/Vaf, Vaf=0 is the M4a answer, negatives reject, builder-vs-card equivalence. bjt_early.cir example. |
| (this commit) | Diode Rs (series resistance — the second deferred-physics pull-in): the junction moves to an internal 'name#mid' node (SPICE3's topology, the L Rser precedent) — anode --[Rs/area]-- mid --[Shockley + Cjo companion]-- cathode. Device::apply_model gained a NodeResolver& parameter (the model, hence Rs, resolves only after every factory has run; resolve_models sits before the §6.6 graph checks, so the claimed node joins them — the signature is the single drift-free way to give a model an internal node). Rs = 0 (the default) creates NO node and keeps the M3 stamp byte-identical; Rs scales as Rs/area (SPICE3); negative rejects loudly. Gate: the .op is the Lambert-W form with R+Rs in the loop (1e-7), v(D1#mid) carries the true junction voltage, .dc tracks Lambert-W per point with area scaling, .tran through the Rs×Cjo pole settles at the same OP (tail-mean over the trap ring), .ac is Rs + 1/(gd + j*w*Cj) at the frozen OP, the netlist round-trip re-emits Rs, rejections are loud. diode_rs.cir example. |

## Known sharp edges

- **PTC rescue cases now exercise the diode** — the §5.2 ladder's stage
  4 (PTC) was promoted from the backlog in the 2026-10-29 revision but
  had no nonlinear device to rescue; M3's diode + pnjlim is what makes
  the ladder load-bearing. The stiff-bias test (diode at 100V) confirms
  plain Newton + pnjlim converges without needing gmin/source/PTC.
- **The diode's Vt is frozen at Tnom** (M3): `.options temp` does NOT
  re-evaluate the diode's thermal voltage (temp-dependent models are
  M5). The Lambert-W gate uses the default 27°C; `.dc temp` over a
  diode circuit will give wrong currents at non-27°C points. M5 fixes
  this by threading `temp` through the load context.
- **B `write_card` cannot round-trip .param-referencing expressions**
  (M4d): a behavioral source whose expression names a `.param` fails
  the netlist writer naming the device (no AST rewrite yet); plain
  expressions round-trip fine. The B source otherwise participates in
  write_netlist like any device.
- **Deferred device physics** (accepted-but-not-implemented model keys,
  so ngspice decks parse): the MOS Meyer caps (Cgs/Cgd/Cgb) and the
  diode/BJT diffusion charge (TT/Tf/Tr). The BJT Early voltages (Vaf/Var)
  and the diode Rs have landed. The rest queue behind M5
  (temp/breakdown/noise) unless pulled earlier.
- **PTC still has no load-bearing case** — the §5.2 ladder's stage 4
  was promoted in the 2026-10-29 revision, and the BJT/MOSFET have
  since arrived, but every test converges under plain Newton + pnjlim
  (gmin/source/PTC never engage); the genuinely multi-stable circuit
  that NEEDS stage 4 remains to be found.
- **`update_state` uses `is_` (area=1) for the residual I** — a minor
  approximation when `area≠1`; the voltage-delta check compensates, and
  at convergence Vd stabilizes so I stabilizes regardless. The Lambert-W
  gate uses area=1.
- **AC expression save-lists reject loudly** (vm()/vp() derived forms
  are the §7.4 follow-up), and .ac tables carry re()/im() pairs.
- **`.dc temp` / `.step temp` on a Simulator-created circuit** is a
  Usage error naming the limitation (temp re-elaborates from the deck;
  a built circuit has none).
- **-Wmissing-field-initializers is off project-wide**: designated
  initializers with defaulted tails are the intended aggregate API
  spelling (`Tran{.tstop = 5_ms}`); GCC flags exactly that.
- **Windows-only breaks stay possible**: GCC's flag set is not MSVC's
  (`-Wshadow` off, no /RTCu, no checked iterators). Run the five
  `tools/audit_*` / `tools/build_*` parity checks before handing a
  build to Windows — that is exactly how `70e7e84`/`b265ae7` were
  found and will be how the next one is.

- **The T-line publishes no LTE of its own** (M4e, deliberate per
  §6.2/§6.3): the companion is exact between delayed vertices (the
  delay-crossing breakpoints land them), and the line integrates no
  charge/flux. The gap: a deck driving a line from a SMOOTH source with
  no dynamic device anywhere (B+R+T only) samples at dtmax — the
  pickup interpolates a coarse chord of the waveform. ngspice's
  tratrunc slope-change check covers that case reactively; pull it in
  only if a real deck needs it (it false-rejects at corners, which is
  why it was not ported).
- **The T-line history ring is fixed at 2048 samples** (M4e): more
  than 2048 accepted steps inside one delay window is the loud
  `spice.timestep.tline_history` error naming the deck fixes (reduce
  TD, coarsen edges, loosen tolerances) — never a silent overwrite.
  Long-delay + fast-ringing decks can legitimately hit it.
- **A .tran whose early steps jump a diode past vcrit needs itl4 > 10**
  (found landing diode Rs): the pnjlim walk costs ~Vd/(2*Vt) Newton
  iterations and the transient's itl4 is SPICE's default 10 - the same
  cold-start story the .op ladder handles with itl1=100. Slow the ramp
  or raise itl4; a stiff first step is the deck's choice, not a solver
  bug. Relatedly, a stiff Rs x Cjo pole under dt >> tau carries the
  classic trapezoidal ring (inside trtol-LTE, so the 6.1 BE damping
  never fires - ngspice rings identically); average a tail or cap tmax
  when the exact settle matters.
- **T-line UIC without IC=** seeds the waves from the UIC vector (v
  from .ic nodes, i = 0) where ngspice zeroes them — the C/L ic=
  precedent ("the deck's choice"); like every cold UIC start, never
  write an order-accuracy test on it.

## Next (per §11)

M4 is closed: the MOSFET §10.1 rows landed in `4514d91`, M4d (the
behavioral B source) in `3a675b1`, M4e (the lossless T-line) in this
commit. Next: the rest of the deferred device physics (Meyer caps, TT/Tf/Tr,
diode Rs) or **M5** (`.noise`/`.nodeset`, temp threading through the
load context — the diode's frozen Vt), with M0b (the KLU backend
decision) and M6 hardening after.
