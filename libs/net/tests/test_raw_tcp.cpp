// Real-socket tests for the unframed public TCP byte-stream API.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <ws/net/address.hpp>
#include <ws/net/event_loop.hpp>
#include <ws/net/raw_tcp.hpp>

namespace {
using ws::net::Address;
using ws::net::EventLoop;
using ws::net::NetError;
using ws::net::RawTcpConnection;
using ws::net::RawTcpListener;

struct Watchdog {
  EventLoop& loop;
  ws::core::Result<ws::net::TimerHandle> timer;
  bool bitten = false;

  explicit Watchdog(EventLoop& event_loop) : loop(event_loop) {
    timer = loop.start_timer(10000, [this] {
      bitten = true;
      loop.stop();
    });
  }
  ~Watchdog() {
    if (timer.has_value()) (void)loop.cancel_timer(timer.value());
  }
};
}  // namespace

TEST_CASE("raw TCP transfers exact unframed bytes and closes gracefully") {
  EventLoop loop;
  Watchdog watchdog{loop};

  auto listener_result = RawTcpListener::listen(loop, Address::loopback(0));
  REQUIRE(listener_result.has_value());
  RawTcpListener::Ptr listener = *listener_result;
  const Address bound = listener->local();
  REQUIRE(bound.port != 0);

  constexpr std::string_view request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
  const auto payload = std::as_bytes(std::span{request.data(), request.size()});

  RawTcpConnection::Ptr server;
  RawTcpConnection::Ptr client;
  bool received_exact_payload = false;
  bool callback_replaced_itself = false;
  int server_closed = 0;
  int client_closed = 0;

  listener->set_on_connection([&](RawTcpConnection::Ptr accepted) {
    server = std::move(accepted);
    server->set_on_bytes([&](std::span<const std::byte> bytes) {
      callback_replaced_itself = true;
      // Replacing a callback from inside itself must not destroy the
      // std::function currently executing.
      server->set_on_bytes([](std::span<const std::byte>) {});
      (void)server->send(bytes);
    });
    server->set_on_closed([&](NetError why) {
      CHECK(why == NetError::connection_closed);
      ++server_closed;
      listener->close();
      if (client_closed != 0) loop.stop();
    });
  });

  auto client_result = RawTcpConnection::connect(loop, bound);
  REQUIRE(client_result.has_value());
  client = *client_result;
  client->set_on_connected([&] {
    const auto sent = client->send(payload);
    CHECK(sent.has_value());
  });
  client->set_on_bytes([&](std::span<const std::byte> bytes) {
    received_exact_payload =
        bytes.size() == payload.size() &&
        std::equal(bytes.begin(), bytes.end(), payload.begin(), payload.end());
    (void)client->close();
  });
  client->set_on_closed([&](NetError why) {
    CHECK(why == NetError::connection_closed);
    ++client_closed;
    if (server_closed != 0) loop.stop();
  });

  const auto run_result = loop.run();
  REQUIRE(run_result.has_value());
  CHECK_FALSE(watchdog.bitten);
  CHECK(callback_replaced_itself);
  CHECK(received_exact_payload);
  CHECK(server_closed == 1);
  CHECK(client_closed == 1);
}
