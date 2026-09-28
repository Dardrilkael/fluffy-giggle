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
- `apps/playground` - smoke-test app for the shell.

Each `libs/<name>/CLAUDE.md` documents that lib's own rules and tests.
