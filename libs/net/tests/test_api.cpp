#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <iterator>
#include <set>
#include <string>
#include <string_view>

#include <ws/core/result.hpp>
#include <ws/net/address.hpp>
#include <ws/net/errors.hpp>
#include <ws/net/visibility.hpp>

TEST_CASE("describe covers every NetError, distinctly, from static storage") {
  using ws::net::NetError;
  const NetError codes[] = {
      NetError::invalid_argument,  NetError::invalid_address,
      NetError::loop_stopped,      NetError::invalid_handle,
      NetError::bind_failed,       NetError::connect_failed,
      NetError::socket_error,      NetError::message_too_large,
      NetError::malformed_frame,   NetError::connection_closed,
      NetError::connection_reset,  NetError::already_closed,
  };
  std::set<std::string_view> distinct;
  for (const auto code : codes) {
    const std::string_view text = ws::net::describe(code);
    CHECK_FALSE(text.empty());
    distinct.insert(text);
  }
  CHECK(distinct.size() == std::size(codes));  // no two codes share a line

  // An out-of-range cast falls back instead of misbehaving.
  CHECK(ws::net::describe(static_cast<NetError>(999)) ==
        "unknown net error");
}

TEST_CASE("fail builds category-net errors through the enum") {
  const auto err = ws::net::fail(ws::net::NetError::bind_failed, "port busy");
  const ws::core::Result<int> as_result = err;  // converts into any Result
  CHECK(as_result.error().code ==
        static_cast<std::int32_t>(ws::net::NetError::bind_failed));
  CHECK(std::string_view{as_result.error().category} == "net");
  CHECK(as_result.error().message == "port busy");

  // The default message is describe()'s table text - never empty.
  const ws::core::Result<int> defaulted =
      ws::net::fail(ws::net::NetError::connect_failed);
  CHECK(defaulted.error().message ==
        ws::net::describe(ws::net::NetError::connect_failed));
}

TEST_CASE("Address helpers pin the loopback/any/broadcast spellings") {
  CHECK(ws::net::Address::loopback(8080).host == "127.0.0.1");
  CHECK(ws::net::Address::loopback(8080).port == 8080);
  CHECK(ws::net::Address::any(0).host == "0.0.0.0");
  CHECK(ws::net::Address::broadcast(4711).host == "255.255.255.255");

  CHECK_FALSE(ws::net::Address{}.valid());          // empty host
  CHECK(ws::net::Address::any(0).valid());          // port 0 is legal
  CHECK(ws::net::Address("localhost", 80) == ws::net::Address("localhost", 80));
  CHECK(ws::net::Address("a", 1) != ws::net::Address("a", 2));
}

TEST_CASE("Address::parse accepts v4, names and bracketed v6") {
  const auto v4 = ws::net::Address::parse("127.0.0.1:8080");
  REQUIRE(v4.has_value());
  CHECK(*v4 == ws::net::Address("127.0.0.1", 8080));

  const auto name = ws::net::Address::parse("localhost:80");
  REQUIRE(name.has_value());
  CHECK(name->host == "localhost");
  CHECK(name->port == 80);

  const auto v6 = ws::net::Address::parse("[::1]:9000");
  REQUIRE(v6.has_value());
  CHECK(v6->host == "::1");  // brackets never stored
  CHECK(v6->port == 9000);
}

TEST_CASE("Address::parse rejects the malformed shapes") {
  const std::string_view bad[] = {
      "",           "8080",        "host",        ":8080",     "host:",
      "host:0",     "host:65536",  "host:port",   "host:-1",   "[::1]",
      "[::1]8080",  "[::1]:",      "[]:8080",     "host:8080:1",
  };
  for (const std::string_view text : bad) {
    const auto res = ws::net::Address::parse(text);
    CHECK_MESSAGE(!res.has_value(), text);
    if (!res.has_value()) {
      CHECK(res.error().code ==
            static_cast<std::int32_t>(ws::net::NetError::invalid_address));
      CHECK(std::string_view{res.error().category} == "net");
    }
  }
}
