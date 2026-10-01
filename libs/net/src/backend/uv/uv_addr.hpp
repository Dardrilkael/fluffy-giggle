#pragma once

// Internal: Address <-> sockaddr conversion, uv-backend only. This is the
// single spot where a ws::net::Address becomes a platform sockaddr - the
// public type never learns what a sockaddr is (address.hpp), and a future
// poll backend supplies its own pair of helpers.

#include <uv.h>

#include <ws/net/address.hpp>

namespace ws::net::detail {

// Numeric hosts only (v1: no DNS) - "127.0.0.1", "::1". IPv6 is detected
// by a ':' in the host. false: the address is not puttable on the wire.
[[nodiscard]] bool address_to_sockaddr(const Address& address,
                                      sockaddr_storage& out);

// The inverse: peer/local addresses reported by the kernel.
[[nodiscard]] Address sockaddr_to_address(const sockaddr* sa);

}  // namespace ws::net::detail
