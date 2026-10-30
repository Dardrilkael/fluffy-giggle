# SPICE arc - plan, status, and continuation notes

This file is the **continuation memory** for the `libs/spice` + `apps/spice`
work in this repo. The original ask was: clone the upstream
`github.com/Dardrilkael/fluffy-giggle` workspace and implement `libs/spice`
from the ground up against its `DESIGN.md`. M0 + M1 are done; M2+ were
explicitly scoped out. If a future session needs to resume, start here.

The design contract is `libs/spice/DESIGN.md` (~880 lines). Where this file
and DESIGN.md disagree, DESIGN.md wins.

## Status (2026-10-29)

**Done and committed on `main`:**

| Commit    | Milestone | What landed |
|-----------|-----------|-------------|
| `400a65b` | M0        | `ws::math` Eigen sparse LU facade (analyze/factorize/solve/transpose, real + complex), tested via `libs/math/tests/test_sparse.cpp`. |
| `9d369f1` | M1 start  | `libs/spice` skeleton + error taxonomy (kind enum -> int32_t code, `WS_*` error message strings, `Warning`/`parse_error` records). |
| `a62d2b1` | M1a       | Records with `{file,line,col}` provenance through `.include`, the SPICE number grammar, the tokenizer. |
| `5e97912` | M1        | Cards + `.param` scopes + expression VM, elaboration (`.subckt` flatten, node table, graph checks), devices R/V/I/E/F/G/H, MNA assembly over the dry-run pattern, `.op` and `.dc` drivers (incl. nested two-source sweeps, warm start, `temp` re-elaboration), the Results API + CSV writer, the `apps/spice` CLI, and the M1 test gate (`test_run.cpp`). |
| `8b5a79d` | tooling   | Windows build scripts `tools/build.ps1` + `tools/build-spice.ps1`, the `spice-debug`/`spice-release` CMake presets, and this plan file. |
| `60fa50c` | docs      | Pinned the delivery commit hash in this file. |
| `e4cfd8c` | MSVC      | Fixed the friend/anonymous-namespace ambiguity and the Eigen Triplet narrowing so the Windows build is clean under `/WX`. |
| `970f260` | tools     | Dropped the stray `cmake -T` call from `build.ps1`'s test phase. |
| `1ee0167` | spice     | Fixed the CSV header shape and added the title-line-looks-like-a-card warning. |
| `fa45283` | spice     | Four `.dc`/`.op` correctness fixes: convergence class passthrough, temp flow, first-point handling, and report counts. |
| `7b9649e` | spice     | M1 hardening: `.dc` point-count guards, nested-temp/duplicate-source rejection, `prepare()` error wrapping, `AnalysisCard` validation, empty-deck rejection, warning dedup. |
| `ace0a9f` | M2        | C/L/K/S/W devices, PULSE/SIN/EXP/PWL sources, `.tran` (trap+BE, LTE, breakpoints, .ic/UIC), `.ac` (complex MNA, re()/im() CSV), switch `.model SW`, the M2 analytic gate (RC/RLC/LC/AC/K/switch rows, trap-order via `fixed_dt`). |
| `8662844` | tools  | `tools/smoke-spice.ps1` - the self-verifying Windows CLI smoke (12 cases, 110 assertions; validated against a simulated all-zeros exe). |
| `8373c6e` | §3.5 rev | Param/state arenas: immutable Circuit + per-run RunState; pattern-hash check retired; subckt-local value inlining; gauss()/flat(). |
| `5f2c8e7` | §7.8 rev | Columnar results: signals by name, spans; CSV byte-identical. |
| `d8a7da0` | §2.1 rev | CircuitBuilder + units + Simulator + netlist writer + equivalence tests (caught the Rser §6.6 false positive). |
| `8ab9534` | §6 rev | Steppable/cancellable transient (TranMarch/TransientRun, StopToken, closed-loop set_source). |
| `025e91d` | §5.2 rev | Convergence ladder with PTC as stage 4 (backlog promotion). |
| `01d5c56` | §7.6 | .step families + Monte-Carlo (recorded seeds, keep-going). |
| `1c0145b` | §0.4 | measure:: library + .meas parser (--strict-meas). |
| `705cfd3` | §0.5 | .save/.print expressions + @dev[param], post-hoc. |
| `f9e794a` | misc | .raw writer, `spice check` lint, compat=strict, CLI flags. |
| `2126db1` | docs | The revision's docs: DESIGN.md Revision 2, `libs/spice/CLAUDE.md`, `libs/spice/STATUS.md`. |
| `ec349ec` | spice | .meas over .step families measures each point (regression fix). |
| `70e7e84` | spice | MSVC /W4 /WX portability: `struct Circuit` forward decl (C4099) + six shadowing renames (C4456/C4458). |
| `b265ae7` | spice | .dc crash on MSVC debug fixed: `run_dc` assigned through empty `std::optional`s - UB that stayed silent on GCC and aborted (exit 3) on Windows. Found via ASan; see the audit tooling below. |
| (M3, this commit) | spice | The diode + the §3.6 current iterate: `DcLoadContext::node_v/branch_i/gmin`, `AcLoadContext::state_`, `NonlinResid` + the §5.1 nonlinear-residual convergence gate, `update_state` `x_prev` param, pnjlim (SPICE3 devlim), `solve_tran` upgraded to a real Newton loop, `gminsteps`/`srcsteps`/`gshunt` options, `Builder::diode()`/`diode_model()`. Gate: Lambert-W .op (1e-7 rel at reltol=1e-9), .dc sweep vs Lambert-W, pnjlim stiff-bias convergence (diode at 100V), rectifier .tran sanity, Jacobian vs central finite differences, builder .op equivalence. 2306 assertions / 106 cases / 0 failures. |
| (HEAD, after `b265ae7`) | tools | Linux-side audit scanners for MSVC parity (`tools/audit_*.py`, `tools/build_*.py`) + this plan refresh. |

