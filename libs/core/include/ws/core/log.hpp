#pragma once

// ws::core logging: levels, categories, sinks, patterns and compile-time
// filtering, all behind one thread-safe registry and zero third-party deps.
//
//   ws::core::info("loaded {} items", n);            // global logger
//   ws::core::Logger log("net");                     // categorized logger
//   log.info("peer {} connected", id);
//   WS_LOG_INFO("captures file:line automatically"); // macro form
//
// Routing: every record goes through the active pattern (set_pattern) into
// every registered sink (add_sink) whose own min level admits it. A default
// ConsoleSink is installed at startup; on Android it mirrors into logcat
// under the "ws" tag. Records at or above the flush level (default: error)
// flush every sink after writing.

// Define WS_LOG_COMPILE_MIN before including this header (or on the command
// line) to compile lower levels out - see the macros at the bottom.
#ifndef WS_LOG_COMPILE_MIN
#define WS_LOG_COMPILE_MIN 0
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <format>
#include <functional>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ws/core/result.hpp>

namespace ws::core {

// ---------------------------------------------------------------------------
// Levels
// ---------------------------------------------------------------------------

// Ordered: comparisons with < and >= work across the whole family. `off` is
// a configuration value for set_min_level(), never a record level.
enum class LogLevel : std::uint8_t {
  trace = 0,
  debug = 1,
  info = 2,
  warn = 3,
  error = 4,
  critical = 5,
  off = 6,
};

// Upper-case name ("TRACE" ... "OFF"); "?" for values outside the enum.
const char* to_string(LogLevel level) noexcept;

// Case-insensitive parse of the names above ("warn", "WARN", ...). Returns
// false for unknown names and leaves `out` untouched.
bool parse_level(std::string_view name, LogLevel& out) noexcept;

// ---------------------------------------------------------------------------
// Records and sinks
// ---------------------------------------------------------------------------

// One log line in flight. All views point into storage owned by the caller
// (the formatted message and `text` live in dispatch-owned strings) - they
// are valid only during LogSink::write. Sinks must copy what they keep.
struct LogRecord {
  LogLevel level = LogLevel::info;
  std::string_view category;  // "" = uncategorized
  std::string_view message;   // raw, unformatted message
  std::string_view file;      // basename of the call site, "" when unknown
  int line = 0;               // 0 when unknown
  std::string_view text;      // full line through the active pattern
};

// Sink interface. write() is called under the logging registry lock, so
// implementations do not need their own synchronization unless they are
// driven directly from user code. write()/flush() must not log through
// ws::core (recursive calls are detected and dropped, not deadlocked) and
// must not throw (throwing sinks lose the line; dispatch survives).
class LogSink {
 public:
  virtual ~LogSink() = default;

  virtual void write(const LogRecord& record) = 0;
  virtual void flush();

  // Per-sink floor in addition to the global/category filters.
  void set_min_level(LogLevel level) noexcept;
  LogLevel min_level() const noexcept;

 private:
  std::atomic<LogLevel> min_level_{LogLevel::trace};
};

using LogSinkPtr = std::shared_ptr<LogSink>;

// stdout/stderr console sink (warn and above go to stderr). Colors the whole
// line per level when enabled: auto_detect colors only on a terminal,
// always/never force the choice. On Android there is no console: every line
// mirrors into logcat under the "ws" tag instead.
class ConsoleSink final : public LogSink {
 public:
  enum class ColorMode { auto_detect, always, never };

  explicit ConsoleSink(ColorMode colors = ColorMode::auto_detect);

  void write(const LogRecord& record) override;
  void flush() override;

  void set_color_mode(ColorMode mode) noexcept;
  ColorMode color_mode() const noexcept;

 private:
  std::atomic<ColorMode> colors_;
};

// Append-mode file sink with optional size-based rotation. Rotation moves
// the current file to "<path>.1" (shifting ".1"->".2", ..., dropping the
// oldest) once appending the next line would exceed max_bytes; backups = 0
// with max_bytes > 0 means the file is simply restarted. A failed open
// leaves the sink inert: lines are dropped, one note goes to stderr, and
// nothing throws. Not copyable/movable - the registry owns it by shared_ptr.
class FileSink final : public LogSink {
 public:
  struct Rotate {
    std::uintmax_t max_bytes;  // 0 = never rotate
    int backups;               // rotated files kept: <path>.1 ... .N
  };

  explicit FileSink(std::string_view path, bool truncate = false,
                    Rotate rotate = Rotate{});
  ~FileSink() override;
  FileSink(const FileSink&) = delete;
  FileSink& operator=(const FileSink&) = delete;
  FileSink(FileSink&&) = delete;
  FileSink& operator=(FileSink&&) = delete;

