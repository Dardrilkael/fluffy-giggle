# apps - CLAUDE.md

The executables. Every app includes only `ws/...` headers and links only
`ws::` targets (the workspace rule, root CLAUDE.md); the per-app
CMakeLists only wire ws libraries together.

- `apps/playground` - smoke-test app for the shell.
- `apps/wavplayer` - headless ws::audio smoke app (desktop console app):
  plays a synthesized arpeggio (or the wav passed as argv[1]) on the
  default device.
- `apps/netdemo` - headless ws::net example: one EventLoop running framed
  TCP over loopback, a UDP round trip and LAN discovery, then an explicit
  teardown with loop work still in flight.
- `apps/netchat` - terminal chat over ws::net: running copies find each
  other via LAN + loopback discovery beats (the smaller listen port
  dials, one framed TCP connection per pair); lines typed in one terminal
  print in all the others.
- `apps/chatter` - netchat's protocol behind a gui (built for Android,
  runs on desktop too): the same discovery beats and HELLO/SAY frames, so
  phones and terminals share one chat; input is a tap-phrase palette.
- `apps/bench_physics` - headless ws::physics throughput bench (desktop
  console app): steps a falling-boxes scene and prints per-tick wall-clock
  cost while awake and after sleep, per dimension and thread config.
- `apps/bounce` - the three-lib composition example (windowed app, desktop
  and Android; its APK is `tools/package-android.ps1 -App bounce`):
  a ws::physics2d pit of clickable boxes/circles drawn through ws::ui with
  render interpolation, a ws-gui panel, and ws::audio thuds whose pitch and
  gain follow each impact's speed. On the phone the gravity sensor steers
  the pit (tilt to tumble; a dedicated NDK sensor thread feeds
  `set_gravity`, which wakes the sleepers for you).