Working tree is clean; `main` is 14 commits ahead of `origin/main` and
**deliberately not pushed** (no remote credentials). If you want to
share the work, push `main` to a remote you control, or zip the repo and
hand it over.

## Locked decisions (do NOT re-litigate)

These four were batched and approved up front:

1. **Scope = M0 + M1 + M2 + M3.** M3 (the diode + Newton limiting +
   the convergence ladder) was reopened and landed by the user's
   `follow the plan` directive. M4+ (BJT/MOS/B/T-line, `.noise`,
   semiconductor caps) remain out of scope and deliberately reject
   at parse with file:line (DESIGN.md working rule 5).
2. **All 5 DESIGN.md open questions answered with the default:**
   - Eigen sparse LU backend (NOT KLU/KLUAdopt).
   - AC CSV layout uses `re()/im()` columns (NOT mag/phase).
   - `.step` default = abort (run stops at the first failed step).
   - `.meas` failure is **non-fatal** (warning only).
   - ngspice-canonical dialect (LTspice forms accepted where the design
     lists them; nothing beyond that).
3. **Build verification = Linux preset added.** `CMakePresets.json`
   `linux` = Ninja + GCC + `WS_BUILD_LIBS=core;math;spice`. Builds only the
   three libs and runs their ctest. This was the headless verification path
   during development.
4. **Git discipline = one commit per milestone on `main`, no push.** M0 =
   one commit; M1 was split into 3 (start / a / complete) because of its
   size. M2 landed as one commit. M3+ would each get one commit when they
   land.

## Build / test / run

### Linux (the dev-verification path; do this first on this machine)

```sh
cmake --preset linux              # configures ws_core + ws_math + ws_spice
cmake --build --preset linux      # builds the three libs + apps/spice + tests
ctest --preset linux              # runs ws_core, ws_math, ws_spice tests

./build/linux/bin/spice run path/to/deck.cir -o out.csv
```

cmake/ninja are not on PATH in the dev sandbox; install with
`pip install cmake ninja` and `export PATH="$HOME/.local/bin:$PATH"` first.

