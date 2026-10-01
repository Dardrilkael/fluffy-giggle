// Real-socket TCP over the public API (loopback, ephemeral ports - CLAUDE
// .md needs nothing beyond the machine itself). Every case drives ONE
// EventLoop on the test thread: listener and client live on it, run()
// turns until the case closes everything, and a watchdog timer stops a
// wedged case so the suite fails fast instead of hanging. Callbacks only
// set flags - doctest asserts live at test level (a throw would cross
// the libuv C boundary).

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <ws/net/event_loop.hpp>
#include <ws/net/tcp.hpp>

namespace {

using ws::net::Address;
using ws::net::EventLoop;
using ws::net::NetError;
using ws::net::TcpConnection;
using ws::net::TcpListener;

std::vector<std::byte> payload(std::size_t size, std::uint8_t fill) {
  return std::vector<std::byte>(size, std::byte{fill});
}

// Stops the loop after `ms`, so a case that wedged fails at its CHECKs
// instead of hanging ctest.
struct Watchdog {
  EventLoop& loop;
  ws::core::Result<ws::net::TimerHandle> timer;
  bool bitten = false;

  explicit Watchdog(EventLoop& l, std::uint64_t ms = 15000) : loop(l) {
    timer = l.start_timer(ms, [this] {
      bitten = true;
      loop.stop();
    });
  }

  ~Watchdog() {
    if (timer.has_value()) {
      (void)loop.cancel_timer(timer.value());
    }
  }
};

}  // namespace

TEST_CASE("an echo server round-trips messages of varied sizes") {
  EventLoop loop;
  Watchdog watchdog{loop};

  auto listener_result = TcpListener::listen(loop, Address::loopback(0));
  REQUIRE(listener_result.has_value());
  TcpListener::Ptr listener = *listener_result;
  const Address bound = listener->local();
  REQUIRE(bound.port != 0);  // ephemeral :0 got a real port

  const std::vector<std::size_t> sizes{0, 1, 10, 1000, 100000};
  std::vector<std::size_t> echoed_sizes;
  std::vector<ws::core::Status> send_results;
  bool client_connected = false;
  bool server_connected = false;
  int client_closed = 0;
  std::int32_t client_closed_code = -1;
  int server_conn_closed = 0;
  std::int32_t server_closed_code = -1;
  TcpConnection::Ptr server_conn;  // hold the accepted end for the case

  auto maybe_stop = [&] {
    if (client_closed > 0 && server_conn_closed > 0) {
      loop.stop();
    }
  };

  listener->set_on_connection([&](TcpConnection::Ptr conn) {
    server_conn = conn;  // the Ptr dies with this callback otherwise
    conn->set_on_connected([&] { server_connected = true; });
    conn->set_on_message([&](std::span<const std::byte> message) {
      // NOT `conn`: that parameter's slot dies with on_connection, and this
      // fires long after. The case-scope holder is the safe route in.
      (void)server_conn->send(message);  // the echo
    });
    conn->set_on_closed([&](NetError why) {
      server_conn_closed += 1;
      server_closed_code = static_cast<std::int32_t>(why);
      listener->close();  // client is done: stop accepting
      maybe_stop();
    });
  });

  auto client_result = TcpConnection::connect(loop, bound);
  REQUIRE(client_result.has_value());
  TcpConnection::Ptr client = *client_result;
  client->set_on_connected([&] {
    client_connected = true;
    for (const std::size_t size : sizes) {
      send_results.push_back(client->send(payload(size, 0xAB)));
    }
  });
  client->set_on_message([&](std::span<const std::byte> message) {
    echoed_sizes.push_back(message.size());
    if (echoed_sizes.size() == sizes.size()) {
      (void)client->close();  // graceful: last echo seen, we hang up
    }
  });
  client->set_on_closed([&](NetError why) {
    client_closed += 1;
    client_closed_code = static_cast<std::int32_t>(why);
    maybe_stop();
  });

  const auto run_result = loop.run();
  REQUIRE(run_result.has_value());
  REQUIRE(send_results.size() == sizes.size());
  for (const auto& s : send_results) {
    REQUIRE(s.has_value());
  }
  CHECK(!watchdog.bitten);
  CHECK(client_connected);
  CHECK(server_connected);
  CHECK(echoed_sizes == sizes);
  CHECK(client_closed == 1);
  CHECK(client_closed_code ==
        static_cast<std::int32_t>(NetError::connection_closed));
  CHECK(server_conn_closed == 1);
  CHECK(server_closed_code ==
        static_cast<std::int32_t>(NetError::connection_closed));
}

