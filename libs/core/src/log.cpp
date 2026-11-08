#include <ws/core/assert.hpp>
#include <ws/core/log.hpp>

#include <chrono>
#include <cctype>
#include <iterator>
#include <mutex>
#include <thread>
#include <utility>

#include <ctime>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#elif !defined(__ANDROID__)
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sys/syscall.h>
#endif

#ifdef __ANDROID__
#include <android/log.h>  // NDK logcat (liblog) - stderr is invisible there
#endif

namespace ws::core {
namespace {

constexpr char k_default_pattern[] = "[{time}] [{level}] {message}";

// The registry is leaked on purpose: logging during static destruction (and
// from other threads that outlive main) must never touch a destroyed object.
struct Registry {
  std::mutex mutex;
  std::vector<LogSinkPtr> sinks;
  std::string pattern = k_default_pattern;
  LogLevel flush_level = LogLevel::error;
  std::vector<std::pair<std::string, LogLevel>> category_rules;

  Registry() { sinks.push_back(std::make_shared<ConsoleSink>()); }
};

Registry& registry() {
  static Registry* const reg = new Registry();
  return *reg;
}

// Lock-free fast paths: the global floor is read on every log call, and the
// has-rules flag lets uncategorized logging skip the mutex entirely.
std::atomic<LogLevel> g_min_level{LogLevel::debug};
std::atomic<bool> g_has_category_rules{false};

// Reentrancy guard: a sink (or a callback it invokes) logging back through
// ws::core would deadlock on the registry mutex - drop those lines instead.
thread_local bool t_in_dispatch = false;

struct DispatchGuard {
  DispatchGuard() { t_in_dispatch = true; }
  ~DispatchGuard() { t_in_dispatch = false; }
  DispatchGuard(const DispatchGuard&) = delete;
  DispatchGuard& operator=(const DispatchGuard&) = delete;
};

// --- small platform helpers ------------------------------------------------

std::string_view basename(std::string_view path) noexcept {
  const auto slash = path.find_last_of("/\\");
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::uint64_t current_thread_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentThreadId());
#elif defined(__linux__)
  return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#elif defined(__APPLE__)
  return static_cast<std::uint64_t>(::pthread_mach_thread_np(::pthread_self()));
#else
  return static_cast<std::uint64_t>(
      std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

void append_timestamp(std::string& out) {
  const auto now = std::chrono::system_clock::now();
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch()) %
      1000;
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_MSC_VER)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::format_to(std::back_inserter(out),
                 "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                 tm.tm_min, tm.tm_sec, ms.count());
}

bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    const unsigned char ca = static_cast<unsigned char>(a[i]);
    const unsigned char cb = static_cast<unsigned char>(b[i]);
    if (std::tolower(ca) != std::tolower(cb)) {
      return false;
    }
  }
  return true;
}

#ifdef __ANDROID__
android_LogPriority logcat_priority(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::trace:    return ANDROID_LOG_VERBOSE;
    case LogLevel::debug:    return ANDROID_LOG_DEBUG;
    case LogLevel::info:     return ANDROID_LOG_INFO;
    case LogLevel::warn:     return ANDROID_LOG_WARN;
    case LogLevel::error:    return ANDROID_LOG_ERROR;
    case LogLevel::critical: return ANDROID_LOG_FATAL;
    case LogLevel::off:      return ANDROID_LOG_DEFAULT;
  }
  return ANDROID_LOG_DEFAULT;
}
#endif

#ifndef __ANDROID__
const char* ansi_color(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::trace:    return "\x1b[90m";   // bright black
    case LogLevel::debug:    return "\x1b[36m";   // cyan
    case LogLevel::info:     return "\x1b[32m";   // green
    case LogLevel::warn:     return "\x1b[33m";   // yellow
    case LogLevel::error:    return "\x1b[31m";   // red
    case LogLevel::critical: return "\x1b[1;31m"; // bold red
    case LogLevel::off:      return "";
  }
  return "";
}

