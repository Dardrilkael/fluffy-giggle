#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <ws/core/result.hpp>
#include <ws/net/visibility.hpp>

namespace ws::net {

// A ws-owned endpoint: host (numeric or a name the backend resolves) + port.
// No sockaddr/errno/WSA type ever appears in the public API - conversion to
// platform form happens inside the backend, at bind/connect/send time.
// IPv6 hosts never keep their brackets ("::1", not "[::1]").
struct Address {
  std::string host;
  std::uint16_t port = 0;

  Address() = default;
  Address(std::string host_, std::uint16_t port_)
      : host(std::move(host_)), port(port_) {}

  [[nodiscard]] static Address loopback(std::uint16_t port);  // 127.0.0.1
  [[nodiscard]] static Address any(std::uint16_t port);       // 0.0.0.0 - bind-all
  [[nodiscard]] static Address broadcast(std::uint16_t port); // 255.255.255.255 - send-only

  // Parses "host:port" (IPv6 requires brackets: "[::1]:8080"). The port is
  // mandatory, 1..65535; an empty host is invalid. Port 0 is expressible
  // only through the constructors: "ephemeral" where the backend binds
  // (local() reports the real one), "unset" everywhere else.
  [[nodiscard]] static WS_NET_API ws::core::Result<Address> parse(
      std::string_view host_colon_port);

  // A host is required; port 0 is legal (bind-time "ephemeral").
  [[nodiscard]] bool valid() const noexcept { return !host.empty(); }

  friend bool operator==(const Address& a, const Address& b) {
    return a.port == b.port && a.host == b.host;
  }
  friend bool operator!=(const Address& a, const Address& b) {
    return !(a == b);
  }
};

}  // namespace ws::net
