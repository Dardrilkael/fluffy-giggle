#pragma once

// Small steady-clock helpers for profiling and frame instrumentation.
// These are header-only so callers pay only the duration conversion they use.

#include <chrono>

namespace ws::core::time {

using SteadyClock = std::chrono::steady_clock;
using TimePoint = SteadyClock::time_point;

[[nodiscard]] inline float milliseconds_between(
    TimePoint start, TimePoint end) noexcept {
  return std::chrono::duration<float, std::milli>(end - start).count();
}

[[nodiscard]] inline float milliseconds_since(TimePoint start) noexcept {
  return milliseconds_between(start, SteadyClock::now());
}

}  // namespace ws::core::time