bool stream_is_tty(std::FILE* stream) noexcept {
#if defined(_WIN32)
  return ::_isatty(::_fileno(stream)) != 0;
#else
  return ::isatty(::fileno(stream)) != 0;
#endif
}

#if defined(_WIN32)
// Terminal apps on Windows need ENABLE_VIRTUAL_TERMINAL_PROCESSING before
// ANSI sequences do anything; one attempt per handle is enough.
bool enable_vt(std::FILE* stream) noexcept {
  const HANDLE handle = (stream == stderr) ? ::GetStdHandle(STD_ERROR_HANDLE)
                                           : ::GetStdHandle(STD_OUTPUT_HANDLE);
  if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
    return false;
  }
  DWORD mode = 0;
  if (::GetConsoleMode(handle, &mode) == 0) {
    return false;
  }
  if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
    return true;
  }
  return ::SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}
#else
bool enable_vt(std::FILE*) noexcept { return true; }
#endif

// The color choice depends on both the mode and the stream, so it is resolved
// per write; the level's own escape is picked there.
const char* color_for_level(ConsoleSink::ColorMode mode, LogLevel level,
                            std::FILE* stream) noexcept {
  switch (mode) {
    case ConsoleSink::ColorMode::never:
      return "";
    case ConsoleSink::ColorMode::always:
      return enable_vt(stream) ? ansi_color(level) : "";
    case ConsoleSink::ColorMode::auto_detect:
      break;
  }
  if (stream_is_tty(stream)) {
    return enable_vt(stream) ? ansi_color(level) : "";
  }
  return "";
}
#endif  // !__ANDROID__

// --- category rules ----------------------------------------------------------

// Does `category` fall under this rule ("net" covers "net", "net.tcp"; not
// "network")? Caller holds the registry lock.
bool rule_matches(const std::string& rule, std::string_view category) noexcept {
  if (category == rule) {
    return true;
  }
  return category.size() > rule.size() &&
         category.substr(0, rule.size()) == rule &&
         category[rule.size()] == '.';
}

// Most specific (longest) matching rule wins; no rule means allowed.
bool category_allows(const Registry& reg, LogLevel level,
                     std::string_view category) noexcept {
  const std::string* best_rule = nullptr;
  LogLevel best_level = LogLevel::off;
  for (const auto& [rule, rule_level] : reg.category_rules) {
    if (rule_matches(rule, category) &&
        (best_rule == nullptr || rule.size() >= best_rule->size())) {
      best_rule = &rule;
      best_level = rule_level;
    }
  }
  return best_rule == nullptr || level >= best_level;
}

// --- pattern formatting -------------------------------------------------------

void format_line(const LogRecord& record, const std::string& pattern,
                 std::string& out) {
  out.clear();
  out.reserve(pattern.size() + record.message.size() + 64);
  std::size_t i = 0;
  while (i < pattern.size()) {
    if (pattern[i] != '{') {
      out.push_back(pattern[i]);
      ++i;
      continue;
    }
    const std::size_t close = pattern.find('}', i + 1);
    if (close == std::string::npos) {  // dangling '{' - keep it verbatim
      out.append(pattern, i, std::string::npos);
      break;
    }
    const std::string_view token(pattern.data() + i + 1, close - i - 1);
    if (token == "time") {
      append_timestamp(out);
    } else if (token == "level") {
      std::format_to(std::back_inserter(out), "{:<5}",
                     std::string_view{to_string(record.level)});
    } else if (token == "category") {
      if (record.category.empty()) {
        out.push_back('-');
      } else {
        out += record.category;
      }
    } else if (token == "thread") {
      std::format_to(std::back_inserter(out), "{}", current_thread_id());
    } else if (token == "message") {
      out += record.message;
    } else if (token == "file") {
      out += record.file;
    } else if (token == "line") {
      std::format_to(std::back_inserter(out), "{}", record.line);
    } else {  // unknown token: render verbatim so patterns stay greppable
      out.push_back('{');
      out += token;
      out.push_back('}');
    }
    i = close + 1;
  }
}

}  // namespace

