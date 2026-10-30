# ws::spice — STATUS

State of the arc at a glance. The contract is `DESIGN.md` (Revision 2,
2026-10-29); the rules are `CLAUDE.md`. Where this file and DESIGN.md
disagree, DESIGN.md wins and this file gets fixed.

## Where we are

M0 + M1 + M2 + **M3 (the diode)** complete, plus the 2026-10-29
post-review revision (four structural changes + six usage wins) — all
committed on `main`, all gates green: **2306 assertions, 106 test cases,
0 failures** (Linux preset: GCC 14, `-Wall -Wextra -Wpedantic -Werror`).
Windows parity restored the same day: the MSVC `/W4 /WX` build is clean
again and the full smoke passes on Windows (97+6 cases → all green after
`70e7e84` + `b265ae7`); the Linux-side parity checks live in
`tools/audit_*.py` / `tools/build_*.py` (documented in
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
| (M3, this commit) | §3.6 current-iterate threading (`DcLoadContext::node_v/branch_i/gmin`, `AcLoadContext::state_`), `NonlinResid` + the §5.1 nonlinear-residual convergence check, the diode (Shockley + pnjlim + 3 state slots + `.model D`), `solve_tran` upgraded to a real Newton loop, `gminsteps`/`srcsteps`/`gshunt` options, `Builder::diode()`/`diode_model()`. Gate: Lambert-W .op (1e-7 rel), .dc sweep, pnjlim stiff-bias convergence, rectifier .tran sanity, Jacobian vs central finite differences, builder .op equivalence. |

## Known sharp edges

- **PTC rescue cases now exercise the diode** — the §5.2 ladder's stage
  4 (PTC) was promoted from the backlog in the 2026-10-29 revision but
  had no nonlinear device to rescue; M3's diode + pnjlim is what makes
  the ladder load-bearing. The stiff-bias test (diode at 100V) confirms
  plain Newton + pnjlim converges without needing gmin/source/PTC; real
  chattering/non-converging diode circuits (the kind that NEED stage 4)
  arrive with M4's BJT/MOS where the operating point is genuinely
  multi-stable.
- **The diode's Vt is frozen at Tnom** (M3): `.options temp` does NOT
  re-evaluate the diode's thermal voltage (temp-dependent models are
  M5). The Lambert-W gate uses the default 27°C; `.dc temp` over a
  diode circuit will give wrong currents at non-27°C points. M5 fixes
  this by threading `temp` through the load context.
- **The diode has no Cjo** (M3): junction capacitance (the
  depletion+diffusion cap for .tran and .ac) lands with M4's compact
  device cap model. A diode in .tran works (it's a nonlinear resistor
  at each operating point); a diode in .ac is a pure conductance (no
  j*omega*Cjo term).
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

## Next (per §11, unchanged)

M4: BJT (Gummel–Poon), MOS level 1, behavioral B with dual-number AD
(§8.5), lossless T-line. Then M5 (.noise/.nodeset, temp-dependent
diode model), M0b (KLU backend decision), M6 hardening.
