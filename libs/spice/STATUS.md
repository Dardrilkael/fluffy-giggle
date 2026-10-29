# ws::spice — STATUS

State of the arc at a glance. The contract is `DESIGN.md` (Revision 2,
2026-10-29); the rules are `CLAUDE.md`. Where this file and DESIGN.md
disagree, DESIGN.md wins and this file gets fixed.

## Where we are

M0 + M1 + M2 complete, plus the 2026-10-29 post-review revision (four
structural changes + six usage wins) — all committed on `main`, all
gates green: **2213 assertions, 98 test cases, 0 failures** (Linux
preset: GCC 14, `-Wall -Wextra -Wpedantic -Werror`).

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

## Known sharp edges

- **PTC rescues are untested with real nonlinear devices** — with the
  linear+switch set, chattering circuits provably have no fixed point
  (clean full-ladder failure, the ngspice verdict); real homotopy/PTC
  rescue tests arrive with the M3 diode, exactly as §11 planned.
- **AC expression save-lists reject loudly** (vm()/vp() derived forms
  are the §7.4 follow-up), and .ac tables carry re()/im() pairs.
- **`.dc temp` / `.step temp` on a Simulator-created circuit** is a
  Usage error naming the limitation (temp re-elaborates from the deck;
  a built circuit has none).
- **-Wmissing-field-initializers is off project-wide**: designated
  initializers with defaulted tails are the intended aggregate API
  spelling (`Tran{.tstop = 5_ms}`); GCC flags exactly that.
- The M3 follow-up stands: nonlinear devices need the current iterate
  threaded into `load()` (tracked in `docs/SPICE_PLAN.md`).

## Next (per §11, unchanged)

M3: diode + Newton limiting + ladder constants pinned against the ref
harness (now including real PTC rescue cases). Then M4 (BJT/MOS/B/T-line
+ AD), M5 (.noise/.nodeset), M0b (KLU backend decision), M6 hardening.
