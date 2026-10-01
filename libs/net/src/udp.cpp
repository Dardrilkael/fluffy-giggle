#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <utility>

#include "core/transport.hpp"
#include <ws/net/errors.hpp>
#include <ws/net/event_loop.hpp>
#include <ws/net/udp.hpp>

namespace ws::net {

struct UdpSocket::Impl final {
  // The sink adapter: Impl cannot both inherit IDatagramSink and keep
  // a user-callback member named on_packet (a member function and a
  // data member cannot share a name), so a tiny inner struct forwards.
  struct Sink final : public detail::IDatagramSink {
    Impl* impl;
    explicit Sink(Impl* owner) : impl(owner) {}
    void on_packet(std::span<const std::byte> data, Address from) override {
      impl->deliver(data, from);
    }
  };

  explicit Impl(std::unique_ptr<detail::IDatagram> socket)
      : datagram(std::move(socket)) {}

  void deliver(std::span<const std::byte> data, Address from) {
    if (on_packet) {
      on_packet(data, from);
    }
  }

  std::unique_ptr<detail::IDatagram> datagram;
  std::unique_ptr<Sink> sink;
  std::function<void(std::span<const std::byte> data, Address from)> on_packet;
  bool closed = false;
};

UdpSocket::UdpSocket(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

ws::core::Result<UdpSocket::Ptr> UdpSocket::bind(EventLoop& loop,
                                                 const Address& local,
                                                 bool reuse) {
  if (!local.valid()) {
    return fail(NetError::invalid_argument, "bind: the address has no host");
  }
  std::unique_ptr<detail::IDatagram> datagram =
      detail::make_datagram(*loop.backend(), local, reuse);
  if (datagram == nullptr) {
    return fail(NetError::bind_failed,
                "bind: the backend refused the address (in use? IPv4 only in v1)");
  }
  auto impl = std::make_shared<Impl>(std::move(datagram));
  impl->sink = std::make_unique<Impl::Sink>(impl.get());
  impl->datagram->attach_sink(impl->sink.get());
  impl->datagram->set_keepalive(impl);  // the ctx holds us until its close cb
  return Ptr{new UdpSocket(std::move(impl))};
}

void UdpSocket::set_on_packet(
    std::function<void(std::span<const std::byte> data, Address from)>
        on_packet) {
  impl_->on_packet = std::move(on_packet);
}

ws::core::Status UdpSocket::send_to(const Address& to,
                                    std::span<const std::byte> data) {
  if (impl_->closed) {
    return fail(NetError::already_closed, "send_to: the socket is closed");
  }
  if (!to.valid()) {
    return fail(NetError::invalid_argument, "send_to: the address has no host");
  }
  if (data.empty()) {
    return fail(NetError::invalid_argument,
                "send_to: an empty datagram is noise - refused");
  }
  if (data.size() > kMaxDatagramSize) {
    return fail(NetError::message_too_large,
                "send_to: datagram over the IPv4 wire cap (65507)");
  }
  return impl_->datagram->send_to(to, data);
}

Address UdpSocket::local() const { return impl_->datagram->local(); }

bool UdpSocket::is_open() const { return !impl_->closed; }

ws::core::Status UdpSocket::close() {
  if (impl_->closed) {
    return fail(NetError::already_closed, "close: the socket is already closed");
  }
  impl_->closed = true;
  // The close callback may run synchronously here (Windows, nothing
  // pending) and drop the keepalive; the user's own Ptr keeps the Impl
  // alive through the call, and nothing below touches impl_ again.
  impl_->datagram->close();
  return {};
}

}  // namespace ws::net
