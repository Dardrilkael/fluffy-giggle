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
| (this delivery) | tooling   | Windows build scripts `tools/build.ps1` + `tools/build-spice.ps1`, the `spice-debug`/`spice-release` CMake presets, and this plan file. |

Working tree is clean; `main` is 4 + 1 = 5 commits ahead of `origin/main`
and **deliberately not pushed** (no remote credentials). If you want to
share the work, push `main` to a remote you control, or zip the repo and
hand it over.

## Locked decisions (do NOT re-litigate)

These four were batched and approved up front:

1. **Scope = M0 + M1.** M2 (`.tran`/`.ac`/`.noise`, time integration,
   PULSE/SIN/EXP/PWL sources) and beyond are **out of scope** and
   deliberately reject at parse with file:line (DESIGN.md working rule 5).
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
   size. M2+ would each get one commit when they land.

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
```

The CLI binary lands at `build\<preset>\bin\spice.exe` (or
`build/<preset>/bin/spice` on Linux).

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
    mna.cpp                      # pattern assembly + numeric contexts
    analysis.cpp                 # .op and .dc drivers
    output.cpp                   # CSV
    cards.hpp / circuit.hpp / mna.hpp  # src-only internals
  tests/
    test_spice.cpp               # error taxonomy (owns the doctest main)
    test_parse.cpp               # records/numbers/includes
    test_run.cpp                 # M1 gate: analytic .op/.dc, parse goldens, CSV shape

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
  build.ps1                      # the configurable build driver (this delivery)
  build-spice.ps1                # the spice-subset one-liner (this delivery)
  dev-shell.ps1                  # (pre-existing) loads vcvars64 in the prompt
  compile_shaders.ps1            # (pre-existing) ui shader regen
  package-android.ps1           # (pre-existing)
  check_deps.ps1                 # (pre-existing) the dep-rule ctest
docs/
  SPICE_PLAN.md                  # this file
  claude-md-archive.md            # (pre-existing)
```

## What is NOT done (deliberately deferred)

These all reject at parse with `file:line:col: error:` today (DESIGN.md
working rule 5) and would each be a future milestone:

- **M2** - `.tran`, `.ac`, `.noise`. Time integration (trapezoidal / Gear),
  PULSE/SIN/EXP/PWL source waveforms, AC small-signal phasor analysis.
- **M0b (alt backend)** - KLU sparse solver behind `solver=klu` in
  `.options`. The Eigen backend is the only one wired; the design leaves
  the policy layer abstract enough to add KLU later.
- **M3** - semiconductor devices: Diode, BJT (NPN/PNP), MOSFET
  (NMOS/PMOS), JFET. Linearization + Newton iteration for `.op`.
- **M4** - lossy/lossless transmission lines (T, U), switches (S/W).
- **M5** - `.options temp` and per-device temperature params (device
  temperature coefficient models), `.measure` directives (already non-fatal
  by decision but not implemented), `.step` (already default-abort by
  decision but not implemented).
- **`.lib`** - the ngspice library-file include mechanism. Backlog.
- **Nonlinear convergence polish** - gmin stepping, source stepping, damped
  Newton, junction charge models. M3+ land.

## Next steps if memory is cleared

In rough priority order:

1. **Re-read this file + `libs/spice/DESIGN.md` + `libs/spice/CLAUDE.md`.**
   DESIGN.md is the contract; CLAUDE.md is the lived rules-bite list; this
   file is the status. The three together are the full state.
2. **Confirm the working tree.** `git log --oneline -10` should show the
   5 commits listed above. `git status` should be clean.
3. **Run the gate.** `cmake --preset linux && cmake --build --preset linux
   && ctest --preset linux` should pass. If it does not, the break is
   load-bearing - investigate before adding anything.
4. **Pick the next milestone.** The most natural next move is **M2** (`.op`
   and `.dc` already drive MNA; `.tran` needs a time stepper over the same
   MNA frame, `.ac` needs the complex LU path which already exists). Each
   new analysis = one commit, one test row, one CLAUDE.md note - that's
   the established rhythm (M1 commit 5e97912 is the template).
5. **Do not regress scope.** If a request lands that wants M3+ devices,
   that's a new arc with its own decision round. The four locked decisions
   above stay locked unless the user explicitly reopens them.

## Useful grep anchors

- The error kinds enum: `enum class ErrorKind` in
  `libs/spice/include/ws/spice/error.hpp`.
- The Device interface (the staged load): `struct Device` in
  `libs/spice/include/ws/spice/device.hpp`.
- The MNA pattern = what load touches: `class MnaPattern` in
  `libs/spice/src/mna.hpp`.
- The analysis dispatch: `run_deck` in `libs/spice/src/analysis.cpp`.
- The graph checks (voltage-source loops, no-DC-path): `check_graph` in
  `libs/spice/src/elaborate.cpp`.
- The 1e-9 rel tolerance gate: `test_run.cpp` `approx` helper.