// --- levels -------------------------------------------------------------------

const char* to_string(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::trace:    return "TRACE";
    case LogLevel::debug:    return "DEBUG";
    case LogLevel::info:     return "INFO";
    case LogLevel::warn:     return "WARN";
    case LogLevel::error:    return "ERROR";
    case LogLevel::critical: return "CRITICAL";
    case LogLevel::off:      return "OFF";
  }
  return "?";
}

bool parse_level(std::string_view name, LogLevel& out) noexcept {
  struct Entry {
    const char* name;
    LogLevel level;
  };
  static constexpr Entry k_entries[] = {
      {"trace", LogLevel::trace},   {"debug", LogLevel::debug},
      {"info", LogLevel::info},     {"warn", LogLevel::warn},
      {"error", LogLevel::error},   {"critical", LogLevel::critical},
      {"off", LogLevel::off},
  };
  for (const Entry& entry : k_entries) {
    if (iequals(name, entry.name)) {
      out = entry.level;
      return true;
    }
  }
  return false;
}

// --- sink base -----------------------------------------------------------------

void LogSink::flush() {}

void LogSink::set_min_level(LogLevel level) noexcept {
  min_level_.store(level, std::memory_order_relaxed);
}

LogLevel LogSink::min_level() const noexcept {
  return min_level_.load(std::memory_order_relaxed);
}

// --- console sink ---------------------------------------------------------------

ConsoleSink::ConsoleSink(ColorMode colors)
    : colors_(colors) {}

void ConsoleSink::write(const LogRecord& record) {
#ifdef __ANDROID__
  // Desktop keeps its console on stdout/stderr; Android has none, so every
  // line mirrors into logcat under the "ws" tag (`adb logcat -s ws`).
  // __android_log_write takes the string verbatim (the _print variants
  // reformat), so a '%' inside a message can never reinterpret it. logcat
  // timestamps by itself - only the level tag rides along, and snprintf
  // truncates safely past the buffer.
  char line[512];
  const int n = std::snprintf(line, sizeof(line), "[%-8s] %.*s",
                              to_string(record.level),
                              static_cast<int>(record.message.size()),
                              record.message.data());
  if (n > 0) {
    __android_log_write(logcat_priority(record.level), "ws", line);
  }
#else
  std::FILE* out = (record.level >= LogLevel::warn) ? stderr : stdout;
  const char* const color =
      color_for_level(colors_.load(std::memory_order_relaxed), record.level, out);
  if (color != nullptr && *color != '\0') {
    std::fputs(color, out);
    std::fwrite(record.text.data(), 1, record.text.size(), out);
    std::fputs("\x1b[0m\n", out);
  } else {
    std::fwrite(record.text.data(), 1, record.text.size(), out);
    std::fputc('\n', out);
  }
#endif
}

void ConsoleSink::flush() {
#ifndef __ANDROID__
  std::fflush(stdout);
  std::fflush(stderr);
#endif
}

void ConsoleSink::set_color_mode(ColorMode mode) noexcept {
  colors_.store(mode, std::memory_order_relaxed);
}

ConsoleSink::ColorMode ConsoleSink::color_mode() const noexcept {
  return colors_.load(std::memory_order_relaxed);
}

// --- file sink -------------------------------------------------------------------

FileSink::FileSink(std::string_view path, bool truncate, Rotate rotate)
    : path_(path), rotate_(rotate) {
  open(truncate);
}

