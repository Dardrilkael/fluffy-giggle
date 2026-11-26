#include <doctest/doctest.h>

#include <chrono>

#include <ws/core/time.hpp>

TEST_CASE("core time: steady-clock duration conversion") {
  using Clock = ws::core::time::SteadyClock;
  const auto start = Clock::time_point{};
  const auto end = start + std::chrono::microseconds{125000};
  CHECK(ws::core::time::milliseconds_between(start, end) ==
        doctest::Approx(125.0f));
  CHECK(ws::core::time::milliseconds_between(end, start) ==
        doctest::Approx(-125.0f));
}
