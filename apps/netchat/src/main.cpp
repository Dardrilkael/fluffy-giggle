// apps/netchat - terminal chat over ws::net. Every running copy is both
// listener and dialer: each announces itself by discovery beat - LAN
// broadcast for peers on other machines, a loopback-directed broadcast
// (127.255.255.255, all of 127/8 is local) for peers in other terminals
// on this machine - and each browser that hears a beat dials the
// announcer's listener when its own listen port is the smaller of the
// two, so every pair of terminals ends up with exactly one framed TCP
// connection (equal ports - vanishingly rare with ephemeral picks -
// simply never pair). A beat this copy cannot dial (its port is the
// bigger one) still earns an answer: one unicast copy of our own beat,
// straight back at the beat's source - a LAN hop can carry broadcasts
// one way only (the wired PC here has never received the phone's Wi-Fi
// beats), and the unicast answer crosses that hop, so both sides end
// up holding both candidates and the port rule still gives the pair
// exactly one connection. The first frame on any connection is HELLO with a
// name to print; every frame after is a chat line. Whatever is typed in
// one terminal prints in all the others; /quit (or end of input) closes
// that copy down with connections still live.
//
// Several terminals on one machine: run netchat in each. Across a LAN:
// run it on several machines.

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <ws/core/log.hpp>
#include <ws/core/result.hpp>
#include <ws/net/address.hpp>
#include <ws/net/discovery.hpp>
#include <ws/net/errors.hpp>
#include <ws/net/event_loop.hpp>
#include <ws/net/tcp.hpp>

namespace {

// std::byte <-> text, the netdemo translation pair.
std::span<const std::byte> to_bytes(std::string_view text) {
  return std::as_bytes(std::span<const char>{text.data(), text.size()});
}

std::string_view as_text(std::span<const std::byte> data) {
  if (data.empty()) {
    return {};
  }
  return {reinterpret_cast<const char*>(data.data()), data.size()};
}

// One live connection plus the name its HELLO carried. The vector in
// main's scope holds the ownership; nothing ever captures a connection
// Ptr by value inside that connection's own stored callbacks (the
// shared_ptr cycle rule from tcp.hpp's lifetime notes) - callbacks hold
// the name tag by value and this connection by raw pointer only.
struct Peer {
  ws::net::TcpConnection::Ptr conn;
  std::shared_ptr<std::string> name;  // "?" until HELLO lands
};

}  // namespace

