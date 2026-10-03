// LAN discovery through the public API, on a private loopback channel:
// a browser on an ephemeral port, an announcer aimed straight at it
// (the wire protocol is identical to the broadcast one, and CI machines
// must not depend on broadcast being routable). Same rules as the
// other real-socket files: one EventLoop per case, callbacks only set
// flags, a watchdog stops a wedged case.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <ws/net/discovery.hpp>
#include <ws/net/errors.hpp>
#include <ws/net/event_loop.hpp>
#include <ws/net/udp.hpp>

namespace {

using ws::net::Address;
using ws::net::DiscoveryAnnouncer;
using ws::net::DiscoveryBrowser;
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

// The port is protocol surface: 27474 is documented and deliberate
// (discovery.hpp spells out the why) - a refactor may not quietly
// change it.
static_assert(ws::net::kDefaultDiscoveryPort == std::uint16_t{27474},
              "the discovery port is protocol surface, not an accident");

}  // namespace

TEST_CASE("an announcer's beats reach a browser, magic stripped") {
  EventLoop loop;
  Watchdog watchdog{loop};

  auto browser_result = DiscoveryBrowser::browse(loop, 0, true);
  REQUIRE(browser_result.has_value());
  DiscoveryBrowser::Ptr browser = *browser_result;
  const Address browser_at = browser->local();
  REQUIRE(browser_at.port != 0);

  const std::vector<std::byte> body = payload(32, 0xA7);
  std::vector<std::vector<std::byte>> heard;
  std::vector<Address> froms;
  browser->set_on_peer([&](std::span<const std::byte> p, Address from) {
    heard.emplace_back(p.begin(), p.end());
    froms.push_back(from);
    if (heard.size() == 2) {
      loop.stop();
    }
  });

  // Directed at the browser, not at broadcast - everything about the
  // beats except the destination address is the real protocol.
  auto announcer_result = DiscoveryAnnouncer::announce(
      loop, body, Address::loopback(browser_at.port), 50);
  REQUIRE(announcer_result.has_value());
  DiscoveryAnnouncer::Ptr announcer = *announcer_result;

  (void)loop.run();

  CHECK(watchdog.bitten == false);
  REQUIRE(heard.size() == 2);  // the immediate posted beat + one 50ms beat
  CHECK(heard[0] == body);    // payload only - the 4-byte magic is gone
  CHECK(heard[1] == body);
  REQUIRE(froms.size() == 2);
  CHECK(froms[0] == froms[1]);  // one announcer socket behind every beat
  CHECK(froms[0].port != 0);
  CHECK(froms[0].host == "127.0.0.1");
}

TEST_CASE("send_once answers off-cadence from the announcer's own socket") {
  EventLoop loop;
  Watchdog watchdog{loop};

  auto browser_result = DiscoveryBrowser::browse(loop, 0, true);
  REQUIRE(browser_result.has_value());
  DiscoveryBrowser::Ptr browser = *browser_result;
  const Address browser_at = browser->local();

  const std::vector<std::byte> body = payload(16, 0x5C);
  std::vector<std::vector<std::byte>> heard;
  std::vector<Address> froms;
  int beats = 0;
  browser->set_on_peer([&](std::span<const std::byte> p, Address from) {
    ++beats;
    heard.emplace_back(p.begin(), p.end());
    froms.push_back(from);
    if (beats == 1) {
      loop.stop();  // park the loop at the cadence's first beat
    }
  });

  // A cadence far beyond the case: the only scheduled beat is the
  // immediate posted one, so anything after it must be a send_once.
  auto announcer_result = DiscoveryAnnouncer::announce(
      loop, body, Address::loopback(browser_at.port), 60000);
  REQUIRE(announcer_result.has_value());
  DiscoveryAnnouncer::Ptr announcer = *announcer_result;

  (void)loop.run();
  REQUIRE(beats == 1);

  // The loop is parked, so the send is synchronous - exactly the state
  // a browser callback answers from (same thread, same socket).
  REQUIRE(
      announcer->send_once(Address::loopback(browser_at.port)).has_value());
  // No host, no beat.
  CHECK(announcer->send_once(Address{}).error().code ==
        static_cast<std::int32_t>(NetError::invalid_argument));

  // Plenty of loop time for the answer to land - and nothing else may.
  (void)loop.start_timer(300, [&loop] { loop.stop(); });
  (void)loop.run();

  CHECK(watchdog.bitten == false);
  REQUIRE(beats == 2);
  CHECK(heard[1] == body);       // the same payload...
  CHECK(froms[0] == froms[1]);   // ...from the same announcer socket
  CHECK(froms[1].host == "127.0.0.1");
}

