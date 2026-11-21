#pragma once

// Raw byte-stream TCP API. Unlike TcpConnection, this surface does NOT add
// or remove message framing; intended for protocols such as HTTP.
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/net/address.hpp>
#include <ws/net/errors.hpp>
#include <ws/net/visibility.hpp>

namespace ws::net {
class EventLoop;

class WS_NET_API RawTcpConnection {
 public:
  using Ptr = std::shared_ptr<RawTcpConnection>;
  ~RawTcpConnection();
  RawTcpConnection(const RawTcpConnection&) = delete;
  RawTcpConnection& operator=(const RawTcpConnection&) = delete;

  [[nodiscard]] static ws::core::Result<Ptr> connect(EventLoop& loop,
                                                       const Address& peer);
  void set_on_connected(std::function<void()> callback);
  void set_on_bytes(std::function<void(std::span<const std::byte>)> callback);
  void set_on_closed(std::function<void(NetError)> callback);
  // Copies and queues exactly these bytes; no framing or transformation.
  [[nodiscard]] ws::core::Status send(std::span<const std::byte> bytes);
  // Flush queued bytes, send FIN, then close.
  [[nodiscard]] ws::core::Status close();
  // Immediate reset.
  void abort();
  [[nodiscard]] Address peer() const noexcept;

 private:
  struct Impl;
  explicit RawTcpConnection(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
  friend class RawTcpListener;
};

class WS_NET_API RawTcpListener {
 public:
  using Ptr = std::shared_ptr<RawTcpListener>;
  ~RawTcpListener();
  RawTcpListener(const RawTcpListener&) = delete;
  RawTcpListener& operator=(const RawTcpListener&) = delete;

  [[nodiscard]] static ws::core::Result<Ptr> listen(
      EventLoop& loop, const Address& local, int backlog = 128);
  void set_on_connection(std::function<void(RawTcpConnection::Ptr)> callback);
  [[nodiscard]] Address local() const;
  void close();

 private:
  struct Impl;
  explicit RawTcpListener(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};
}  // namespace ws::net
