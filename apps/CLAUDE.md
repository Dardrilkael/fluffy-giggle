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
  the app owns outright off the raw state() seam), and the platform
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
