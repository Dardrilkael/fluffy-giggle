# libs/core — CLAUDE.md

Foundation library. Everything else may depend on it; it depends on nothing.

## Purpose
Logging (`ws/core/log.hpp`), assertions (`ws/core/assert.hpp`) and the shared
error/result types (`ws/core/result.hpp`) used by all other libs and apps.

## Allowed dependencies
tl::expected v1.1.0 (header-only, CC0 — `tl::expected`, INTERFACE target) —
the one sanctioned exception to the PRIVATE link rule: it is linked PUBLIC
because `ws::core::Result` IS tl::expected in the public API, always behind a
ws-owned alias. `tl::` names may appear ONLY inside result.hpp. Never add a
dependency on another ws lib here.

## Public API
- `ws::core::debug/info/warn/error(fmt, ...)` — std::format-based logging,
  timestamped, warn/error go to stderr.
- `ws::core::set_min_level(...)` / `min_level()`.
- `ws::core::log_error(const Error&)` — logs category, code, message and
  file:line from the error's source_location.
- `ws::core::Error` — copyable `{int32_t code, const char* category,
  string message, source_location where}`; `explicit operator bool` is true
  when `code != 0`.
- `ws::core::Result<T>` / `Status` (`Result<void>`) — the error-handling
  currency of the workspace (root CLAUDE.md, "Error handling").
- `ws::core::make_error(code, category, message)` / `fail(...)` — build an
  `Error` / an `unexpected<Error>` that converts into any `Result<T>`;
  `where` defaults to the call site's `std::source_location`.
- `WS_TRY(expr)` — early-return propagation for functions returning
  `Result`/`Status`; `WS_TRY_ASSIGN(lhs, expr)` unwraps the value or
  propagates. MSVC-safe: no statement expressions, unique per-line temp
  names.
- `WS_CORE_ASSERT(cond)` — logs expr/file/line and aborts. **Debug only**;
  compiles to nothing under NDEBUG. Never use it for release-mode error
  handling.

## Invariants / rules
- Logging is thread-safe (single mutex); keep it that way.
- No global state other than the min log level.
- Headers must stay self-contained and free of platform-specific includes.
- `Error` stays lib-agnostic: no lib-specific enum lives here; libs convert
  at their own boundaries.

## Tests
```
ctest --preset debug -R core
```
