#include <ws/net/address.hpp>
#include <ws/net/errors.hpp>  // NetError, fail - parse's rejection paths

namespace ws::net {
namespace {

// Port parse: all digits, 1..65535. A lone "0" is not accepted - port 0
// reaches the API only via the constructors, where it means "ephemeral".
bool parse_port(std::string_view text, std::uint16_t& out) {
  if (text.empty() || text.size() > 5) {
    return false;
  }
  std::uint32_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + static_cast<std::uint32_t>(c - '0');
  }
  if (value == 0 || value > 65535) {
    return false;
  }
  out = static_cast<std::uint16_t>(value);
  return true;
}

}  // namespace

Address Address::loopback(std::uint16_t port) { return {"127.0.0.1", port}; }

Address Address::any(std::uint16_t port) { return {"0.0.0.0", port}; }

Address Address::broadcast(std::uint16_t port) {
  return {"255.255.255.255", port};
}

ws::core::Result<Address> Address::parse(std::string_view host_colon_port) {
  std::string_view host_text;
  std::string_view port_text;

  if (!host_colon_port.empty() && host_colon_port.front() == '[') {
    // "[v6]:port" - the only IPv6 spelling that carries a port.
    const std::size_t close = host_colon_port.find(']');
    if (close == std::string_view::npos ||
        close + 1 >= host_colon_port.size() ||
        host_colon_port[close + 1] != ':') {
      return fail(NetError::invalid_address, "expected \"[v6host]:port\"");
    }
    host_text = host_colon_port.substr(1, close - 1);
    port_text = host_colon_port.substr(close + 2);
  } else {
    // "host:port" - the LAST colon separates. A non-bracket host may not
    // contain a colon at all (that is IPv6's territory: use brackets).
    const std::size_t colon = host_colon_port.rfind(':');
    if (colon == std::string_view::npos) {
      return fail(NetError::invalid_address, "expected \"host:port\"");
    }
    host_text = host_colon_port.substr(0, colon);
    port_text = host_colon_port.substr(colon + 1);
    if (host_text.find(':') != std::string_view::npos) {
      return fail(NetError::invalid_address,
                  "IPv6 hosts need brackets: \"[host]:port\"");
    }
  }

  if (host_text.empty()) {
    return fail(NetError::invalid_address, "empty host");
  }
  std::uint16_t port = 0;
  if (!parse_port(port_text, port)) {
    return fail(NetError::invalid_address, "port must be 1..65535");
  }
  return Address{std::string{host_text}, port};
}

}  // namespace ws::net
