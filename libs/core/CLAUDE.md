# libs/core — CLAUDE.md

Foundation library. Everything else may depend on it; it depends on nothing.

## Purpose
Logging (`ws/core/log.hpp`) and assertions (`ws/core/assert.hpp`) used by all
other libs and apps.

## Allowed dependencies
None (standard library only). Never add a dependency on another ws lib here.

## Public API
- `ws::core::debug/info/warn/error(fmt, ...)` — std::format-based logging,
  timestamped, warn/error go to stderr.
- `ws::core::set_min_level(...)` / `min_level()`.
- `WS_CORE_ASSERT(cond)` — logs expr/file/line and aborts. **Debug only**;
  compiles to nothing under NDEBUG. Never use it for release-mode error
  handling.

## Invariants / rules
- Logging is thread-safe (single mutex); keep it that way.
- No global state other than the min log level.
- Headers must stay self-contained and free of platform-specific includes.

## Tests
```
ctest --preset debug -R core
```
