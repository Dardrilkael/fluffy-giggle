#pragma once

namespace ws::core::detail {
[[noreturn]] void assert_fail(const char* expr, const char* file, int line);
}  // namespace ws::core::detail

// Debug-only assertion: logs the expression, file and line, then aborts.
// Compiled out entirely in release builds (NDEBUG).
#ifdef NDEBUG
#define WS_CORE_ASSERT(cond) ((void)0)
#else
#define WS_CORE_ASSERT(cond)                                                    \
  do {                                                                          \
    if (!(cond)) {                                                              \
      ::ws::core::detail::assert_fail(#cond, __FILE__, __LINE__);               \
    }                                                                           \
  } while (0)
#endif
