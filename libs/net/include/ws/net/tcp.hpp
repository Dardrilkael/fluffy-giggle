#pragma once

// Public TCP: framed, message-oriented connections (the 4-byte length
// prefix from the framing core) plus a listener. All callbacks fire on
// the loop thread and must not throw (event_loop.hpp); the errors they
// carry are NetError values whose descriptions come from the static
// describe() table - never a per-call buffer.
//
// Lifetime: a connection/listener stays alive on its own until its
// socket has fully closed - dropping your Ptr mid-flight never frees a
// live handle. Dropping it does NOT close it either: call close() or
// abort() to end one. An open connection counts as loop work: run()
// returns when everything is closed, not when you stopped looking.
//
// Addressing: numeric hosts only in v1 ("127.0.0.1", "::1") - no DNS.
// connect() fails synchronously only for unusable addresses; refused,
// unreachable and reset report through on_closed.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include <ws/core/result.hpp>
#include <ws/net/address.hpp>
#include <ws/net/errors.hpp>
#include <ws/net/visibility.hpp>

namespace ws::net {

class EventLoop;

class WS_NET_API TcpConnection {
 public:
  using Ptr = std::shared_ptr<TcpConnection>;

  // 1 MiB - matches the framing core's default receive cap.
  static constexpr std::uint32_t kDefaultMaxMessageSize = 1u << 20;

  // Connects asynchronously on `loop`. on_connected fires on a later
  // loop turn (set the callbacks right after this returns); a refused
  // or otherwise failed connect reports through
  // on_closed(connection_refused, ...). Loop-thread only (or before
  // run()); passing an unusable address fails the Result itself.
  [[nodiscard]] static ws::core::Result<Ptr> connect(EventLoop& loop,
                                                     const Address& peer);

  ~TcpConnection();

  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;
  TcpConnection(TcpConnection&&) = delete;
  TcpConnection& operator=(TcpConnection&&) = delete;

  // --- callbacks (loop thread, never throw) ---

  void set_on_connected(std::function<void()> on_connected);
  // The message span is valid ONLY inside the callback - debug builds
  // poison the bytes right after it returns. Copy what you keep.
  void set_on_message(
      std::function<void(std::span<const std::byte> message)> on_message);
  // Exactly once, always the last callback; NetError is the outcome
  // (connection_closed = graceful, the rest = what killed it).
  void set_on_closed(std::function<void(NetError why)> on_closed);

  // --- traffic ---

  // Receive cap for incoming messages. Set before traffic arrives
  // (right after connect / inside on_connection); an oversized incoming
  // frame kills the connection with on_closed(message_too_large).
  void set_max_message_size(std::uint32_t max);

  // Frames and COPIES `message` (the documented tradeoff: your buffer
  // is free to die with the call). A payload over the cap fails the
  // Result and leaves the connection healthy.
  [[nodiscard]] ws::core::Status send(std::span<const std::byte> message);

  // Graceful: queued messages flush, then FIN, then
  // on_closed(connection_closed). A second close() fails.
  [[nodiscard]] ws::core::Status close();

  // RST now - everything queued dies with it;
  // on_closed(connection_reset).
  void abort();

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] Address peer() const noexcept;
  [[nodiscard]] std::size_t pending_bytes() const noexcept;

 private:
  struct Impl;
  explicit TcpConnection(std::shared_ptr<Impl> impl);
  friend class TcpListener;  // builds server-side connections

  std::shared_ptr<Impl> impl_;
};

class WS_NET_API TcpListener {
 public:
  using Ptr = std::shared_ptr<TcpListener>;

  // Binds and listens on `loop` (port 0 = ephemeral: local() reports the
  // real one). Accepts fire on later loop turns, so set_on_connection
  // right after this returns is in time for every connection.
  [[nodiscard]] static ws::core::Result<Ptr> listen(EventLoop& loop,
                                                    const Address& local,
                                                    int backlog = 128);

  ~TcpListener();

  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;
  TcpListener(TcpListener&&) = delete;
  TcpListener& operator=(TcpListener&&) = delete;

  // Loop thread. Each accepted connection arrives as a TcpConnection
  // already alive: set its callbacks here - its on_connected fires
  // after this callback returns.
  void set_on_connection(std::function<void(TcpConnection::Ptr)> on_connection);

  // The address actually bound (the assigned port when bound to 0).
  [[nodiscard]] Address local() const;

  // Stops accepting and releases the port; existing connections are
  // unaffected. The listener dies on its own after; dropping the Ptr
  // alone does not stop it.
  void close();

 private:
  struct Impl;
  explicit TcpListener(std::shared_ptr<Impl> impl);

  std::shared_ptr<Impl> impl_;
};

}  // namespace ws::net
