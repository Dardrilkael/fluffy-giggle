#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <source_location>
#include <string_view>

#include <ws/core/assert.hpp>
#include <ws/core/log.hpp>
#include <ws/core/result.hpp>

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

namespace {

ws::core::Status ok_status() {
  return {};
}

ws::core::Status bad_status() {
  return ws::core::fail(3, "test", "bad status");
}

ws::core::Result<int> ok_value() {
  return 42;
}

ws::core::Result<int> bad_value() {
  return ws::core::fail(9, "test", "bad value");
}

ws::core::Status try_propagates() {
  WS_TRY(ok_status());
  WS_TRY(bad_status());
  return {};
}

ws::core::Status try_passes_through() {
  WS_TRY(ok_status());
  return {};
}

ws::core::Result<int> try_assign_ok() {
  WS_TRY_ASSIGN(const auto v, ok_value());
  return v;
}

ws::core::Result<int> try_assign_propagates() {
  WS_TRY_ASSIGN(const auto v, bad_value());
  return v;
}

}  // namespace

TEST_CASE("fail() produces the right code, category and message") {
  const auto here = std::source_location::current().line();
  const auto failed = ws::core::fail(7, "test", "boom");
  const ws::core::Error err = failed.value();
  CHECK(err.code == 7);
  CHECK(std::string_view(err.category) == "test");
  CHECK(err.message == "boom");
  CHECK(err.where.line() == here + 1);  // captured one line above the call
  CHECK(bool(err));
}

TEST_CASE("WS_TRY propagates an error out of a Status function") {
  const ws::core::Status s = try_propagates();
  CHECK(!s);
  CHECK(s.error().code == 3);
  CHECK(s.error().message == "bad status");
  CHECK(try_passes_through().has_value());
}

TEST_CASE("WS_TRY_ASSIGN unwraps a value or propagates the error") {
  const ws::core::Result<int> ok = try_assign_ok();
  REQUIRE(ok);
  CHECK(*ok == 42);
  const ws::core::Result<int> bad = try_assign_propagates();
  CHECK(!bad);
  CHECK(bad.error().code == 9);
}

TEST_CASE("Result<void> success path") {
  const ws::core::Status s{};
  CHECK(s.has_value());
  CHECK(bool(s));
  CHECK(ok_status().has_value());
}

TEST_CASE("Error converts to bool: truthy only with a nonzero code") {
  CHECK(!ws::core::Error{});
  CHECK(ws::core::Error{1, "test", "x", {}});
}

TEST_CASE("log_error formats without crashing") {
  ws::core::log_error(ws::core::make_error(5, "test", "logged error"));
  CHECK(true);  // reaching here without crashing is the test
}