### Windows (the user-facing path; this is what the new scripts are for)

```powershell
# from a plain PowerShell prompt (no need to dot-source dev-shell.ps1 -
# build.ps1 loads vcvars64 inline if cl.exe is not already on PATH):

.\tools\build-spice.ps1             # spice subset, debug, build only
.\tools\build-spice.ps1 -Test       # also run ctest
.\tools\build-spice.ps1 -Clean      # wipe build/spice-debug first

.\tools\build-spice.ps1 -Preset spice-release   # optimized spice subset

# Full workspace (heavy - pulls Vulkan, OpenAL, libuv, Box2D, Jolt):
.\tools\build.ps1 -Preset debug
.\tools\build.ps1 -Preset debug -Test

# Windows smoke: builds spice-debug, then runs 12 analytic decks through
# the real CLI asserting closed-form values, headers, row counts and exit
# codes (110 assertions; exit 0 = all pass). Add -KeepWork to keep the
# temp decks, -SkipBuild to reuse the built exe, -Spice <path> to smoke a
# specific exe, -Preset spice-release for the release build:
.\tools\smoke-spice.ps1
.\tools\smoke-spice.ps1 -SkipBuild -KeepWork
.\tools\smoke-spice.ps1 -Spice C:\some\old\spice.exe   # e.g. repro a report
```

The CLI binary lands at `build\<preset>\bin\spice.exe` (or
`build/<preset>/bin/spice` on Linux).

Windows "all zeros" follow-up (2026-10-29): the reported symptom could not
be reproduced in the shared code - the parser survives every Windows-typical
deck encoding (CRLF, UTF-8 BOM, missing final newline all produce
byte-identical results; only legacy CR-only line endings reject, loudly),
and the smoke battery passes 110/110 against the Linux exe (plus 57/57 in
the bash mirror). The earlier zero-output observations most plausibly
predate the `1ee0167`/`fa45283` fixes. If it recurs on a Windows box, run
`.\tools\smoke-spice.ps1 -Spice <the offending exe> -KeepWork` and send the
failure list - it prints expected vs got per case plus the raw CSV and
stderr, which localizes the break in one run.

### Running the simulator

```
spice run deck.cir                      # CSV to stdout
spice run deck.cir -o out.csv           # CSV to file
spice run deck.cir -I ./include -I ../  # add `.include` search paths
```

Exit codes (DESIGN.md §9.5): 0 ok; 1 deck or simulation error; 2 usage or
file-I/O error. Warnings print to stderr and never fail the run.

## What exists (file map, M0 + M1)

