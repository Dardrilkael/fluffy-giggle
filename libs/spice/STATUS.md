# ws::spice — STATUS

State of the arc at a glance. The contract is `DESIGN.md` (Revision 2,
2026-10-29); the rules are `CLAUDE.md`. Where this file and DESIGN.md
disagree, DESIGN.md wins and this file gets fixed.

## Where we are

M0 + M1 + M2 + M3 + M4 (CLOSED) + the 2026-10-29 post-review revision +
**M5** (temp M5a, .nodeset M5b, .noise M5c, Meyer caps M5d) + **the
BSIM6-style MOS core** (M6, LEVEL=77) — all committed on `main`.
Latest recorded gate: **3165 assertions, 178 test cases, 0 failures**
(Linux preset: GCC 14, `-Wall -Wextra -Wpedantic -Werror`).
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
| (this commit) | Diode Rs (series resistance — the second deferred-device-physics pull-in): the junction moves to an internal 'name#mid' node (SPICE3's topology, the L Rser precedent) — anode --[Rs/area]-- mid --[Shockley + Cjo companion]-- cathode. Device::apply_model gained a NodeResolver& parameter (the model, hence Rs, resolves only after every factory has run; resolve_models sits before the §6.6 graph checks, so the claimed node joins them — the signature is the single drift-free way to give a model an internal node). Rs = 0 (the default) creates NO node and keeps the M3 stamp byte-identical; Rs scales as Rs/area (SPICE3); negative rejects loudly. Gate: the .op is the Lambert-W form with R+Rs in the loop (1e-7), v(D1#mid) carries the true junction voltage, .dc tracks Lambert-W per point with area scaling, .tran through the Rs×Cjo pole settles at the same OP (tail-mean over the trap ring), .ac is Rs + 1/(gd + j*w*Cj) at the frozen OP, the netlist round-trip re-emits Rs, rejections are loud. diode_rs.cir example. |
| `3590c82` | M5a — temperature threading (§7.7): temp_k() on both load contexts + the update_state/terminal_current tails; physconst.hpp (CODATA 2018 k/q, the SPICE3 variable Eg(T), rc_temp_factor); the diode's frozen-Vt sharp edge is GONE (Is(T)/Vt(T)/vcrit(T) via diotemp.c tlev=0, Xti/Eg/Tnom implemented); the BJT factlog + XTB beta scaling; the MOS level-1 pbfact chain (Phi(T)/Vto(T)/Kp(T)); R/C gain TC1/TC2 (instances + `.model name R(...)`, ngspice's `tc=a,b` spelling); per-instance temp= everywhere; `.options tnom=` patched into models lacking one. T == Tnom is bit-identical everywhere. Gate: 6 §10.1 rows incl. the diode .dc temp sweep (1e-12) and the MOS pbfact chain vs ngspice 44.2 (9 significant figures). |
| `1949f7a` | M5b — .nodeset (§7.2): v(node)=expr cards seed the cold-start iterate and stamp a soft g=1 Norton on iteration 1 of EVERY ladder stage (released after — unlike .ic's stiff hold). `.dc` hints the first point only; .ac/.tran OPs honor it. Gate: the bistable cubic-behind-a-load-line deck — a hint picks the basin, both satisfy KCL to 1e-9. |
| `824d54e` | M5c — .noise (§7.5): the adjoint analysis. NoiseContext reports per-device current PSDs folded with one adjoint solve per frequency (A^T x = e_out via ComplexSparseLu::solve_transpose); the forward gain carries only the named input (ngspice's MODEACNOISE). Sources: R thermal + carbon flicker, diode _rs/_id/_1overf, BJT _ic/_ib/_fn, MOS _thermal(2/3·gm)/_1overf. Integrated totals via the ninteg.c power-law segments; spectra + per-device summary tables. ALSO fixed a REAL gap the noise tests caught: the MOSFET stamped NOTHING into .ac (any .ac deck carrying one saw an open channel) — channel_eval() now shared by load_dc/load_ac/load_noise; state grows kVbsPrev (5 slots). Gate: kT/C within 3e-4 (ngspice 44.2 lands on the same 2.035120e-6), the diode shot through the loaded impedance (1e-9 rel), the MOS channel row, 4 rejection goldens. |
| (this commit) | M5d — the MOS Meyer gate caps (§6.2, SPICE3 DEVqmeyer): TOX/CGSO/CGDO/CGBO land on the level-1 model. meyer_half is DEVqmeyer verbatim (half-caps; the caller doubles and adds the overlaps — mos1load.c's `2*state0+overlap` small-signal form, pinned against the ngspice source); gate_caps adds the reverse-mode (vds<0) source/drain swap and the body-effected von. The state grows 42 slots when caps exist (a (V,Q,I,geq,jeq) block + a 3×(t,q) LTE history per terminal pair, MIRROR-frame pair voltages vgs/vgd/vgb — vgb = vgs − vbs); load_tran stamps the three charge Nortons (jeq flips with sgn); load_ac stamps j*ω*C at the frozen OP; the tran charge evolves q += (v_new − v_old)·C(v_new) (SPICE3's mos1load form); the t=0 seed is the TRANOP q = v·C (the doubled caps + overlaps). The caps are dynamic-only (TOX or an overlap present; a bare model keeps the M4c stamp byte-identical). Gate: the §10.1 Meyer rows — the .ac gate-cap table in cutoff/triode/saturation/weak-inversion (the drive-all-but-one-terminal extraction, 1e-9), the PMOS mirror row, the cutoff-region gate RC step vs the closed form, the overlaps-only (no TOX) row. mos_ac_meyer.cir + mos_tran_meyer.cir examples. |
| (this commit) | M6 — **the BSIM6-style MOS core (LEVEL=77)**: a surface-potential, charge-based channel built from the exact physics in the new src-only `bsim6_core.hpp` (~460 lines, every equation gated). LEVEL=77 follows Xyce ("MOSFET (Level 77) BSIM6") and Simetrix (BSIM-BULK: LEVEL=77, VERSION=106) — ngspice ships no BSIM6 (verified against its NEWS/DEVICES; modern compact models arrive there via OSDI), and 77 is free in every ngspice level table. The core: the exact space-charge PSI(psi,Vch) (both carriers, Poisson+Boltzmann), psi solved per channel node by damped Newton + analytic seeds + bisection fallback (residual 1.4e-14 over the full bias grid incl. accumulation), dpsi by implicit differentiation; the charge-sheet split qi/qb/qg (smooth in depletion AND strong inversion, qi smoothly clamped at 0 for the accumulation/forward-body corners where the surface density dips under bulk equilibrium); Id = mueff(W/L)·∫qi dVch by 16-point GL with an exponential-concentrating substitution when the channel decays (subthreshold integrands of ~50 e-foldings become degree-≤31 polynomials — GL16-exact; a linear map was 2× off at 1e-22 A); velocity saturation via the implicit Vdsat boundary (G_sat = Id_cs(V) − vsat·W·qi(Vsb+V) = 0, scale-RELATIVE Newton tolerance — an absolute one stops percent-early at pA) + the BSIM4-style Vdseff smoothing (DELTA=0.02); the terminal charges as channel-charge-weighted integrals with the smooth chi partition (50/50 linear, 0/100 deep sat); EVERY derivative analytic (the master (Vgb',Vsb,Vds) partials scatter to node voltages; a dqi sign slip that doubled two cancelling cox-scale terms in subthreshold was caught by the FD gate at pA currents). Params: TOX, EPSROX, NDEP (cm⁻³), VTH0 (= the Vgb at which psi_s = 2*phif — Vfb backs out exactly), U0, UA/UB (SI semantics — BSIM deck values do NOT transfer), VSAT (≤0 disables the layer), RDSW (half per side onto internal 'name#d'/'name#s' nodes, the diode-Rs topology), CGSO/CGDO/CGBO, TNOM; temp via ni(T)/phif(T)/gamma(T)/Vfb(T). The device: the M card dispatches on level; PMOS by the mirror frame; REVERSE drain operation by an S/D-swap wrapper (the core is symmetric, the Vdseff layer is not); .tran companions per intrinsic pair (g-s', g-d', g-b) with state-FUNCTION charges (conservation exact — no Meyer-style incremental tracking), the overlaps folded into the same pair charges; .ac stamps the full 3×4 transcapacitance matrix (row g sums to +jω·dQg/dv — conservation) + the overlaps; a frozen-Vdsat state slot keeps begin_step/commit on one linearization point (a mid-step refresh desynced the charges from the solved companion — a constant spurious gate current was the symptom); psi/Vdsat failures are loud Convergence errors through load_error (the T-line latch precedent). Gate: 7 rows — the .dc Id vs an INDEPENDENT Pao-Sah double integral computed in the test (3%: the charge-sheet gap, 0.6% strong→2.5% deep subthreshold, pinned by tools/bsim6_core_check.cpp: psi residual 1.4e-14, FD partials 1.5e-6, quadrature 1e-8 vs Simpson, continuity, swing 68.0 vs 67.7 mV/dec, the vsat bound); threshold/subthreshold/swing; vsat flatness; PMOS mirror + S/D swap symmetry + zero gate DC; RDSW exact resistor drops on #d/#s; .tran gate hold + capacitive .ac; 3 rejection goldens (Level=49, unknown param, Tox≤0). mos_b6_iv.cir + mos_b6_tran.cir examples. |

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
  so ngspice decks parse): the diode/BJT diffusion charge (TT/Tf/Tr) and
  the breakdown pair (Bv/Ibv). The BJT Early voltages (Vaf/Var), the
  diode Rs and the MOS Meyer caps have landed. The rest queue behind M6
  (diffusion charge) unless pulled earlier.
- **The Meyer charge increment evaluates C at the NEW bias** (q += Δv·C(v_new))
  — the diode/BJT companion convention. ngspice's mos1load instead
  averages the DEVqmeyer halves across the step endpoints
  (`state0+state1+overlap`). Identical within a region (C constant);
  both O(h) at a boundary crossing; our form matches the Cjo/Cje
  companions and the analytic within-region gates exactly. Documented
  deviation, same spirit as the T-line UIC seed.
- **Meyer caps are not charge-conserving under large swings** (the
  classic Meyer-model property; the incremental q tracking IS the
  SPICE3 semantics we implement). The charge-conserving core is what
  level 77 brings (its pair charges are state functions of the
  terminal voltages - conservation exact by construction).
- **The BSIM6 core is a documented SUBSET** (level 77): the exact
  surface-potential charge-sheet channel + mobility/vsat/Rds/charges, but
  NO CLM (gds → 0 in deep saturation, like level-1 with Lambda=0), no
  noise sources, no body diodes, and the BSIM4/6 second-order tail
  (K1/K2, DVT*, CDSC, PCLM/PDIBL*, NFACTOR, geometry shrinks, ...) is
  ACCEPTED but ignored so production decks parse - the diode-TT
  precedent. UA/UB carry SI semantics (m/V on Eeff in V/m): BSIM deck
  values do NOT transfer. The Vdseff smoothing (DELTA=0.02) trades a
  ~2% triode-slope compression and a sqrt(DELTA) knee softening for
  C-infinity continuity. The chi partition can push Cgd slightly below
  the overlap-only value near the knee (the classic 50/50-class
  artifact); the Ward-Dutton moment is the follow-up.
- **The b6 core costs ~7 channel sweeps per load** (the final integral +
  the Vdsat Newton's ~4 + the partials at the root): fine for the gate
  decks, the obvious v2 optimization is caching psi(Vch)/Vdsat in the
  state slice across Newton iterations (the seeds already warm-start
  within a sweep).
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

M5 is closed (temp, .nodeset, .noise, Meyer caps) and the BSIM6-style core
(level 77) has landed with its physics gates. Next: the diffusion-charge
pair (diode TT, BJT Tf/Tr — the famous reverse-recovery gate), the b6
follow-ups (CLM/PCLM output conductance, the Ward-Dutton partition
moment, noise sources, the psi/Vdsat state caching), with M0b (the KLU
backend decision) and M6 hardening after.
