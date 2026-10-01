#include "uv_tcp.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>

#include "uv_addr.hpp"
#include "uv_loop.hpp"
#include <ws/core/log.hpp>

namespace ws::net::detail {

namespace {

// The NetError a stream death reports for a uv failure code. ECANCELED
// never maps here: canceled ops are the close path, and the close path
// reports through whatever initiated it.
NetError map_stream_error(int status) {
  if (status == UV_ECONNREFUSED) {
    return NetError::connection_refused;
  }
  if (status == UV_ECONNRESET) {
    return NetError::connection_reset;
  }
  return NetError::socket_error;
}

}  // namespace

// ---- UvStreamPeer ----

struct UvStreamPeer::WriteReq {
  uv_write_t req{};
  StreamCtx* ctx = nullptr;
  std::vector<std::byte> bytes;
};

struct UvStreamPeer::StreamCtx {
  UvLoopBackend* backend = nullptr;
  uv_tcp_t tcp{};
  uv_connect_t connect_req{};
  uv_shutdown_t shutdown_req{};
  UvStreamPeer* owner = nullptr;    // null once the peer object is gone
  std::shared_ptr<void> keepalive;  // the public object, held until close
  IStreamSink* sink = nullptr;      // null after the peer detached
  std::deque<std::unique_ptr<WriteReq>> queue;  // front may be in flight
  std::size_t pending_bytes = 0;    // queued, not yet handed to uv_write
  std::array<std::byte, 65536> read_buf{};
  sockaddr_storage target{};        // where the client connect aims
  Address peer_addr;
  enum class Phase : std::uint8_t {
    connecting,  // client: connect not completed yet
    open,
    finishing,   // finish_and_close: FIN queued behind the writes
    dead,
  };
  Phase phase = Phase::connecting;
  bool write_in_flight = false;
  bool closing = false;        // uv_close issued on tcp
  bool shutdown_issued = false;  // uv_shutdown handed to libuv
  bool shutdown_wanted = false;  // close/EOF while writes still chain:
                                 // the FIN waits for the drain
  bool eof_seen = false;       // remote FIN: our writes still flush
  bool started = false;
  bool server_side = false;

