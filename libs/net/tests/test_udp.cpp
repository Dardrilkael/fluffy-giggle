// Real-socket UDP over the public API (loopback, ephemeral ports - the
// suite needs nothing beyond the machine itself). Same shape as
// test_tcp: one EventLoop per case, run() turns until the case closes
// things, a watchdog stops a wedged case, and callbacks only set
// flags - doctest asserts live at test level.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <ws/net/errors.hpp>
#include <ws/net/event_loop.hpp>
#include <ws/net/udp.hpp>

namespace {

using ws::net::Address;
using ws::net::EventLoop;
using ws::net::NetError;
using ws::net::UdpSocket;

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

TEST_CASE("udp round-trips a datagram and reports its sender") {
  EventLoop loop;
  Watchdog watchdog{loop};

  auto rx_result = UdpSocket::bind(loop, Address::loopback(0));
  REQUIRE(rx_result.has_value());
  UdpSocket::Ptr rx = *rx_result;
  const Address bound = rx->local();
  REQUIRE(bound.port != 0);  // ephemeral :0 got a real port

  auto tx_result = UdpSocket::bind(loop, Address::loopback(0));
  REQUIRE(tx_result.has_value());
  UdpSocket::Ptr tx = *tx_result;

  int packets = 0;
  std::vector<std::byte> seen;
  Address from;
  rx->set_on_packet([&](std::span<const std::byte> data, Address sender) {
    ++packets;
    seen.assign(data.begin(), data.end());
    from = sender;
    loop.stop();
  });

  const std::vector<std::byte> body = payload(1000, 0x5A);
  REQUIRE(tx->send_to(bound, body).has_value());
  (void)loop.run();

  CHECK(watchdog.bitten == false);
  REQUIRE(packets == 1);
  CHECK(seen == body);          // one piece, byte for byte
  CHECK(from == tx->local());   // host + the sender's real source port
}

TEST_CASE("udp delivers datagrams whole and in order on loopback") {
  EventLoop loop;
  Watchdog watchdog{loop};

  auto rx_result = UdpSocket::bind(loop, Address::loopback(0));
  REQUIRE(rx_result.has_value());
  UdpSocket::Ptr rx = *rx_result;
  const Address bound = rx->local();

  auto tx_result = UdpSocket::bind(loop, Address::loopback(0));
  REQUIRE(tx_result.has_value());
  UdpSocket::Ptr tx = *tx_result;

  std::vector<std::size_t> sizes_seen;
  std::vector<std::vector<std::byte>> bodies;
  rx->set_on_packet([&](std::span<const std::byte> data, Address) {
    sizes_seen.push_back(data.size());
    bodies.emplace_back(data.begin(), data.end());
    if (sizes_seen.size() == 3) {
      loop.stop();
    }
  });

  // Tiny, ordinary, and near the wire cap - none of them may arrive
  // fragmented or dropped (loopback does not fragment UDP).
  const std::vector<std::size_t> sizes{1, 1500, 60000};
  for (const std::size_t size : sizes) {
    const std::uint8_t fill = static_cast<std::uint8_t>(size & 0xFF);
    REQUIRE(tx->send_to(bound, payload(size, fill)).has_value());
  }
  (void)loop.run();

  CHECK(watchdog.bitten == false);
  REQUIRE(sizes_seen.size() == 3);
  CHECK(sizes_seen == sizes);
  for (std::size_t i = 0; i < sizes.size(); ++i) {
    CHECK(bodies[i] == payload(sizes[i], static_cast<std::uint8_t>(
                                            sizes[i] & 0xFF)));
  }
}

TEST_CASE("a send to a dead port never kills the sender") {
  EventLoop loop;
  Watchdog watchdog{loop};

  auto tx_result = UdpSocket::bind(loop, Address::loopback(0));
  REQUIRE(tx_result.has_value());
  UdpSocket::Ptr tx = *tx_result;

  Address dead;
  {
    auto gone_result = UdpSocket::bind(loop, Address::loopback(0));
    REQUIRE(gone_result.has_value());
    dead = (*gone_result)->local();
    REQUIRE((*gone_result)->close().has_value());
  }  // a closed socket used to own that port

  // Two sends into the void: the second wave is where Windows lands the
  // ICMP "port unreachable" (as a recv error on this socket), and the
  // backend has to swallow it - the case passes by surviving.
  REQUIRE(tx->send_to(dead, payload(10, 1)).has_value());
  REQUIRE(tx->send_to(dead, payload(10, 2)).has_value());
  (void)loop.start_timer(200, [&loop] { loop.stop(); });
  (void)loop.run();
  CHECK(watchdog.bitten == false);
  CHECK(tx->is_open());

  // And the socket still works afterwards - the reset was noise.
  auto rx_result = UdpSocket::bind(loop, Address::loopback(0));
  REQUIRE(rx_result.has_value());
  UdpSocket::Ptr rx = *rx_result;
  int packets = 0;
  rx->set_on_packet([&](std::span<const std::byte>, Address) {
    ++packets;
    loop.stop();
  });
  REQUIRE(tx->send_to(rx->local(), payload(7, 0x66)).has_value());
  (void)loop.run();
  CHECK(watchdog.bitten == false);
  CHECK(packets == 1);
}

TEST_CASE("close is exactly-once and stops delivery") {
  EventLoop loop;
  Watchdog watchdog{loop};

  auto rx_result = UdpSocket::bind(loop, Address::loopback(0));
  REQUIRE(rx_result.has_value());
  UdpSocket::Ptr rx = *rx_result;
  const Address closed_at = rx->local();
  REQUIRE(closed_at.port != 0);

  auto tx_result = UdpSocket::bind(loop, Address::loopback(0));
  REQUIRE(tx_result.has_value());
  UdpSocket::Ptr tx = *tx_result;

  int packets = 0;
  rx->set_on_packet([&](std::span<const std::byte>, Address) { ++packets; });

  REQUIRE(rx->close().has_value());
  CHECK(rx->close().error().code ==
        static_cast<std::int32_t>(NetError::already_closed));
  CHECK(rx->is_open() == false);
  CHECK(rx->send_to(tx->local(), payload(4, 1)).error().code ==
        static_cast<std::int32_t>(NetError::already_closed));

  // A datagram into the closed port must simply vanish: no delivery,
  // no error back at the sender (best-effort).
  REQUIRE(tx->send_to(closed_at, payload(4, 2)).has_value());
  (void)loop.start_timer(200, [&loop] { loop.stop(); });
  (void)loop.run();

  CHECK(watchdog.bitten == false);
  CHECK(packets == 0);
  CHECK(tx->is_open());  // the sender of the lost datagram is fine
}

TEST_CASE("send_to rejects the impossible, then ships the possible") {
  EventLoop loop;
  Watchdog watchdog{loop};

  auto tx_result = UdpSocket::bind(loop, Address::loopback(0));
  REQUIRE(tx_result.has_value());
  UdpSocket::Ptr tx = *tx_result;

  std::vector<std::byte> over(UdpSocket::kMaxDatagramSize + 1, std::byte{1});
  CHECK(tx->send_to(Address::loopback(9000), over).error().code ==
        static_cast<std::int32_t>(NetError::message_too_large));
  CHECK(tx->send_to(Address::loopback(9000), {}).error().code ==
        static_cast<std::int32_t>(NetError::invalid_argument));
  CHECK(tx->send_to(Address{}, payload(4, 1)).error().code ==
        static_cast<std::int32_t>(NetError::invalid_argument));

  // The rejects were argument-level; the socket never degraded.
  auto rx_result = UdpSocket::bind(loop, Address::loopback(0));
  REQUIRE(rx_result.has_value());
  UdpSocket::Ptr rx = *rx_result;
  int packets = 0;
  std::vector<std::byte> seen;
  rx->set_on_packet([&](std::span<const std::byte> data, Address) {
    ++packets;
    seen.assign(data.begin(), data.end());
    loop.stop();
  });
  const std::vector<std::byte> body = payload(64, 0x7E);
  REQUIRE(tx->send_to(rx->local(), body).has_value());
  (void)loop.run();

  CHECK(watchdog.bitten == false);
  CHECK(packets == 1);
  CHECK(seen == body);
}

TEST_CASE("a dropped Ptr keeps receiving until the loop cleans up") {
  EventLoop loop;
  Watchdog watchdog{loop};

  auto tx_result = UdpSocket::bind(loop, Address::loopback(0));
  REQUIRE(tx_result.has_value());
  UdpSocket::Ptr tx = *tx_result;

  Address orphan_at;
  int ghost_packets = 0;
  {
    auto s_result = UdpSocket::bind(loop, Address::loopback(0));
    REQUIRE(s_result.has_value());
    UdpSocket::Ptr s = *s_result;
    s->set_on_packet([&](std::span<const std::byte>, Address) {
      ++ghost_packets;
    });
    orphan_at = s->local();
    // `s` dies here with the socket still open and no close() in
    // sight - the whole point: nothing may free a live handle.
  }

  REQUIRE(tx->send_to(orphan_at, payload(10, 3)).has_value());
  (void)loop.start_timer(200, [&loop] { loop.stop(); });
  (void)loop.run();

  CHECK(watchdog.bitten == false);
  // The socket's keepalive held the Impl - user callback included -
  // alive after every outside reference was gone.
  CHECK(ghost_packets == 1);
}

TEST_CASE("a stashed on_packet span is poison after the callback") {
  EventLoop loop;
  Watchdog watchdog{loop};

  auto rx_result = UdpSocket::bind(loop, Address::loopback(0));
  REQUIRE(rx_result.has_value());
  UdpSocket::Ptr rx = *rx_result;
  const Address bound = rx->local();

  auto tx_result = UdpSocket::bind(loop, Address::loopback(0));
  REQUIRE(tx_result.has_value());
  UdpSocket::Ptr tx = *tx_result;

  std::span<const std::byte> stash;  // the "do not do this" stash
  rx->set_on_packet([&](std::span<const std::byte> data, Address) {
    stash = data;
    loop.stop();
  });
  REQUIRE(tx->send_to(bound, payload(16, 0x42)).has_value());
  (void)loop.run();
  CHECK(watchdog.bitten == false);

#ifndef NDEBUG
  // The backend poisons the receive buffer the moment the callback
  // returns - the same contract test_frame pins for on_message. In
  // release builds the bytes are merely unspecified, so we pin nothing
  // there.
  REQUIRE(stash.size() == 16);
  int poisoned = 0;
  for (const std::byte b : stash) {
    poisoned += (b == std::byte{0xCD}) ? 1 : 0;
  }
  CHECK(poisoned == 16);
#endif
}
