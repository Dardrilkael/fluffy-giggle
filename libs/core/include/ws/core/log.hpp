#pragma once

#include <format>
#include <string_view>
#include <utility>

#include <ws/core/result.hpp>

namespace ws::core {

enum class LogLevel { debug, info, warn, error };

// Messages below this level are dropped. Default: debug (everything).
void set_min_level(LogLevel level);
LogLevel min_level();

void log(LogLevel level, std::string_view message);

// printf-style logging via std::format:
//   ws::core::info("loaded {} items", n);
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

// Logs an Error's category, code, message and file:line from its
// source_location at the error level.
void log_error(const Error& err);

}  // namespace ws::core
