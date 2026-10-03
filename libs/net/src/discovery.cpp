#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "core/transport.hpp"
#include <ws/net/discovery.hpp>
#include <ws/net/errors.hpp>
#include <ws/net/event_loop.hpp>
#include <ws/net/udp.hpp>

namespace ws::net {

namespace {

// The discovery wire header: magic + payload. Browsers strip the
// magic; anything without it is foreign traffic on the port, dropped
// silently (malformed_frame stays reserved for a future header check).
constexpr std::byte kDiscoveryMagic[4] = {
    static_cast<std::byte>('W'), static_cast<std::byte>('S'),
    static_cast<std::byte>('D'), static_cast<std::byte>('1')};
constexpr std::size_t kMagicSize = sizeof(kDiscoveryMagic);

}  // namespace

// ---- DiscoveryAnnouncer ----

struct DiscoveryAnnouncer::Impl final
    : public std::enable_shared_from_this<Impl>,
      public detail::IDatagramSink {
  EventLoop* loop = nullptr;
  std::unique_ptr<detail::IDatagram> datagram;
  std::vector<std::byte> wire;  // magic + payload, built once
  Address to;
  std::uint64_t period_ms = 0;
  ws::core::Result<TimerHandle> timer;  // the live cadence arm
  bool stopped = false;

  // One cadence beat: send, then schedule the next. The send is
  // fire-and-forget by design (the whole protocol is best-effort); a
  // scheduling failure means the loop is ending - go quiet, never
  // crash a teardown.
  void tick() {
    if (stopped) {
      return;
    }
    (void)datagram->send_to(to, wire);
    timer = loop->start_timer(
        period_ms, [self = shared_from_this()] { self->tick(); });
    if (!timer.has_value()) {
      stopped = true;  // cadence over: teardown will close the socket
    }
  }

  // Announcers never listen. The bind is send-only in practice, but
  // the socket may still hand us stray packets - drop them.
  void on_packet(std::span<const std::byte> /*data*/,
                 Address /*from*/) override {}
};

DiscoveryAnnouncer::DiscoveryAnnouncer(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ws::core::Result<DiscoveryAnnouncer::Ptr> DiscoveryAnnouncer::announce(
    EventLoop& loop, std::span<const std::byte> payload, const Address& to,
    std::uint64_t period_ms) {
  if (payload.empty()) {
    return fail(NetError::invalid_argument, "announce: an empty payload says nothing");
  }
  if (!to.valid()) {
    return fail(NetError::invalid_argument,
                "announce: the target address has no host");
  }
  if (period_ms == 0) {
    return fail(NetError::invalid_argument, "announce: the period must be positive");
  }
  if (payload.size() + kMagicSize > UdpSocket::kMaxDatagramSize) {
    return fail(NetError::message_too_large,
                "announce: payload plus the wire header over the datagram cap");
  }
  std::unique_ptr<detail::IDatagram> datagram =
      detail::make_datagram(*loop.backend(), Address::any(0), false);
  if (datagram == nullptr) {
    return fail(NetError::bind_failed, "announce: no ephemeral socket to announce from");
  }
  auto impl = std::make_shared<Impl>();
  impl->loop = &loop;
  impl->datagram = std::move(datagram);
  impl->to = to;
  impl->period_ms = period_ms;
  impl->wire.assign(kDiscoveryMagic, kDiscoveryMagic + kMagicSize);
  impl->wire.insert(impl->wire.end(), payload.begin(), payload.end());
  impl->datagram->attach_sink(impl.get());
  impl->datagram->set_keepalive(impl);  // the ctx holds us until its close cb
  // First beat as a post (lands at run() entry or the next turn); the
  // beat itself schedules the next, so there is exactly ONE timer
  // chain. Arming a timer here as well would double the cadence -
  // every tick re-arms, and so would this arm, two chains where the
  // caller asked for one.
  const std::shared_ptr<Impl> beat = impl;
  (void)loop.post([beat] { beat->tick(); });
  return Ptr{new DiscoveryAnnouncer(std::move(impl))};
}

ws::core::Status DiscoveryAnnouncer::send_once(const Address& to) {
  if (!to.valid()) {
    return fail(NetError::invalid_argument,
                "send_once: the target address has no host");
  }
  // The announcer's own socket and already-built wire, one send, no
  // cadence touched. A stopped announcer fails naturally - its socket
  // is closed, and send_to reports already_closed.
  return impl_->datagram->send_to(to, impl_->wire);
}

void DiscoveryAnnouncer::stop() {
  impl_->stopped = true;  // first: a beat mid-flight must not re-arm
  if (impl_->timer.has_value() && impl_->timer.value().valid()) {
    (void)impl_->loop->cancel_timer(impl_->timer.value());
  }
  // Idempotent at the seam; the close callback may run synchronously
  // and nothing below touches impl_ again.
  impl_->datagram->close();
}

// ---- DiscoveryBrowser ----

struct DiscoveryBrowser::Impl final {
  // The sink adapter - same reason as UdpSocket's: the Impl keeps a
  // user-callback member named on_peer, so the IDatagramSink
  // implementation lives one level down.
  struct Sink final : public detail::IDatagramSink {
    Impl* impl;
    explicit Sink(Impl* owner) : impl(owner) {}
    void on_packet(std::span<const std::byte> data, Address from) override {
      impl->deliver(data, from);
    }
  };

  std::unique_ptr<detail::IDatagram> datagram;
  std::unique_ptr<Sink> sink;
  std::function<void(std::span<const std::byte> payload, Address from)> on_peer;

  // Magic check first: foreign packets on the port are not ours to
  // interpret - drop them without ceremony.
  void deliver(std::span<const std::byte> data, Address from) {
    if (data.size() < kMagicSize) {
      return;
    }
    if (!std::equal(kDiscoveryMagic, kDiscoveryMagic + kMagicSize, data.begin(),
                    data.begin() + kMagicSize)) {
      return;
    }
    if (on_peer) {
      on_peer(
          std::span<const std::byte>{data.begin() + kMagicSize, data.end()},
          from);
    }
  }
};

DiscoveryBrowser::DiscoveryBrowser(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ws::core::Result<DiscoveryBrowser::Ptr> DiscoveryBrowser::browse(
    EventLoop& loop, std::uint16_t port, bool reuse) {
  std::unique_ptr<detail::IDatagram> datagram =
      detail::make_datagram(*loop.backend(), Address::any(port), reuse);
  if (datagram == nullptr) {
    return fail(NetError::bind_failed,
                "browse: could not bind the discovery port (in use? IPv4 only in v1)");
  }
  auto impl = std::make_shared<Impl>();
  impl->datagram = std::move(datagram);
  impl->sink = std::make_unique<Impl::Sink>(impl.get());
  impl->datagram->attach_sink(impl->sink.get());
  impl->datagram->set_keepalive(impl);
  return Ptr{new DiscoveryBrowser(std::move(impl))};
}

void DiscoveryBrowser::set_on_peer(
    std::function<void(std::span<const std::byte> payload, Address from)>
        on_peer) {
  impl_->on_peer = std::move(on_peer);
}

Address DiscoveryBrowser::local() const { return impl_->datagram->local(); }

void DiscoveryBrowser::stop() { impl_->datagram->close(); }

}  // namespace ws::net
