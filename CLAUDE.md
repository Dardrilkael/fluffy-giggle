# ws - CLAUDE.md

C++20 monorepo: each `libs/<name>` builds a `ws::<name>` static library,
`apps/*` are the executables. Build via the CMake presets (Ninja + MSVC; the
MSVC environment is required - `tools/dev-shell.ps1` for interactive use).
One commit per phase on `main`.

## The one workspace rule: dependencies never leak into apps
- Apps include only `ws/...` headers and link only `ws::` targets.
- Every third-party target is linked PRIVATE by the ws_* library that wraps
  it; no third-party header may appear under any `libs/*/include/`.
- When a lib needs more of a third-party library than it wraps today, it
  grows a thin ws-layer wrapper (ws::ui does this in `widgets.hpp`,
  ws::math in its own headers) - it never re-exports third-party types.
- The one exception: tl::expected, the std::expected backport backing
  `ws::core::Result` - linked PUBLIC from ws::core because Result is the
  public API, and named ONLY inside `ws/core/result.hpp` (see below).

## Error handling
- Public APIs return `ws::core::Result<T>` or `Status`; no exceptions across
  lib boundaries.
- Each lib defines its own error enum and converts to `ws::core::Error` at
  its public boundary - core never knows lib-specific codes.
- Audio callbacks and render loops must not allocate to report errors: use
  empty/short messages there.
- `assert.hpp` is for programmer errors only (debug builds, aborts); Result
  is for runtime failures.

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

Each `libs/<name>/CLAUDE.md` documents that lib's own rules and tests.