TEST_CASE("close() flushes a queued message burst before the FIN") {
  EventLoop loop;
  Watchdog watchdog{loop};

  auto listener_result = TcpListener::listen(loop, Address::loopback(0));
  REQUIRE(listener_result.has_value());
  TcpListener::Ptr listener = *listener_result;
  const Address bound = listener->local();
  REQUIRE(bound.port != 0);

  constexpr int kMessages = 200;
  const auto blob = payload(1024, 0x5A);
  int client_messages = 0;
  std::size_t min_seen = 999999;
  std::size_t server_pending_at_close = 0;
  int client_closed = 0;
  int server_closed = 0;
  std::int32_t client_code = -1;
  std::int32_t server_code = -1;
  TcpConnection::Ptr server_conn;

  auto maybe_stop = [&] {
    if (client_closed > 0 && server_closed > 0) {
      loop.stop();
    }
  };

  listener->set_on_connection([&](TcpConnection::Ptr conn) {
    server_conn = conn;  // the Ptr dies with this callback otherwise
    conn->set_on_connected([&] {
      // server_conn, not the dead `conn` parameter (this fires post-return).
      for (int i = 0; i < kMessages; ++i) {
        (void)server_conn->send(std::span<const std::byte>{blob});
      }
      (void)server_conn->close();  // graceful: all 200 flush first
      server_pending_at_close = server_conn->pending_bytes();
    });
    conn->set_on_closed([&](NetError why) {
      server_closed += 1;
      server_code = static_cast<std::int32_t>(why);
      listener->close();
      maybe_stop();
    });
  });

  auto client_result = TcpConnection::connect(loop, bound);
  REQUIRE(client_result.has_value());
  TcpConnection::Ptr client = *client_result;
  client->set_on_message([&](std::span<const std::byte> message) {
    client_messages += 1;
    if (message.size() < min_seen) {
      min_seen = message.size();
    }
  });
  client->set_on_closed([&](NetError why) {
    client_closed += 1;
    client_code = static_cast<std::int32_t>(why);
    maybe_stop();
  });

  const auto run_result = loop.run();
  REQUIRE(run_result.has_value());
  CHECK(!watchdog.bitten);
  CHECK(server_pending_at_close > 0);  // a real queue drained before the FIN
  CHECK(client_messages == kMessages);
  CHECK(min_seen == std::size_t{1024});
  CHECK(client_closed == 1);
  CHECK(server_closed == 1);
  CHECK(client_code == static_cast<std::int32_t>(NetError::connection_closed));
  CHECK(server_code == static_cast<std::int32_t>(NetError::connection_closed));
}

TEST_CASE("abort() delivers connection_reset to both ends") {
  EventLoop loop;
  Watchdog watchdog{loop};

  auto listener_result = TcpListener::listen(loop, Address::loopback(0));
  REQUIRE(listener_result.has_value());
  TcpListener::Ptr listener = *listener_result;
  const Address bound = listener->local();

  int client_closed = 0;
  int server_closed = 0;
  std::int32_t client_code = -1;
  std::int32_t server_code = -1;
  bool client_connected = false;
  int server_messages = 0;
  TcpConnection::Ptr server_conn;

  auto maybe_stop = [&] {
    if (client_closed > 0 && server_closed > 0) {
      loop.stop();
    }
  };

  listener->set_on_connection([&](TcpConnection::Ptr conn) {
    server_conn = conn;  // the Ptr dies with this callback otherwise
    conn->set_on_message([&](std::span<const std::byte>) {
      server_messages += 1;
    });
    conn->set_on_closed([&](NetError why) {
      server_closed += 1;
      server_code = static_cast<std::int32_t>(why);
      listener->close();
      maybe_stop();
    });
  });

  auto client_result = TcpConnection::connect(loop, bound);
  REQUIRE(client_result.has_value());
  TcpConnection::Ptr client = *client_result;
  client->set_on_connected([&] {
    client_connected = true;
    client->abort();  // the abrupt disconnect, deliberately mid-stream
  });
  client->set_on_closed([&](NetError why) {
    client_closed += 1;
    client_code = static_cast<std::int32_t>(why);
    maybe_stop();
  });

  const auto run_result = loop.run();
  REQUIRE(run_result.has_value());
  CHECK(!watchdog.bitten);
  CHECK(client_connected);
  CHECK(client_closed == 1);
  CHECK(client_code == static_cast<std::int32_t>(NetError::connection_reset));
  CHECK(server_closed == 1);
  CHECK(server_code == static_cast<std::int32_t>(NetError::connection_reset));
  CHECK(server_messages == 0);
}