```
libs/math/
  include/ws/math/sparse.hpp      # public sparse LU facade (analyze/factorize/solve)
  include/ws/math/lu.hpp          # (pre-existing) dense LU
  src/sparse.cpp                  # Eigen-backed sparse LU impl
  tests/test_sparse.cpp           # symbolic-reuse + numeric + complex tests

libs/spice/
  DESIGN.md                      # the contract (~880 lines)
  CLAUDE.md                      # the lib's own rules-bite file (kept current)
  include/ws/spice/
    parse.hpp                    # records, ParseOptions, read_deck
    deck.hpp                     # DeckAst, Card, ParamTable
    expr.hpp                     # the expression VM
    error.hpp                    # ErrorKind, Warning, render formats
    device.hpp                   # the Device interface (staged load/stamp)
    run.hpp                      # RunSettings, run_deck, RunOutput
    results.hpp                  # Table/Results, results_csv
  src/
    parse.cpp                    # records + tokenizer + SPICE numbers + .include
    deck.cpp                     # cards, .param scopes
    expr.cpp                     # expression VM
    elaborate.cpp                # .subckt flatten, node table, graph checks
    devices.cpp                  # R V I E F G H + factory table
    devices_m2.cpp               # C L K S W (M2 companions + switches)
    devices_m3.cpp               # D - the diode (Shockley + pnjlim)
    mna.cpp                      # pattern assembly + numeric contexts
    analysis.cpp                 # .op and .dc drivers
    output.cpp                   # CSV
    cards.hpp / circuit.hpp / mna.hpp  # src-only internals
  tests/
    test_spice.cpp               # error taxonomy (owns the doctest main)
    test_parse.cpp               # records/numbers/includes
    test_run.cpp                 # M1+M2 gate: analytic .op/.dc/.tran/.ac,
                                 # parse goldens, CSV shape, trap-order hook

apps/spice/
  CMakeLists.txt                 # ws_add_app(spice DEPENDS ws::spice)
  src/main.cpp                   # argv + file I/O, exit codes; nothing else

cmake/
  WsHelpers.cmake                # ws_wants(), ws_add_library/_app/_test
  ThirdParty.cmake               # gated fetches; spice pulls Eigen + doctest + tl::expected
  WsWarnings.cmake               # the workspace warning policy
CMakePresets.json                # debug/release/vs-*/linux/android + spice-debug/spice-release
libs/CMakeLists.txt              # WS_BUILD_LIBS closure + per-lib gating
apps/CMakeLists.txt              # per-app gating on its lib set
tools/
  build.ps1                      # the configurable build driver
  build-spice.ps1                # the spice-subset one-liner
  smoke-spice.ps1                # the self-verifying CLI smoke
  audit_class_struct.py          # MSVC-parity audits (see the tooling
  audit_shadow.py                #   section above; read-only scanners +
  audit_uninit.py                #   instrumented-build helpers, Python 3,
  build_asan.py                  #   no deps beyond a GCC build tree)
  build_checked.py
  dev-shell.ps1                  # (pre-existing) loads vcvars64 in the prompt
  compile_shaders.ps1            # (pre-existing) ui shader regen
  package-android.ps1           # (pre-existing)
  check_deps.ps1                 # (pre-existing) the dep-rule ctest
docs/
  SPICE_PLAN.md                  # this file
  claude-md-archive.md            # (pre-existing)
```

## Windows/Linux parity: audit tooling

The Windows build is MSVC `/W4 /WX` + `/RTC1` (debug); the Linux preset
is GCC `-Wall -Wextra -Wpedantic -Werror`. Those flag sets are NOT
equal - two real Windows-only breaks shipped because of it (C4099
class/struct mismatches in `70e7e84`, and the empty-optional UB in
`b265ae7` that GCC's zero-filled stack masked). The five Python tools
below close the gap from the Linux side; run them before handing a
build to Windows:

```
python3 tools/audit_class_struct.py   # C4099: class vs struct key mismatches
                                      #   (pure Python, no build tree needed)
python3 tools/audit_shadow.py         # C4456/57/58/59: shadowing (-Wshadow
                                      #   recompile; output to /dev/null)
python3 tools/audit_uninit.py         # /RTCu: uninitialized reads
                                      #   (-O2 -Wmaybe-uninitialized screen)
python3 tools/build_asan.py           # ASan+UBSan build - the strong check;
                                      #   this is what caught b265ae7
python3 tools/build_checked.py        # _GLIBCCXX_ASSERTIONS = MSVC checked
                                      #   iterators (bounds-checked operator[])
```

Notes:
- The three `audit_*` tools are read-only (safe on any build tree). The
  two `build_*` tools **instrument the tree in place** - clean-rebuild it
  afterwards (`cmake --build <dir> --target clean`), or point them at a
  scratch tree via `--build-dir`.
- All five need the `linux` preset's `compile_commands.json` (GCC/Clang
  commands). MSVC `cl.exe` entries are detected and skipped with a note
  - on that side /W4 + /RTC1 already cover it.
- `--libs core,math,spice,apps/spice` selects the TUs (default shown).
- ASan reports leaks at exit by default; `ASAN_OPTIONS=detect_leaks=0`
  focuses on hard errors.

## What is NOT done (deliberately deferred)

These all reject at parse with `file:line:col: error:` today (DESIGN.md
working rule 5) and would each be a future milestone:

- **M0b (alt backend)** - KLU sparse solver behind `solver=klu` in
  `.options`. The Eigen backend is the only one wired; the design leaves
  the policy layer abstract enough to add KLU later.
