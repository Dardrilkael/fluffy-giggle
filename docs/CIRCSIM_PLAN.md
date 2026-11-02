# circsim arc - plan, status, and continuation notes

This file is the **continuation memory** for the `libs/circsim` +
`apps/circsim_cli` work (the schematic-capture app growing on top of
`libs/spice`). If a future session needs to resume, start here, then
read `libs/circsim/CLAUDE.md` (the lived rules) and the SPICE arc's
`docs/SPICE_PLAN.md` for the simulator underneath.

## Status (2026-11-02)

Working tree clean; `main` ahead of origin and deliberately not pushed
(no remote credentials) - same policy as the SPICE arc.

| Commit    | Stage | What landed |
|-----------|-------|-------------|
| `ce7c14a` | 1     | Schematic model (parts/junctions/wires/labels, ids never reused), the `.csim` JSON format (unknown fields preserved, tolerant load, migrations, atomic writes), graph-based net building (reference edges + exact-position coincidence, crossings never connect), ERC (floating pins, V/L loops, I cutsets, ...), the ws::spice deck export with the back-map, `run_schematic` through spice's own front end, the `circsim_cli` run/erc/netlist subcommands, the smoke suite. |
| (this commit) | 2 | The `.csym` symbol library: `SymbolLibrary` (built-ins + layered files, ci, later wins, override warnings), typed `ParamSpec`s (Text/Number/Choice - Number validated with ws::spice's grammar; defaults are new-part prefill, never injected at export), `DrawPrim` drawing primitives + `transform_pos` + `symbol_bounds`, ERC's stage-2 checks (bad_param, missing_param on non-optional emits, unused_param), `-S` on the CLI + the `symbols` subcommand (--json full-table dump), `tline.csym`/`tline_demo.csim` (a library-only T symbol simulated through the quarter-wave closed form). |

## Locked decisions

1. **Headless core first.** Every stage lands headless in `libs/circsim`
   (no ui include); the editor is a later stage in ws::ui on top of this
   API. The CLI is the acceptance surface until then.
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
ctest --preset linux                                    # full gate
ctest --preset linux -R circsim                         # this arc's tests
python3 tools/smoke_circsim.py --cli build/linux/bin/circsim_cli
```

The stage-2 gate at land time: 581/581 ctest (70 doctest cases / 433
assertions in test_circsim, of which 17 cases / ~120 assertions are
test_symbols.cpp), smoke suite green including the tline quarter-wave
cases, ASan+UBSan clean, shadow audit clean over the circsim TUs (the
settings/ui class-struct mismatch and the spice devices_m4d
maybe-uninitialized finding are pre-existing, outside this arc's files).

## What exists (stage 2 file map)

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
  src/
    symbols.cpp   # the built-in table, now with specs + draw
    library.cpp   # the .csym reader/writer + the layering rules
    ...           # model/nets/format/erc/export (stage 1, threaded)
  examples/
    divider.csim, rc_step.csim      # stage-1 smoke decks
    tline.csym, tline_demo.csim     # the stage-2 library demo
  tests/
    test_symbols.cpp                # the stage-2 gate (no main; test_format owns it)
apps/circsim_cli/src/main.cpp       # run/erc/netlist/symbols, -S
tools/smoke_circsim.py              # the CLI smoke (incl. library cases)
```

## Not done (the natural next stages)

- **Stage 3 - the editor.** The ws::ui app: canvas over the model
  (place/move/rotate/mirror parts through undo commands, wire routing
  with the ortho constraint, the property grid over ParamSpecs, the
  Problems panel over Diags with select-and-zoom through the export
  back-map, the palette over `symbols()`). Everything headless it needs
  is in place: transform_pos, symbol_bounds, draw prims, specs,
  version() for cache staleness.
- **Schematic-side libraries of the model's own**: a .csim "libraries"
  field naming .csym files (the CLI's -S is the seam today; the format
  deliberately does not carry it yet - it would be a .csim version bump).
- **Net classes / ERC rules configuration** (power nets, unconnected
  pin allowances per symbol) - would ride ParamSpec-shaped data in
  .csym; nothing today.
- **Multi-page / hierarchical sheets** (subsheet ports) - not started.
