#pragma once

// Internal: the backend seam (the swappable-backend contract). Core logic
// and the test fakes talk ONLY to these interfaces; src/backend/uv is the
// sole implementation today and the only place uv.h exists. A future
// WSAPoll/epoll backend implements the same interface and nothing above
// src/ changes - no public header, no user code.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/net/address.hpp>
#include <ws/net/errors.hpp>

namespace ws::net::detail {

class ILoopBackend {
 public:
  virtual ~ILoopBackend() = default;

  // Thread-safe by contract - the ONE call allowed from other threads.
  // Wakes the loop and runs fn on the loop thread, FIFO with earlier
  // posts. Posted work runs on the loop's next turn even when posted from
  // the loop thread itself; it never keeps the loop alive on its own.
  virtual void post(std::function<void()> fn) = 0;

  // One-shot timer, delay_ms from now. Returns the timer id, 0 on backend
  // failure (0 is never a valid id). Loop-thread only (or before run()).
  virtual std::uint32_t start_timer(std::uint64_t delay_ms,
                                    std::function<void()> on_fire) = 0;

  // false: unknown id, or the timer already fired / is closing.
  virtual bool cancel_timer(std::uint32_t id) = 0;

  // Runs until stop() or until no user work remains (timers, streams,
  // datagrams - not the post channel). Posts queued before run() execute
  // at entry: the unref'd wake channel cannot make an otherwise-empty
  // loop "alive", so the entry drain is what runs them.
  virtual void run() = 0;

  // One turn; may block until an event completes.
  virtual void run_once() = 0;

  // Thread-safe, idempotent - wakes a sleeping run() so it returns
  // promptly instead of at the next event.
  virtual void stop() = 0;

  // True on the thread currently inside run()/run_once().
  [[nodiscard]] virtual bool on_loop_thread() = 0;
};

// Implemented in src/backend/uv (backend_factory.cpp).
[[nodiscard]] std::unique_ptr<ILoopBackend> make_loop_backend();

// ---- TCP stream seam ----

// Upcalls from a stream peer into the connection logic driving it. All
// fire on the loop thread. on_stream_dead is exactly once and always
// the last call a live peer ever makes; after it the peer is inert
// (writes dropped, no further upcalls).
class IStreamSink {
 public:
  virtual ~IStreamSink() = default;

  // Socket established (client connect completed; server sides fire it
  // immediately after accept).
  virtual void on_connected() = 0;

  // Bytes arrived from the remote. The span is valid only for the call.
  virtual void on_bytes(std::span<const std::byte> data) = 0;

  // The stream is gone. `reason` is connection_closed when a graceful
  // close completed, or the failure code (connection_reset,
  // socket_error, ...) that killed it.
  virtual void on_stream_dead(NetError reason) = 0;
};

// The stream side of the seam: what connection logic drives. The uv
// implementation lives in src/backend/uv; tests fake it
// (tests/fake_transport.hpp).
class IStreamPeer {
 public:
  virtual ~IStreamPeer() = default;

  // Give the sink someone to report to. Called once by the owner right
  // after construction, before anything else.
  virtual void attach_sink(IStreamSink* sink) = 0;

  // Kick the peer's I/O off: a client begins connecting, an accepted
  // socket announces itself (on_connected) and starts reading. Called
  // once, after attach_sink; late or repeated calls are no-ops.
  virtual void start() = 0;

  // The peer keeps `owner` alive until its handle's close callback has
  // run - dropping every outside reference mid-flight must never free a
  // live backend handle (the CLAUDE.md lifetime rule). The type is
  // erased on purpose so the seam stays backend-agnostic.
  virtual void set_keepalive(std::shared_ptr<void> owner) = 0;

  // Queue bytes (the copy is taken here). No-op once dead.
  virtual void write(std::vector<std::byte> bytes) = 0;

  // Graceful: drain queued writes, half-close the write side (FIN), then
  // close the handle - reports on_stream_dead(connection_closed) when
  // the dance completes. A remote EOF triggers the same dance from the
  // peer's side, so pending writes still flush.
  virtual void finish_and_close() = 0;

