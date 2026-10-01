#include <ws/net/tcp.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <utility>

#include "core/connection_logic.hpp"
#include "core/transport.hpp"
#include <ws/net/event_loop.hpp>

namespace ws::net {

// ---- TcpConnection ----

// The pimpl holds the user's current callbacks; ConnectionLogic holds
// forwarder lambdas that read them. set_on_* therefore only ever swaps
// plain members - re-setting a callback from inside a callback is safe
// (no live std::function is destroyed under its own stack).
struct TcpConnection::Impl final : std::enable_shared_from_this<Impl> {
  std::function<void()> on_connected;
  std::function<void(std::span<const std::byte>)> on_message;
  std::function<void(NetError)> on_closed;
  std::unique_ptr<detail::ConnectionLogic> logic;
  Address peer_addr;

  Impl(std::unique_ptr<detail::IStreamPeer> stream, std::uint32_t cap,
       Address peer)
      : peer_addr(std::move(peer)) {
    detail::ConnectionEvents events;
    events.on_connected = [this] {
      if (on_connected) {
        on_connected();
      }
    };
    events.on_message = [this](std::span<const std::byte> message) {
      if (on_message) {
        on_message(message);
      }
    };
    events.on_closed = [this](NetError why) {
      if (on_closed) {
        on_closed(why);
      }
    };
    logic = std::make_unique<detail::ConnectionLogic>(
        std::move(stream), std::move(events), cap);
  }

  // The stream peer holds us (type-erased) until its close callback
  // ran: dropping the public Ptr never frees a live socket.
  void arm() { logic->peer()->set_keepalive(shared_from_this()); }
  void start() { logic->start(); }
};

TcpConnection::TcpConnection(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

TcpConnection::~TcpConnection() = default;

ws::core::Result<TcpConnection::Ptr> TcpConnection::connect(EventLoop& loop,
                                                           const Address& peer) {
  if (!peer.valid()) {
    return fail(NetError::invalid_argument, "connect: empty address");
  }
  auto stream = detail::make_stream_peer_connect(*loop.backend(), peer);
  if (!stream) {
    return fail(NetError::connect_failed,
                "connect: the backend cannot use this address");
  }
  auto impl = std::make_shared<Impl>(std::move(stream),
                                     kDefaultMaxMessageSize, peer);
  impl->arm();
  Ptr conn{new TcpConnection(std::move(impl))};
  conn->impl_->start();  // the async connect completes on a later turn
  return conn;
}

void TcpConnection::set_on_connected(std::function<void()> on_connected) {
  impl_->on_connected = std::move(on_connected);
}

void TcpConnection::set_on_message(
    std::function<void(std::span<const std::byte> message)> on_message) {
  impl_->on_message = std::move(on_message);
}

void TcpConnection::set_on_closed(std::function<void(NetError why)> on_closed) {
  impl_->on_closed = std::move(on_closed);
}

void TcpConnection::set_max_message_size(std::uint32_t max) {
  impl_->logic->set_max_message(max);
}

ws::core::Status TcpConnection::send(std::span<const std::byte> message) {
  return impl_->logic->send(message);
}

ws::core::Status TcpConnection::close() {
  return impl_->logic->close();
}

void TcpConnection::abort() {
  impl_->logic->abort();
}

bool TcpConnection::is_open() const noexcept {
  return impl_->logic->is_open();
}

Address TcpConnection::peer() const noexcept {
  return impl_->peer_addr;
}

std::size_t TcpConnection::pending_bytes() const noexcept {
  return impl_->logic->pending_bytes();
}

// ---- TcpListener ----

struct TcpListener::Impl final : std::enable_shared_from_this<Impl> {
  std::function<void(TcpConnection::Ptr)> on_connection;
  std::unique_ptr<detail::IStreamAcceptor> acceptor;

  // The acceptor holds us (type-erased) until its close callback ran.
  void arm() { acceptor->set_keepalive(shared_from_this()); }
};

TcpListener::TcpListener(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

TcpListener::~TcpListener() = default;

ws::core::Result<TcpListener::Ptr> TcpListener::listen(EventLoop& loop,
                                                        const Address& local,
                                                        int backlog) {
  if (!local.valid()) {
    return fail(NetError::invalid_argument, "listen: empty address");
  }
  if (backlog <= 0) {
    return fail(NetError::invalid_argument, "listen: backlog must be positive");
  }
  auto impl = std::make_shared<Impl>();
  // Built inside listen (a friend of TcpConnection) so the glue can
  // construct server-side connections. impl_raw stays valid through any
  // accept callback: the acceptor's keepalive holds `impl` until its
  // close callback ran.
  Impl* const impl_raw = impl.get();
  detail::StreamAcceptFn glue =
      [&loop, impl_raw](std::unique_ptr<detail::IStreamPeer> stream,
                        Address from) {
        auto conn_impl = std::make_shared<TcpConnection::Impl>(
            std::move(stream), TcpConnection::kDefaultMaxMessageSize, from);
        conn_impl->arm();
        // The posted start holds its own reference - taken BEFORE the
        // move into the shell, which empties conn_impl. Reading (and
        // on_connected) starts on the posted turn, so set_on_* inside
        // on_connection land first.
        std::shared_ptr<TcpConnection::Impl> posted = conn_impl;
        TcpConnection::Ptr conn{new TcpConnection(std::move(conn_impl))};
        (void)loop.post([posted] { posted->start(); });
        if (impl_raw->on_connection) {
          impl_raw->on_connection(std::move(conn));
        } else {
          conn->abort();  // nobody ever set the callback: tear it down
        }
      };
  impl->acceptor =
      detail::make_stream_acceptor(*loop.backend(), local, backlog,
                                   std::move(glue));
  if (!impl->acceptor) {
    return fail(NetError::bind_failed, "listen: bind or listen failed");
  }
  impl->arm();
  Ptr listener{new TcpListener(std::move(impl))};
  return listener;
}

void TcpListener::set_on_connection(
    std::function<void(TcpConnection::Ptr)> on_connection) {
  impl_->on_connection = std::move(on_connection);
}

Address TcpListener::local() const {
  return impl_->acceptor->local();
}

void TcpListener::close() {
  impl_->acceptor->close();
}

}  // namespace ws::net
