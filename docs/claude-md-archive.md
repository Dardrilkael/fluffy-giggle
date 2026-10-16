# docs/claude-md-archive.md - the pre-restructure root CLAUDE.md

Snapshot of the root CLAUDE.md "Layout" section as it stood before the
2026-10 restructure (lean root + read-when pointers). A plain docs file:
Claude Code never auto-loads it - it is kept for reference, just in case.

The living versions: each lib's detail is maintained in its
`libs/<name>/CLAUDE.md`; the app bullets moved verbatim to `apps/CLAUDE.md`.
If this file ever disagrees with those, those win.

---

## Layout
- `libs/core` - logging + asserts + the shared error/result types
  (`Error`, `Result<T>`, `Status`, `WS_TRY`/`WS_TRY_ASSIGN`).
- `libs/math` - dense `Matrix<T>` + LU (Eigen stays private to lu.cpp) and
  ws-owned fixed-size vecmath.
- `libs/ui` - app shell (GLFW/Vulkan/ImGui/ImPlot, all private) + the
  `widgets.hpp` drawing API.
- `libs/audio` - WAV decoding (dr_wav, private) + OpenAL Soft device playback
  behind a Result-only API. Cross-platform: same OpenAL pimpl on Windows and
  Android (OpenAL Soft rides Oboe -> AAudio there, both private).
- `libs/net` - networking: single-threaded event loop (one thread-safe
  `post`), length-prefixed message framing on TCP, UDP, LAN discovery.
  libuv is private to `src/backend/uv` - the backend seam means a
  WSAPoll/epoll swap later touches no public header (no uv.h under
  `include/`).
- `libs/physics` - 2D + 3D physics behind one API (`ws::physics2d` /
  `ws::physics3d`): generational bodies/joints, drained contact events,
  queries, fixed-timestep stepping with zero steady-state allocation.
  Box2D (2D) and Jolt (3D) are both private behind the `src/backend`
  seam - an engine swap touches no public header (no engine header
  under `include/`).
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

Each `libs/<name>/CLAUDE.md` documents that lib's own rules and tests.