int main(int argc, char* argv[]) {
  ws::net::EventLoop loop;

  // The listen port picks itself; the name comes from the command line
  // or falls back to the port. The port is also the pairing rule's
  // tiebreaker, so every copy needs it as a number.
  auto listener_r =
      ws::net::TcpListener::listen(loop, ws::net::Address::any(0));
  if (!listener_r) {
    ws::core::log_error(listener_r.error());
    return 1;
  }
  ws::net::TcpListener::Ptr listener = *listener_r;
  // Any-bind caveat: local() reports 0.0.0.0 - bind-all, a port to READ,
  // never a destination. Only the port is real.
  const int my_port = static_cast<int>(listener->local().port);
  const std::string my_name =
      argc > 1 ? argv[1] : "anon-" + std::to_string(my_port);

  // Main-scope state, captured by reference by every callback below.
  std::vector<Peer> peers;  // every live connection, dialed and accepted
  // Announced ports this copy has dialed, and the connection each got.
  // Keyed by the announced port alone: a peer is its listen port in this
  // protocol, whatever source address its beats arrive from. on_closed
  // clears the entry, so a beat that keeps arriving can re-dial a peer
  // that came back.
  std::map<std::string, ws::net::TcpConnection*> dialed;

  // Everything a fresh connection needs, dialed or accepted alike:
  // HELLO on connect, HELLO/SAY parsing after, bookkeeping on close.
  const auto adopt = [&](const ws::net::TcpConnection::Ptr& conn) {
    const auto tag = std::make_shared<std::string>("?");
    ws::net::TcpConnection* self = conn.get();
    peers.push_back({conn, tag});
    // Both sides greet: the dialed connection's on_connected, and the
    // accepted one's - which fires after on_connection returns.
    conn->set_on_connected([self, my_name]() {
      (void)self->send(to_bytes("HELLO " + my_name));
    });
    conn->set_on_message([tag](std::span<const std::byte> message) {
      std::string_view text = as_text(message);
      if (text.starts_with("HELLO ")) {
        text.remove_prefix(6);
        *tag = std::string{text};  // copy: the span dies with the callback
        ws::core::info("netchat: {} joined", *tag);
        return;
      }
      // SAY is this app's chat wrapper. Anything else (a peer speaking
      // some other protocol, a plain text frame) still prints as-is.
      const std::string_view body =
          text.starts_with("SAY ") ? text.substr(4) : text;
      ws::core::info("netchat: [{}] {}", *tag, body);
    });
    conn->set_on_closed([tag, self, &peers, &dialed](ws::net::NetError why) {
      if (*tag != "?") {
        ws::core::info("netchat: {} left ({})", *tag, ws::net::describe(why));
      }
      std::erase_if(peers,
                    [self](const Peer& p) { return p.conn.get() == self; });
      std::erase_if(dialed, [self](const std::pair<const std::string,
                                                 ws::net::TcpConnection*>& kv) {
        return kv.second == self;
      });
    });
  };

  listener->set_on_connection([&](ws::net::TcpConnection::Ptr conn) {
    ws::core::info("netchat: a peer dialed in");
    adopt(conn);
  });

  // --- discovery: a browser to hear beats, two announcers to send them --

  const std::string beat = "netchat " + std::to_string(my_port) + " " + my_name;

  ws::net::DiscoveryAnnouncer::Ptr lan_announcer;
  auto lan_r = ws::net::DiscoveryAnnouncer::announce(
      loop, to_bytes(beat),
      ws::net::Address::broadcast(ws::net::kDefaultDiscoveryPort), 500);
  if (lan_r) {
    lan_announcer = *lan_r;
  } else {
    ws::core::log_error(lan_r.error());
    ws::core::warn("netchat: no LAN beats - only this machine can see this copy");
  }

  // The same-machine rendezvous: Windows does not loop 255.255.255.255
  // sends back to the sender's own listeners (the netdemo run showed 0
  // local beats), but 127.255.255.255 is loopback's own directed
  // broadcast - it stays on this machine and reaches every browser
  // bound here. The library already sets SO_BROADCAST on every UDP
  // socket, so this announce needs no special arrangement.
  ws::net::DiscoveryAnnouncer::Ptr loopback_announcer;
  auto loopback_r = ws::net::DiscoveryAnnouncer::announce(
      loop, to_bytes(beat),
      ws::net::Address{"127.255.255.255", ws::net::kDefaultDiscoveryPort},
      500);
  if (loopback_r) {
    loopback_announcer = *loopback_r;
  } else {
    ws::core::log_error(loopback_r.error());
  }

  // The answer a heard beat earns when this copy is not the dialer: one
  // unicast copy of our own beat, straight back at the beat's source -
  // the announcer's own socket, off-cadence. A LAN hop can carry
  // broadcasts one way only (the wired PC here has never once received
  // the phone's Wi-Fi beats), so the peer on the far side may never
  // learn this copy exists; the unicast answer crosses that hop, and
  // the port rule on its side ends up holding both candidates.
  // Throttled per host - beats keep arriving every 500ms and each would
  // earn a fresh answer otherwise; a peer already connected to us
  // ignores the answer (its dialed map holds our port), so nothing
  // extra tracks "paired".
  std::map<std::string, std::chrono::steady_clock::time_point> answered;
  const auto answer_beat = [&](const ws::net::Address& from) {
    if (!lan_announcer) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    auto [it, fresh] = answered.try_emplace(from.host, now);
    if (!fresh && now - it->second < std::chrono::seconds{2}) {
      return;
    }
    it->second = now;
    (void)lan_announcer->send_once(
        ws::net::Address{from.host, ws::net::kDefaultDiscoveryPort});
  };

  ws::net::DiscoveryBrowser::Ptr browser;
  auto browser_r = ws::net::DiscoveryBrowser::browse(loop);
  if (browser_r) {
    browser = *browser_r;
  } else {
    ws::core::log_error(browser_r.error());
    ws::core::warn("netchat: no discovery - this copy runs alone");
  }
  if (browser) {
    browser->set_on_peer(
        [&](std::span<const std::byte> payload, ws::net::Address from) {
          std::string_view text = as_text(payload);
          constexpr std::string_view kPrefix = "netchat ";
          if (!text.starts_with(kPrefix)) {
            return;  // another app's beat (netdemo says hello, ...) - not ours
          }
          text.remove_prefix(kPrefix.size());
          const std::size_t space = text.find(' ');
          if (space == std::string_view::npos) {
            return;
          }
          const std::string_view port_s = text.substr(0, space);
          int port = 0;
          const auto [end, ec] =
              std::from_chars(port_s.data(), port_s.data() + port_s.size(), port);
          if (ec != std::errc{} || end != port_s.data() + port_s.size() ||
              port <= 0 || port > 65535) {
            return;
          }
          if (port == my_port) {
            return;  // my own beat, looped back to me
          }
          if (dialed.count(std::string{port_s}) != 0) {
            return;  // already dialing, or talking, to this announced port
          }
          if (my_port >= port) {
            answer_beat(from);  // teach a one-way-deaf peer this copy exists
            return;  // the smaller listen port dials: one connection per pair
          }
          auto conn_r = ws::net::TcpConnection::connect(
              loop,
              ws::net::Address{from.host, static_cast<std::uint16_t>(port)});
          if (!conn_r) {
            ws::core::log_error(conn_r.error());  // the next beat retries
            return;
          }
          ws::net::TcpConnection::Ptr conn = *conn_r;
          dialed[std::string{port_s}] = conn.get();
          adopt(conn);
        });
  }

  ws::core::info("netchat: {} here (port {}) - type to talk, /quit to leave",
                 my_name, my_port);

  // stdin on its own thread: getline blocks, so it cannot live on the
  // loop thread. Each finished line crosses over with post() - the one
  // thread-safe call - carrying its own copy of the text.
  std::thread input([&]() {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line == "/quit") {
        break;
      }
      if (line.empty()) {
        continue;
      }
      std::string outgoing = "SAY " + line;
      (void)loop.post([outgoing = std::move(outgoing), &peers]() {
        ws::core::info("netchat: you: {}",
                       std::string_view{outgoing}.substr(4));
        for (const Peer& p : peers) {
          (void)p.conn->send(to_bytes(outgoing));
        }
      });
    }
    (void)loop.post([&]() {
      // Close everything, live connections included - the supported
      // shutdown shape; run() drains and returns. on_closed fires during
      // the drain and clears peers, so iterate a snapshot.
      const std::vector<Peer> snapshot = peers;
      for (const Peer& p : snapshot) {
        (void)p.conn->close();
      }
      listener->close();
      if (lan_announcer) {
        lan_announcer->stop();
      }
      if (loopback_announcer) {
        loopback_announcer->stop();
      }
      if (browser) {
        browser->stop();
      }
    });
  });

  const ws::core::Status ran = loop.run();
  if (!ran) {
    ws::core::log_error(ran.error());
    input.detach();  // getline may still be blocked; the process is exiting
    return 1;
  }
  input.join();
  ws::core::info("netchat: {} signed off", my_name);
  return 0;
}
