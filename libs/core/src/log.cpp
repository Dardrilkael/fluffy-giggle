#include <ws/core/assert.hpp>
#include <ws/core/log.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

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

}  // namespace

void set_min_level(LogLevel level) {
  std::lock_guard lock(g_mutex);
  g_min_level = level;
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
  localtime_s(&tm, &t);
  std::FILE* out = (level >= LogLevel::warn) ? stderr : stdout;
  std::fprintf(out, "[%02d:%02d:%02d] [%-5s] %.*s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
               tag(level), static_cast<int>(message.size()), message.data());
}

namespace detail {

[[noreturn]] void assert_fail(const char* expr, const char* file, int line) {
  log(LogLevel::error, "ASSERT FAILED: ");
  std::fprintf(stderr, "  expression: %s\n  at %s:%d\n", expr, file, line);
  std::fflush(stderr);
  std::abort();
}

}  // namespace detail
}  // namespace ws::core