  [[nodiscard]] bool alive() const {
    return phase != Phase::dead && !closing;
  }
};

UvStreamPeer::UvStreamPeer(UvLoopBackend& backend, const Address& target_addr) {
  ctx_ = new StreamCtx;
  ctx_->backend = &backend;
  ctx_->owner = this;
  ctx_->peer_addr = target_addr;
  ctx_->tcp.data = ctx_;
  if (!address_to_sockaddr(target_addr, ctx_->target)) {
    // No uv handle exists yet: a plain delete is safe. Inert peer - the
    // factory turns it into a nullptr result.
    delete ctx_;
    ctx_ = nullptr;
    return;
  }
  if (uv_tcp_init(backend.raw(), &ctx_->tcp) != 0) {
    ws::core::error("net: uv_tcp_init failed");
    delete ctx_;
    ctx_ = nullptr;
    return;
  }
  backend.track(reinterpret_cast<uv_handle_t*>(&ctx_->tcp),
                &UvStreamPeer::on_stream_closed);
}

UvStreamPeer::UvStreamPeer(UvLoopBackend& backend, uv_stream_t* accepted) {
  ctx_ = new StreamCtx;
  ctx_->backend = &backend;
  ctx_->owner = this;
  ctx_->tcp.data = ctx_;
  if (uv_tcp_init(backend.raw(), &ctx_->tcp) != 0) {
    ws::core::error("net: uv_tcp_init failed");
    delete ctx_;
    ctx_ = nullptr;
    return;
  }
  if (uv_accept(accepted, reinterpret_cast<uv_stream_t*>(&ctx_->tcp)) != 0) {
    // Close the initialized handle properly (never free it raw), then
    // go inert - there is nothing to hand up.
    ws::core::error("net: uv_accept failed");
    ctx_->closing = true;
    uv_close(reinterpret_cast<uv_handle_t*>(&ctx_->tcp),
             &UvStreamPeer::on_stream_closed);
    ctx_->owner = nullptr;  // the orphaned ctx closes alone
    ctx_ = nullptr;
    return;
  }
  sockaddr_storage sa{};
  int len = static_cast<int>(sizeof(sa));
  if (uv_tcp_getpeername(&ctx_->tcp, reinterpret_cast<sockaddr*>(&sa),
                         &len) == 0) {
    ctx_->peer_addr = sockaddr_to_address(reinterpret_cast<const sockaddr*>(&sa));
  }
  ctx_->server_side = true;
  ctx_->phase = StreamCtx::Phase::open;
  backend.track(reinterpret_cast<uv_handle_t*>(&ctx_->tcp),
                &UvStreamPeer::on_stream_closed);
}

UvStreamPeer::~UvStreamPeer() {
  if (ctx_ != nullptr) {
    // Normally unreachable: the keepalive holds the public object until
    // the close callback ran (and detached us). Defensive, never a raw
    // free of a live handle.
    ctx_->owner = nullptr;
    ctx_->sink = nullptr;
    if (!ctx_->closing &&
        !uv_is_closing(reinterpret_cast<uv_handle_t*>(&ctx_->tcp))) {
      ctx_->closing = true;
      uv_close(reinterpret_cast<uv_handle_t*>(&ctx_->tcp),
               &UvStreamPeer::on_stream_closed);
    }
  }
}

void UvStreamPeer::attach_sink(IStreamSink* sink) {
  if (ctx_ != nullptr) {
    ctx_->sink = sink;
  }
}

void UvStreamPeer::start() {
  if (ctx_ == nullptr || ctx_->started || !ctx_->alive()) {
    return;
  }
  ctx_->started = true;
  if (ctx_->server_side) {
    uv_read_start(reinterpret_cast<uv_stream_t*>(&ctx_->tcp),
                  &UvStreamPeer::on_alloc, &UvStreamPeer::on_read);
    if (ctx_->sink != nullptr) {
      ctx_->sink->on_connected();  // server sides are connected by accept
    }
    return;
  }
  ctx_->connect_req.data = ctx_;
  const int rc = uv_tcp_connect(&ctx_->connect_req, &ctx_->tcp,
                                reinterpret_cast<const sockaddr*>(&ctx_->target),
                                &UvStreamPeer::on_connect);
  if (rc != 0) {
    // The synchronous part of connect (setup) failed; the refused/
    // unreachable part of the story arrives the async way.
    die_with(ctx_, NetError::connect_failed);
  }
}

void UvStreamPeer::set_keepalive(std::shared_ptr<void> owner) {
  if (ctx_ != nullptr) {
    ctx_->keepalive = std::move(owner);
  }
}

void UvStreamPeer::write(std::vector<std::byte> bytes) {
  if (ctx_ == nullptr || ctx_->phase != StreamCtx::Phase::open ||
      bytes.empty() || ctx_->shutdown_issued || ctx_->shutdown_wanted) {
    // Dead, finishing, inert - or the FIN is already going out (remote
    // EOF or a graceful close): a late write has nowhere to go. Drop it
    // rather than let uv_write's EPIPE kill the connection over it.
    return;  // (the connection logic guards the user-facing paths)
  }
  auto req = std::make_unique<WriteReq>();
  req->ctx = ctx_;
  req->bytes = std::move(bytes);
  ctx_->pending_bytes += req->bytes.size();
  ctx_->queue.push_back(std::move(req));
  if (!ctx_->write_in_flight) {
    issue_front(ctx_);
  }
}

void UvStreamPeer::finish_and_close() {
  if (ctx_ == nullptr || ctx_->phase != StreamCtx::Phase::open) {
    return;
  }
  ctx_->phase = StreamCtx::Phase::finishing;
  // libuv queues the shutdown behind pending writes: the graceful close
  // drains first by construction, no drain bookkeeping needed here.
  begin_shutdown(ctx_);
}

void UvStreamPeer::abort() {
  if (ctx_ != nullptr) {
    die_with(ctx_, NetError::connection_reset, /*rst=*/true);
  }
}

std::size_t UvStreamPeer::pending() const {
  return ctx_ != nullptr ? ctx_->pending_bytes : 0;
}

Address UvStreamPeer::peer_address() const noexcept {
  return ctx_ != nullptr ? ctx_->peer_addr : Address{};
}

void UvStreamPeer::issue_front(StreamCtx* ctx) {
  WriteReq* wr = ctx->queue.front().get();
  wr->req.data = wr;
  uv_buf_t buf{};
  buf.base = reinterpret_cast<char*>(wr->bytes.data());
  buf.len = static_cast<decltype(uv_buf_t::len)>(wr->bytes.size());
  const int rc = uv_write(&wr->req, reinterpret_cast<uv_stream_t*>(&ctx->tcp),
                          &buf, 1, &UvStreamPeer::on_write_done);
  if (rc != 0) {
    // Never queued: complete the request here and report the failure.
    ctx->pending_bytes -= wr->bytes.size();  // never handed to the OS
    ctx->queue.pop_front();
    die_with(ctx, NetError::socket_error);
    return;
  }
  ctx->write_in_flight = true;
  ctx->pending_bytes -= wr->bytes.size();  // handed to the OS
}

void UvStreamPeer::begin_shutdown(StreamCtx* ctx) {
  if (ctx->closing || ctx->shutdown_issued) {
    return;
  }
  if (!ctx->queue.empty()) {
    // libuv only defers uv_shutdown behind writes already handed to
    // uv_write; the rest sit in OUR chain, and uv_write after uv_shutdown
    // fails with EPIPE (win/stream.c clears UV_HANDLE_WRITABLE at once).
    // So wait for the chain to drain - on_write_done re-issues this.
    ctx->shutdown_wanted = true;
    return;
  }
  ctx->shutdown_issued = true;
  ctx->shutdown_req.data = ctx;
  const int rc = uv_shutdown(&ctx->shutdown_req,
                              reinterpret_cast<uv_stream_t*>(&ctx->tcp),
                              &UvStreamPeer::on_shutdown_done);
  if (rc != 0) {
    die_with(ctx, NetError::socket_error);  // socket already unusable
  }
}

void UvStreamPeer::die_with(StreamCtx* ctx, NetError reason, bool rst) {
  if (ctx->phase == StreamCtx::Phase::dead) {
    return;  // the sink hears about a stream exactly once
  }
  auto* handle = reinterpret_cast<uv_handle_t*>(&ctx->tcp);
  if (uv_is_closing(handle)) {
    // Closed underneath us (the backend teardown force-closes without
    // touching StreamCtx, so `closing` reads false): its close callback
    // owns the cleanup from here. Teardown reports nothing.
    ctx->phase = StreamCtx::Phase::dead;
    ctx->closing = true;
    return;
  }
  ctx->phase = StreamCtx::Phase::dead;
  if (!ctx->closing) {
    ctx->closing = true;
    uv_read_stop(reinterpret_cast<uv_stream_t*>(&ctx->tcp));  // best effort
    // uv_tcp_close_reset sends a true RST (SO_LINGER 0) so the peer sees
    // a reset, not a polite FIN. It refuses when a shutdown is already
    // out, and fails on a socket that doesn't exist (mid-connect); the
    // uv_close fallback is exactly those polite/last-resort closes.
    if (!rst ||
        uv_tcp_close_reset(&ctx->tcp, &UvStreamPeer::on_stream_closed) != 0) {
      uv_close(handle, &UvStreamPeer::on_stream_closed);
    }
  }
  if (ctx->sink != nullptr) {
    // Exactly once, always the peer's last report; after it the sink
    // sees an inert peer.
    ctx->sink->on_stream_dead(reason);
  }
}

void UvStreamPeer::on_connect(uv_connect_t* req, int status) {
  auto* ctx = static_cast<StreamCtx*>(req->data);
  if (status == UV_ECANCELED) {
    // Aborted or force-closed (teardown) mid-connect: whoever closed
    // already reported - or is tearing down silently.
    return;
  }
  if (ctx->phase == StreamCtx::Phase::dead) {
    return;  // aborted mid-connect: the close is already in
  }
  if (status < 0) {
    die_with(ctx, map_stream_error(status));  // refused, unreachable, ...
    return;
  }
  ctx->phase = StreamCtx::Phase::open;
  uv_read_start(reinterpret_cast<uv_stream_t*>(&ctx->tcp),
                &UvStreamPeer::on_alloc, &UvStreamPeer::on_read);
  if (ctx->sink != nullptr) {
    ctx->sink->on_connected();
  }
}

void UvStreamPeer::on_alloc(uv_handle_t* handle, std::size_t /*suggested*/,
                            uv_buf_t* buf) {
  auto* ctx = static_cast<StreamCtx*>(handle->data);
  buf->base = reinterpret_cast<char*>(ctx->read_buf.data());
  buf->len = static_cast<decltype(uv_buf_t::len)>(ctx->read_buf.size());
}

void UvStreamPeer::on_read(uv_stream_t* stream, ssize_t nread,
                           const uv_buf_t* /*buf*/) {
  auto* ctx = static_cast<StreamCtx*>(stream->data);
  if (ctx->phase == StreamCtx::Phase::dead || ctx->sink == nullptr) {
    return;  // closing/closed or the owner is gone: nothing to deliver
  }
  if (nread == UV_EOF) {
    // Remote FIN: our queued writes still flush (the shutdown queues
    // behind them), then the connection closes cleanly.
    ctx->eof_seen = true;
    uv_read_stop(stream);
    begin_shutdown(ctx);
    return;
  }
  if (nread < 0) {
    die_with(ctx, nread == UV_ECONNRESET ? NetError::connection_reset
                                         : NetError::socket_error);
    return;
  }
  if (nread > 0) {
    ctx->sink->on_bytes(std::span<const std::byte>{
        ctx->read_buf.data(), static_cast<std::size_t>(nread)});
  }
}

void UvStreamPeer::on_write_done(uv_write_t* req, int status) {
  auto* wr = static_cast<WriteReq*>(req->data);
  StreamCtx* const ctx = wr->ctx;
  ctx->write_in_flight = false;
  ctx->queue.pop_front();  // the completed request - writes chain
  if (status == UV_ECANCELED) {
    return;  // aborted or force-closed underneath: nothing to report
  }
  if (ctx->phase == StreamCtx::Phase::dead) {
    return;  // died underneath (the close already reported)
  }
  if (status < 0) {
    die_with(ctx, NetError::socket_error);  // a queued write blew up
    return;
  }
  if (!ctx->queue.empty()) {
    issue_front(ctx);
  } else if (ctx->shutdown_wanted) {
    ctx->shutdown_wanted = false;
    begin_shutdown(ctx);  // the drain finished: the FIN can go out now
  }
}

void UvStreamPeer::on_shutdown_done(uv_shutdown_t* req, int status) {
  auto* ctx = static_cast<StreamCtx*>(req->data);
  if (status == UV_ECANCELED) {
    return;  // force-closed underneath (teardown): nothing to report
  }
  if (ctx->phase == StreamCtx::Phase::dead) {
    return;  // aborted under the shutdown: the close is already in
  }
  // The graceful dance completed: last byte out (or the FIN itself
  // failed, which surfaces as its own error).
  die_with(ctx,
           status < 0 ? NetError::socket_error : NetError::connection_closed);
}

void UvStreamPeer::on_stream_closed(uv_handle_t* handle) {
  auto* ctx = static_cast<StreamCtx*>(handle->data);
  ctx->backend->untrack(handle);
  std::shared_ptr<void> keep = std::move(ctx->keepalive);
  if (ctx->owner != nullptr) {
    ctx->owner->detach();  // the object must not touch the ctx again
  }
  delete ctx;  // the sanctioned point: the handle finished closing
  // `keep` drops here, after the ctx is gone - whatever public object it
  // held may be destroyed now without ever touching freed memory.
}

// ---- UvStreamAcceptor ----

struct UvStreamAcceptor::ServerCtx {
  UvLoopBackend* backend = nullptr;
  uv_tcp_t server{};
  UvStreamAcceptor* owner = nullptr;
  std::shared_ptr<void> keepalive;
  StreamAcceptFn on_accept;
  bool closing = false;
};

UvStreamAcceptor::UvStreamAcceptor(UvLoopBackend& backend,
                                   const Address& local_addr, int backlog,
                                   StreamAcceptFn on_accept) {
  if (backlog <= 0 || !on_accept) {
    return;  // inert: the factory reports nullptr
  }
  sockaddr_storage sa{};
  if (!address_to_sockaddr(local_addr, sa)) {
    return;
  }
  ctx_ = new ServerCtx;
  ctx_->backend = &backend;
  ctx_->owner = this;
  ctx_->on_accept = std::move(on_accept);
  ctx_->server.data = ctx_;
  if (uv_tcp_init(backend.raw(), &ctx_->server) != 0) {
    ws::core::error("net: uv_tcp_init failed");
    delete ctx_;
    ctx_ = nullptr;
    return;
  }
  if (uv_tcp_bind(&ctx_->server, reinterpret_cast<const sockaddr*>(&sa),
                  0) != 0) {
    ws::core::error("net: uv_tcp_bind failed");
    ctx_->closing = true;
    uv_close(reinterpret_cast<uv_handle_t*>(&ctx_->server),
             &UvStreamAcceptor::on_server_closed);
    ctx_->owner = nullptr;
    ctx_ = nullptr;
    return;
  }
  if (uv_listen(reinterpret_cast<uv_stream_t*>(&ctx_->server), backlog,
                &UvStreamAcceptor::on_connection) != 0) {
    ws::core::error("net: uv_listen failed");
    ctx_->closing = true;
    uv_close(reinterpret_cast<uv_handle_t*>(&ctx_->server),
             &UvStreamAcceptor::on_server_closed);
    ctx_->owner = nullptr;
    ctx_ = nullptr;
    return;
  }
  backend.track(reinterpret_cast<uv_handle_t*>(&ctx_->server),
                &UvStreamAcceptor::on_server_closed);
}

UvStreamAcceptor::~UvStreamAcceptor() {
  if (ctx_ != nullptr) {
    // Normally unreachable (the keepalive held us until close ran);
    // defensive, never a raw free of a live handle.
    ctx_->owner = nullptr;
    if (!ctx_->closing &&
        !uv_is_closing(reinterpret_cast<uv_handle_t*>(&ctx_->server))) {
      ctx_->closing = true;
      uv_close(reinterpret_cast<uv_handle_t*>(&ctx_->server),
               &UvStreamAcceptor::on_server_closed);
    }
  }
}

void UvStreamAcceptor::close() {
  if (ctx_ == nullptr || ctx_->closing ||
      uv_is_closing(reinterpret_cast<uv_handle_t*>(&ctx_->server))) {
    return;  // already closing - possibly underneath us (backend teardown)
  }
  ctx_->closing = true;
  uv_close(reinterpret_cast<uv_handle_t*>(&ctx_->server),
           &UvStreamAcceptor::on_server_closed);
}

Address UvStreamAcceptor::local() const {
  if (ctx_ == nullptr) {
    return {};
  }
  sockaddr_storage sa{};
  int len = static_cast<int>(sizeof(sa));
  if (uv_tcp_getsockname(&ctx_->server, reinterpret_cast<sockaddr*>(&sa),
                         &len) != 0) {
    return {};
  }
  return sockaddr_to_address(reinterpret_cast<const sockaddr*>(&sa));
}

void UvStreamAcceptor::set_keepalive(std::shared_ptr<void> owner) {
  if (ctx_ != nullptr) {
    ctx_->keepalive = std::move(owner);
  }
}

void UvStreamAcceptor::on_connection(uv_stream_t* server, int status) {
  auto* ctx = static_cast<ServerCtx*>(server->data);
  if (status < 0) {
    return;  // a pending connection failed; the listener stays up
  }
  auto* raw = new UvStreamPeer(*ctx->backend, server);
  std::unique_ptr<IStreamPeer> peer{raw};
  if (!raw->valid()) {
    return;  // accept failed: the peer closed itself up
  }
  if (!ctx->on_accept) {
    raw->abort();  // nobody home: tear the socket down now
    return;
  }
  const Address from = raw->peer_address();
  ctx->on_accept(std::move(peer), from);
}

void UvStreamAcceptor::on_server_closed(uv_handle_t* handle) {
  auto* ctx = static_cast<ServerCtx*>(handle->data);
  ctx->backend->untrack(handle);
  std::shared_ptr<void> keep = std::move(ctx->keepalive);
  if (ctx->owner != nullptr) {
    ctx->owner->detach();
  }
  delete ctx;
}

// ---- factories (the seam's entry points; see transport.hpp) ----

std::unique_ptr<IStreamPeer> make_stream_peer_connect(ILoopBackend& loop,
                                                      const Address& peer) {
  auto& backend = static_cast<UvLoopBackend&>(loop);  // the seam's one downcast
  auto* raw = new UvStreamPeer(backend, peer);
  if (!raw->valid()) {
    delete raw;
    return nullptr;
  }
  return std::unique_ptr<IStreamPeer>{raw};
}

std::unique_ptr<IStreamAcceptor> make_stream_acceptor(
    ILoopBackend& loop, const Address& local, int backlog,
    StreamAcceptFn on_accept) {
  auto& backend = static_cast<UvLoopBackend&>(loop);
  auto* raw = new UvStreamAcceptor(backend, local, backlog,
                                   std::move(on_accept));
  if (!raw->valid()) {
    delete raw;
    return nullptr;
  }
  return std::unique_ptr<IStreamAcceptor>{raw};
}

}  // namespace ws::net::detail
