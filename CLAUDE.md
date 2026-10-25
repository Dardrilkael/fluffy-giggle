# ws - CLAUDE.md

C++20 monorepo: each `libs/<name>` builds a `ws::<name>` static library,
`apps/*` are the executables. One commit per phase on `main`, pushed to
origin as it lands.

## Commands
The debug/release presets are Ninja + MSVC and need the MSVC environment -
`tools/dev-shell.ps1` opens a shell with it loaded.

```
cmake --preset debug                   # configure (also: release, vs-debug,
                                        # vs-release, android)
cmake --build --preset debug           # build (--target <name> for one)
ctest --preset debug                   # all tests; -R <lib> for one lib
tools/package-android.ps1 -App <name>  # APK (default playground)
```

`check_deps` runs with the suites (it is `tools/check_deps.ps1`): the one
workspace rule below, enforced as a test.

## The one workspace rule: dependencies never leak into apps
- Apps include only `ws/...` headers and link only `ws::` targets.
- Every third-party target is linked PRIVATE by the ws_* library that wraps
  it; no third-party header may appear under any `libs/*/include/`.
- When a lib needs more of a third-party library than it wraps today, it
  grows a thin ws-layer wrapper (ws::ui does this in `widgets.hpp`,
  ws::math in its own headers) - it never re-exports third-party types.
- The one exception: tl::expected, the std::expected backport backing
  `ws::core::Result` - linked PUBLIC from ws::core because Result is the
  public API, and named ONLY inside `ws/core/result.hpp`
  (`libs/core/CLAUDE.md` spells out the carve-out).

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
One line each - the per-tree CLAUDE.md files carry the detail:

- `libs/core` - logging + asserts + the shared error/result types.
- `libs/math` - dense `Matrix<T>` + LU and ws-owned fixed-size vecmath.
- `libs/input` - the input family promoted out of ui (stage 3a): the
  GLFW-free event types + the `InputFrame` snapshot (touch/mouse -> one
  pointer list). Pure CPU, core-only; ws::ui's platform TUs are the only
  writers of an InputFrame.
- `libs/gfx` - the Vulkan device layer promoted out of ui (stage 3b):
  loader/instance, surface-via-callback, device/queue selection, the VMA
  allocator, the swapchain and frame sync, and (phase C) the
  buffer/image/shader-module helpers ui's renderer draws through - no
  vma target or vk_mem_alloc.h include remains under libs/ui. Public API
  is opaque void* handles; volk/vma stay PRIVATE and ui (the only
  consumer) is the unwrap point.
- `libs/ui` - app shell (GLFW/Vulkan/ImGui/ImPlot, all private) + the
  widget/renderer/gui drawing APIs.
- `libs/audio` - WAV decode + OpenAL Soft playback behind a Result-only API.
- `libs/net` - event loop, framed TCP, UDP, LAN discovery.
- `libs/physics` - 2D + 3D physics behind one API (Box2D/Jolt, private).
- `libs/jobs` - the headless job system: a Result-shaped pool with
  priorities, cooperative cancellation and completions marshalled to the
  owner thread; ws::ui's App drains it once per frame.
- `libs/settings` - the headless settings store: a typed schema with
  constraints, layered sources with provenance, change observers, and
  JSON persistence through a minimal FileSystem seam.
- `libs/platform` - the headless platform-services lib: standard
  locations (supplied roots win over the OS truth), lexical path
  helpers, the debounced file watch and the recent-files MRU (pinned,
  pruned, persisted) over the settings FileSystem + json seams;
  dialogs and autosave come later.
- `apps/*` - the executables.

## Read when
- Touching `libs/<name>/` - read `libs/<name>/CLAUDE.md` (that lib's rules,
  API surface and tests). Gui internals: `libs/ui/src/gui/CLAUDE.md`.
- Touching `apps/` - read `apps/CLAUDE.md` (what each app is, what it
  demonstrates, APK packaging).
- Adding a third-party dependency anywhere - re-read the workspace rule
  above; `tools/check_deps.ps1` lists the signatures the test catches.
- Wanting the fuller pre-restructure layout text -
  `docs/claude-md-archive.md` (a snapshot, not the living docs).
