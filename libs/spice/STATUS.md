# ws::spice — STATUS

State of the arc at a glance. The contract is `DESIGN.md` (Revision 2,
2026-10-29); the rules are `CLAUDE.md`. Where this file and DESIGN.md
disagree, DESIGN.md wins and this file gets fixed.

## Where we are

M0 + M1 + M2 + **M3 (the diode)** + **M4 (the compact devices — BJT,
junction caps, MOSFET)** complete: M4a `a756afe`, M4b `3644e21` +
`48e050d`, M4c `117d647` — plus the 2026-10-29 post-review revision
(four structural changes + six usage wins) — all committed on `main`.
Latest recorded gate (M4c): **2347 assertions, 118 test cases, 0
failures** (Linux preset: GCC 14, `-Wall -Wextra -Wpedantic -Werror`).
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
- **The MOSFET has no §10.1 analytic test row** (M4c): validation is
  the ngspice 39 cross-check (exact) + the mos_* decks; the test-row
  gap is the first thing the next pass should close.
- **Deferred device physics** (accepted-but-not-implemented model keys,
  so ngspice decks parse): the MOS Meyer caps (Cgs/Cgd/Cgb), BJT Early
  effect (Vaf/Var), diode/BJT diffusion charge (TT/Tf/Tr), diode Rs.
  These queue behind M5 (temp/breakdown/noise) unless pulled earlier.
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

## Next (per §11)

Close the M4 gap first: the MOSFET's §10.1 analytic test row. Then
M4d (behavioral B with dual-number AD), M4e (lossless T-line). The
deferred device physics (Meyer caps, Early effect, TT/Tf/Tr, diode Rs)
and the temp-dependent models (the diode's frozen Vt) queue behind M5
(.noise/.nodeset, temp threading through the load context), with M0b
(KLU backend decision) and M6 hardening after.
