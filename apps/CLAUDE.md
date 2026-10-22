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
  page of sections (buttons, choices, sliders, text, lists, a table, tabs,
  the overlays, the disabled scope), all five modal-dialog button sets, the
  toast store's whole story, and a status bar the page keeps clear of. No
  assets: the font is the system face resolved at the platform boundary
  (like playground's), so Windows and Android build it from one source
  unchanged.
