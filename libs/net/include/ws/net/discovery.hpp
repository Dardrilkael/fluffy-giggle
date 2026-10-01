#pragma once

// LAN discovery over UDP broadcast: a service announces itself by
// sending a small payload to the broadcast address on a fixed cadence;
// anyone browsing hears it and learns the payload plus the sender's
// address. The wire format is 4 magic bytes ("WSD1") + the payload -
// foreign traffic on the port is dropped silently, and browsers strip
// the header before handing the payload up. No handshake, no
// reliability: a vanished announcement simply means "not seen lately"
// (a browser that wants liveness prunes on a timeout of its own
// choosing).

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include <ws/core/result.hpp>
#include <ws/net/address.hpp>
#include <ws/net/visibility.hpp>

namespace ws::net {

class EventLoop;

// The port discovery lives on when nobody overrides it. 27474: no
// IANA-registered service claims it (public searches show only
// scan-traffic trackers, nothing assigned), and it sits below every
// major platform's ephemeral range (Linux 32768-60999, Windows
// 49152-65535, macOS 49152-65535), so a browser binding it can never
// lose the port to an ephemeral allocation. An echo of the 47474
// first sketched in the v1 plan, leading 4 swapped for a 2 to clear
// the Linux floor. Every entry point takes the port explicitly -
// this is only the default.
inline constexpr std::uint16_t kDefaultDiscoveryPort = 27474;

class WS_NET_API DiscoveryAnnouncer {
 public:
  using Ptr = std::shared_ptr<DiscoveryAnnouncer>;

  // Sends `payload` to `to` every period_ms, first beat immediately
  // (queued as a post - it lands at run() entry or the next turn).
  // Binds an ephemeral socket; announcements carry that socket's
  // address as their source. The cadence re-arms itself until stop()
  // or until the loop can no longer schedule - the announcer then goes
  // quiet; it never errors the loop.
  [[nodiscard]] static ws::core::Result<Ptr> announce(
      EventLoop& loop, std::span<const std::byte> payload,
      const Address& to = Address::broadcast(kDefaultDiscoveryPort),
      std::uint64_t period_ms = 1000);

  DiscoveryAnnouncer(const DiscoveryAnnouncer&) = delete;
  DiscoveryAnnouncer& operator=(const DiscoveryAnnouncer&) = delete;
  DiscoveryAnnouncer(DiscoveryAnnouncer&&) = delete;
  DiscoveryAnnouncer& operator=(DiscoveryAnnouncer&&) = delete;

  // Stops the cadence and releases the socket. Idempotent. Announcers
  // never receive anything, so there is no callback to interrupt.
  void stop();

 private:
  struct Impl;
  explicit DiscoveryAnnouncer(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};

class WS_NET_API DiscoveryBrowser {
 public:
  using Ptr = std::shared_ptr<DiscoveryBrowser>;

  // Binds the discovery port and listens. `reuse` lets several
  // browsers share one machine; port 0 picks an ephemeral port - what
  // tests use for a private loopback channel.
  [[nodiscard]] static ws::core::Result<Ptr> browse(
      EventLoop& loop, std::uint16_t port = kDefaultDiscoveryPort,
      bool reuse = true);

  DiscoveryBrowser(const DiscoveryBrowser&) = delete;
  DiscoveryBrowser& operator=(const DiscoveryBrowser&) = delete;
  DiscoveryBrowser(DiscoveryBrowser&&) = delete;
  DiscoveryBrowser& operator=(DiscoveryBrowser&&) = delete;

  // One announcement heard: the payload (magic already stripped) and
  // the announcer's address (host + its source port). Same
  // span-lifetime rule as everywhere: the bytes are valid only for
  // the call.
  void set_on_peer(
      std::function<void(std::span<const std::byte> payload, Address from)>
          on_peer);

  // The address actually bound (the real port when the bind used 0).
  [[nodiscard]] Address local() const;

  // Stops listening and releases the port. Idempotent.
  void stop();

 private:
  struct Impl;
  explicit DiscoveryBrowser(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};

}  // namespace ws::net