  // Immediate teardown (RST): drops queued writes, closes now. Reports
  // on_stream_dead(connection_reset) unless already dead. Wins over an
  // in-flight finish_and_close.
  virtual void abort() = 0;

  // Bytes queued but not yet handed to the OS (backpressure metric).
  [[nodiscard]] virtual std::size_t pending() const = 0;
};

// The listener side of the seam. close() is idempotent; accepted
// connections outlive the acceptor they came from.
class IStreamAcceptor {
 public:
  virtual ~IStreamAcceptor() = default;

  // Stop accepting and release the listening handle. The accept callback
  // never fires again; existing connections are unaffected.
  virtual void close() = 0;

  // The address actually bound (the real port when the bind used 0).
  [[nodiscard]] virtual Address local() const = 0;

  // Same lifetime rule as IStreamPeer::set_keepalive.
  virtual void set_keepalive(std::shared_ptr<void> owner) = 0;
};

// Accepted peers are handed to the owner together with the remote's
// address (the sink wiring, keepalive and start are the owner's job).
using StreamAcceptFn =
    std::function<void(std::unique_ptr<IStreamPeer> peer, Address from)>;

// ---- backend factories (implemented in src/backend/uv) ----

// Starts a client connect. nullptr: the backend could not even begin
// (an address it cannot put on the wire). The asynchronous part of
// connecting - refused, unreachable, reset - reports through
// on_stream_dead like any other death.
[[nodiscard]] std::unique_ptr<IStreamPeer> make_stream_peer_connect(
    ILoopBackend& loop, const Address& peer);

// Binds and listens. nullptr: bind or listen failed (address in use,
// invalid for the backend, ...).
[[nodiscard]] std::unique_ptr<IStreamAcceptor> make_stream_acceptor(
    ILoopBackend& loop, const Address& local, int backlog,
    StreamAcceptFn on_accept);

// ---- UDP datagram seam ----

// Upcalls from a datagram socket into its owner. All fire on the loop
// thread; `data` is valid only for the call (debug builds poison the
// bytes right after, same as the framing core's delivered spans).
class IDatagramSink {
 public:
  virtual ~IDatagramSink() = default;

  // One datagram arrived. Empty datagrams never arrive (the backend
  // skips them; the public send refuses to make one).
  virtual void on_packet(std::span<const std::byte> data, Address from) = 0;
};

// The datagram side of the seam: what a bound UDP socket offers. Same
// lifetime discipline as the stream half - the ctx outlives the object
// and frees itself in the uv close callback.
class IDatagram {
 public:
  virtual ~IDatagram() = default;

  // Give the sink someone to report to. Called once by the owner right
  // after construction; datagrams arriving before it are dropped.
  virtual void attach_sink(IDatagramSink* sink) = 0;

  // Same lifetime rule as IStreamPeer::set_keepalive.
  virtual void set_keepalive(std::shared_ptr<void> owner) = 0;

  // Sends one datagram (the copy is taken here). Synchronous failures -
  // a dead socket, an address the backend cannot use, a payload over
  // the wire cap - fail the Status; asynchronous ones (an ICMP
  // "port unreachable" answering an earlier send) are deliberately
  // not reported anywhere: UDP is best-effort by design.
  [[nodiscard]] virtual ws::core::Status send_to(
      const Address& to, std::span<const std::byte> data) = 0;

  // Closes the socket: on_packet never fires again, the port is
  // released. Idempotent at the seam (the public layer reports
  // already_closed on the paths that matter).
  virtual void close() = 0;

  // The address actually bound (the real port when the bind used 0).
  [[nodiscard]] virtual Address local() const = 0;
};

// ---- datagram backend factory (implemented in src/backend/uv) ----

// Binds a UDP socket and starts receiving (port 0 = ephemeral).
// `reuse` lets a second socket bind the same port - the discovery
// pattern, where several local browsers share one machine. nullptr:
// bind or the receive start failed.
[[nodiscard]] std::unique_ptr<IDatagram> make_datagram(ILoopBackend& loop,
                                                       const Address& local,
                                                       bool reuse);

}  // namespace ws::net::detail
