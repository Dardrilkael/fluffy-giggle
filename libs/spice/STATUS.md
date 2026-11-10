# ws::spice — STATUS

State of the arc at a glance. The contract is `DESIGN.md` (Revision 2,
2026-10-29); the rules are `CLAUDE.md`. Where this file and DESIGN.md
disagree, DESIGN.md wins and this file gets fixed.

## Where we are

M0 + M1 + M2 + M3 + M4 (CLOSED) + the 2026-10-29 post-review revision +
**M5** (temp M5a, .nodeset M5b, .noise M5c, Meyer caps M5d, diode TT M5e)
+ **the BSIM6-style MOS core** (M6, LEVEL=77) + **the BJT diffusion
charges** (M7, Tf/Tr) + **§7.9 — `.four`** — all committed on `main`.
Latest recorded gate: **12808 assertions, 201 test cases, 0 failures**
(Linux preset: GCC 14, `-Wall -Wextra -Wpedantic -Werror`; the .four arc
added 7 cases / 76 assertions; test_circsim 145/1993 green).
Windows parity: the MSVC `/W4 /WX` build is clean (the one M4a shadow,
C4456, fixed in `523774b`; the earlier 97+6-case Windows smoke ran
green after `70e7e84` + `b265ae7`); the Linux-side parity checks live
in `tools/audit_*.py` / `tools/build_*.py` (documented in
`docs/SPICE_PLAN.md`).

## Landed (commit order)

