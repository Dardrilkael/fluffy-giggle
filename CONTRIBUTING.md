# Contributing to ws

Thanks for your interest in contributing! This document covers the essentials.

## Build & Test

```bash
# Windows (MSVC + Ninja)
cmake --preset debug && cmake --build --preset debug && ctest --preset debug

# Linux (GCC, headless subset)
cmake --preset linux && cmake --build --preset linux && ctest --preset linux

# With sanitizers
cmake --preset linux-asan && cmake --build --preset linux-asan && ctest --preset linux-asan
```

Before opening a PR, ensure `ctest` passes on at least one platform.

## Workspace Rules

### Dependencies never leak into apps

This is the one workspace rule, enforced by `tools/check_deps.ps1` as a ctest:

1. **Apps** include only `ws/...` headers and link only `ws::` targets
2. **Every third-party target** is linked `PRIVATE` by the `ws_*` library that wraps it
3. **No third-party header** may appear under any `libs/*/include/`
4. When a lib needs more of a third-party library than it wraps, it grows a thin `ws::`-layer wrapper — it never re-exports third-party types
5. The one exception: `tl::expected` (the `std::expected` backport), linked PUBLIC from `ws::core` because `Result` is the public API

### Error handling

- Public APIs return `ws::core::Result<T>` or `Status` — no exceptions across lib boundaries
- Each lib defines its own error enum, converting to `ws::core::Error` at its boundary
- `assert.hpp` is for programmer errors (debug-only, aborts); `Result` is for runtime failures
- Audio callbacks and render loops must not allocate to report errors

### C++ style

- C++20, no compiler extensions (`CMAKE_CXX_EXTENSIONS OFF`)
- Namespace: `ws::<libname>::...` — every public type lives under `ws::`
- Headers: public headers under `libs/<name>/include/ws/<name>/`, sources under `libs/<name>/src/`
- Tests: every lib has a `tests/` directory, registered with ctest

## Workflow

1. **Branch** from `main` — one commit per logical phase, pushed as it lands
2. **Read the relevant `CLAUDE.md`** before touching a directory:
   - Touching `libs/<name>/` → read `libs/<name>/CLAUDE.md`
   - Touching `apps/` → read `apps/CLAUDE.md`
   - GUI internals → read `libs/ui/src/gui/CLAUDE.md`
3. **Add tests** for new functionality — the test suites are the safety net
4. **Run `check_deps`** if you add or change any dependency linkage
5. **Keep commits focused** — one phase per commit, descriptive message

## Adding a Third-Party Dependency

1. Re-read the workspace rule above
2. Add the dependency in `cmake/ThirdParty.cmake` via FetchContent
3. Wrap it behind a `ws::` interface in the appropriate lib
4. Link it `PRIVATE` — never `PUBLIC` (except `tl::expected` in `ws::core`)
5. Run `tools/check_deps.ps1` to verify the rule still passes

## Filing Issues

When filing an issue, include:
- Platform and preset (e.g., `linux-asan`, `debug`, `android`)
- Steps to reproduce
- Expected vs actual behavior
- Relevant `CLAUDE.md` section if applicable
