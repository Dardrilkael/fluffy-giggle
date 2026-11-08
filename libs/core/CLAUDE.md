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
- `ws::core::trace/debug/info/warn/error/critical(fmt, ...)` —
  std::format-based logging through the global logger; warn and above go to
  stderr on the console sink.
- `ws::core::LogLevel` — `trace < debug < info < warn < error < critical <
  off` (`off` is a configuration value, never a record level).
  `to_string(level)` / `parse_level(name, out)` convert both ways.
- `ws::core::WS_LOG_TRACE/.../WS_LOG_CRITICAL(fmt, ...)` — macro form that
  also captures the call site's file:line; compiled out (arguments never
  evaluated) below the `WS_LOG_COMPILE_MIN` build define
  (`target_compile_definitions(ws_foo PRIVATE WS_LOG_COMPILE_MIN=2)`);
  `WS_LOG_COMPILE_ACTIVE(level)` guards expensive trace blocks.
- `ws::core::Logger log("net")` — a named logger stamping one category on
  every record; same level methods as the global logger.
- Filters: global `set_min_level(...)`/`min_level()`; hierarchical per-category
  floors `set_category_min_level("net", ...)` (dot-boundary children inherit,
  most specific rule wins, global floor still applies underneath) —
  categories can only tighten, never loosen.
- Sinks: `add_sink(...)`/`remove_sink(...)`/`sinks()`/`clear_sinks()` over the
  `LogSink` interface, with a default `ConsoleSink` (ANSI colors on TTYs,
  logcat mirroring on Android), `FileSink` (append/truncate, size-based
  rotation with `<path>.1..N` backups) and `CallbackSink` (tests, in-app
  log panels). Each sink has its own min level.
- Patterns: `set_pattern("[{time}] [{level}] {message}")` — tokens
  `{time} {level} {category} {thread} {message} {file} {line}`, unknown
  tokens verbatim; default `"[{time}] [{level}] {message}"`.
- `set_flush_level(...)`/`flush_level()` — records at or above the level
  (default: error) flush every sink after writing.
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
- Logging is thread-safe: one registry mutex guards sinks, pattern, flush
  level and category rules; the global min level is a lock-free atomic on
  the fast path. Keep it that way.
- `log()` never throws and never deadlocks: sink exceptions are swallowed
  (the line is lost, a note goes to stderr), and a sink logging back through
  ws::core is detected and dropped, not re-entered.
- LogRecord views are valid only during `LogSink::write` — sinks copy what
  they keep.
- Sinks are called under the registry lock: implementations must be fast
  and need no locking of their own unless driven directly from user code.
- No third-party dependencies beyond tl::expected; platform code (isatty,
  VT enable, logcat, thread ids) stays in `src/log.cpp`.
- Headers must stay self-contained and free of platform-specific includes.
- `Error` stays lib-agnostic: no lib-specific enum lives here; libs convert
  at their own boundaries.

## Tests
```
ctest --preset debug -R core
```
Cases that mutate global logging state take `LogStateGuard` (snapshot +
restore of min level, flush level, pattern, sinks and category rules) and
capture records through a `CallbackSink`; the file sink suite rotates inside
a temp directory and cleans up after itself.
