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

## Layout
- `libs/core` - logging + asserts.
- `libs/math` - dense `Matrix<T>` + LU (Eigen stays private to lu.cpp) and
  ws-owned fixed-size vecmath.
- `libs/ui` - app shell (GLFW/Vulkan/ImGui/ImPlot, all private) + the
  `widgets.hpp` drawing API.
- `apps/playground` - smoke-test app for the shell.

Each `libs/<name>/CLAUDE.md` documents that lib's own rules and tests.