| Commit | What |
|---|---|
| (this commit) | §7.9 — `.four FREQ [NHARMS] sig...`: the fourier:: library (the trapezoidal DFT at the run's last whole fundamental period's exact harmonic bins, on the adaptive grid; whole-run window when shorter, periods 0) + the card riding the .meas machinery (the family_parts split with @k suffixes, notes never failures, `.op` answering the DC component alone, no table a note); `RunOutput::fouriers` printed by both CLIs and carried by `results_json`; the Results panel's Fourier table (.tran/.op views only); `four_two_tone.cir` + `fourier.csim` through the smoke. |
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
| (this commit) | M5e — **the diode transit-time diffusion charge (TT)**: Q_tt = TT·(Id + Is) (the stored minority charge; the −Is generation component is not stored), C_tt = TT·gd — both scale with the bias, so the .ac response poles at exactly 1/(2π·TT), bias-free (the §10.1 row checks every .ac point against the closed form). TT>0 flips the diode dynamic even with Cjo=0 (state 3 → 18 slots: the folded Is(T)·area / n·Vt(T) mirrors evaluate at the INSTANCE temp — load_temp_k, so the companion can never disagree with the loads). Two companion correctness fixes the recovery deck forced: (1) the BE branch now uses the consistent tangent jeq = −geq·V_comp (the trap branch always had it; the old −Q/dt origin form is exact only for q = C·v — for the exponential diffusion charge Q = Vt·C, a factor 27 off at 0.7 V, and it dumped amperes into BE restarts); (2) the reverse-recovery SNAP: when the stored charge empties, C_tt collapses while the terminal current stays mA — dv/dt → ∞, a true discontinuity. The frozen companion's tangent crosses the charge's zero there; committing its fiction current rang the trap memory forever (a 2-cycle pinned to rail ± I·R, observed). Fix: the snap guard in commit (tangent-implied TT charge ≤ 0 → commit the honest rate (q_new − Q)/dt) + the phys::kChargeFloor cutoff (the SPICE3 chgtol, 1e-14 C: below it the TT companion goes fully silent — no geq, no memory — and the circuit pins the node algebraically). Negative TT rejects loudly; TT rides the raw model tokens through the netlist round-trip. Gate: the .ac pole (1e-6 rel, every row), the .tran reverse recovery (the Lambert-W OP, the storage window, ts ≈ TT·ln(1+If/Ir) ≈ 2.8 ns, the snap to the rail, i → gmin), rejection + round-trip. ALSO fixed: three tests read `t.signals[0]` as the time base — signals[] holds only the saved columns (v(src) is first in the M5e deck, v(gin) in the M5d/M6 decks), so the M5d per-point RC loop and the M6 gate-hold loop were DEAD CODE (the wrong "time" never entered their windows); all three now use t.axis() and the loops are live (the assertion count nearly tripled). diode_tt_ac.cir + diode_tt_recovery.cir examples. |
| (this commit) | M7 — **the BJT transit-time diffusion charges (Tf/Tr)**: bjtload.c's form exactly — Q_be = Tf·If·oef at Vbe>0 (SPICE3 divides the forward diode current by the base charge qb = 1/oef: the stored charge tracks the Early base-width modulation, and the charge carries a dQ/dVbc cross term — geqcb), plain Tf·If at Vbe≤0; Q_bc = Tr·Ir with no base-charge scaling (SPICE3 likewise). The charges fold into the M4b junction companions (state 4→34 when dynamic: the cross-conductance slot + the folded Is(T)·area / Nf·Vt(T) / Nr·Vt(T) mirrors, the diode-TT fold precedent); the diode-TT companion machinery carries over per junction (the chgtol charge floor silences a numerically dead diffusion charge, the snap guard commits the honest rate when the tangent-implied charge crosses the floor, the consistent −geq·V BE tangent — which also retires the M4b BE-branch −Q/dt debt). .tran stamps the per-junction companions + the Early cross (a VCCS-shaped geqcb between the (b,e) and (b,c) pairs); .ac stamps the full transcapacitance matrix (jω·∂Q/∂v, the level-77 precedent — ngspice's bjtacld.c stamps only the scalar capbe, identical whenever Vaf=Var=0 where the cross is exactly 0). Negative Tf/Tr reject loudly; Xtf/Itf/Vtf/Ptf ride the accepted-but-ignored tail. ALSO fixed a REAL M4a bug the current-forced-base gates caught: the BJT's Norton offsets evaluated at the RAW node voltages while the linearization sat at the pnjlim-clamped point — a current-biased base (I1 into the base) cold-starts at |v_b| ~ source/gmin ~ 1e8 V and the raw-referenced offset pins it there forever (the whole §5.2 ladder fails; every M4a deck was voltage-driven, which hid it). The offsets now evaluate at a node triple consistent with the clamped junction voltages (the diode's vd_lim precedent, shift-invariant by the zero row sums, byte-identical when no clamp is active). Gate: the fT pole (every .ac point vs 1/(gπ+jωTf·gm_f), 1e-6), the Early cross cap (Vaf=50: the oef scaling + the SUBTRACTING cross, 1e-6), the saturation .ac (both diffusion caps live at a two-junction-forward OP), the .tran forward-active storage (the charge-control exponential + the ts = Tf·Bf·ln(1+IB1/IR) snap at 1 ns dtmax, the post-snap silence), the saturated-switch storage knee vs τS = (Tf+Tr)/(1/Bf+1/Br) charge control + the Tr=0 twin collapsing the knee 20×, the temp fold at 127 °C (a companion folded at 27 °C would miss the im column by 33%), rejections + the round-trip. bjt_tf_ac.cir + bjt_sat_switch.cir examples. |
| (this commit) | M8 — **the b6 CLM output conductance (PCLM)**: the saturation ro layer, closing the v1 "gds→0 in deep saturation" gap (every analog gain was infinite). A Litovski log-law drain-end shortening dL = (lc/PCLM)·Vdsat·ln(1+Vxs/Vdsat), lc = sqrt(3·Tox·XJ) (XJ now a param, default 0.15 µm the BSIM default; PCLM ≤ 0 = the layer OFF, the VSAT precedent — v1 decks byte-identical), Vxs = s·softplus((Vds−Vdsat)/s), s = DELTA·Vdsat (the softplus tail keeps the triode untouched to e^{−8} at 0.1 V under the knee). The BSIM4 FOLD shape: the shortened channel carries more current at the same saturation window, so Vdseff solves the implicit I1(Vsb,Vdseff) = I1(Vsb,Vdseff_smooth)·L/(L−dL) — bracketed Newton + bisection + IFT partials, the G_sat pattern verbatim — and the ONE window variable keeps driving Id and the charges alike (conservation by construction; .ac/.tran/the frozen-Vdsat companions unchanged). Vdsat is the pinch region's voltage scale (the BSIM litlk analog; the exact BSIM4 litlk algebra lives on Vgsteff/Abulk/esat, which a surface-potential core does not have — this is house physics in the PCLM spirit, documented equation-by-equation in the header). Absurd PCLM (< ~0.05) degrades gracefully (Vdseff pinned to Vds / the 0.9L clamp), never a failure; the layer rides the vsat boundary so VSAT ≤ 0 makes PCLM inert (a documented sharp edge). Deep saturation: gds → Id·(lc/PCLM)·Vdsat/(L·Vds). ALSO fixed a REAL M6 quadrature bug the CLM degradation branch exposed: channel_sweep substituted the exponential-concentrating mapping over the WHOLE decaying window, but a past-pinchoff window is FLAT then decaying — the flat prefix mapped onto a 1/(1−s)-type kernel GL16 cannot integrate (up to 6e-3 error, NON-MONOTONE in the window length; never gated because the bias grids' saturation windows ride the vsat layer while subthreshold — where the substitution IS exact — was gated instead). Fix: SPLIT the window at the c where qi crosses 0.05·qi_a (12 bisections) — plain GL16 over the flat prefix (machine-exact, 1e-15 vs Simpson), the substitution only over the exponential tail — pinned 4.6e-9 vs Simpson over the full bias range and monotone to solver dust (the new long-window gate). Gate: 6 rows — the tail vs the Litovski law with Vdsat from the test's OWN G_sat bisection (the differential PCLM=1 − PCLM=1e9 isolation, 3%), the knee monotone + the triode byte-clean (1e-6), Pclm→∞ recovers v1 + the VSAT=0 inertness, the drain-driven .ac re(i(Vd)) vs the .dc FD gds (1e-3), the .tran settled current on the fold (1e-3) + measurably above v1, rejections (Pclm<0, Xj≤0) + the round-trip. tools/bsim6_core_check.cpp grows 6 sections (CLM FD partials 8.9e-6 over a deep-sat grid, the tail law, PCLM→∞, the frozen-Vdsat partials 1.6e-8, the absurd-PCLM monotonicity, the long-window gate). mos_b6_clm.cir example. |

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
  so ngspice decks parse): the BJT breakdown pair (Bv/Ibv is diode-
  side) and the parasitic resistances (Rb/Rc/Re); the BJT diffusion
  charge (Tf/Tr), the BJT Early voltages (Vaf/Var), the diode Rs, the
  MOS Meyer caps and the diode TT diffusion charge have landed.
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
  NO noise sources, no body diodes, and the BSIM4/6 second-order tail
  (K1/K2, DVT*, CDSC, PDIBL*, NFACTOR, geometry shrinks, ...) is
  ACCEPTED but ignored so production decks parse - the diode-TT
  precedent. PCLM is implemented (M8: the CLM output conductance; PCLM
  ≤ 0 off, and the layer needs the vsat boundary so VSAT ≤ 0 leaves it
  inert; XJ feeds only lc = sqrt(3·tox·xj)). UA/UB carry SI semantics
  (m/V on Eeff in V/m): BSIM deck values do NOT transfer. The Vdseff
  smoothing (DELTA=0.02) trades a ~2% triode-slope compression and a
  sqrt(DELTA) knee softening for C-infinity continuity; the CLM
  turn-on is the sharper DELTA·Vdsat scale (the triode tail is
  e^{-8} at 0.1 V under the knee). The chi partition can push Cgd
  slightly below the overlap-only value near the knee (the classic
  50/50-class artifact); the Ward-Dutton moment is the follow-up.
