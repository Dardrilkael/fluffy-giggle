# circsim arc - plan, status, and continuation notes

This file is the **continuation memory** for the `libs/circsim` +
`apps/circsim_cli` work (the schematic-capture app growing on top of
`libs/spice`). If a future session needs to resume, start here, then
read `libs/circsim/CLAUDE.md` (the lived rules) and the SPICE arc's
`docs/SPICE_PLAN.md` for the simulator underneath.

## Status (2026-11-03)

Working tree clean. The arc lives on origin now (the earlier
"pushed never, no credentials" policy ended when the remote gained the
arc's history - the P1b font fix `f7ad8b0` and the Windows getenv fix
`a516992` ride on top of stage 4); stages 5 and 6 landed on fresh
fetches of that history.

| Commit    | Stage | What landed |
|-----------|-------|-------------|
| `ce7c14a` | 1     | Schematic model (parts/junctions/wires/labels, ids never reused), the `.csim` JSON format (unknown fields preserved, tolerant load, migrations, atomic writes), graph-based net building (reference edges + exact-position coincidence, crossings never connect), ERC (floating pins, V/L loops, I cutsets, ...), the ws::spice deck export with the back-map, `run_schematic` through spice's own front end, the `circsim_cli` run/erc/netlist subcommands, the smoke suite. |
| `812fc68` | 2     | The `.csym` symbol library: `SymbolLibrary` (built-ins + layered files, ci, later wins, override warnings), typed `ParamSpec`s (Text/Number/Choice - Number validated with ws::spice's grammar; defaults are new-part prefill, never injected at export), `DrawPrim` drawing primitives + `transform_pos` + `symbol_bounds`, ERC's stage-2 checks (bad_param, missing_param on non-optional emits, unused_param), `-S` on the CLI + the `symbols` subcommand (--json full-table dump), `tline.csym`/`tline_demo.csim` (a library-only T symbol simulated through the quarter-wave closed form). |
| `99ae630` | 3     | The editor: `apps/circsim_editor`, the ws::ui::shell app over the model - the schematic canvas (place/move/rotate/mirror parts, wire routing with the ortho constraint, labels; every edit an undo command with typing-run coalescing), the palette over `symbols()` with fitted mini previews, the property grid over ParamSpecs (plus the analysis-setup and directives editors), the Problems panel over Diags with select-and-zoom through the back-map (the new headless `libs/circsim/locate` seam, tested), the netlist view, and the simulate job over `run_schematic` (status row + summary toast). The bare editor keys (R/M/W/L/F/Del/Esc) bind on the resolver's PANEL rung; the text rule keeps them out of focused fields. |
| `c015926` | 4     | The results story: `libs/circsim/plot.hpp` - the headless axis math (nice 1/2/5 ticks, SI engineering labels, range padding, min/max decimation that keeps every bucket's y-extremes so spikes and envelope survive any pixel budget), 14 test cases; the editor's Results panel over it - one analysis at a time from a combo, per-signal visibility checkboxes with live legend readouts (the hover sample, else the last), the plot through the canvas layer (frame, grid + engineering tick labels, colored series, NaN-broken segments), a hover crosshair with per-series markers, `.op` tables and `.meas` as value rows, and CSV/JSON export riding the writers ws::spice already owns (the document's stem, the fs seam). The panel's draw is gated headless by `tests/test_results_panel.cpp` through the new `Editor::plot_canvas` seam (a CommandListCanvas asserting the recorded geometry: frame, grid, series point counts/colors/mapping, decimation envelope, crosshair, markers, the no-plot .op shape). The fix the visual gate caught: the stage-3 document factory's extension check compared the last 4 chars with ".csim" (5 chars) - never equal, so every File > Open and session restore was refused; now `ends_with`. |
| `8f42b2d` | 5     | The plot's hands: `plot.hpp` gains the manual-view math - `clamp_view` (a window slides back inside home, wider-than-home answers home, non-finite recovers to home), `zoom_range` (anchor-pinned, floored at home's span x 1e-6 before the float mush, clamped into home so a wheel spun out forever settles on the data), `pan_range` (slides and parks flush at the data's edge) - 3 more test_plot cases. The Results panel rides them: the wheel zooms both axes toward the cursor at the canvas's 1.15-per-notch factor (the plot registers its own wheel claim - the innermost rect beats the enclosing scroll panel's; the gui's one-frame settle is why the first notch after arriving still scrolls), a left-drag rubber band zooms to the box (an accent-washed rect with a dx/dy readout while it flies), dragging on an axis strip stretches that axis alone (right/up = in), the middle/space drag pans, a double-click resets, a right-click cancels and restores the press-time view exactly. Every gesture rebases from its press-time capture each frame; `ResultsCache` carries the per-table `PlotView` + the in-flight `PlotGesture` (a table switch, a fresh run, a document switch drop back to the auto fit); the decimation window slices to the visible x samples (one slack sample each side) so a zoomed-in tran spends its pixel budget on what it shows. |
| (this commit) | 6 | The second spine: `plot.hpp` gains the two-axis split - `PlotYAxis` + `signal_y_axis(name)`, the V/I classifier (one `re(`/`im(` wrapper unwraps when it opens at byte 0 and closes at the last byte; a leading `i(` - case-insensitively - answers Right; everything else answers Left, the conservative reading for a name the rule cannot vouch for), 2 more test_plot cases. The Results panel splits its y fit per visible company: the volts keep the left spine, the amps earn a right one - own padded home, own tick ladder (marks on the frame's right edge + labels in a widened right margin; no second set of grid lines, the left axis's rows carry the rails), own drag strip, own manual window. Every stage-5 gesture now works on three windows: the wheel zooms x toward the cursor and each y spine toward its own mapped anchor, the band commits its corners through each spine's own transform, the pan slides both y windows by the same pixels, the right strip's drag stretches the amp window alone, the cancel restores both. A table with no visible current column draws exactly the stage-5 shape (the margin, strip and ticks only exist when the second spine does). `PlotView` carries `y1_*` beside `y_*`; `PlotGesture` carries `zoom_axis` + the right spine's press-time capture. The legend rows grow a quiet V/A spine tag between the checkbox and the readout. |

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

The stage-5 gate at land time (this headless box still cannot
configure the ui tree - no vulkan/xkb dev packages - so the app-side
verification rode the hand build, see below): 608/608 ctest on the
`linux` preset (test_circsim 97 cases / 944 assertions, the 3 new
plot cases among them), the CLI smoke green, and the panel suite
hand-built with g++ against the linux preset's own archives (the
official ws_add_test TU list: the test + editor/document/
results_panel.cpp + the gui archive's gui/canvas/shell TUs + the r2d
stub + the font bake + input/app/platform's headless halves, linked
over prebuilt core/math/settings/jobs/circsim/spice) - 12 cases /
178 assertions, ASan+UBSan clean over the same 12 (the arc-owned TUs
instrumented; the prebuilt archives un-instrumented), the remaining
app TUs (main/canvas_panel/side_panels) syntax-clean. The band's dx/dy
readout is the one bit the headless gate cannot see: it rides the font
seam the bench leaves null. The editor-under-Xvfb visual pass stays
with the ui tree's owner.

The stage-6 gate at land time (the same headless box, the same
shape): 610/610 ctest on the `linux` preset (test_circsim 99 cases /
962 assertions, the 2 new classifier cases among them), the CLI smoke
green, and the panel suite hand-built with the stage-5 script - now
15 cases / 209 assertions (the 3 new two-spine cases and the stage-5
constants re-derived per spine: the mixed fixture's y home SPLIT, so
its old single-scale numbers retired), ASan+UBSan clean over the same
15, the app TUs syntax-clean. The spine tag's width and the right
labels' spacing are the bits the fontless bench cannot see; the
editor-under-Xvfb visual pass stays with the ui tree's owner.

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
                  # + clamp_view/zoom_range/pan_range (stage 5 - the
                  # manual view's clamp/zoom/pan over the home fit)
                  # + PlotYAxis/signal_y_axis (stage 6 - the V/I
                  # classifier behind the two-spine split)
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
  src/results_panel.hpp/.cpp # stage 4-6: the Results panel (plot + legend
                        # + value rows + export buttons; the axis math
                        # stays in ws::circsim/plot; stage 5 adds the
                        # wheel/band/axis-drag/pan/reset gestures over it;
                        # stage 6 splits the y fit per V/I company - the
                        # right spine's ticks/strip/window)
  src/main.cpp          # the App: shell wiring, commands, the run job
                        # (delivering to the Results panel), the export
  tests/test_results_panel.cpp  # the stage-4 app gate: the panel driven
                        # headless through the plot_canvas seam; stage 5
                        # extends it to the gestures (the wheel's slice,
                        # the band's geometry + commit, the axis drags,
                        # the pan, the reset, the cancel, the per-table
                        # drop); stage 6 extends it to the two spines
                        # (the split mapping, the right strip's drag,
                        # the v-only shape)
tools/smoke_circsim.py              # the CLI smoke (incl. library cases)
```

## Not done (the natural next stages)

- **Stage 7 candidates, editor-side:** a CSV/JSON save dialog (the
  export derives the path from the document today - deliberate v1),
  waypoint editing on placed wires, multi-select + marquee, an android
  port (the touch interactions need their own pass). The plot's own
  remaining wants: a zoom-to-fit-y-in-window after an x-only zoom
  (which per spine would answer "fit the visible span of MY signals"),
  and a cursor pair (two pinned readouts) beyond the drag-time dx/dy.
- **Schematic-side libraries of the model's own**: a .csim "libraries"
  field naming .csym files (the CLI's -S is the seam today; the format
  deliberately does not carry it yet - it would be a .csim version
  bump). The editor currently uses the built-in table only; a
  File > Load Symbol Library riding -S's loader is the cheap first cut.
- **Net classes / ERC rules configuration** (power nets, unconnected
  pin allowances per symbol) - would ride ParamSpec-shaped data in
  .csym; nothing today.
- **Multi-page / hierarchical sheets** (subsheet ports) - not started.

## Stage 17 (2026-11-03): the symbol set redrawn

Seven commits, one per fix, on the stage-17a render seam
(`libs/circsim/render.hpp`: `render_symbol` flattens a placed symbol
into world-space strokes - the editor paints them, the CLI's
`symbols --svg` writes them, the tests assert them; no parallel
transforms to drift):

| Commit | What landed |
|-----------|-------------|
| `2e457a4` | 17a: the seam itself, with characterization tests pinning the stage-2 warts it moved verbatim. |
| `0c2b24d` | 17b: `circsim_cli symbols --svg DIR` - one SVG per symbol plus a contact sheet, pins marked; the before/after harness (both sets under `download/symbol-redesign/`). |
| `4f06879` | 17c: the closed-path fix - Poly renders OPEN per symbols.hpp's contract (the resistor's zigzag wore a phantom closing segment); CommandListCanvas records the `closed` flag so tests can see it. |
| `3451a29` | 17d: the v/i sources move their pins to +-3 with real lead prims (the safety net's stub used to cross each body), line-drawn +/− polarity marks, the i arrow a filled head - which earns DrawPrim a `PolyFill` kind ("polyfill" in .csym). The tline demo's ground followed v.n (a coincidence connection); golden netlists unchanged. |
| `69a7431` | 17e: the ground's pin points up out of its bars (dir 90); the supply's bar and label sit ABOVE its node with the pin pointing down (dir 270). |
| `30a7540` | 17f: BJT emitter arrowheads (filled, direction-computed on the emitter stroke - out for NPN, in for PNP); the MOSFET bulk tees off the CHANNEL BAR's bottom instead of the source stroke's start (it read as a source-bulk short), the substrate arrow at the top of the drop, and the b pin points left (dir 180). |
| `27bf83d` | 17g: the code split (symbols.cpp -> symbols.cpp + symbols_table.cpp; test_symbol_draw.cpp its own TU) and the invariant suite - pin reachability, pin direction with its lead, non-degenerate fills, whole-grid pin offsets, over every built-in AND the tline example. First run: caught the INDUCTOR's phantom stubs (its pins ride the arcs' curves; the touch rule is geometric now - a pin on a curve is touched). |

The stage-17 gate at the final commit: 625/625 ctest on the `linux`
preset (test_circsim 113 cases / 1288 assertions), the CLI smoke
green (62 checks, golden decks byte-identical), the hand-built
ui-canvas suite (this box builds no ui tree) 18 cases / 250
assertions green, the shadow audit clean, the editor TUs
syntax-clean against the seam. The svg sheets pixel-probed at every
step: the phantom closing line, the source stubs, the supply's old
orientation and the bulk's old tee all verified gone.

## Stage 18 (2026-11-03): the silent joins get their dots

A wiring audit (a Python mirror of nets.cpp's rules, cross-checked
against the CLI netlist partition on every deck) proved two classes of
connection that NOTHING on the canvas showed and NO diagnostic named:
a wire whose path passes through a pin position picks the pin up
mid-segment (a 1k with both pins riding one wire netlists as
`R1 N001 N001 1k` - shorted, ERC 0/0), and a wire's interior vertex
riding another wire's path merges the nets (its own `R N002 N002`).
The editor cannot author either through the pointer (wire commits land
on pins/junctions/minted junctions), but part drags and hand-written
decks produce them freely. Fix #1 of the three options - report them,
paint them:

| Commit | What landed |
|-----------|-------------|
| `2965623` | 18a: `NetBuild::silent_joins` - nets.cpp records every pickup its coincidence rules make where no authored junction sits: `PinOnWire` (a pin on a wire's path interior, the part/pin/wire named) and `WireOnWire` (a wire vertex on another wire's interior), one entry per position, a pin entry upgrading a wire entry at the same spot. Landings (wire ends on named entities), crossings and pin-to-pin touches never report (the tline deck's grounding idiom stays clean). The editor paints the positions as auto-junction dots - the same dot the authored junctions wear - through `Editor::auto_junction_dots()`, one build_nets per edit cached against `Schematic::version()`, reset with the document. 5 test_nets cases; the probe on the three example decks answers 0 joins each, on the two adversarial decks exactly the proven shorts. |
| `b12c688` | 18c: the commit-side zero-length guard fired for Free landings only, so a second click on the START entity (the same pin, junction or wire-point) committed a degenerate self-loop - identical endpoints, an empty folded route, and for a wire-point start two junctions minted at one position - while the preview drew nothing for the case. The guard now checks the position alone, whatever the landing kind; nothing is lost (the same position is already one net through the coincidence rules) and the draft stays armed. 4 `test_canvas_panel` cases drive the real two-click draft through a scripted InputFrame; against the pre-fix code three of them fail. |
| `a546d84` | 18b: the wire tool's snap was pin -> junction -> grid - never a wire's body. A click near another wire landed a hair off the grid, the new wire's endpoint sat at the wrong position, nets.cpp's exact `on_polyline` missed the connection, the wire looked connected and was not. The fix: `Editor::hit_wire_point` projects the cursor onto each wire's path, snaps the projection to the fine grid, verifies the snap is on the segment (exact `on_segment` - the same check nets.cpp runs), and falls back to the nearest path vertex when the grid snap would drift off. `snap_wire_target` adds `Kind::WirePoint` after pin/junction, the wire-tool start arms on it (the start's junction mint deferred to commit so a cancel leaves nothing), the commit mints a junction at the snap point and binds the new wire's end to it - the standard EDA T-tap. 5 `test_canvas_panel` cases pin the geometry. |

Stage-18 candidates that did NOT land (the audit's other two options):
ERC warnings on un-authored pickups (the data is there now -
`silent_joins` is the list), and an on-wire snap that mints a junction
when a wire endpoint or part drag lands on another wire's path.

## Stage 19 (2026-11-03): the editor bug sweep - toasts, undo, picks, routes, buttons

A full adversarial review of `circsim_editor` (nine numbered bugs, four
wire-tool UX gaps, a structure/quality list) audited line-by-line
against the code; every claim checked out or revealed something worse.
One task per commit, on the `stage19-editor-fixes` branch:

| Commit | What landed |
|-----------|-------------|
| `2b5fd9c` | 19a: the toasts borrow their text (Toast::text is a plain `const char*`), but a `static std::string holder` served every toast - two toasts in one frame showed one text, and a reallocation dangled the earlier one. Each `Editor` now owns a `std::deque<std::string>` (stable addresses; the 16-string cap only ever retires bytes whose 8-slot toast is long gone). |
| `9a30ebd` | 19b: the drag release restores the press position before `push_move_*` (the live drag had left the entity at the landing, the command read `from == to`, undo was a no-op); the drag-landing formula (live, release, feedback) is one `drag_landing()` helper; a modal veil freezes the wheel and the pan (only the tools checked `claims_open()`); an armed pan keeps the pointer off-panel; the canvas scale resets after `pop_transform()`. |
| `543a630` | 19c: `route_wire`'s collinear fold read a REVERSAL as a continuation - a pin at (0,0) facing +x with a target at (-5,0) lost its lead and the wire ran straight back through the part body. The fold now extends only a continuing leg (`along > 0`); directions normalize once at the top (360 used to fall through `start_horizontal`'s raw 0/180 test and turned the L the wrong way). |
| `e7afbbf` | 19d: `hit_any` returned the FIRST wire in range (the nearest-tracking was dead), tested parts' AABBs before wires (a wire crossing a part's box was unpickable) and picked the first part while the last one draws on top; the label reach was a fixed world unit (400px at max zoom). Now: junctions, then the nearest wire, then the LAST part whose box holds the point, then labels with a zoom-scaled reach (`hit_any` grew a `label_radius` parameter). `hit_wire_point` accepts on the SEGMENT's distance - measuring the snapped candidate missed a cursor riding the wire between grid lines at high zoom. |
| `416a755` | 19e: the pick ring (0.35 world) and the dots (0.14 world) agreed with the 9px pick only near zoom 26. The ring IS the pick radius now; the dots never shrink under 2px on screen. |
| `fcdcb9b` | 19f: the wire tool keeps the conversation - a click on FREE grid starts a draft (the commit side always supported free landings; the start junction mints at commit), every commit CHAINS (the next draft starts at the landing - a multi-segment net is one run of clicks, no W press per segment), the first right click (or Esc) cancels only the draft and the second disarms, and Shift at the start flips the L's first leg (recorded at start so preview and commit route identically). The commit revalidates its start anchor (bug: a draft surviving undo/deletes could commit a dangling endpoint) and cancels with a word when the anchor died. |
| `9ca8f4d` | 19g: waypoints are absolute, so a moved endpoint turned the adjacent wire leg diagonal. The move commands now carry `WireRoute` cargo: every ORTHO wire attached to the moved entity re-routes (straight/one-bend wires get a fresh `route_wire` L; multi-bend wires keep their Z - only the adjacent waypoint slides), the push captures each wire's current waypoints as the undo shape, and undo restores both the entity and the exact former bend. The junction-dot cache sits still through a drag's per-frame bumps (one rebuild per edit, not per frame). |
| `408934a` | 19h: the panels read `mouse_down[1]` as "middle" and `mouse_pressed[2]` as "right" - GLFW's indices are 0 left, 1 RIGHT, 2 MIDDLE, and libs/input passes them straight through: panning rode the right button and the cancel the middle one. Named `kMouseLeft/kMouseRight/kMouseMiddle` constants (the GLFW truth) across both panels and both test harnesses. |
| `7b6bd53` | 19i: the magic numbers get names (`kZoomMin/kZoomMax/kZoomDefault`, `eps_eq`'s `kEps` default, `kZoomStep`); the wheel zooms by `pow(kZoomStep, scroll_dy)` clamped, so trackpads stop jumping whole notches; `document_changed` clears the Properties field buffers, stamp, selection keys and analysis index (a new document could show the old one's typing); the ghost footprint transforms each corner once; the local `floor` no longer shadows `::floor`; the `p.pos.x - 0.4f` mixes went double; the header comment admits the no-document `gui::text` exception. |
| `7494304` | 19j: the 260-line `handle_pointer` and the 360-line `draw_canvas_panel` split by concern - wheel/pan, drag live/release, per-tool presses (the wire start/commit their own functions), right-click staging; the draw per layer (grid, snap feedback, wires+junctions+dots+labels, parts, target ring, preview, ghosts). The wire tool's `snap_wire_target` runs ONCE per idle frame (the pointer handler's press edges and the draw halves share it; a click frame recomputes for the draw - the click may have changed the schematic) instead of up to three times per frame. `part_bounds` grew a `const Part&` overload so the draw and hit loops stop re-finding by id. |

The stage-19 gate at `7494304`: 944/953 ctest on the full workspace
(the 9 audio cases fail on this box for lack of an audio device -
pre-existing and unrelated), `test_circsim_editor_canvas` 22 cases /
125 assertions green against the split file, `test_circsim_editor_panel`
15 cases / 209, `test_circsim_editor_palette` 4 / 25, the shadow audit
clean (114 TUs), the conversions audit clean on every circsim/editor
TU (the physics/math backends' pre-existing sign-conversion noise
untouched), the uninit and class-struct audits at their known
pre-existing single findings (spice devices, gui_context).

Not landed from the review: the Editor god-struct split (a whole-stage
refactor touching every seam; the per-tool/per-layer function split in
19j is its first slice), and re-routing wires that pass THROUGH a
moved junction as a T (the silent-join dots already show the lost
join).

## Stage 20 (2026-11-05): the stage-19 refactor's regressions put back

The squashed "Stage 19 editor refactor" (`2cc347d`, pre-existing working
tree) regressed seven certified stage-19 behaviors - the canvas gate
fell from 22 cases to 13 passing - and the follow-up cache commit
(`54c8a54`) added an eighth bug of its own. Every fix below restores
the exact stage-19j semantics the suites pin (audited against
`7494304`, the last certified-green state), on the refactored 4-arg
APIs:

| What | The bug | The fix |
|-----------|---------|---------|
| `route_wire` | the refactor's candidate/spike picker picked the jog-around route, and its fold read a REVERSAL as a continuation - the 19c lead-out corner was lost again (a pin facing +x with a target behind it folded its lead away) | back to the single-L route + the `along > 0` fold guard (a reversal is collinear but never folds) |
| `hit_wire_point` | acceptance measured the distance to the SNAPPED candidate - the 19d fix was gone, a cursor riding a wire between grid lines at high zoom missed (pick < half a cell) | accept on the SEGMENT's distance, return the grid-aligned candidate |
| `hit_any` | parts were tested before wires (a wire crossing a part's box was unpickable), the first part won instead of the topmost, and the label reach was a fixed `radius + 1.0` world units | junctions, nearest wire, LAST part, labels last - with the zoom-scaled reach (`radius + 6px/zoom`) |
| right click | read `mouse_pressed[2]` - GLFW's MIDDLE, not RIGHT - so the cancel never fired on a real right click, and when it did (middle) it killed draft AND tool in one press | `kRight`, and the 19f staging: draft first, tool second, selection last (the Esc command's own ladder) |
| wire starts | a click on FREE grid was ignored at the start (the 19f free-start was gone); `a_free` was missing from WireDraft so a minted start could not skip anchor revalidation | free starts arm the draft (`a_mint`, `a_free`, `a_dir = -1`), the junction mints at commit, and `a_free` answers `true` to the validity check |
| wire commits | every commit DISARMED the tool (the 19f chain was gone - every segment cost a W press); the per-frame draft cancel ate the commit click and re-armed a fresh draft in the same press | the commit chains the next draft at the landing endpoint, and the dead-anchor check moved back into the commit (a toast cancels it) |
| move re-route | `attached_wire_routes` was deleted - a moved part/junction left its attached ortho wires' legs DIAGONAL (waypoints are absolute), the 19g fix gone | restored, and the drag release lands it: entity to the landing, routes read there, entity restored to the press position, `push_move_*` carries them |
| `part_bounds` cache | a version change only re-stamped the ONE entry asked for - every other part served bounds from before the edit forever (a dragged part's highlight and refdes text lagged at the pre-drag position while the symbol moved) | a new version retires the WHOLE cache |
| wheel/pan | the wheel and the pan lost `claims_open()` (a modal veil no longer froze them), the pan armed anywhere on screen (a middle-drag in another panel panned the schematic), and the wheel jumped a whole notch per frame - the 19b/19i fixes gone | claims-gated, armed inside the panel only (held keeps it), `pow(1.15, clamp(dy, -4, 4))` per frame |
| `document_changed` | the 19i Properties reset was gone - a new document could show the old one's typing | `fields`/stamp/selection keys/analysis index reset again |

Also fixed while in the tree: `circsim_cli/svg_dump.cpp` printed a
`std::filesystem::path` with `%ls` unconditionally - right on Windows
(wchar_t), a `-Werror=format=` error on POSIX (char); the error print
is now per-platform.

The stage-20 gate: the linux preset builds clean (the GCC 14
`-Wmaybe-uninitialized` false positives on default-constructed
`AnalysisSpec` variants in three circsim test TUs are suppressed with
local-only pragmas, MSVC unaffected), `test_circsim_editor_canvas`
20/20 (102 assertions), `test_circsim_editor_panel` 15/15 (209),
`test_circsim_editor_palette` 4/4 (25), and the full ctest 1008/1008
including both CLI smokes.

## Stage 21 (2026-11-06): the UI/UX refinement pass

The editor's chrome catches up with its content (the commits' own
record; the plan doc catch-up lands with stage 22):

- **Toolbar icons** - the chrome actions (new/open/save, undo/redo,
  delete, rotate, mirror, wire, erc) wear the built-in icon set's
  standard glyphs with name + chord tooltips; `circuit.run` keeps its
  text button (Simulate is the one act that deserves the word).
- **Fuzzy palette search** - the search box matches on word starts,
  case-insensitive, across name/key/group ("res" finds "resistor",
  "vsrc" finds the sources); Enter arms the first match.
- **The right panel tabs** - Properties / Netlist / Simulation instead
  of the vertical stack; each concern gets the full height, the
  selection, the deck and the setup never scroll off under each other.
- **Shortcut keys** - Ctrl+Z/Ctrl+Y (redo keeps Ctrl+Shift+Z as the
  alias), R/M/Del bind on the panel rung through the resolver's
  text-editing rule (a focused field keeps its letters).

## Stage 22 (2026-11-07): the typography & text rendering pass

The editor renders its own text, so it owns the hierarchy - three
rungs, one theme token each, every one covered by a suite:

- **The font hierarchy** - `gui::heading()` is the section break: the
  theme's `font_strong` face at its own baked size on a row taller
  than a body row. The editor bakes the semibold at 18 px against the
  16 px body ("bold and slightly larger"), and the palette's `tools`,
  the Simulation tab's `schematic` and `analyses (deck order)` wear
  it. `font_strong` finally has its first consumer; a null strong face
  falls back to `font` (the one-font rule), fontless to the taller
  bar.
- **Mono for data** - JetBrains Mono v2.304 (OFL) is vendored and
  subset beside the Inter faces, embedded through `embed_font.py` and
  served by the new `r2d::load_default_mono_font()` - the same bundled
  answer the UI face gets, no system font walk (the pre-pass walked
  `consola.ttf`/`DejaVuSansMono.ttf`, so a box without them silently
  lost its columns; `WS_MONO_FONT_PATH` stays as the override). The
  Netlist tab's deck lines and the Results measurements already wore
  the mono face; the **Problems rows** join them (a finding line is
  code-ish text - refdeses, net names, `[E]/[W]/[N]` tags - and reads
  easier with honest columns), through the swapped-theme idiom the
  panels already use for fades. The netlist draw's unconditional
  `*theme.font` deref is gone too: a fontless theme now falls back to
  plain rows instead of UB.
- **The muted rung** - `text_muted` is the theme's own token for the
  chrome hints that must not compete with content: the brief pinned
  `#858585` on the dark stock, the light stock picks its own gray
  `(120,122,128)` because the dark one cannot clear the light faces.
  The text field's placeholder and the inactive tab's label consume it
  (both were alpha dims of body text before); the class is WCAG's
  placeholder/inactive-component exemption, so the pair checks sit at
  the 3:1 component ratio against panel, control and bar faces - the
  `text_on_accent` precedent - and the token round-trips through the
  theme json like every face.

The build fixes the pass needed on the posix box: imgui's GLFW
backend compiles with `GLFW_INCLUDE_NONE` (it needs GLFW's types,
never the GL headers `glfw3.h` pulls by default - the workspace's own
`window_glfw.cpp` already spelled it), and the autorun probe's MSVC
pragma gains its `#if defined(_MSC_VER)` guard.

The stage-22 gate: the full ctest 1058/1058 - `test_ui_font` 8/8 (the
mono table's uniform-advance contract), `test_ui_theme_tokens` 10/10
(the muted rung's pinned gray, its 3:1 pairs, the json round-trip),
`test_ui_gui` 58/58 (the heading's strong face and taller row, the
placeholder's muted color, the inactive tab's), and
`test_circsim_editor_side_panels` 15/15 (the palette heading through
the strong face, the problems rows through the mono face, the netlist
tab's fontless survival).

## Stage 23 (2026-11-08): the interactive canvas - the run paints back

The canvas stops being a passive drawing the moment a run lands. Three
layers, one seam each:

- **The i(Dn) probe (ws::spice)** - the save list grows terminal-current
  columns: a device without a branch row that can evaluate its current
  at the accepted solution (the diode - Shockley at the terminal
  voltages, the Rs>0 path reading the series element) answers
  `probes_current()`/`terminal_current()`, and `default_save_list`
  carries `i(D1)` in .op/.dc/.tran (.ac opts out - a DC probe cannot
  answer the small-signal system). The Lambert-W test pins it against
  the closed form and the series loop's own i(V1).
- **The LED (ws::circsim)** - the 13th built-in: the diode's SPICE
  identity plus a display-only `color` Choice (spec'd, never emitted -
  the deck cannot tell an LED from a diode) and two emission
  arrowheads. `led_demo.csim` ships the acceptance picture: 5 V over
  220/680/2.2k into red/yellow/green - 15/5/1.6 mA of glow, the smoke
  suite's three brightness steps.
- **The canvas (apps/circsim_editor)** - `Editor::net_cache()` shares
  one build_nets per edit between the auto-junction dots and the new
  wire coloring; the mode toggle (top-right, canvas-drawn: Plain /
  Nets / Voltage) picks the ink. Nets hashes each net's NAME onto the
  series wheel the Results plot already wears (ground a fixed slate);
  Voltage rides the run's node voltages through a sqrt-spread
  blue-gray-red ramp with a legend; unknown nets honestly fall back to
  the plain ink (a missing reading is not a zero reading). The LED
  glows: the probe current maps through a 20 mA-full sqrt brightness
  into three halo discs in the part's own color, eased ~120 ms. A
  .tran run grows the transport strip - play/pause, a scrub bar, the
  format_eng readout - whose playhead the voltages and glows follow:
  a delivery parks it at the run's end (the settled state), one press
  sweeps the whole transient across the canvas in ~4 s, looping. The
  run's net build and back-map now live in the Results cache (the
  Problems cache keeps its copies for locate), so an ERC pass can
  never dim the canvas.

The stage-23 gate: the full linux ctest 633/633, the circsim and
spice smokes green (the LED demo's three steps among them), and the
editor's headless suites over the r2d stub - canvas 33/33 (the wire
modes' ink contract, the ramp reference, the glow discs, the
transport's park/play/scrub), side panels 34/34, results 16/16,
palette 5/5 (the LED joins Semiconductors).

## Stage 24 (2026-11-10): the orphan devices join the palette

ws::spice always parsed `E F G H B S W T K` - nine device letters the
built-in symbol table never carried, reachable only through a hand-
written `.csym`. This stage promotes them all to built-ins and adds the
one part the engine could never express from a single symbol: the
2-winding transformer.

- **The nine symbols** (`symbols_table.cpp`): `e`/`g` (VCVS/VCCS, four
  pins in card order OUT+ OUT- IN+ IN-, LTspice-style with the sense
  pair stacked left and the output right, +/− marks on the voltage
  output, an arrow on the current one), `f`/`h` (CCCS/CCVS - two pins
  plus the controlling V source's NAME, both params because the card
  grammar spells the source before the gain), `b` (behavioral, one of
  `vexpr`/`iexpr` - both empty or both filled rejects loudly at parse
  and maps back onto the part), `s` (voltage switch: the contact runs
  horizontally, the control pair hangs below like relay terminals) and
  `w` (current switch), `tline` (promoted from `tline.csym` verbatim -
  same geometry, same `Z0=`/`TD=`; `tline_demo.csim` now runs with no
  `-S` at all) and `k` (the coupling: deliberately PINLESS - it names
  two inductors by ref and touches no net; a pinless part flows through
  nets/ERC/export untouched because those loops only ever iterate pins).
- **The transformer** (`xfmr`): one part, three cards - `L<ref>p`,
  `L<ref>s` and the `K` that names them. The new `CardEmit` layout
  (`symbols.hpp`) spells a multi-card symbol declaratively: per card the
  device letter, the name suffix, the pin indices it connects and its
  own param tail; `name_siblings` lets the coupling name its siblings,
  so the K card never goes stale when the ref changes. Export derives
  every card name from the part's ref (`sub_element_name`, following
  `element_name`'s own rules), claims them all through the same dedup
  (two transformers named K1 answer K1/K1_2; a user inductor named L1p
  lands on L1p_3), and maps all three lines back onto the one part so
  locate and the probe tool highlight the symbol. The `.csym` spelling
  (`"cards"`) round-trips through `library.cpp`, so a library-defined
  multi-card symbol is first-class too. ERC reads a `CardEmit`'s
  non-optional emits as required params (the transformer's lp/ls/k).
- **The palette** grows a Switches shelf (`S`/`W`); the controlled and
  behavioral sources join Sources, the line/coupling/transformer join
  Passive - `palette_category` maps by letter, and a `.csym`'s own
  `category` field still wins.
- **Six examples** (`libs/circsim/examples/`): `controlled` (E at x10
  across a decade sweep), `current_controlled` (F at 2×5 mA = 10 V and
  H at 100 Ω×5 mA = 0.5 V), `bsource` (`V=3*v(in)`), `switch` (the S
  transfer curve over a .dc sweep, `.model` in the directives),
  `transformer` (the xfmr answering k·sqrt(ls/lp) = 1.98) and
  `coupled` (two inductors + the pinless k part at 0.9·sqrt(10)). The
  smoke suite drives all six through the real CLI with closed-form
  assertions.

Two honest engine contracts the examples teach: an ideal V directly
across an L is the graph check's own V-L-loop rejection (the primaries
carry a series Rs, like ws::spice's own xfmr test), and a W reads a
DEDICATED 0 V sense source's branch current (a delivering source's own
`i()` is negative in the SPICE convention).

The stage-24 gate: the full linux ctest 735/735 (test_circsim 145
cases / 1993 assertions - the new battery: golden decks, closed-form
runs, the ERC requireds, the collision dedup, the built-in tline
quarter-wave), the circsim smoke green with the six new examples, the
spice smoke untouched.

## Stage 25 (2026-11-10): the built-in model library

Hand-writing a `.model` line in the directives block was the only way
to give a semiconductor a part number - "that's not a product". This
stage ships the shelf: 25 curated cards the property grid offers as a
picker under the Model field, and export auto-emits when a part names
one. The user never touches the directives for a stock part again.

- **The table** (`models.hpp` / `models_table.cpp`): diodes (1N4148,
  1N4007, BAT54, the 1N4733 zener, three LEDs), BJTs (2N2222, 2N3904,
  BC547, 2N3906, BC557), MOSFETs (2N7000, IRF540, BS250, IRF9540 -
  Level-1 cards tuned at the default W/L so the part's own L=/W=
  scales the channel), JFETs (J2N5457/J2N5459, 2N5460), switches
  (ideal, TTL) and op amps (ideal, uA741, LM358, TL071). Every card
  sticks to the parameter sets the engine's `apply_model` actually
  IMPLEMENTS - the accepted-but-ignored vendor tail is deliberately
  absent, because a library card is a promise, not a wish. The op amps
  are linear macromodels (E/R/C `.subckt`s: differential Ri, the GBW
  pole, Avol, Ro - the supplies are sense pins), so no `.model` rides
  inside a subckt body.
- **`SymbolDef::model_kind`** (`symbols.hpp`, `symbols_table.cpp`): what
  KIND of card the `model` param names - "d"/"npn"/"pnp"/"nmos"/"pmos"/
  "njf"/"pjf"/"sw", plus "opamp" for the new five-pin X symbol (out,
  in+, in-, v+, v-; the triangle with supply stubs, every pin a
  whole-grid endpoint with its lead into the body). The field
  round-trips through `.csym` (`"model_kind"` in `library.cpp`, saved
  only when set so classic files stay byte-identical), and unknown
  kinds ride as-is.
- **The picker** (`side_panels.cpp`): a combo of the symbol's kind's
  cards directly under the Model text row - picking one lands the
  card's NAME through the same seam the text row rides (one undo step
  per pick), and the hints below tell the story: a shelf card's
  description, a wrong-kind card's ERC story, a custom name's pointer
  at the directives, an empty field's invitation. The hand-typed text
  row stays first-class - the escape hatch is a feature.
- **Export** (`export.cpp`): a part naming a library card appends the
  card's text (multi-line for the `.subckt`s) after the directives and
  before the cards, attributed through a new `Origin::Kind::Library`
  whose id names the FIRST part that referenced it - a bad card locates
  onto the pick that brought it in. Two rules keep it honest: the
  DIRECTIVES WIN (a user-defined card of the same name is skipped, so
  the override stays an override) and emission is by NAME, not kind (a
  wrong-kind pairing still decks, and ws::spice's own type check
  rejects it mapped back onto the part).
- **ERC** (`erc.cpp`): `circsim.erc.model_kind` - a diode naming 2n3904
  is a library card of the wrong kind, said HERE at the part in the
  picker's own vocabulary instead of dying at a deck line mid-run. A
  name the library does not know stays the directives story (a custom
  model, the engine's to check, as always).
- **ws::spice**: one elaborator fix the op amps forced - ground stays
  GLOBAL through a `.subckt` boundary (SPICE3's own rule; the prefixing
  used to strand a floating `X<n>:0` that gmin solved to garbage,
  silently).
- **CLI**: `circsim_cli models [--json]` lists the shelf (name, kind,
  title, description; the JSON dump carries the full card text) - the
  same listing the picker rides.
- **Two examples** (`bjt_amp.csim`, `opamp_amp.csim`): the fixed-bias
  common-emitter stage (v(vc) = 3.969 V; β_forced = Bf·(1+(Vce−Vbe)/
  Vaf) = 215.13 holds to 1e-3) and the non-inverting stage (v(out) =
  10·Avol/(10+Avol) = 9.99950 exactly - the FINITE-gain answer, not
  the wishful 10). Both run with an EMPTY directives block - the whole
  point.

The stage-25 gate: the full linux ctest 778/778 and editor 1273/1273
(test_circsim 163 cases / 3169 assertions - the new battery: the golden
library deck, the directives override, the wrong-kind mapping, the
opamp closed form, the 25-card sweep that runs EVERY card through its
own device via ws::spice's public path, the ERC kinds, the symbol
shapes, the `.csym` round-trip), the circsim smoke green with the two
new examples and the `models` listing, the spice smoke untouched at
110/110.
