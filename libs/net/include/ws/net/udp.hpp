#pragma once

// A bound UDP socket: fire datagrams at any address, receive them in
// one-piece reads. UDP is best-effort - datagrams can be dropped or
// reordered on the wire, and send_to reports only synchronous failures
// (bad address, closed socket, payload over the wire cap); anything
// that happens after the datagram left (an ICMP "port unreachable"
// answering an earlier send, say) is deliberately silent. Packets
// arrive on the loop thread through on_packet, and the payload span is
// valid only for the callback - debug builds poison the bytes right
// after it returns, so a stashed span reads garbage immediately.
//
// Lifetime matches TCP: the returned Ptr keeps the socket usable from
// anywhere, close() is exactly-once (a second call is already_closed),
// and dropping every reference mid-flight never frees a live backend
// handle - the socket dies on its own once the loop drains, or with
// the EventLoop.

#include <cstddef>
#include <functional>
#include <memory>
#include <span>

#include <ws/core/result.hpp>
#include <ws/net/address.hpp>
#include <ws/net/visibility.hpp>

namespace ws::net {

class EventLoop;

class WS_NET_API UdpSocket {
 public:
  using Ptr = std::shared_ptr<UdpSocket>;

  // The largest IPv4 UDP payload that can ever fit on the wire
  // (65535 - 20 IP - 8 UDP). send_to refuses more; a bound socket can
  // receive anything under it in one piece.
  static constexpr std::size_t kMaxDatagramSize = 65507;

  // Binds and starts receiving. Port 0 picks an ephemeral port - read
  // it back with local(). `reuse` lets a second socket bind the same
  // port (the discovery pattern: several browsers per machine).
  [[nodiscard]] static ws::core::Result<Ptr> bind(EventLoop& loop,
                                                  const Address& local,
                                                  bool reuse = false);

  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;
  UdpSocket(UdpSocket&&) = delete;
  UdpSocket& operator=(UdpSocket&&) = delete;

  // Fired on the loop thread for every datagram. `data` is valid only
  // for the call; `from` is the sender's address (host + source port).
  void set_on_packet(
      std::function<void(std::span<const std::byte> data, Address from)>
          on_packet);

  // Queues one datagram. Synchronous failures only - see the header
  // comment for why asynchronous ones are silent by design.
  [[nodiscard]] ws::core::Status send_to(const Address& to,
                                         std::span<const std::byte> data);

  // The address actually bound (the real port when the bind used 0).
  [[nodiscard]] Address local() const;

  // False once close() has been called.
  [[nodiscard]] bool is_open() const;

  // Stops receiving and releases the port. Exactly-once: a second call
  // is already_closed. In-flight sends are dropped silently - UDP has
  // no graceful-shutdown dance worth exposing.
  [[nodiscard]] ws::core::Status close();

 private:
  struct Impl;
  explicit UdpSocket(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};

}  // namespace ws::net
