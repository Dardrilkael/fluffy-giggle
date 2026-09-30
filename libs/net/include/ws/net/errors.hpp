#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <ws/core/result.hpp>

namespace ws::net {

// ws::net's error codes (root CLAUDE.md, "Error handling"): every public
// failure returns a ws::core::Error with category "net" and one of these as
// its code. The enum stays ws::net's - core never knows these values. Codes
// are append-only: never renumber, only add.
enum class NetError : std::int32_t {
  invalid_argument = 1,   // malformed argument (empty host, bad port, ...)
  invalid_address = 2,    // Address::parse could not read "host:port" out of it
  loop_stopped = 3,       // loop already stopped or torn down
  invalid_handle = 4,     // unknown, stale or never-issued timer id
  bind_failed = 5,
  connect_failed = 6,     // synchronous part of connect (resolve/setup)
  socket_error = 7,       // a backend call failed; Result messages carry detail
  message_too_large = 8,  // a TCP frame's length prefix exceeded the receive cap
  malformed_frame = 9,    // a datagram failed its protocol header check
  connection_closed = 10, // clean close (local close() completed, or a peer FIN)
  connection_reset = 11,  // abrupt peer disconnect (RST or a read error)
  already_closed = 12,    // API used after close completed
};

// One-line description per code, from a static table. This is the ONLY
// message surface inside callbacks (the workspace rule: callbacks never
// allocate to report errors): the string_view points at this table, so it
// stays valid forever - nothing builds messages per call.
[[nodiscard]] constexpr std::string_view describe(NetError code) noexcept {
  switch (code) {
    case NetError::invalid_argument: return "invalid argument";
    case NetError::invalid_address: return "invalid address";
    case NetError::loop_stopped: return "loop is stopped";
    case NetError::invalid_handle: return "invalid handle";
    case NetError::bind_failed: return "bind failed";
    case NetError::connect_failed: return "connect failed";
    case NetError::socket_error: return "socket error";
    case NetError::message_too_large: return "message too large";
    case NetError::malformed_frame: return "malformed frame";
    case NetError::connection_closed: return "connection closed";
    case NetError::connection_reset: return "connection reset";
    case NetError::already_closed: return "already closed";
  }
  return "unknown net error";
}

[[nodiscard]] inline ws::core::Error make_net_error(NetError code,
                                                    std::string message) {
  return ws::core::make_error(static_cast<std::int32_t>(code), "net",
                              std::move(message));
}

// unexpected<Error> that converts into any Result<T>:
//   return fail(NetError::bind_failed, "port already in use");
// RESULT-PATH ONLY: this allocates (ws::core::Error::message is a
// std::string by design). Callback surfaces carry NetError + describe()
// instead, never a dynamically built message. The name hides ws::core::fail
// inside ws::net on purpose: net errors go through the enum, never raw codes.
[[nodiscard]] inline ws::core::unexpected<ws::core::Error> fail(
    NetError code, std::string message = {}) {
  if (message.empty()) {
    message = std::string{describe(code)};
  }
  return ws::core::unexpected<ws::core::Error>(
      make_net_error(code, std::move(message)));
}

}  // namespace ws::net
