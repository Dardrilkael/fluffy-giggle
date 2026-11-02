# circsim arc - plan, status, and continuation notes

This file is the **continuation memory** for the `libs/circsim` +
`apps/circsim_cli` work (the schematic-capture app growing on top of
`libs/spice`). If a future session needs to resume, start here, then
read `libs/circsim/CLAUDE.md` (the lived rules) and the SPICE arc's
`docs/SPICE_PLAN.md` for the simulator underneath.

## Status (2026-11-02)

Working tree clean; `main` ahead of origin and deliberately not pushed
(no remote credentials) - same policy as the SPICE arc. Stage 4 landed
on top of stage 3's `99ae630`.

| Commit    | Stage | What landed |
|-----------|-------|-------------|
| `ce7c14a` | 1     | Schematic model (parts/junctions/wires/labels, ids never reused), the `.csim` JSON format (unknown fields preserved, tolerant load, migrations, atomic writes), graph-based net building (reference edges + exact-position coincidence, crossings never connect), ERC (floating pins, V/L loops, I cutsets, ...), the ws::spice deck export with the back-map, `run_schematic` through spice's own front end, the `circsim_cli` run/erc/netlist subcommands, the smoke suite. |
| `812fc68` | 2     | The `.csym` symbol library: `SymbolLibrary` (built-ins + layered files, ci, later wins, override warnings), typed `ParamSpec`s (Text/Number/Choice - Number validated with ws::spice's grammar; defaults are new-part prefill, never injected at export), `DrawPrim` drawing primitives + `transform_pos` + `symbol_bounds`, ERC's stage-2 checks (bad_param, missing_param on non-optional emits, unused_param), `-S` on the CLI + the `symbols` subcommand (--json full-table dump), `tline.csym`/`tline_demo.csim` (a library-only T symbol simulated through the quarter-wave closed form). |
| `99ae630` | 3     | The editor: `apps/circsim_editor`, the ws::ui::shell app over the model - the schematic canvas (place/move/rotate/mirror parts, wire routing with the ortho constraint, labels; every edit an undo command with typing-run coalescing), the palette over `symbols()` with fitted mini previews, the property grid over ParamSpecs (plus the analysis-setup and directives editors), the Problems panel over Diags with select-and-zoom through the back-map (the new headless `libs/circsim/locate` seam, tested), the netlist view, and the simulate job over `run_schematic` (status row + summary toast). The bare editor keys (R/M/W/L/F/Del/Esc) bind on the resolver's PANEL rung; the text rule keeps them out of focused fields. |
| (this commit) | 4 | The results story: `libs/circsim/plot.hpp` - the headless axis math (nice 1/2/5 ticks, SI engineering labels, range padding, min/max decimation that keeps every bucket's y-extremes so spikes and envelope survive any pixel budget), 14 test cases; the editor's Results panel over it - one analysis at a time from a combo, per-signal visibility checkboxes with live legend readouts (the hover sample, else the last), the plot through the canvas layer (frame, grid + engineering tick labels, colored series, NaN-broken segments), a hover crosshair with per-series markers, `.op` tables and `.meas` as value rows, and CSV/JSON export riding the writers ws::spice already owns (the document's stem, the fs seam). The panel's draw is gated headless by `tests/test_results_panel.cpp` through the new `Editor::plot_canvas` seam (a CommandListCanvas asserting the recorded geometry: frame, grid, series point counts/colors/mapping, decimation envelope, crosshair, markers, the no-plot .op shape). The fix the visual gate caught: the stage-3 document factory's extension check compared the last 4 chars with ".csim" (5 chars) - never equal, so every File > Open and session restore was refused; now `ends_with`. |

## Locked decisions

1. **Headless core first.** Every stage lands headless in `libs/circsim`
   (no ui include) - stage 3 kept that rule: the editor's only lib-side
   addition is `locate.hpp`, pure over the model, and the app itself
   lives in `apps/circsim_editor`. The CLI remains the headless
   acceptance surface; the editor is the ui one.
2. **ws::spice is the only simulator.** Export -> read_deck ->
   build_deck_ast -> run_deck; never a second solver, never a second
   parser (the CLI's --analysis grammar reuses spice's number grammar).
3. **Layering = replacement, later wins.** A `.csym` symbol with a
   built-in's key replaces the whole definition (spice letter and ref
   prefix included - forgetting `"spice"` turns it into a power-like
   never-exported symbol, on purpose); later files win over earlier
   ones; every redefinition warns.
4. **Defaults are prefill, not injection.** `default_params` feeds new
   parts; export emits exactly what the schematic stores.
5. **Unknown-field preservation at root + symbol level only.** Inside
   pins/emit/params/draw, unknown fields warn and drop (tight objects;
   a typo is news). This split is deliberate.
6. **One commit per stage on `main`**, pushed never (no credentials);
   hand off via zip, like the SPICE arc.

## Build / test / run

```sh
cmake --preset linux && cmake --build --preset linux   # core..circsim + CLIs
ctest --preset linux                                    # headless gate
python3 tools/smoke_circsim.py --cli build/linux/bin/circsim_cli
```

The stage-4 gate at land time: 605/605 ctest on the `linux` preset
(headless subset; test_circsim now 94 cases / 589 assertions, of which
14 cases are test_plot.cpp) and 872/872 on the Linux ui tree (the
whole-workspace build `ae60fd2` unlocked: every headless suite
including test_ui/test_ui_gui/test_ui_canvas/test_ui_shell, the
circsim_editor link, and the new test_circsim_editor_panel - 6 cases /
115 assertions over the Results panel's draw), ASan+UBSan clean over
the arc's own suites (circsim+spice: 261/261; the pre-existing
physics.3D allocation-counting cases are sanitizer artifacts,
untouched by this arc), the conversion audit clean over the arc's own
TUs (the vecmath.hpp header finding is pre-existing, in every
stage-1..3 TU alike), the shadow audit clean over the arc's own
files (the ui/input findings are pre-existing, outside this arc's
TUs), the CLI smoke green. The visual gate: the editor booted under
Xvfb + lavapipe, restored a session, simulated (Ctrl+Enter), and the
Results panel drew legend readouts and the plot - the run that found
the stage-3 factory bug above.

The Linux ui tree (the editor's own build):

```sh
# needs the X dev packages a headless box lacks; the posix commit's
# story. GLFW_BUILD_WAYLAND=OFF + the X prefix on the search paths.
cmake -S . -B build/linux-ui -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DWS_BUILD_LIBS="core;math;spice;jobs;settings;circsim;input;gfx;app;platform;ui" \
  -DFETCHCONTENT_BASE_DIR=build/fc-linux \
  -DCMAKE_PREFIX_PATH=<X prefix>/usr -DGLFW_BUILD_WAYLAND=OFF
cmake --build build/linux-ui        # + circsim_editor, shell_example
ctest --test-dir build/linux-ui     # 851 cases
```

The editor itself is a windowed app (Windows the shipped desktop; a
posix box runs it under X with the dejavu/liberation font fallbacks
and the Unavailable* dialog refusals - see apps/CLAUDE.md).

## What exists (stage 4 file map)

```
libs/circsim/
  include/ws/circsim/
    model.hpp     # Part/Junction/Wire/Label/Schematic (stage 1)
    symbols.hpp   # SymbolDef/PinDef/ParamEmit/ParamSpec/DrawPrim,
                  # builtin_symbols/find_symbol/pin_pos/transform_pos/
                  # symbol_bounds/default_params
    library.hpp   # SymbolLibrary, .csym load_text/save, builtin_library()
    nets.hpp      # build_nets(s, SymbolLibrary* = nullptr)
    format.hpp    # .csim load/save (stage 1)
    erc.hpp       # run_erc(s[, build][, SymbolLibrary*])
    export.hpp    # export_deck/run_schematic (+ SymbolLibrary*), results_json
    locate.hpp    # locate_diag/focus_bounds (stage 3 - the jump seam)
    plot.hpp      # nice_ticks/format_eng/expand_range/decimate_minmax
                  # (stage 4 - the axis math every plot surface shares)
  src/
    symbols.cpp   # the built-in table, now with specs + draw
    library.cpp   # the .csym reader/writer + the layering rules
    locate.cpp    # the where-parser + the focus bounds
    plot.cpp      # the tick ladder, the SI prefix walk, the bucket walk
    ...           # model/nets/format/erc/export (stage 1, threaded)
  examples/
    divider.csim, rc_step.csim      # stage-1 smoke decks
    tline.csym, tline_demo.csim     # the stage-2 library demo
  tests/
    test_symbols.cpp                # the stage-2 gate (no main; test_format owns it)
    test_locate.cpp                 # the stage-3 lib gate (no main)
    test_plot.cpp                   # the stage-4 lib gate (no main)
apps/circsim_cli/src/main.cpp       # run/erc/netlist/symbols, -S
apps/circsim_editor/                # stage 3: the editor (see its src/)
  src/editor.hpp        # the shared core: EditorView (y-up camera),
                        # Selection/Tool/PinHit, the caches (now the
                        # ResultsCache + the plot_canvas test seam),
                        # Editor
  src/editor.cpp        # snap/bounds/hit/route + draw_symbol + refreshers
  src/document.hpp/.cpp # the .csim Document + the undo command set
  src/canvas_panel.hpp/.cpp  # the Schematic panel (input + world render)
  src/side_panels.hpp/.cpp   # palette/properties/problems/netlist
  src/results_panel.hpp/.cpp # stage 4: the Results panel (plot + legend
                        # + value rows + export buttons; the axis math
                        # stays in ws::circsim/plot)
  src/main.cpp          # the App: shell wiring, commands, the run job
                        # (delivering to the Results panel), the export
  tests/test_results_panel.cpp  # the stage-4 app gate: the panel driven
                        # headless through the plot_canvas seam
tools/smoke_circsim.py              # the CLI smoke (incl. library cases)
```

## Not done (the natural next stages)

- **Stage 5 candidates, editor-side:** zoom/pan and rubber-band cursor
  reads on the plot itself (the crosshair is sample-snapped today;
  axis-drag zoom and a box zoom are the natural next cuts), multiple
  Y axes for mixed V/I legends, a CSV/JSON save dialog (the export
  derives the path from the document today - deliberate v1), waypoint
  editing on placed wires, multi-select + marquee, an android port
  (the touch interactions need their own pass).
- **Schematic-side libraries of the model's own**: a .csim "libraries"
  field naming .csym files (the CLI's -S is the seam today; the format
  deliberately does not carry it yet - it would be a .csim version
  bump). The editor currently uses the built-in table only; a
  File > Load Symbol Library riding -S's loader is the cheap first cut.
- **Net classes / ERC rules configuration** (power nets, unconnected
  pin allowances per symbol) - would ride ParamSpec-shaped data in
  .csym; nothing today.
- **Multi-page / hierarchical sheets** (subsheet ports) - not started.
