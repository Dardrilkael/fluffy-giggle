#include <ws/core/assert.hpp>
#include <ws/core/log.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#ifdef __ANDROID__
#include <android/log.h>  // NDK logcat (liblog) - stderr is invisible there
#endif

namespace ws::core {
namespace {

LogLevel g_min_level = LogLevel::debug;
std::mutex g_mutex;

const char* tag(LogLevel level) {
  switch (level) {
    case LogLevel::debug: return "DEBUG";
    case LogLevel::info:  return "INFO";
    case LogLevel::warn:  return "WARN";
    case LogLevel::error: return "ERROR";
  }
  return "?";
}

#ifdef __ANDROID__
android_LogPriority logcat_priority(LogLevel level) {
  switch (level) {
    case LogLevel::debug: return ANDROID_LOG_DEBUG;
    case LogLevel::info:  return ANDROID_LOG_INFO;
    case LogLevel::warn:  return ANDROID_LOG_WARN;
    case LogLevel::error: return ANDROID_LOG_ERROR;
  }
  return ANDROID_LOG_DEFAULT;
}
#endif

}  // namespace

void set_min_level(LogLevel level) {
  std::lock_guard lock(g_mutex);
  g_min_level = level;
}

void log_error(const Error& err) {
  error("[{}] #{}: {} ({}:{})", err.category, err.code, err.message,
        err.where.file_name(), err.where.line());
}

LogLevel min_level() {
  std::lock_guard lock(g_mutex);
  return g_min_level;
}

void log(LogLevel level, std::string_view message) {
  std::lock_guard lock(g_mutex);
  if (level < g_min_level) {
    return;
  }
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_MSC_VER)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::FILE* out = (level >= LogLevel::warn) ? stderr : stdout;
  std::fprintf(out, "[%02d:%02d:%02d] [%-5s] %.*s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
               tag(level), static_cast<int>(message.size()), message.data());
#ifdef __ANDROID__
  // Desktop keeps its console on stdout/stderr; Android has none, so every
  // line mirrors into logcat under the "ws" tag (`adb logcat -s ws`).
  // __android_log_write takes the string verbatim (the _print variants
  // reformat), so a '%' inside a message can never reinterpret it. logcat
  // timestamps by itself - only the level tag rides along. snprintf
  // truncates safely past the buffer.
  char line[512];
  std::snprintf(line, sizeof(line), "[%-5s] %.*s", tag(level),
                static_cast<int>(message.size()), message.data());
  __android_log_write(logcat_priority(level), "ws", line);
#endif
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