- **The b6 core costs ~7 channel sweeps per load** (the final integral +
  the Vdsat Newton's ~4 + the partials at the root), ~4 more when PCLM
  is on (the fold's own Newton + the smoothed-window sweep), and a
  decaying-window sweep now carries 12 crossing-bisection node evals
  (the M8 split; the seeds warm-start within a sweep): fine for the
  gate decks, the obvious v2 optimization is caching psi(Vch)/Vdsat in
  the state slice across Newton iterations.
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
- **The frozen companion's tangent lags an exponential tail at coarse
  dtmax** (M7, the house architecture's known trade): begin_step
  freezes the charge companion at the last committed V, and on a
  diffusion-charge tail dv/dt grows as 1/I (the junction voltage is
  logarithmic in the charge), so at a coarse dtmax single steps move
  vbe past Vt where the tangent leaves the curve. The charge LTE
  cannot see it — the committed q(t) trajectory stays smooth (v is
  log in q), so the knee/crossing lands a few % late (observed 15% at
  dtmax = 20 ns on the Tf·Bf = 2 µs tail, 0.2% at 1 ns). dtmax is
  the accuracy knob — the Rs×Cjo ring precedent; ask for a finer
  tstep when the exact tail timing matters.
- **A .tran whose early steps jump a diode past vcrit needs itl4 > 10**
  (found landing diode Rs): the pnjlim walk costs ~Vd/(2*Vt) Newton
  iterations and the transient's itl4 is SPICE's default 10 - the same
  cold-start story the .op ladder handles with itl1=100. Slow the ramp
  or raise itl4 (the M7 sat-switch deck carries itl4=100 for its 1 ns
  full swing); a stiff first step is the deck's choice, not a solver
  bug. Relatedly, a stiff Rs x Cjo pole under dt >> tau carries the
  classic trapezoidal ring (inside trtol-LTE, so the 6.1 BE damping
  never fires - ngspice rings identically); average a tail or cap tmax
  when the exact settle matters.
- **T-line UIC without IC=** seeds the waves from the UIC vector (v
  from .ic nodes, i = 0) where ngspice zeroes them — the C/L ic=
  precedent ("the deck's choice"); like every cold UIC start, never
  write an order-accuracy test on it.

## Next (per §11)

M5 is closed (temp, .nodeset, .noise, Meyer caps, the diode TT diffusion
charge), the BSIM6-style core (level 77) has landed with its physics
gates, the BJT diffusion charges (M7, Tf/Tr) close the last deferred BJT
dynamics, and M8 (the CLM output conductance, PCLM) closes the wrong-
answer gap that every analog gain was hitting (infinite ro) — plus the
M6 quadrature hardening it forced (the split decaying-window sweep).
Next: the remaining b6 follow-ups (the Ward-Dutton partition moment,
noise sources, the psi/Vdsat state caching), with M0b (the KLU backend
decision) after. A small alignment debt remains: the Meyer/b6 BE
companions still use the -Q/dt origin form (their charges are
volt-scale, so it is benign there; the BJT and diode branches were
aligned in M5e/M7) - one line each if a BE-mode gate ever needs it.
