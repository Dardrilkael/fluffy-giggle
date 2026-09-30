#pragma once

// Internal: the socket-free TCP connection state machine. Framing, the
// graceful-close drain, oversized-frame aborts and the callback-order
// guarantees all live here so they can be tested without a backend
// (tests/fake_transport.hpp fakes IStreamPeer; src/backend/uv plugs the
// real thing in N5).
//
// Callback contract (these are the public API guarantees, enforced here):
//   on_connected -> on_message* -> on_closed, all on the loop thread;
//   on_closed fires exactly once, last, always;
//   on_message spans die with the callback - debug builds poison the
//   bytes (0xCD) the moment it returns (see frame.hpp).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "core/frame.hpp"
#include "core/transport.hpp"
#include <ws/core/result.hpp>
#include <ws/net/errors.hpp>

namespace ws::net::detail {

struct ConnectionEvents {
  std::function<void()> on_connected;
  std::function<void(std::span<const std::byte>)> on_message;
  std::function<void(NetError)> on_closed;
};

class ConnectionLogic final : public IStreamSink {
 public:
  // Starts in `connecting`; on_connected() moves it to open. Server-side
  // connections just get their on_connected() immediately.
  ConnectionLogic(std::unique_ptr<IStreamPeer> peer,
                  ConnectionEvents events,
                  std::uint32_t max_message = kDefaultMaxPayload);

  // --- IStreamSink: the peer drives these ---
  void on_connected() override;
  void on_bytes(std::span<const std::byte> data) override;
  void on_stream_dead(NetError reason) override;

  // --- user side (the public TcpConnection pimpl forwards these) ---
  [[nodiscard]] ws::core::Status send(std::span<const std::byte> payload);
  [[nodiscard]] ws::core::Status close();
  void abort();
  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] std::size_t pending_bytes() const noexcept;

 private:
  enum class State : std::uint8_t {
    connecting,  // socket not up yet; sends buffer locally
    open,
    closing,  // close() called: flushing writes, draining reads
    closed,
  };

  std::unique_ptr<IStreamPeer> peer_;
  ConnectionEvents events_;
  FrameParser parser_;
  State state_ = State::connecting;
  // Set when WE chose the outcome (abort, oversize, a cancelled connect);
  // unset on a graceful close so a mid-flush write failure surfaces its
  // own error instead of a cosmetic connection_closed.
  std::optional<NetError> close_reason_;
  std::vector<std::byte> pre_connect_;  // framed sends queued pre-connect
};

}  // namespace ws::net::detail
