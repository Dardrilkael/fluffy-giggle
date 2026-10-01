#pragma once

// Internal: the libuv TCP backend - the stream/acceptor halves of the
// core seam (core/transport.hpp). Everything uv-shaped about TCP lives
// in this directory; the connection state machine
// (core/connection_logic.*) stays socket-free and backend-agnostic.

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include <uv.h>

#include "core/transport.hpp"
#include <ws/net/address.hpp>

namespace ws::net::detail {

class UvLoopBackend;

// One tcp socket (connecting, connected or accepted). The socket, its
// write queue and every uv-facing state live in a heap StreamCtx (the
// handle's data pointer) which outlives this object whenever the
// connection dies underneath it and frees itself in the uv close
// callback - never before (the CLAUDE.md lifetime rule, made
// mechanical).
class UvStreamPeer final : public IStreamPeer {
 public:
  ~UvStreamPeer() override;  // detaches; never frees a live handle

  void attach_sink(IStreamSink* sink) override;
  void start() override;
  void set_keepalive(std::shared_ptr<void> owner) override;
  void write(std::vector<std::byte> bytes) override;
  void finish_and_close() override;
  void abort() override;
  [[nodiscard]] std::size_t pending() const override;

  // Internal: false when construction failed (the factory turns an
  // inert peer into a nullptr result).
  [[nodiscard]] bool valid() const noexcept { return ctx_ != nullptr; }

  // The remote end (filled at accept time for server-side peers).
  [[nodiscard]] Address peer_address() const noexcept;

  // Internal: the ctx close callback calls this right before freeing
  // itself - the object must never touch the ctx again.
  void detach() noexcept { ctx_ = nullptr; }

 private:
  friend class UvStreamAcceptor;  // constructs accepted peers
  friend std::unique_ptr<IStreamPeer> make_stream_peer_connect(
      ILoopBackend& loop, const Address& peer);

  // Client: begin connecting at start(). Accepted: handed over by
  // UvStreamAcceptor (announces + reads at start()).
  UvStreamPeer(UvLoopBackend& backend, const Address& target);
  UvStreamPeer(UvLoopBackend& backend, uv_stream_t* accepted);

  struct StreamCtx;  // defined in uv_tcp.cpp
  struct WriteReq;   // ditto

  static void on_connect(uv_connect_t* req, int status);
  static void on_alloc(uv_handle_t* handle, std::size_t suggested_size,
                       uv_buf_t* buf);
  static void on_read(uv_stream_t* stream, ssize_t nread,
                      const uv_buf_t* buf);
  static void on_write_done(uv_write_t* req, int status);
  static void on_shutdown_done(uv_shutdown_t* req, int status);
  static void on_stream_closed(uv_handle_t* handle);

  static void issue_front(StreamCtx* ctx);
  static void begin_shutdown(StreamCtx* ctx);
  // rst: close with SO_LINGER 0 so the peer sees a real reset, not a FIN
  // (uv_tcp_close_reset falls back to a plain close inside when refused).
  static void die_with(StreamCtx* ctx, NetError reason, bool rst = false);

  StreamCtx* ctx_ = nullptr;
};

// The listening socket half of the seam. Same ctx-outlives-object
// discipline as UvStreamPeer.
class UvStreamAcceptor final : public IStreamAcceptor {
 public:
  ~UvStreamAcceptor() override;

  void close() override;
  [[nodiscard]] Address local() const override;
  void set_keepalive(std::shared_ptr<void> owner) override;

  [[nodiscard]] bool valid() const noexcept { return ctx_ != nullptr; }
  void detach() noexcept { ctx_ = nullptr; }

 private:
  friend std::unique_ptr<IStreamAcceptor> make_stream_acceptor(
      ILoopBackend& loop, const Address& local, int backlog,
      StreamAcceptFn on_accept);

  UvStreamAcceptor(UvLoopBackend& backend, const Address& local,
                   int backlog, StreamAcceptFn on_accept);

  struct ServerCtx;  // defined in uv_tcp.cpp

  static void on_connection(uv_stream_t* server, int status);
  static void on_server_closed(uv_handle_t* handle);

  ServerCtx* ctx_ = nullptr;
};

}  // namespace ws::net::detail
