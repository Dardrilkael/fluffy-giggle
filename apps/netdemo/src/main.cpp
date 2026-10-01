// apps/netdemo - the headless ws::net example. One EventLoop runs every
// feature of the library once: framed TCP over loopback (a listener, a
// client, and an echoing server side), a UDP round trip, and LAN
// discovery on the default broadcast port - then everything is closed
// explicitly, in one place, with loop work still in flight. Every log
// line is a step of that walkthrough; run it on two machines on one LAN
// and each netdemo's browser prints the other's beats.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <ws/core/log.hpp>
#include <ws/core/result.hpp>
#include <ws/net/address.hpp>
#include <ws/net/discovery.hpp>
#include <ws/net/errors.hpp>
#include <ws/net/event_loop.hpp>
#include <ws/net/tcp.hpp>
#include <ws/net/udp.hpp>

namespace {

// The library speaks std::byte; the example speaks text. These two are
// the whole translation layer.
std::span<const std::byte> to_bytes(std::string_view text) {
  return std::as_bytes(std::span<const char>{text.data(), text.size()});
}

std::string_view as_text(std::span<const std::byte> data) {
  if (data.empty()) {
    return {};
  }
  return {reinterpret_cast<const char*>(data.data()), data.size()};
}

// The echo server's transform: lowercase ASCII to uppercase, built from
// a COPY of the delivered span - the span itself dies with the callback
// (debug builds poison the bytes right after it returns).
std::vector<std::byte> shout_back(std::span<const std::byte> message) {
  std::vector<std::byte> out(message.size());
  for (std::size_t i = 0; i < message.size(); ++i) {
    const unsigned char c = std::to_integer<unsigned char>(message[i]);
    out[i] = (c >= 'a' && c <= 'z')
                 ? static_cast<std::byte>(c - 'a' + 'A')
                 : message[i];
  }
  return out;
}

}  // namespace

