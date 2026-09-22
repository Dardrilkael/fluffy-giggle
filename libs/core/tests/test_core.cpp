#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/core/assert.hpp>
#include <ws/core/log.hpp>

TEST_CASE("logging functions accept plain and formatted messages") {
  ws::core::debug("plain debug message");
  ws::core::info("formatted: {} + {} = {}", 2, 2, 4);
  ws::core::warn("a warning");
  ws::core::error("an error: {}", "details here");
  CHECK(true);  // reaching here without crashing is the test
}

TEST_CASE("log levels are ordered") {
  CHECK(ws::core::LogLevel::debug < ws::core::LogLevel::info);
  CHECK(ws::core::LogLevel::info < ws::core::LogLevel::warn);
  CHECK(ws::core::LogLevel::warn < ws::core::LogLevel::error);
}

TEST_CASE("min level is stored and retrievable") {
  const auto previous = ws::core::min_level();
  ws::core::set_min_level(ws::core::LogLevel::warn);
  CHECK(ws::core::min_level() == ws::core::LogLevel::warn);
  ws::core::info("this message is filtered out");
  ws::core::set_min_level(previous);
}

TEST_CASE("assert with true condition is a no-op") {
  WS_CORE_ASSERT(1 + 1 == 2);
  CHECK(true);
}