TEST_CASE("connecting where nothing listens reports connection_refused") {
  EventLoop loop;
  Watchdog watchdog{loop};

  // Grab an ephemeral port, then kill its listener BEFORE connecting.
  auto squat = TcpListener::listen(loop, Address::loopback(0));
  REQUIRE(squat.has_value());
  const Address dead = (*squat)->local();
  REQUIRE(dead.port != 0);
  (*squat)->close();
  const auto turn = loop.run_once();  // let the listener's close complete
  REQUIRE(turn.has_value());

  bool connected = false;
  int closed = 0;
  std::int32_t code = -1;

  auto client_result = TcpConnection::connect(loop, dead);
  REQUIRE(client_result.has_value());
  TcpConnection::Ptr client = *client_result;
  client->set_on_connected([&] { connected = true; });
  client->set_on_closed([&](NetError why) {
    closed += 1;
    code = static_cast<std::int32_t>(why);
    loop.stop();
  });

  const auto run_result = loop.run();
  REQUIRE(run_result.has_value());
  CHECK(!watchdog.bitten);
  CHECK(!connected);
  CHECK(closed == 1);
  CHECK(code == static_cast<std::int32_t>(NetError::connection_refused));
}

TEST_CASE("an oversized frame kills the receiver, not the sender's send") {
  EventLoop loop;
  Watchdog watchdog{loop};

  auto listener_result = TcpListener::listen(loop, Address::loopback(0));
  REQUIRE(listener_result.has_value());
  TcpListener::Ptr listener = *listener_result;
  const Address bound = listener->local();

  int server_closed = 0;
  std::int32_t server_code = -1;
  int server_messages = 0;
  int client_closed = 0;
  std::int32_t client_code = -1;
  ws::core::Status oversize_send;
  TcpConnection::Ptr server_conn;

  auto maybe_stop = [&] {
    if (client_closed > 0 && server_closed > 0) {
      loop.stop();
    }
  };

  listener->set_on_connection([&](TcpConnection::Ptr conn) {
    server_conn = conn;  // the Ptr dies with this callback otherwise
    conn->set_max_message_size(16);  // tiny cap for the case
    conn->set_on_message([&](std::span<const std::byte>) {
      server_messages += 1;
    });
    conn->set_on_closed([&](NetError why) {
      server_closed += 1;
      server_code = static_cast<std::int32_t>(why);
      listener->close();
      maybe_stop();
    });
  });

  auto client_result = TcpConnection::connect(loop, bound);
  REQUIRE(client_result.has_value());
  TcpConnection::Ptr client = *client_result;
  bool still_open_after_reject = false;
  client->set_on_connected([&] {
    // The sender's own cap (1 MiB default) refuses this locally and
    // leaves the connection alive:
    oversize_send = client->send(payload(2 * 1024 * 1024, 1));
    still_open_after_reject = client->is_open();
    (void)client->send(payload(4096, 0xEE));  // over the SERVER's cap: kills it
  });
  client->set_on_closed([&](NetError why) {
    client_closed += 1;
    client_code = static_cast<std::int32_t>(why);
    maybe_stop();
  });

  const auto run_result = loop.run();
  REQUIRE(run_result.has_value());
  REQUIRE(oversize_send.has_value() == false);
  CHECK(oversize_send.error().code ==
        static_cast<std::int32_t>(NetError::message_too_large));
  CHECK(still_open_after_reject);
  CHECK(!watchdog.bitten);
  CHECK(server_closed == 1);
  CHECK(server_code ==
        static_cast<std::int32_t>(NetError::message_too_large));
  CHECK(server_messages == 0);
  CHECK(client_closed == 1);
  // The receiver RST'd (abort on oversize), which is what the sender sees.
  CHECK(client_code == static_cast<std::int32_t>(NetError::connection_reset));
}

