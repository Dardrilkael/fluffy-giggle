#include <ws/net/raw_tcp.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "core/transport.hpp"
#include <ws/net/event_loop.hpp>

namespace ws::net {

struct RawTcpConnection::Impl final
    : detail::IStreamSink, std::enable_shared_from_this<Impl> {
  std::unique_ptr<detail::IStreamPeer> peer_stream;
  Address peer_address;
  std::function<void()> connected_callback;
  std::function<void(std::span<const std::byte>)> bytes_callback;
  std::function<void(NetError)> closed_callback;
  bool dead = false;
  bool closing = false;

  Impl(std::unique_ptr<detail::IStreamPeer> stream, Address address)
      : peer_stream(std::move(stream)), peer_address(std::move(address)) {
    peer_stream->attach_sink(this);
  }
  void arm() { peer_stream->set_keepalive(shared_from_this()); }
  void start() { peer_stream->start(); }
  void on_connected() override {
    auto callback = connected_callback;
    if (!dead && callback) callback();
  }
  void on_bytes(std::span<const std::byte> bytes) override {
    // Copy before invocation: callbacks may replace themselves (or clear
    // the callback slot) while they are running.
    auto callback = bytes_callback;
    if (!dead && callback) callback(bytes);
  }
  void on_stream_dead(NetError why) override {
    if (dead) return;
    dead = true;
    auto callback = closed_callback;
    if (callback) callback(why);
  }
};

RawTcpConnection::RawTcpConnection(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
RawTcpConnection::~RawTcpConnection() = default;

ws::core::Result<RawTcpConnection::Ptr> RawTcpConnection::connect(
    EventLoop& loop, const Address& peer) {
  if (!peer.valid()) {
    return fail(NetError::invalid_argument, "connect: empty address");
  }
  auto stream = detail::make_stream_peer_connect(*loop.backend(), peer);
  if (!stream) {
    return fail(NetError::connect_failed,
                "connect: the backend cannot use this address");
  }
  auto impl = std::make_shared<Impl>(std::move(stream), peer);
  impl->arm();
  Ptr conn{new RawTcpConnection(std::move(impl))};
  conn->impl_->start();
  return conn;
}

void RawTcpConnection::set_on_connected(std::function<void()> callback) {
  impl_->connected_callback = std::move(callback);
}

void RawTcpConnection::set_on_bytes(
    std::function<void(std::span<const std::byte>)> callback) {
  impl_->bytes_callback = std::move(callback);
}
void RawTcpConnection::set_on_closed(
    std::function<void(NetError)> callback) {
  impl_->closed_callback = std::move(callback);
}
ws::core::Status RawTcpConnection::send(std::span<const std::byte> bytes) {
  if (impl_->dead || impl_->closing) return fail(NetError::already_closed, "raw TCP connection is closed");
  if (bytes.empty()) return {};
  impl_->peer_stream->write(std::vector<std::byte>(bytes.begin(), bytes.end()));
  return {};
}
ws::core::Status RawTcpConnection::close() {
  if (impl_->dead || impl_->closing) return fail(NetError::already_closed, "raw TCP connection is closed");
  impl_->closing = true;
  impl_->peer_stream->finish_and_close();
  return {};
}
void RawTcpConnection::abort() {
  if (!impl_->dead) impl_->peer_stream->abort();
}
Address RawTcpConnection::peer() const noexcept { return impl_->peer_address; }

struct RawTcpListener::Impl final : std::enable_shared_from_this<Impl> {
  std::function<void(RawTcpConnection::Ptr)> on_connection;
  std::unique_ptr<detail::IStreamAcceptor> acceptor;
  void arm() { acceptor->set_keepalive(shared_from_this()); }
};
RawTcpListener::RawTcpListener(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
RawTcpListener::~RawTcpListener() = default;

ws::core::Result<RawTcpListener::Ptr> RawTcpListener::listen(
    EventLoop& loop, const Address& local, int backlog) {
  if (!local.valid()) return fail(NetError::invalid_argument, "listen: empty address");
  if (backlog <= 0) return fail(NetError::invalid_argument, "listen: backlog must be positive");
  auto impl = std::make_shared<Impl>();
  Impl* const raw = impl.get();
  detail::StreamAcceptFn accept =
      [&loop, raw](std::unique_ptr<detail::IStreamPeer> stream, Address from) {
        auto conn_impl = std::make_shared<RawTcpConnection::Impl>(
            std::move(stream), std::move(from));
        conn_impl->arm();
        auto conn = RawTcpConnection::Ptr(new RawTcpConnection(conn_impl));
        (void)loop.post([conn_impl] { conn_impl->start(); });
        if (raw->on_connection) raw->on_connection(std::move(conn));
        else conn->abort();
      };
  impl->acceptor = detail::make_stream_acceptor(
      *loop.backend(), local, backlog, std::move(accept));
  if (!impl->acceptor) return fail(NetError::bind_failed, "listen: bind or listen failed");
  impl->arm();
  return Ptr{new RawTcpListener(std::move(impl))};
}
void RawTcpListener::set_on_connection(
    std::function<void(RawTcpConnection::Ptr)> callback) {
  impl_->on_connection = std::move(callback);
}
Address RawTcpListener::local() const { return impl_->acceptor->local(); }
void RawTcpListener::close() { impl_->acceptor->close(); }

}  // namespace ws::net