  void write(const LogRecord& record) override;
  void flush() override;

  bool is_open() const noexcept;
  const std::string& path() const noexcept;

 private:
  bool open(bool truncate);
  void rotate();
  std::string backup_path(int index) const;

  std::string path_;
  std::FILE* file_ = nullptr;
  Rotate rotate_{};
  std::uintmax_t written_ = 0;
  bool open_note_logged_ = false;
};

// Routes records into a callback - for tests, in-app log panels and bridges
// to host logging systems. The callback runs under the registry lock: it
// must be fast, must not throw and must not log through ws::core.
class CallbackSink final : public LogSink {
 public:
  explicit CallbackSink(std::function<void(const LogRecord&)> callback);

  void write(const LogRecord& record) override;

 private:
  std::function<void(const LogRecord&)> callback_;
};

// ---------------------------------------------------------------------------
// Registry and configuration (all thread-safe)
// ---------------------------------------------------------------------------

// Installs a sink (a null pointer is ignored). Keep your own LogSinkPtr to
// remove it later; the registry holds a strong reference.
void add_sink(LogSinkPtr sink);

// Removes the sink registered at that address; false when not found.
bool remove_sink(const LogSink* sink);

// Snapshot of the registered sinks (default at startup: one ConsoleSink).
std::vector<LogSinkPtr> sinks();

// Removes every sink - logging goes nowhere until add_sink() runs again.
void clear_sinks();

// Global floor for all records (atomic: lock-free on the log fast path).
void set_min_level(LogLevel level) noexcept;
LogLevel min_level() noexcept;

// Records at or above this level flush every sink after writing.
// Default: error.
void set_flush_level(LogLevel level) noexcept;
LogLevel flush_level() noexcept;

// Line pattern. Tokens, all optional, any literal text around them:
//   {time}     local wall clock, "YYYY-MM-DD HH:MM:SS.mmm"
//   {level}    name, left-padded to 5 ("INFO ", "CRITICAL")
//   {category} record category, "-" when uncategorized
//   {thread}   OS thread id of the emitting thread
//   {message}  the message itself
//   {file}     basename of the call site ("" without a location)
//   {line}     line of the call site (0 without a location)
// Unknown tokens render verbatim ("{nope}" stays "{nope}").
// Default: "[{time}] [{level}] {message}".
void set_pattern(std::string_view pattern);
std::string pattern();

// Would a record at this level (optionally in this category) be emitted?
// Respects the global floor and the category rules below.
bool should_log(LogLevel level) noexcept;
bool should_log(LogLevel level, std::string_view category) noexcept;

// Per-category floors. A rule for "net" also governs "net.tcp" (dot
// boundary, so "network" is untouched); when several rules match, the most
// specific (longest) one wins. The global min level still applies
// underneath: a category rule can only tighten, never loosen.
void set_category_min_level(std::string_view category, LogLevel level);
bool clear_category_min_level(std::string_view category);  // false if absent
void clear_category_min_levels();
std::vector<std::pair<std::string, LogLevel>> category_min_levels();

// ---------------------------------------------------------------------------
// Emitting
// ---------------------------------------------------------------------------

// Raw message through the global logger (no category, no location).
void log(LogLevel level, std::string_view message) noexcept;

// Raw message with category and (optionally) the call site. The default
// argument captures the caller's location.
void log(LogLevel level, std::string_view category,
         std::string_view message,
         std::source_location location = std::source_location::current()) noexcept;

// printf-style logging through the global logger via std::format:
//   ws::core::info("loaded {} items", n);
template <class... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
  log(LogLevel::trace, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
  log(LogLevel::debug, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
  log(LogLevel::info, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
  log(LogLevel::warn, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
  log(LogLevel::error, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void critical(std::format_string<Args...> fmt, Args&&... args) {
  log(LogLevel::critical, std::format(fmt, std::forward<Args>(args)...));
}

// A named logger: same levels and formatting, one category stamped on every
// record. Cheap to construct (it only holds the string); make one per
// subsystem and keep it next to the code it narrates.
class Logger {
 public:
  explicit Logger(std::string category) : category_(std::move(category)) {}

  const std::string& category() const noexcept { return category_; }

  bool should_log(LogLevel level) const noexcept {
    return ws::core::should_log(level, category_);
  }

  void log(LogLevel level, std::string_view message) const noexcept {
    ws::core::log(level, category_, message, std::source_location{});
  }

  template <class... Args>
  void trace(std::format_string<Args...> fmt, Args&&... args) const {
    log(LogLevel::trace, std::format(fmt, std::forward<Args>(args)...));
  }
  template <class... Args>
  void debug(std::format_string<Args...> fmt, Args&&... args) const {
    log(LogLevel::debug, std::format(fmt, std::forward<Args>(args)...));
  }
  template <class... Args>
  void info(std::format_string<Args...> fmt, Args&&... args) const {
    log(LogLevel::info, std::format(fmt, std::forward<Args>(args)...));
  }
  template <class... Args>
  void warn(std::format_string<Args...> fmt, Args&&... args) const {
    log(LogLevel::warn, std::format(fmt, std::forward<Args>(args)...));
  }
  template <class... Args>
  void error(std::format_string<Args...> fmt, Args&&... args) const {
    log(LogLevel::error, std::format(fmt, std::forward<Args>(args)...));
  }
  template <class... Args>
  void critical(std::format_string<Args...> fmt, Args&&... args) const {
    log(LogLevel::critical, std::format(fmt, std::forward<Args>(args)...));
  }

 private:
  std::string category_;
};

namespace detail {

// The compile-time floor this translation unit was built with (tests use it).
constexpr int compile_min_level() noexcept { return WS_LOG_COMPILE_MIN; }

// The predicate behind WS_LOG_COMPILE_ACTIVE: a function (not a raw
// comparison) so constant use in static_assert/if constexpr stays warning-
// free on GCC (-Wtype-limits knows an enum can never be negative).
constexpr bool compile_active(LogLevel level) noexcept {
  return static_cast<int>(level) >= compile_min_level();
}

// The macro entry point: formats, then dispatches with the captured location.
template <class... Args>
void log_loc(std::source_location location, LogLevel level,
             std::string_view category, std::format_string<Args...> fmt,
             Args&&... args) {
  log(level, category, std::format(fmt, std::forward<Args>(args)...),
      location);
}

}  // namespace detail

// Logs an Error's category, code, message and file:line from its
// source_location at the error level.
void log_error(const Error& err);

}  // namespace ws::core

// ---------------------------------------------------------------------------
// Location-capturing, compile-time-strippable macros
// ---------------------------------------------------------------------------
// WS_LOG_<LEVEL>(fmt, args...) behaves like the free functions but also
// captures the call site's file:line into the record ({file}/{line} pattern
// tokens). Define WS_LOG_COMPILE_MIN at build time to compile the lower
// levels out entirely - their arguments are then never evaluated:
//   target_compile_definitions(ws_foo PRIVATE WS_LOG_COMPILE_MIN=2)
// WS_LOG_COMPILE_ACTIVE(level) is the compile-time predicate for guarding
// expensive blocks:
//   if constexpr (WS_LOG_COMPILE_ACTIVE(ws::core::LogLevel::trace)) { ... }

#define WS_LOG_COMPILE_ACTIVE(level) \
  (::ws::core::detail::compile_active(level))

#if WS_LOG_COMPILE_MIN <= 0
#define WS_LOG_TRACE(...)                                                        \
  ::ws::core::detail::log_loc(::std::source_location::current(),                 \
                              ::ws::core::LogLevel::trace, {}, __VA_ARGS__)
#else
#define WS_LOG_TRACE(...) ((void)0)
#endif

#if WS_LOG_COMPILE_MIN <= 1
#define WS_LOG_DEBUG(...)                                                        \
  ::ws::core::detail::log_loc(::std::source_location::current(),                 \
                              ::ws::core::LogLevel::debug, {}, __VA_ARGS__)
#else
#define WS_LOG_DEBUG(...) ((void)0)
#endif

#if WS_LOG_COMPILE_MIN <= 2
#define WS_LOG_INFO(...)                                                         \
  ::ws::core::detail::log_loc(::std::source_location::current(),                 \
                              ::ws::core::LogLevel::info, {}, __VA_ARGS__)
#else
#define WS_LOG_INFO(...) ((void)0)
#endif

#if WS_LOG_COMPILE_MIN <= 3
#define WS_LOG_WARN(...)                                                         \
  ::ws::core::detail::log_loc(::std::source_location::current(),                 \
                              ::ws::core::LogLevel::warn, {}, __VA_ARGS__)
#else
#define WS_LOG_WARN(...) ((void)0)
#endif

#if WS_LOG_COMPILE_MIN <= 4
#define WS_LOG_ERROR(...)                                                        \
  ::ws::core::detail::log_loc(::std::source_location::current(),                 \
                              ::ws::core::LogLevel::error, {}, __VA_ARGS__)
#else
#define WS_LOG_ERROR(...) ((void)0)
#endif

#if WS_LOG_COMPILE_MIN <= 5
#define WS_LOG_CRITICAL(...)                                                     \
  ::ws::core::detail::log_loc(::std::source_location::current(),                 \
                              ::ws::core::LogLevel::critical, {}, __VA_ARGS__)
#else
#define WS_LOG_CRITICAL(...) ((void)0)
#endif