int main() {
  ws::core::info("netdemo: one loop, every ws::net feature once");

  ws::net::EventLoop loop;

  // --- TCP: listener + client over loopback, messages echoed back ------

  auto listener_r =
      ws::net::TcpListener::listen(loop, ws::net::Address::loopback(0));
  if (!listener_r) {
    ws::core::log_error(listener_r.error());
    return 1;
  }
  ws::net::TcpListener::Ptr listener = *listener_r;
  const ws::net::Address at = listener->local();
  ws::core::info("netdemo: tcp listening at {}:{} (port 0 picked the real one)",
                 at.host, at.port);

  // The server side of the accepted connection, held in main's scope on
  // purpose: a Ptr captured BY VALUE inside its own stored callbacks
  // would cycle the shared_ptr. References to this holder cannot.
  std::shared_ptr<ws::net::TcpConnection> server_side;
  int echoes = 0;

  listener->set_on_connection([&](ws::net::TcpConnection::Ptr conn) {
    const ws::net::Address peer = conn->peer();
    ws::core::info("netdemo: tcp server: connection from {}:{}", peer.host,
                   peer.port);
    server_side = std::move(conn);
    server_side->set_on_message([&](std::span<const std::byte> message) {
      ws::core::info("netdemo: tcp server echoing: \"{}\"", as_text(message));
      (void)server_side->send(shout_back(message));
    });
    server_side->set_on_closed([](ws::net::NetError why) {
      ws::core::info("netdemo: tcp server side closed: {}",
                     ws::net::describe(why));
    });
  });

  auto client_r = ws::net::TcpConnection::connect(loop, at);
  if (!client_r) {
    ws::core::log_error(client_r.error());
    return 1;
  }
  ws::net::TcpConnection::Ptr client = *client_r;
  client->set_on_connected([&]() {
    ws::core::info(
        "netdemo: tcp client connected - sending three framed messages");
    // send() copies (the documented tradeoff): each literal may die with
    // the call. Different lengths on purpose - the frames prove the
    // length-prefix protocol, not line conventions.
    (void)client->send(to_bytes("hello"));
    (void)client->send(to_bytes("a longer message, with spaces and punctuation!"));
    (void)client->send(to_bytes("framed by length, not by lines"));
  });
  client->set_on_message([&](std::span<const std::byte> message) {
    ++echoes;
    ws::core::info("netdemo: tcp client heard: \"{}\"", as_text(message));
    if (echoes == 3) {
      ws::core::info("netdemo: tcp round trip done - three frames came back whole");
    }
  });
  client->set_on_closed([](ws::net::NetError why) {
    ws::core::info("netdemo: tcp client closed: {}", ws::net::describe(why));
  });

  // --- UDP: one datagram, one piece, sender's address attached ----------

  auto udp_rx_r = ws::net::UdpSocket::bind(loop, ws::net::Address::loopback(0));
  auto udp_tx_r = ws::net::UdpSocket::bind(loop, ws::net::Address::loopback(0));
  if (!udp_rx_r || !udp_tx_r) {
    ws::core::log_error(udp_rx_r.error());
    ws::core::log_error(udp_tx_r.error());
    return 1;
  }
  ws::net::UdpSocket::Ptr udp_rx = *udp_rx_r;
  ws::net::UdpSocket::Ptr udp_tx = *udp_tx_r;
  int datagrams = 0;
  udp_rx->set_on_packet(
      [&](std::span<const std::byte> data, ws::net::Address from) {
        ++datagrams;
        ws::core::info("netdemo: udp heard \"{}\" from {}:{}", as_text(data),
                       from.host, from.port);
      });
  // A loopback bind's local() is directly sendable. (An any-bind reports
  // 0.0.0.0 - a port to READ; wrap it in Address::loopback() instead.)
  (void)udp_tx->send_to(udp_rx->local(),
                        to_bytes("one datagram, no framing needed"));

  // --- LAN discovery: broadcast beats on the default port ---------------

  ws::net::DiscoveryBrowser::Ptr browser;
  int beats = 0;
  auto browser_r = ws::net::DiscoveryBrowser::browse(loop);
  if (browser_r) {
    browser = *browser_r;
    ws::core::info("netdemo: discovery browser on the default port {}",
                   ws::net::kDefaultDiscoveryPort);
    browser->set_on_peer(
        [&](std::span<const std::byte> payload, ws::net::Address from) {
          ++beats;
          ws::core::info("netdemo: discovery beat \"{}\" from {}:{}",
                         as_text(payload), from.host, from.port);
        });
  } else {
    ws::core::log_error(browser_r.error());
    ws::core::warn("netdemo: continuing without the discovery browser");
  }

  ws::net::DiscoveryAnnouncer::Ptr announcer;
  auto announcer_r = ws::net::DiscoveryAnnouncer::announce(
      loop, to_bytes("netdemo says hello"),
      ws::net::Address::broadcast(ws::net::kDefaultDiscoveryPort), 500);
  if (announcer_r) {
    announcer = *announcer_r;
    ws::core::info(
        "netdemo: announcing every 500 ms - a second machine on the LAN "
        "shows up in the beats");
  } else {
    ws::core::log_error(announcer_r.error());
  }

  // --- shutdown: everything still open, closed in one place ------------

  // post() is THE thread-safe call; queued before run(), it lands at
  // run() entry. A post alone never keeps the loop alive.
  (void)loop.post([]() {
    ws::core::info("netdemo: the loop is live - this line was posted before run()");
  });

  // When this fires the loop still holds live work everywhere: open
  // connections, bound sockets, an armed cadence. Closing all of it now
  // is exactly the supported shutdown; the order does not matter to the
  // library, this order is just a readable one.
  (void)loop.start_timer(2500, [&]() {
    ws::core::info(
        "netdemo: time is up - closing everything with work still in flight");
    (void)client->close();
    // close() is exactly-once - the second call reports it, by design:
    if (auto second = client->close(); !second) {
      ws::core::info("netdemo: second close() says \"{}\", as documented",
                     ws::net::describe(
                         static_cast<ws::net::NetError>(second.error().code)));
    }
    if (server_side) {
      (void)server_side->close();
    }
    listener->close();
    (void)udp_tx->close();
    (void)udp_rx->close();
    if (announcer) {
      announcer->stop();
    }
    if (browser) {
      browser->stop();
    }
    ws::core::info(
        "netdemo: teardown requested - run() returns when the last close lands");
  });

  const ws::core::Status ran = loop.run();
  if (!ran) {
    ws::core::log_error(ran.error());
    return 1;
  }

  ws::core::info("netdemo: summary - {} tcp echoes, {} udp datagrams, {} discovery beats",
                 echoes, datagrams, beats);
  if (browser && beats == 0) {
    ws::core::info(
        "netdemo: no beats came back - broadcast loopback varies by OS, NIC "
        "and firewall; on a real LAN the other machines' netdemo would show "
        "up above");
  }
  return 0;
}