FileSink::~FileSink() {
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

bool FileSink::open(bool truncate) {
  file_ = std::fopen(path_.c_str(), truncate ? "wb" : "ab");
  if (file_ == nullptr) {
    if (!open_note_logged_) {
      std::fprintf(stderr,
                   "ws::core: FileSink cannot open '%s' - log lines dropped\n",
                   path_.c_str());
      open_note_logged_ = true;
    }
    return false;
  }
  written_ = 0;
  if (std::fseek(file_, 0, SEEK_END) == 0) {
    const long size = std::ftell(file_);
    if (size > 0) {
      written_ = static_cast<std::uintmax_t>(size);  // appending: count what's there
    }
  }
  return true;
}

std::string FileSink::backup_path(int index) const {
  return path_ + "." + std::to_string(index);
}

void FileSink::rotate() {
  if (file_ == nullptr) {
    return;
  }
  std::fclose(file_);
  file_ = nullptr;
  written_ = 0;
  if (rotate_.backups <= 0) {
    // No backups configured: restart the file instead of keeping history.
    std::remove(path_.c_str());
  } else {
    // Drop the oldest backup, shift the rest down, install the current log.
    // Rename failures are best-effort: rotation must never take the app down.
    std::remove(backup_path(rotate_.backups).c_str());
    for (int i = rotate_.backups - 1; i >= 1; --i) {
      std::rename(backup_path(i).c_str(), backup_path(i + 1).c_str());
    }
    std::rename(path_.c_str(), backup_path(1).c_str());
  }
  open(false);
}

void FileSink::write(const LogRecord& record) {
  if (file_ == nullptr) {
    return;  // failed open: inert, and the note already went to stderr
  }
  const std::size_t line_len = record.text.size() + 1;  // text + '\n'
  if (rotate_.max_bytes > 0 && written_ + line_len > rotate_.max_bytes) {
    rotate();
    if (file_ == nullptr) {
      return;
    }
  }
  std::fwrite(record.text.data(), 1, record.text.size(), file_);
  std::fputc('\n', file_);
  written_ += line_len;
}

void FileSink::flush() {
  if (file_ != nullptr) {
    std::fflush(file_);
  }
}

bool FileSink::is_open() const noexcept { return file_ != nullptr; }

const std::string& FileSink::path() const noexcept { return path_; }

// --- callback sink ----------------------------------------------------------------

CallbackSink::CallbackSink(std::function<void(const LogRecord&)> callback)
    : callback_(std::move(callback)) {}

void CallbackSink::write(const LogRecord& record) {
  if (callback_) {
    callback_(record);
  }
}

// --- registry -----------------------------------------------------------------------

void add_sink(LogSinkPtr sink) {
  if (sink == nullptr) {
    return;
  }
  Registry& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  reg.sinks.push_back(std::move(sink));
}

bool remove_sink(const LogSink* sink) {
  if (sink == nullptr) {
    return false;
  }
  Registry& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  const auto it = std::remove_if(reg.sinks.begin(), reg.sinks.end(),
                                 [sink](const LogSinkPtr& s) {
                                   return s.get() == sink;
                                 });
  const bool removed = it != reg.sinks.end();
  reg.sinks.erase(it, reg.sinks.end());
  return removed;
}

std::vector<LogSinkPtr> sinks() {
  Registry& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  return reg.sinks;
}

void clear_sinks() {
  Registry& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  reg.sinks.clear();
}

// --- configuration ---------------------------------------------------------------------

void set_min_level(LogLevel level) noexcept {
  g_min_level.store(level, std::memory_order_relaxed);
}

LogLevel min_level() noexcept {
  return g_min_level.load(std::memory_order_relaxed);
}

void set_flush_level(LogLevel level) noexcept {
  Registry& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  reg.flush_level = level;
}

LogLevel flush_level() noexcept {
  Registry& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  return reg.flush_level;
}

void set_pattern(std::string_view pattern) {
  Registry& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  reg.pattern.assign(pattern);
}

std::string pattern() {
  Registry& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  return reg.pattern;
}

bool should_log(LogLevel level, std::string_view category) noexcept {
  if (level >= LogLevel::off) {
    return false;
  }
  if (level < g_min_level.load(std::memory_order_relaxed)) {
    return false;
  }
  if (!g_has_category_rules.load(std::memory_order_relaxed) ||
      category.empty()) {
    return true;
  }
  try {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    return category_allows(reg, level, category);
  } catch (...) {
    return true;  // never block logging on an internal failure
  }
}

bool should_log(LogLevel level) noexcept {
  return should_log(level, std::string_view{});
}

void set_category_min_level(std::string_view category, LogLevel level) {
  if (category.empty()) {
    return;
  }
  Registry& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  for (auto& [rule, rule_level] : reg.category_rules) {
    if (rule == category) {
      rule_level = level;
      g_has_category_rules.store(true, std::memory_order_relaxed);
      return;
    }
  }
  reg.category_rules.emplace_back(category, level);
  g_has_category_rules.store(true, std::memory_order_relaxed);
}

bool clear_category_min_level(std::string_view category) {
  Registry& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  const auto it = std::remove_if(
      reg.category_rules.begin(), reg.category_rules.end(),
      [category](const std::pair<std::string, LogLevel>& rule) {
        return std::string_view(rule.first) == category;
      });
  const bool removed = it != reg.category_rules.end();
  reg.category_rules.erase(it, reg.category_rules.end());
  g_has_category_rules.store(!reg.category_rules.empty(),
                             std::memory_order_relaxed);
  return removed;
}

void clear_category_min_levels() {
  Registry& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  reg.category_rules.clear();
  g_has_category_rules.store(false, std::memory_order_relaxed);
}

std::vector<std::pair<std::string, LogLevel>> category_min_levels() {
  Registry& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  return reg.category_rules;
}

// --- dispatch -----------------------------------------------------------------------------

void log(LogLevel level, std::string_view message) noexcept {
  log(level, std::string_view{}, message, std::source_location{});
}

void log(LogLevel level, std::string_view category, std::string_view message,
         std::source_location location) noexcept {
  if (level >= LogLevel::off) {
    return;  // `off` is a configuration value, not a record level
  }
  if (level < g_min_level.load(std::memory_order_relaxed)) {
    return;
  }
  if (t_in_dispatch) {
    std::fprintf(stderr,
                 "ws::core: log call from inside a log sink was dropped\n");
    return;
  }
  const DispatchGuard guard;
  try {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    // The rules may have changed between the fast check and the lock - the
    // authoritative verdict happens here, under the lock.
    if (!category.empty() && !category_allows(reg, level, category)) {
      return;
    }
    LogRecord record;
    record.level = level;
    record.category = category;
    record.message = message;
    record.file = basename(location.file_name());
    record.line = static_cast<int>(location.line());
    std::string text;
    format_line(record, reg.pattern, text);
    record.text = text;
    const bool force_flush = level >= reg.flush_level;
    for (const LogSinkPtr& sink : reg.sinks) {
      if (level < sink->min_level()) {
        continue;
      }
      try {
        sink->write(record);
        if (force_flush) {
          sink->flush();
        }
      } catch (...) {
        std::fprintf(stderr, "ws::core: a log sink threw; the line was dropped\n");
      }
    }
  } catch (...) {
    std::fprintf(stderr, "ws::core: formatting a log line failed\n");
  }
}

void log_error(const Error& err) {
  error("[{}] #{}: {} ({}:{})", err.category, err.code, err.message,
        err.where.file_name(), err.where.line());
}

namespace detail {

[[noreturn]] void assert_fail(const char* expr, const char* file, int line) {
  log(LogLevel::error, "ASSERT FAILED: ");
  std::fprintf(stderr, "  expression: %s\n  at %s:%d\n", expr, file, line);
  std::fflush(stderr);
#ifdef __ANDROID__
  char text[512];  // not `line` - the parameter holds the line number
  std::snprintf(text, sizeof(text), "ASSERT FAILED: %s at %s:%d", expr, file,
                line);
  __android_log_write(ANDROID_LOG_ERROR, "ws", text);
#endif
  std::abort();
}

}  // namespace detail
}  // namespace ws::core
