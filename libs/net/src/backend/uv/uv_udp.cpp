#include "uv_udp.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "uv_addr.hpp"
#include "uv_loop.hpp"
#include <ws/core/log.hpp>

namespace ws::net::detail {

namespace {

// A legal IPv4 datagram can never exceed 65507 payload bytes, so a
// 64 KiB receive buffer makes UV_ENOBUFS (and truncation) unreachable
// by construction.
constexpr std::size_t kRecvBufSize = 65536;

// The NetError for a uv_udp_send that never queued.
ws::core::Status send_refused(int rc) {
  if (rc == UV_EAFNOSUPPORT || rc == UV_EINVAL) {
    return fail(NetError::invalid_argument,
                "send_to: address not usable by this socket (IPv4 in v1)");
  }
  if (rc == UV_EMSGSIZE) {
    return fail(NetError::message_too_large,
                "send_to: datagram too large for the backend");
  }
  // The catch-all names the backend's own error: a bare "refused" once
  // hid a UV_EACCES (SO_BROADCAST never set) behind a generic message.
  return fail(NetError::socket_error,
              "send_to: backend refused the datagram (" +
                  std::string{uv_strerror(rc)} + ", uv error " +
                  std::to_string(rc) + ")");
}

}  // namespace

struct UvDatagramSocket::SendReq {
  uv_udp_send_t req{};
  DatagramCtx* ctx = nullptr;
  std::vector<std::byte> bytes;
};

struct UvDatagramSocket::DatagramCtx {
  UvLoopBackend* backend = nullptr;
  uv_udp_t udp{};
  UvDatagramSocket* owner = nullptr;  // null once the socket object detached
  std::shared_ptr<void> keepalive;   // the public object, held until close
  IDatagramSink* sink = nullptr;     // null after the owner detached
  std::array<std::byte, kRecvBufSize> recv_buf{};
  bool closing = false;              // uv_close issued on udp
};

UvDatagramSocket::UvDatagramSocket(UvLoopBackend& backend,
                                   const Address& local, bool reuse) {
  sockaddr_storage sa{};
  if (!address_to_sockaddr(local, sa)) {
    return;  // inert: the factory reports nullptr
  }
  ctx_ = new DatagramCtx;
  ctx_->backend = &backend;
  ctx_->owner = this;
  ctx_->udp.data = ctx_;
  if (uv_udp_init(backend.raw(), &ctx_->udp) != 0) {
    ws::core::error("net: uv_udp_init failed");
    delete ctx_;
    ctx_ = nullptr;
    return;
  }
  if (uv_udp_bind(&ctx_->udp, reinterpret_cast<const sockaddr*>(&sa),
                  reuse ? UV_UDP_REUSEADDR : 0) != 0) {
    ws::core::error("net: uv_udp_bind failed");
    ctx_->closing = true;
    uv_close(reinterpret_cast<uv_handle_t*>(&ctx_->udp),
             &UvDatagramSocket::on_udp_closed);
    ctx_->owner = nullptr;  // the orphaned ctx closes alone
    ctx_ = nullptr;
    return;
  }
  // Every socket may broadcast (SO_BROADCAST): discovery needs it, and
  // for unicast traffic the option changes nothing. It can only be set
  // AFTER the bind - libuv creates the socket lazily at bind time, so a
  // call before it returns UV_EBADF and the option silently never lands
  // (every limited-broadcast send then dies with EACCES/WSAEACCES).
  const int bcast_rc = uv_udp_set_broadcast(&ctx_->udp, 1);
  if (bcast_rc != 0) {
    // Degrade, not die: unicast keeps working, and each refused broadcast
    // send reports the backend's own error through send_to.
    ws::core::error("net: uv_udp_set_broadcast failed: {}",
                    uv_strerror(bcast_rc));
  }
  if (uv_udp_recv_start(&ctx_->udp, &UvDatagramSocket::on_alloc,
                        &UvDatagramSocket::on_recv) != 0) {
    ws::core::error("net: uv_udp_recv_start failed");
    (void)uv_udp_recv_stop(&ctx_->udp);
    ctx_->closing = true;
    uv_close(reinterpret_cast<uv_handle_t*>(&ctx_->udp),
             &UvDatagramSocket::on_udp_closed);
    ctx_->owner = nullptr;
    ctx_ = nullptr;
    return;
  }
  backend.track(reinterpret_cast<uv_handle_t*>(&ctx_->udp),
                &UvDatagramSocket::on_udp_closed);
}

UvDatagramSocket::~UvDatagramSocket() {
  if (ctx_ != nullptr) {
    // Normally unreachable (the keepalive held us until close ran);
    // defensive, never a raw free of a live handle.
    ctx_->owner = nullptr;
    ctx_->sink = nullptr;
    if (!ctx_->closing &&
        !uv_is_closing(reinterpret_cast<uv_handle_t*>(&ctx_->udp))) {
      ctx_->closing = true;
      uv_close(reinterpret_cast<uv_handle_t*>(&ctx_->udp),
               &UvDatagramSocket::on_udp_closed);
    }
  }
}

void UvDatagramSocket::attach_sink(IDatagramSink* sink) {
  if (ctx_ != nullptr) {
    ctx_->sink = sink;
  }
}

void UvDatagramSocket::set_keepalive(std::shared_ptr<void> owner) {
  if (ctx_ != nullptr) {
    ctx_->keepalive = std::move(owner);
  }
}

ws::core::Status UvDatagramSocket::send_to(const Address& to,
                                           std::span<const std::byte> data) {
  if (ctx_ == nullptr || ctx_->closing) {
    return fail(NetError::already_closed, "send_to: socket is closed");
  }
  sockaddr_storage sa{};
  if (!address_to_sockaddr(to, sa)) {
    return fail(NetError::invalid_argument,
                "send_to: address not puttable on the wire");
  }
  // The request owns its buffer and frees itself in on_sent - which
  // libuv guarantees runs before the close callback, so an in-flight
  // send can never outlive the ctx.
  auto* wr = new SendReq;
  wr->ctx = ctx_;
  wr->bytes.assign(data.begin(), data.end());
  wr->req.data = wr;
  uv_buf_t buf{};
  buf.base = reinterpret_cast<char*>(wr->bytes.data());
  buf.len = static_cast<decltype(uv_buf_t::len)>(wr->bytes.size());
  const int rc = uv_udp_send(&wr->req, &ctx_->udp, &buf, 1,
                             reinterpret_cast<const sockaddr*>(&sa),
                             &UvDatagramSocket::on_sent);
  if (rc != 0) {
    delete wr;  // never queued
    return send_refused(rc);
  }
  return {};
}

void UvDatagramSocket::close() {
  if (ctx_ == nullptr || ctx_->closing ||
      uv_is_closing(reinterpret_cast<uv_handle_t*>(&ctx_->udp))) {
    return;  // already closing - possibly underneath us (backend teardown)
  }
  ctx_->closing = true;
  (void)uv_udp_recv_stop(&ctx_->udp);  // no further deliveries
  uv_close(reinterpret_cast<uv_handle_t*>(&ctx_->udp),
           &UvDatagramSocket::on_udp_closed);
}

Address UvDatagramSocket::local() const {
  if (ctx_ == nullptr) {
    return {};
  }
  sockaddr_storage sa{};
  int len = static_cast<int>(sizeof(sa));
  if (uv_udp_getsockname(&ctx_->udp, reinterpret_cast<sockaddr*>(&sa),
                         &len) != 0) {
    return {};
  }
  return sockaddr_to_address(reinterpret_cast<const sockaddr*>(&sa));
}

void UvDatagramSocket::on_alloc(uv_handle_t* handle,
                                std::size_t /*suggested*/, uv_buf_t* buf) {
  auto* ctx = static_cast<DatagramCtx*>(handle->data);
  buf->base = reinterpret_cast<char*>(ctx->recv_buf.data());
  buf->len = static_cast<decltype(uv_buf_t::len)>(ctx->recv_buf.size());
}

void UvDatagramSocket::on_recv(uv_udp_t* udp, ssize_t nread,
                               const uv_buf_t* buf, const sockaddr* addr,
                               unsigned /*flags*/) {
  auto* ctx = static_cast<DatagramCtx*>(udp->data);
  if (ctx->sink == nullptr) {
    return;  // nobody attached (or detached): drop
  }
  if (nread == 0) {
    return;  // nothing to read (an empty datagram reads as "no data")
  }
  if (nread < 0) {
    // UV_ECONNRESET is Windows reporting an ICMP "port unreachable"
    // for a PREVIOUS send - routine for best-effort UDP, swallowed
    // like every other asynchronous send outcome. UV_ECANCELED is the
    // close path. Anything else is worth one log line.
    if (nread != UV_ECANCELED && nread != UV_ECONNRESET &&
        nread != UV_ENOBUFS) {
      ws::core::error("net: udp receive error - datagram dropped");
    }
    return;  // the socket survives: UDP has no dead state
  }
  Address from;
  if (addr != nullptr) {
    from = sockaddr_to_address(addr);
  }
  ctx->sink->on_packet(
      std::span<const std::byte>{
          reinterpret_cast<const std::byte*>(buf->base),
          static_cast<std::size_t>(nread)},
      from);
#ifndef NDEBUG
  // The span-promise made mechanical, same as the framing core: bytes
  // a stashed span would read after the callback are poison, not data.
  std::memset(buf->base, 0xCD, static_cast<std::size_t>(nread));
#endif
}

void UvDatagramSocket::on_sent(uv_udp_send_t* req, int /*status*/) {
  // Best-effort by design: asynchronous send failures (ICMP
  // unreachable, a cancel mid-teardown) are deliberately not surfaced.
  // The request owns its buffer - free it here, always.
  delete static_cast<SendReq*>(req->data);
}

void UvDatagramSocket::on_udp_closed(uv_handle_t* handle) {
  auto* ctx = static_cast<DatagramCtx*>(handle->data);
  ctx->backend->untrack(handle);
  std::shared_ptr<void> keep = std::move(ctx->keepalive);
  if (ctx->owner != nullptr) {
    ctx->owner->detach();  // the object must not touch the ctx again
  }
  delete ctx;  // every in-flight SendReq completed before this point
  // `keep` drops here, after the ctx is gone - whatever public object
  // it held may be destroyed now without ever touching freed memory.
}

// ---- factories (the seam's entry points; see transport.hpp) ----

std::unique_ptr<IDatagram> make_datagram(ILoopBackend& loop,
                                         const Address& local, bool reuse) {
  auto& backend = static_cast<UvLoopBackend&>(loop);  // the seam's one downcast
  auto* raw = new UvDatagramSocket(backend, local, reuse);
  if (!raw->valid()) {
    delete raw;
    return nullptr;
  }
  return std::unique_ptr<IDatagram>{raw};
}

}  // namespace ws::net::detail