TEST_CASE("a 1000-message burst survives the backpressure queue") {
  EventLoop loop;
  Watchdog watchdog{loop};

  auto listener_result = TcpListener::listen(loop, Address::loopback(0));
  REQUIRE(listener_result.has_value());
  TcpListener::Ptr listener = *listener_result;
  const Address bound = listener->local();

  constexpr int kMessages = 1000;
  const auto blob = payload(64, 0x33);
  int echoes = 0;
  int client_closed = 0;
  int server_closed = 0;
  TcpConnection::Ptr server_conn;

  auto maybe_stop = [&] {
    if (client_closed > 0 && server_closed > 0) {
      loop.stop();
    }
  };

  listener->set_on_connection([&](TcpConnection::Ptr conn) {
    server_conn = conn;  // the Ptr dies with this callback otherwise
    conn->set_on_message([&](std::span<const std::byte> message) {
      (void)server_conn->send(message);  // echo, via the case-scope holder
    });
    conn->set_on_closed([&](NetError) {
      server_closed += 1;
      listener->close();
      maybe_stop();
    });
  });

  auto client_result = TcpConnection::connect(loop, bound);
  REQUIRE(client_result.has_value());
  TcpConnection::Ptr client = *client_result;
  std::vector<ws::core::Status> send_results;
  client->set_on_connected([&] {
    for (int i = 0; i < kMessages; ++i) {
      send_results.push_back(client->send(std::span<const std::byte>{blob}));
    }
  });
  client->set_on_message([&](std::span<const std::byte>) {
    echoes += 1;
    if (echoes == kMessages) {
      (void)client->close();
    }
  });
  client->set_on_closed([&](NetError) {
    client_closed += 1;
    maybe_stop();
  });

  const auto run_result = loop.run();
  REQUIRE(run_result.has_value());
  REQUIRE(send_results.size() == std::size_t{kMessages});
  for (const auto& s : send_results) {
    REQUIRE(s.has_value());
  }
  CHECK(!watchdog.bitten);
  CHECK(echoes == kMessages);
  CHECK(client_closed == 1);
  CHECK(server_closed == 1);
}

TEST_CASE("teardown with open, abandoned and pending connections is safe") {
  // (a) an OPEN connection abandoned mid-flight: the user drops the Ptr
  //     while a large send is still queued; the loop destructor must
  //     force-close it (no EBUSY leak, no double-close, no crash).
  {
    EventLoop loop;
    auto listener_result = TcpListener::listen(loop, Address::loopback(0));
    REQUIRE(listener_result.has_value());
    TcpListener::Ptr listener = *listener_result;

    int server_closed = 0;
    listener->set_on_connection([&](TcpConnection::Ptr conn) {
      conn->set_on_closed([&](NetError) { server_closed += 1; });
    });

    auto client_result = TcpConnection::connect(loop, listener->local());
    REQUIRE(client_result.has_value());
    TcpConnection::Ptr client = *client_result;
    bool connected = false;
    client->set_on_connected([&] {
      connected = true;
      (void)client->send(payload(70000, 1));  // mid-flight at teardown
      client.reset();  // abandoned OPEN: the socket lives on without us
    });
    int turns = 0;
    while (!connected && turns < 1000) {  // bounded: a wedged connect
      REQUIRE(loop.run_once().has_value());  // fails the case, not hangs it
      turns += 1;
    }
    CHECK(connected);
  }  // ~EventLoop: force-closes listener + the abandoned connection

  // (b) a connect that never completed: destroy the loop while the
  //     socket is still connecting (nothing listens on that port).
  {
    EventLoop loop;
    auto client_result = TcpConnection::connect(loop,
                                                Address::loopback(27474));
    REQUIRE(client_result.has_value());
  }  // ~EventLoop: force-closes the connecting socket
}