- `apps/ui_gallery` - the ws::ui::gui widget set's acceptance view (windowed
  app, desktop and Android; its APK is `tools/package-android.ps1 -App
  ui_gallery`): one screen with every widget family on it - a menu bar and
  a right-click/long-press context menu on the chrome strip, a scrolling
  page of sections (theme - the stage 10 story: the dark/light switch
  with the live OS answer and an adopt button, the accent picker whose
  derived faces wear immediately, font-px and scale sliders that re-bake
  the font (a size change is a new font), the growing font stacking faces
  live with lazy glyphs landing per frame, a swatch grid of every chrome
  token, and a live contrast readout; buttons, choices, sliders, text,
  lists, a table, tabs, a live job row - the ws::jobs bridge: a counting
  worker under a progress bar with a cancel button, a failure that posts
  the error toast, all completions landing on the main thread between
  frames - the overlays, the disabled scope, custom look - the stage 11
  story: one slider value and one text buffer, each drawn three ways: the
  composed widgets, a custom paint over the stock behavior, and a widget
  the app owns outright off the raw state() seam), the skins page (the
  ui-skin plan's P7 acceptance: the SAME widget board under three looks
  side by side - stock dark, stock light, and one custom flat/square
  skin built from a token override plus a painters table replacing three
  rows, no fork of the library; the behaviors read identical under every
  look), and the platform
  page (the stage-12 story: the native file dialogs and the recovery
  ask each running on a ws::jobs worker - they block while the user
  decides - with every answer marshalled back through the frame
  drain; the recent-files MRU with pin/unpin, persisted through the
  settings seams; the clipboard round trip over text, a typed payload
  and a 2x2 image; the file-drop log fed by ws::input's
  FileDroppedEvent; the autosave crash-recovery pair whose startup
  probe asks its yes/no through the native message box, with the
  manual recover/discard pair as the fallback; the window title
  composed live through ws::platform::window_title and handed to the
  OS by ws::ui's set_title, echoed as a line so Android - whose host
  no-ops the seam - sees the composition too; the cursor-shape
  radios over ws::ui's set_cursor; and the single-instance grab +
  open-with argv parse in the desktop main), the commands page
  (the stage-13 story: one ws::app command registry driving a
  generated menu bar, a toolbar and the Ctrl+Shift+P palette - the
  hints are the live bindings, so a rebind renames every column at
  once; the chords resolve through the shortcut resolver before the
  frame begins and stand down while any overlay owns the keyboard;
  the keymap editor rebinds the live table over the persisted keymap
  file, so a rebind survives a restart and reset restores the
  declared chords; and an undo demo whose every edit - typing, an
  append, a clear - is a command pushed on the ws::app undo stack
  through a revert-then-push funnel that keeps the stack the sole
  executor, a typing run coalescing into one step, the Undo/Redo
  labels host-composed from the step names, the dirty flag and the
  save point), all five modal-dialog
  button sets, the toast store's whole story, and a status bar the page
  keeps clear of. No assets: the font is the system face resolved at the
  platform boundary (like playground's), so Windows and Android build it
  from one source unchanged.
- `apps/spice` - the ws::spice CLI (desktop console app): the thin
  parse -> simulate -> write shell over the public API (`run`/`check`
  subcommands, `-o -`, `--format raw`, `--keep-going`,
  `--strict-meas`, `--werror`); argv, file I/O and exit codes only.
- `apps/spice_example` - the smallest possible ws::spice embedding
  (desktop console app): a hardcoded RC-transient deck through
  read_deck -> build_deck_ast -> run_deck, the v(out) column and the
  .meas result printed to stdout - the C++ front door in 15 lines.
- `apps/circsim_cli` - the ws::circsim front door (desktop console
  app): `run file.csim` loads a schematic, exports it to a ws::spice deck and
  runs it (--analysis overrides, CSV/JSON out), `erc` lints the
  schematic (floating pins, V loops, I cutsets, bad params...),
  `netlist` prints the exported deck, and `symbols` lists the effective
  symbol table (--json dumps it whole; --svg DIR renders every symbol
  through ws::circsim's render seam - one SVG per key plus a contact
  sheet with pin markers, the stage-17 headless render and the
  before/after harness for the symbol redesign). `-S lib.csym`
  (repeatable, later wins) layers .csym symbol libraries over the
  built-ins for every subcommand. argv, file I/O and exit codes only.
- `apps/circsim_editor` - the ws::circsim stage-3/4/5 editor (windowed,
  desktop and Android; its APK is `tools/package-android.ps1 -App
  circsim_editor`): the schematic capture app built ONLY from AppShell
  over libs/circsim - six panels (the Schematic canvas through ws::ui's
  canvas layer: place/move/rotate/mirror parts with every edit an undo
  command, wire routing with the ortho constraint, net labels; the
  canvas floor and dot grid ride the canvas layer's stage-16 grid
  system - one GridSpec (Schematic::grid as the base) shared by the
  pixel-snapped draw and the snap, the x5 level ladder fading its
  minors so zooming never pops a mark, the lattice brightening ~150ms
  while a tool is engaged, snap feedback (a ring on the wire tool's
  pin/junction target, an accent dot on the landing intersection) and
  Shift as the coarse major-lattice snap for placements, part drags
  and labels - wires, pins and junctions always snap fine; the canvas
  paints ws::circsim's reported silent joins as auto-junction dots
  (stage 18: pins a wire's path picked up mid-route, wire vertices
  T-riding another wire - the same dot the authored junctions wear,
  Editor::auto_junction_dots caching one build_nets per edit against
  Schematic::version(), never per frame - the dots now carry each
  join's net id and Editor::net_cache shares that one build with the
  wire coloring); the canvas is INTERACTIVE after a run: the wire-color
  mode toggle (top-right, canvas-drawn) picks Plain / Nets / Voltage -
  Nets hashes each net's NAME onto the shared series wheel (the same
  eight the Results plot wears; ground is a fixed slate), Voltage maps
  the last run's node voltages through a sqrt-spread blue-gray-red
  ramp with a legend (and honest fallbacks: no reading, no run, a
  stale run - the plain ink, never a false zero), and the LED symbol
  glows: its i(<element>) probe column (ws::spice's terminal-current
  save) maps through a 20 mA-full sqrt brightness into three halo
  discs under the strokes in the part's `color` param, eased ~120 ms
  so a light reads as a light; a .tran or .dc run grows a transport
  strip (bottom center: play/pause, a scrub bar, the axis's own
  format_eng readout - seconds for a .tran, "V1 = 3.4V" for a swept
  .dc source) whose playhead the voltages and glows follow - a
  delivery parks it at the run's end (the settled state), one press
  sweeps the whole run across the canvas in ~4 s, looping (the
  playhead's units are the sweep table's own axis; a frequency table
  never drives the clock - its log decade grid would lie under a
  linear track, so the Results transport row binds to the same one
  table the canvas strip does);
  the panel's canvas transform composes the exact inverse of its w2s
  (viewport center, zoom with the y flip, -center), so the world
  lands on the same pixels the grid marks at every pan/zoom; the
  Palette over `symbols()` with fitted mini previews and the fuzzy
  word-start search ("res" and "rstr" both arm the resistor; a
  mid-word letter never opens a match, so "res" leaves the sources
  alone); the palette's five fixed shelves are Passive / Sources /
  Semiconductors / Switches / Power (stage 24 grew Switches for the
  S/W symbols and sent E/F/G/H/B to Sources and T/K/xfmr to Passive -
  `palette_category` maps by SPICE letter, a `.csym`'s own `category`
  field still wins); the Select tool's GROUP edit: a press on empty space arms
  the box-select (the drag sweeps a dashed veil, the release lands
  every part/wire/junction/label it holds - Shift extends instead of
  replacing; a plain click on empty space still clears, the degenerate
  box holds nothing), Shift+click toggles one entity into the set,
  dragging any member moves the whole set by one grid-snapped delta
  (relative geometry never distorts) as ONE undo step with the
  attached ortho wires re-routed, Ctrl+C/V/D copy/paste/duplicate on
  the panel rung (a focused text field keeps its own chords), the
  clipboard crossing documents with fresh minted ids, renumbered
  refs (R1 pasted beside R1 becomes R2), lattice-aligned offsets and
  boundary wires carried as minted junction stubs, and Delete takes
  the set as one step (junctions die with their wires, the lone
  rule); the Probe tool (P): a click on a wire, junction, pin or
  label toggles that net's v(net) curve in Results, a click on a
  part BODY toggles its i(element) current - the Results panel
  switches to the table carrying the signal, the legend's checkbox
  state and the pulse answer with it (the Results panel's own
  visibility flags, driven from the canvas); the Properties panel as a three-tab strip - the selection's
  rows, the Netlist view, the Simulation setup (the analysis list - op,
  dc, tran, ac, noise, each with its own field rows; the run temperature
  row (blank = SPICE's 27°C, exported as .options temp=); and the
  directives editor, always reachable instead of hidden behind an
  empty selection);
  the Problems list over Diags with select-and-zoom through
  libs/circsim's locate seam; the Netlist view; the Results panel - the
  stage-4 plot over ws::spice's tables: one analysis at a time through
  ws::circsim's headless axis math (nice 1/2/5 ticks, SI engineering
  labels, min/max decimation for long sweeps), per-signal visibility
  with live legend readouts, a hover crosshair with per-series markers,
  .op tables and .meas as value rows, the .four harmonics table under
  the measurements (.tran/.op views only - the directives block authors
  the card, `.four 1k 9 v(out)`; a .op run answers the DC component
  alone, a failed series reads like a failed measurement), and CSV/JSON
  export through the writers ws::spice already owns; stage 5 gives the plot its hands -
  the wheel zooms both axes toward the cursor (the plot's own wheel
  claim beats the enclosing scroll panel's), a left-drag rubber band
  zooms to the box with a live dx/dy readout, dragging an axis strip
  stretches that axis alone, the middle/space drag pans, a double-click
  resets, a right-click cancels and restores the press-time view - the
  manual window rides ws::circsim's clamp/zoom/pan math and can never
  leave the data, and the decimation slices to the visible x window),
  stage 8 pins the panel's per-frame work to what it depends on (the
  frame-budget caches in ResultsCache, keyed by a run_id every delivery
  bumps): per-signal constants - the finite envelopes the y fit reads,
  the 40-bucket sparkline envelopes, the spine flags - are walked once
  per (run, table), the series decimations once per (x window, pixel
  budget) so an idle frame maps cached keep lists instead of re-scanning
  every column, and the by-index/non-sorted tables (a nested .dc's
  wrapping inner axis) ride ws::circsim::decimate_minmax_index - the
  same min/max envelope the sorted sweeps always had, where they used
  to push every sample every frame (48 signals x 20k rows: ~9x an idle
  frame, opt-in perf smoke behind CIRCSIM_PLOT_PERF=1 in the panel
  suite),
  the .csim document type
  (autosave/crash recovery/session ride the shell), and the
  simulate job over `run_schematic`. The shell toolbar rides the
  icon-and-tooltip spelling: the chrome actions (New/Open/Save/
  Undo/Redo/Delete/Rotate/Mirror/Wire/ERC) wear the built-in icon
  set's standard glyphs through `set_toolbar_icons`, every button
  carries a label+chord tooltip (the live binding, the menus' own
  hint text), and Simulate keeps its text button - the one act that
  deserves the word. Redo's chords are the standard pair, Ctrl+Y
  declared (the hints spell it) with Ctrl+Shift+Z bound as the alias (a status row with cancel, a
  summary toast, the run's mapped problems in the Problems panel, the
  tables delivered to the Results panel). The panel's draw is gated
  headless by `tests/test_results_panel.cpp` through the Editor's
  plot_canvas seam (a CommandListCanvas asserting the plot geometry),
  the gestures among it, and the Schematic panel's camera mapping by
  `tests/test_canvas_panel.cpp` through the canvas_panel_canvas seam
  (every element's recorded geometry equals its w2s answer - the grid
  marks, the snap feedback and the picks speak the same mapping). The
  bare editor keys (R/M/W/L/P/F/Del/Esc and the Ctrl+C/V/D clipboard
  trio) bind on the shortcut resolver's
  panel rung - the Schematic panel's own scope, kept out of focused
  text fields by the resolver's text rule. Windows is the shipped
  desktop (native dialogs, single instance); a posix dev box builds and
  runs it under X with dejavu/liberation font fallbacks and the
  Unavailable* dialog refusals - the `ae60fd2` story.
- `apps/shell_example` - the ws::ui::shell acceptance app (windowed,
  desktop): a text-and-plot "scratchpad" built ONLY from AppShell - the
  host registers a .txt document type (whole-content undo commands, the
  dirty flag riding the undo save point), an Editor panel and a Plot
  panel (a JobSystem job computing a damped sine, listed in the status
  bar with its bar and a cancel button), a few commands
  (undo/redo/clear/about/replot) beside the shell's own File/Window set,
  and the settings schema (shell.theme, plot.points, plot.amplitude)
  the shell's built-in settings panel renders. Everything else - menus,
  the automatic Window menu, toolbar, docking with draggable splitters,
  palette, toasts, recents, autosave/crash recovery, session restore,
  the ordered shutdown - is the shell's.