TEST_CASE("a browser drops foreign packets and keeps the magic ones") {
  EventLoop loop;
  Watchdog watchdog{loop};

  auto browser_result = DiscoveryBrowser::browse(loop, 0, true);
  REQUIRE(browser_result.has_value());
  DiscoveryBrowser::Ptr browser = *browser_result;
  REQUIRE(browser->local().port != 0);
  // local() on an any-bind is 0.0.0.0:port - a port to READ, not a
  // destination. Aim at loopback, like a real announcer would.
  const Address browser_at = Address::loopback(browser->local().port);

  auto raw_result = UdpSocket::bind(loop, Address::loopback(0));
  REQUIRE(raw_result.has_value());
  UdpSocket::Ptr raw = *raw_result;

  int heard = 0;
  std::vector<std::byte> seen;
  browser->set_on_peer([&](std::span<const std::byte> p, Address) {
    ++heard;
    seen.assign(p.begin(), p.end());
    loop.stop();
  });

  // No magic header: foreign traffic on the port, dropped silently.
  REQUIRE(raw->send_to(browser_at, payload(12, 0x77)).has_value());
  // Hand-built magic packet, spelled out on purpose: the wire format
  // is protocol surface, so a format change must fail right here.
  const std::byte magic[4] = {std::byte{'W'}, std::byte{'S'}, std::byte{'D'},
                              std::byte{'1'}};
  std::vector<std::byte> wire(magic, magic + 4);
  wire.push_back(std::byte{'X'});
  REQUIRE(raw->send_to(browser_at, wire).has_value());

  // Fast failure if nothing at all arrives: otherwise the loop would
  // idle to the watchdog.
  (void)loop.start_timer(300, [&loop] { loop.stop(); });
  (void)loop.run();

  CHECK(watchdog.bitten == false);
  REQUIRE(heard == 1);  // the foreign packet never landed
  CHECK(seen.size() == 1);
  CHECK(seen[0] == std::byte{'X'});
}

TEST_CASE("a dropped announcer Ptr keeps beating until the loop cleans up") {
  EventLoop loop;
  Watchdog watchdog{loop};

  auto browser_result = DiscoveryBrowser::browse(loop, 0, true);
  REQUIRE(browser_result.has_value());
  DiscoveryBrowser::Ptr browser = *browser_result;
  const Address browser_at = browser->local();

  int beats = 0;
  browser->set_on_peer([&](std::span<const std::byte>, Address) {
    ++beats;
    if (beats == 2) {
      loop.stop();
    }
  });

  {
    auto announcer_result = DiscoveryAnnouncer::announce(
        loop, payload(8, 0x33), Address::loopback(browser_at.port), 50);
    REQUIRE(announcer_result.has_value());
    // The Ptr dies here with the cadence still armed - the socket's
    // keepalive owns the Impl now, and that is the whole point.
  }

  (void)loop.run();

  CHECK(watchdog.bitten == false);
  CHECK(beats == 2);  // beats kept coming without a single user handle
}

TEST_CASE("stop() freezes the cadence for good") {
  EventLoop loop;
  Watchdog watchdog{loop};

  auto browser_result = DiscoveryBrowser::browse(loop, 0, true);
  REQUIRE(browser_result.has_value());
  DiscoveryBrowser::Ptr browser = *browser_result;
  const Address browser_at = browser->local();

  int beats = 0;
  browser->set_on_peer([&](std::span<const std::byte>, Address) {
    ++beats;
    if (beats == 2) {
      loop.stop();
    }
  });

  auto announcer_result = DiscoveryAnnouncer::announce(
      loop, payload(4, 0x44), Address::loopback(browser_at.port), 50);
  REQUIRE(announcer_result.has_value());
  DiscoveryAnnouncer::Ptr announcer = *announcer_result;

  (void)loop.run();  // returns at the second beat (the callback stops it)
  REQUIRE(beats == 2);

  announcer->stop();  // with the loop parked, this is deterministic
  announcer->stop();  // and idempotent - nothing may double-error

  // Plenty of loop time for a stray beat to land. None may.
  (void)loop.start_timer(250, [&loop] { loop.stop(); });
  (void)loop.run();

  CHECK(watchdog.bitten == false);
  CHECK(beats == 2);  // frozen
}

TEST_CASE("announce refuses what cannot be announced") {
  EventLoop loop;

  CHECK(DiscoveryAnnouncer::announce(loop, {}, Address::loopback(27474), 50)
            .error()
            .code ==
        static_cast<std::int32_t>(NetError::invalid_argument));
  CHECK(DiscoveryAnnouncer::announce(loop, payload(4, 1),
                                     Address::loopback(27474), 0)
            .error()
            .code ==
        static_cast<std::int32_t>(NetError::invalid_argument));
  CHECK(DiscoveryAnnouncer::announce(loop, payload(4, 1), Address{}, 50)
            .error()
            .code ==
        static_cast<std::int32_t>(NetError::invalid_argument));
  // Payload plus the 4-byte wire header must still fit one datagram.
  const std::size_t huge = UdpSocket::kMaxDatagramSize - 3;
  CHECK(DiscoveryAnnouncer::announce(
            loop, std::vector<std::byte>(huge, std::byte{1}),
            Address::loopback(27474), 50)
            .error()
            .code ==
        static_cast<std::int32_t>(NetError::message_too_large));
}
