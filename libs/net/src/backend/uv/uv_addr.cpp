#include "uv_addr.hpp"

#include <cstring>  // std::memcpy — write exactly sizeof(sockaddr_in/_in6), no 128-byte read

namespace ws::net::detail {

namespace {

bool has_ipv6_colon(const std::string& host) {
  return host.find(':') != std::string::npos;
}

}  // namespace

bool address_to_sockaddr(const Address& address, sockaddr_storage& out) {
  if (!address.valid()) {
    return false;
  }
  // uv_ip4_addr / uv_ip6_addr are exactly inet_pton + fill wrapped, so
  // non-numeric hosts fail here (no DNS in v1 - tcp.hpp says so).
  if (has_ipv6_colon(address.host)) {
    sockaddr_in6 v6{};
    if (uv_ip6_addr(address.host.c_str(), address.port, &v6) != 0) {
      return false;
    }
    // Copy by actual size, not reinterpret_cast<sockaddr_storage*>: a
    // sockaddr_in6 is 28 bytes but sockaddr_storage is 128, so the cast
    // would read 100 bytes of stack past v6 (ASan: stack-buffer-overflow).
    std::memcpy(&out, &v6, sizeof(v6));
    return true;
  }
  sockaddr_in v4{};
  if (uv_ip4_addr(address.host.c_str(), address.port, &v4) != 0) {
    return false;
  }
  // Same as above: sockaddr_in is 16 bytes, sockaddr_storage is 128.
  std::memcpy(&out, &v4, sizeof(v4));
  return true;
}

Address sockaddr_to_address(const sockaddr* sa) {
  if (sa == nullptr) {
    return {};
  }
  char buf[64] = {};
  if (sa->sa_family == AF_INET6) {
    const auto* v6 = reinterpret_cast<const sockaddr_in6*>(sa);
    if (uv_ip6_name(v6, buf, sizeof(buf)) != 0) {
      return {};
    }
    return {buf, ntohs(v6->sin6_port)};
  }
  if (sa->sa_family == AF_INET) {
    const auto* v4 = reinterpret_cast<const sockaddr_in*>(sa);
    if (uv_ip4_name(v4, buf, sizeof(buf)) != 0) {
      return {};
    }
    return {buf, ntohs(v4->sin_port)};
  }
  return {};
}

}  // namespace ws::net::detail