- **M4** - semiconductor devices: BJT (Gummel-Poon), MOS level 1,
  behavioral B sources (dual-number AD), lossless T-lines (T). The diode
  (M3) is the foundation; BJT/MOS reuse the §3.6 current-iterate
  threading and the nonlinear-residual convergence gate.
- **M5 (remainder)** - `.noise`, `.nodeset`, per-device temperature
  params (TC on models, temp-dependent diode Vt), text report. (The
  2026-10-29 revision already landed `.step`, `.meas`, `.save`/`.print`
  and `temp` sweeps - see the table above; only these M5 items still
  reject at parse.)
- **Multi-winding K** - a third inductor on an existing pair rejects
  (backlog).
- **`.lib`** - the ngspice library-file include mechanism. Backlog.
- **Gear(2)/TR-BDF2** - integration is trap + BE only (§6.1); Gear
  orders > 2 are rejected by design.
- **Diode M4/M5 params** - Cjo/M/Vj (junction cap, M4), Bv/Ibv
  (breakdown, M5), Tnom/Xti/Eg (temp model, M5), Kf/Af (noise, M5+).
  Accepted at parse but not implemented in M3.

## Next steps if memory is cleared

In rough priority order:

1. **Re-read this file + `libs/spice/DESIGN.md` + `libs/spice/CLAUDE.md`.**
   DESIGN.md is the contract; CLAUDE.md is the lived rules-bite list; this
   file is the status. The three together are the full state.
2. **Confirm the working tree.** `git log --oneline -22` should show the
   commits listed above (HEAD = the audit-tooling commit; `origin/main`
   sits at the `8662844` smoke commit, 14 behind). `git status` should be
   clean.
3. **Run the gate.** `cmake --preset linux && cmake --build --preset linux
   && ctest --preset linux` should pass (106 cases / 2306 assertions at the
   time of writing). If it does not, the break is load-bearing -
   investigate before adding anything. Before a Windows hand-off, also
   run the five `tools/audit_*.py`/`tools/build_*.py` parity checks.
4. **Pick the next milestone.** The natural next moves are **M0b** (the
   KLU backend: both facades are stable, cross-validation tests are the
   gate) or **M4** (BJT Gummel-Poon, MOS level 1, behavioral B with
   dual-number AD, lossless T-line; the diode's §3.6 current-iterate
   threading from M3 is the foundation - BJT/MOS reuse the same
   nonlinear contract). Each new milestone = one commit, one test row,
   one CLAUDE.md note - that's the established rhythm (M1 commit 5e97912,
   the M2 commit, and the M3 commit are the templates).
5. **Do not regress scope.** If a request lands that wants M3+ devices,
   that's a new arc with its own decision round. The four locked decisions
   above stay locked unless the user explicitly reopens them.

## Useful grep anchors

- The error kinds enum: `enum class ErrorKind` in
  `libs/spice/include/ws/spice/error.hpp`.
- The Device interface (the staged load): `struct Device` in
  `libs/spice/include/ws/spice/device.hpp`.
- The MNA pattern = what load touches: `class MnaSystem` (setup = the
  dc+tran+ac union dry run) in `libs/spice/src/mna.hpp`.
- The analysis dispatch: `run_deck` in `libs/spice/src/analysis.cpp`
  (run_op/run_dc/run_tran/run_ac live there too).
- The time loop + LTE/breakpoints: `run_tran` in
  `libs/spice/src/analysis.cpp`; the companions in
  `libs/spice/src/devices_m2.cpp` (`begin_step`/`commit`/`lte_ok`).
- The waveform semantics (PULSE/SIN/EXP/PWL + breakpoints):
  `class Waveform` in `libs/spice/src/waveform.hpp`.
- The graph checks (voltage-source loops, no-DC-path): `check_graph` in
  `libs/spice/src/elaborate.cpp`.
- The 1e-9 rel tolerance gate: `test_run.cpp` `approx` helper.
