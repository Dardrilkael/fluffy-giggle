// apps/chatter/src/chat_core.hpp - the chat protocol core, v2.
//
// Header-only and deliberately gui-free (ws::net + ws::core + std only):
// main.cpp pairs it with the ui layer, and a headless harness can include
// this same header and drive the REAL protocol with no window at all -
// the same split netchat's core enjoys inside its single file, opened up
// so the relay logic is testable.
//
// Two transports, one chat:
//
//   LAN (v1, unchanged): the "netchat <port> <name>" beat every 500 ms on
//   two channels, the smaller-listen-port-dials pairing rule, HELLO/SAY
//   framed TCP per pair. Chatter and the terminal netchat still find each
//   other and share one chat.
//
//   Relay (new): for peers that will never see each other's beats - not
//   on the same network. `--serve` runs a member room: every accepted
//   connection is a member, and this copy forwards traffic between them.
//   `--relay host:port` (repeatable) dials a room from a normal client
//   and keeps the link alive (2 s redial, 5 s connect deadline).
//
// The wire stays HELLO/SAY outward from every client (a relay is just
// another peer to its callers, so v1 peers - netchat included - still
// work). The room answers in two frames a v1 client safely shows as-is:
//
//   SYS <text>       room event: "alice joined", "bob left", the roster
//                    handed to a newcomer ("in the room: ...").
//   FROM <name> <t>  a member's message, attributed on the wire - the
//                    relay's own HELLO names it, not the sender.
//
// Duplicate suppression: a client that has a sender as a DIRECT Lan peer
// drops that sender's FROM frames (direct_names_ below). Without it the
// common setup - friends on one LAN, all also on the same internet relay
// - shows every message twice: once over the direct link, once relayed.
// With it each pair gets exactly one line per message, from whichever
// transport delivered first; the relay cannot know LAN topology, so the
// endpoints decide, and they know.
//
// Threading (v1, unchanged): the loop and every socket live on the net
// thread; the gui/harness touches only start/stop, send(), pull() and
// the status accessors. Nothing queued outlives the core: stop() joins.
// Names are identity (v1's assumption, kept): two members with the same
// display name tint the same and dedup as one - a toy-room tradeoff.
#pragma once

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
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

namespace chatter {

// std::byte <-> text, the netdemo/netchat translation pair.
inline std::span<const std::byte> to_bytes(std::string_view text) {
  return std::as_bytes(std::span<const char>{text.data(), text.size()});
}

inline std::string_view as_text(std::span<const std::byte> data) {
  if (data.empty()) {
    return {};
  }
  return {reinterpret_cast<const char*>(data.data()), data.size()};
}

// One chat line as the gui sees it. `name` is the sender ("you" for own
// sends, the attributed name for relayed FROMs); system lines (joins,
// leaves, the banner, SYS events) render dim and never show a name.
struct ChatLine {
  std::string name;
  std::string text;
  bool system = false;
};

// What a connection is to this copy. The kind picks the parser and the
// close behavior; see the chat_core.hpp header for the frame grammar.
enum class Kind {
  lan,      // discovery-paired direct peer (v1's only kind)
  to_relay, // client side: our link up to a room
  member,   // serve side: a member of the room we host
};

// One live connection plus the name its HELLO carried - netchat's Peer,
// extended for the room. The vector in ChatCore's scope holds the
// ownership; nothing ever captures a connection Ptr by value inside that
// connection's own stored callbacks.
struct Peer {
  ws::net::TcpConnection::Ptr conn;
  std::shared_ptr<std::string> name;  // "?" until HELLO lands
  Kind kind = Kind::lan;
  std::string room_key;  // to_relay: the endpoint this link answers to
};

class ChatCore {
 public:
  // What start() is asked to bring up. Defaults keep v1: a plain LAN
  // chatter with a derived name. serve_port > 0 turns this copy into a
  // room host (announcing/browsing/pairing all off - the room is for
  // peers that will never see a beat); relays then must be empty.
  struct Options {
    std::string name;                     // "" -> derived from the port
    std::vector<ws::net::Address> relays;  // client: dial these rooms
    int serve_port = 0;                   // >0: host a room on this port
  };

  ChatCore() = default;
  ~ChatCore() { stop(); }  // no-op when already stopped; joins first
  ChatCore(const ChatCore&) = delete;
  ChatCore& operator=(const ChatCore&) = delete;

  // Brings up the listener (the room's, or an ephemeral one for pairing)
  // then the v1 discovery stack (client mode only), dials every relay,
  // and starts the net thread.
  void start(Options options) {
    serving_ = options.serve_port > 0;
    const std::uint16_t listen_port =
        serving_ ? static_cast<std::uint16_t>(options.serve_port) : 0;
    auto listener_r =
        ws::net::TcpListener::listen(loop_, ws::net::Address::any(listen_port));
    if (listener_r) {
      listener_ = *listener_r;
      // Any-bind caveat: local() reports 0.0.0.0 - bind-all, a port to
      // READ, never a destination. Only the port is real.
      port_ = static_cast<int>(listener_->local().port);
    } else {
      ws::core::log_error(listener_r.error());
      // No listener: a client still browses (a feed), a host of an
      // unopenable room is dead weight but runs - the failure is on the
      // screen via the status, same lenience as v1.
    }
    name_ = !options.name.empty()
                ? options.name
                : (serving_ ? std::string{"relay-"} : std::string{"chatter-"}) +
                      std::to_string(port_);

    if (listener_) {
      listener_->set_on_connection([this](ws::net::TcpConnection::Ptr conn) {
        adopt(std::move(conn), serving_ ? Kind::member : Kind::lan, {});
      });
    }

    if (!serving_) {
      auto browser_r = ws::net::DiscoveryBrowser::browse(loop_);
      if (browser_r) {
        browser_ = *browser_r;
        browser_->set_on_peer(
            [this](std::span<const std::byte> payload, ws::net::Address from) {
              on_beat(payload, from);
            });
      } else {
        ws::core::log_error(browser_r.error());
      }
      announce_beats();
      for (const ws::net::Address& room : options.relays) {
        relays_.push_back(room);
      }
    }
    // else: a host is not a LAN chatter. No beats out (nothing may pair
    // with it directly - that is the point, it exists for the far side),
    // no browser (nothing to dial), no guesting elsewhere while serving.
    // Its own feed below is the operator's.

    push_line({name_,
               serving_ ? "serving on port " + std::to_string(port_)
                        : "here (port " + std::to_string(port_) + ")",
               true});
    for (const ws::net::Address& room : relays_) {
      push_line({name_,
                 "relay link: " + room.host + ":" +
                     std::to_string(room.port),
                 true});
    }

    running_.store(true);
    thread_ = std::thread([this]() {
      const ws::core::Status ran = loop_.run();
      if (!ran) {
        ws::core::log_error(ran.error());
      }
      running_.store(false);
    });

    // Room dials ride post() onto the loop: connect() is loop-thread-only,
    // and the net thread is already inside run() by the time we get here.
    // post() is the one thread-safe call - the same bridge every tap uses.
    for (const ws::net::Address& room : relays_) {
      (void)loop_.post([this, room]() { dial_room(room); });
    }
  }

  // Gui thread -> the loop. A tap posts one SAY broadcast to every live
  // peer (Lan pairs, rooms, members alike - a host speaks as everyone
  // else does); the sent line echoes into the queue from the loop thread
  // exactly like received lines, so ordering is the wire's ordering.
  void send(std::string_view line) {
    if (!running_.load() || line.empty()) {
      return;
    }
    std::string outgoing = "SAY " + std::string{line};
    (void)loop_.post([this, outgoing = std::move(outgoing)]() {
      push_line({"you",
                 std::string{std::string_view{outgoing}.substr(4)}, false});
      for (const Peer& p : peers_) {
        (void)p.conn->send(to_bytes(outgoing));
      }
    });
  }

  // Net thread -> gui: swaps out every line queued since the last pull.
  std::vector<ChatLine> pull() {
    std::vector<ChatLine> out;
    {
      const std::lock_guard<std::mutex> lock{mtx_};
      out.swap(pending_);
    }
    return out;
  }

  // Posts the shutdown - every live connection closed in flight (on_closed
  // runs inside it and must not book redials or farewells: stopping_ is
  // set first), then the listener, the announcers, the browser; run()
  // drains and returns - and joins the net thread. Safe to call twice;
  // the dtor calls it defensively.
  void stop() {
    if (!thread_.joinable()) {
      return;  // never started, or already stopped
    }
    if (running_.load()) {
      (void)loop_.post([this]() {
        stopping_ = true;
        // on_closed fires during these closes and erases peers_, so
        // iterate a snapshot.
        const std::vector<Peer> snapshot = peers_;
        for (const Peer& p : snapshot) {
          (void)p.conn->close();
        }
        if (listener_) {
          listener_->close();
        }
        if (lan_announcer_) {
          lan_announcer_->stop();
        }
        if (loopback_announcer_) {
          loopback_announcer_->stop();
        }
        if (browser_) {
          browser_->stop();
        }
      });
    }
    thread_.join();
  }

  int port() const { return port_; }
  const std::string& name() const { return name_; }
  int peers() const { return live_.load(); }
  bool serving() const { return serving_; }

 private:
  // Everything a fresh connection needs, every kind: HELLO on connect
  // (a room greets its members, a client greets the room, Lan pairs greet
  // each other), the kind's parser after, bookkeeping on close.
  void adopt(const ws::net::TcpConnection::Ptr& conn, Kind kind,
             std::string room_key) {
    const auto tag = std::make_shared<std::string>("?");
    ws::net::TcpConnection* self = conn.get();
    // The close callback's copy, taken before the Peer's move empties
    // room_key - the to_relay case schedules its redial through it.
    const std::string key = room_key;
    peers_.push_back({conn, tag, kind, std::move(room_key)});
    live_.fetch_add(1);
    conn->set_on_connected([self, this]() {
      (void)self->send(to_bytes("HELLO " + name_));
    });
    conn->set_on_message([tag, kind, self, this](
                              std::span<const std::byte> message) {
      std::string_view text = as_text(message);
      switch (kind) {
        case Kind::lan:
          parse_lan(text, tag);
          return;
        case Kind::to_relay:
          parse_room_link(text, tag);
          return;
        case Kind::member:
          parse_member(text, tag, self);
          return;
      }
    });
    conn->set_on_closed([tag, kind, self, this, key](ws::net::NetError why) {
      switch (kind) {
        case Kind::lan: {
          if (*tag != "?") {
            forget_direct(*tag);
            push_line({*tag,
                       std::string{"left ("}
                           .append(ws::net::describe(why))
                           .append(")"),
                       true});
          }
          std::erase_if(peers_,
                        [self](const Peer& p) { return p.conn.get() == self; });
          std::erase_if(
              dialed_,
              [self](const std::pair<const std::string,
                                     ws::net::TcpConnection*>& kv) {
                return kv.second == self;
              });
          break;
        }
        case Kind::to_relay: {
          // The link died; the room is still out there. Say it, book the
          // redial, and drop the dead Peer before it can be dialled onto.
          push_line({"",
                     std::string{"room lost ("}
                         .append(ws::net::describe(why))
                         .append("), retrying"),
                     true});
          schedule_room_retry(key);
          std::erase_if(peers_,
                        [self](const Peer& p) { return p.conn.get() == self; });
          break;
        }
        case Kind::member: {
          if (*tag != "?") {
            push_line({*tag,
                       std::string{"left ("}
                           .append(ws::net::describe(why))
                           .append(")"),
                       true});
            room_broadcast("SYS " + *tag + " left", self);
          }
          std::erase_if(peers_,
                        [self](const Peer& p) { return p.conn.get() == self; });
          break;
        }
      }
      live_.fetch_sub(1);
    });
  }

  // v1's parser, verbatim, plus the direct-name bookkeeping the relay's
  // duplicate rule reads.
  void parse_lan(std::string_view text,
                 const std::shared_ptr<std::string>& tag) {
    if (text.starts_with("HELLO ")) {
      text.remove_prefix(6);
      const bool first = *tag == "?";
      *tag = std::string{text};  // copy: the span dies with the callback
      if (first) {
        // One count per HELLO that ever lands: a re-greet must not
        // double the entry.
        ++direct_names_[*tag];
      }
      push_line({*tag, "joined", true});
      return;
    }
    // SAY is this app's chat wrapper. Anything else still shows as-is.
    const std::string_view body =
        text.starts_with("SAY ") ? text.substr(4) : text;
    push_line({*tag, std::string{body}, false});
  }

  // Our link up to a room: HELLO names the room's operator (that is who
  // a SAY from here is), SYS is a room event, FROM is a member speaking.
  void parse_room_link(std::string_view text,
                       const std::shared_ptr<std::string>& tag) {
    if (text.starts_with("HELLO ")) {
      text.remove_prefix(6);
      *tag = std::string{text};
      push_line({*tag, "room linked", true});
      return;
    }
    if (text.starts_with("SYS ")) {
      text.remove_prefix(4);
      push_line({"", std::string{text}, true});
      return;
    }
    if (text.starts_with("FROM ")) {
      text.remove_prefix(5);
      const std::size_t space = text.find(' ');
      if (space == std::string_view::npos) {
        return;  // a malformed frame a room never sends; drop it quiet
      }
      const std::string sender{text.substr(0, space)};
      if (direct_names_.count(sender) != 0) {
        // I have this sender direct: the SAY already came over the Lan
        // pair, this relayed copy would be the second showing. Drop.
        return;
      }
      push_line({sender, std::string{text.substr(space + 1)}, false});
      return;
    }
    // SAY straight from the room is the operator speaking (this link's
    // tag); anything else shows as-is, v1's lenient tail.
    const std::string_view body =
        text.starts_with("SAY ") ? text.substr(4) : text;
    push_line({*tag, std::string{body}, false});
  }

  // A member of the room we host: HELLO names it, SAY is its chat (shown
  // here attributed, forwarded to the room as FROM), and the farewells.
  void parse_member(std::string_view text,
                    const std::shared_ptr<std::string>& tag,
                    ws::net::TcpConnection* self) {
    if (text.starts_with("HELLO ")) {
      text.remove_prefix(6);
      *tag = std::string{text};
      push_line({*tag, "joined", true});
      room_broadcast("SYS " + *tag + " joined", self);
      // The roster in one line: the operator (us) first, then members.
      std::string roster = name_;
      for (const Peer& p : peers_) {
        if (p.kind == Kind::member && p.conn.get() != self &&
            *p.name != "?") {
          roster += ", " + *p.name;
        }
      }
      (void)self->send(to_bytes("SYS in the room: " + roster));
      return;
    }
    // SAY: show it here under the member's own name, then attribute it
    // on the wire for everyone else. A member that speaks before its
    // HELLO ("?") is shown but not forwarded - a nameless FROM helps
    // nobody.
    if (text.starts_with("SAY ")) {
      text.remove_prefix(4);
      if (*tag != "?") {
        room_broadcast("FROM " + *tag + " " + std::string{text}, self);
      }
      push_line({*tag, std::string{text}, false});
      return;
    }
    push_line({*tag, std::string{text}, false});  // as-is, v1's lenient tail
  }

  // Every member except `except` (and nobody at all once stopping - no
  // farewells for a room that is being torn down).
  void room_broadcast(const std::string& frame,
                       const ws::net::TcpConnection* except) {
    if (stopping_) {
      return;
    }
    for (const Peer& p : peers_) {
      if (p.kind == Kind::member && p.conn.get() != except) {
        (void)p.conn->send(to_bytes(frame));
      }
    }
  }

  // -- the room link, client side --------------------------------------

  static std::string endpoint_key(const ws::net::Address& addr) {
    return addr.host + ":" + std::to_string(addr.port);
  }

  // One dial attempt at a room. Failures and drops both funnel into
  // schedule_room_retry; the 5 s deadline keeps a dead host from holding
  // a half-open connect (the net patch's connect deadline - without it a
  // SYN into a void sits there until the OS gives up).
  void dial_room(const ws::net::Address& room) {
    if (stopping_) {
      return;
    }
    const std::string key = endpoint_key(room);
    if (room_waiting_.count(key) != 0) {
      return;  // a retry is already booked for this endpoint
    }
    auto conn_r = ws::net::TcpConnection::connect(loop_, room);
    if (!conn_r) {
      ws::core::log_error(conn_r.error());
      schedule_room_retry(key);
      return;
    }
    adopt(*conn_r, Kind::to_relay, key);
  }

  // 2 s per endpoint, one timer in flight per endpoint at most - the
  // room_waiting_ set is the "already booked" guard both callers check.
  void schedule_room_retry(const std::string& key) {
    if (stopping_ || key.empty()) {
      return;
    }
    if (room_waiting_.count(key) != 0) {
      return;
    }
    room_waiting_.insert(key);
    (void)loop_.start_timer(2000, [this, key]() {
      room_waiting_.erase(key);
      for (const ws::net::Address& room : relays_) {
        if (endpoint_key(room) == key) {
          dial_room(room);
          return;
        }
      }
    });
  }

  // One direct-name uncount: --, erase at zero. (The drop rule reads
  // "count != 0", so a name two peers share stays live until both go.)
  void forget_direct(const std::string& tag) {
    const auto it = direct_names_.find(tag);
    if (it == direct_names_.end()) {
      return;
    }
    if (--it->second <= 0) {
      direct_names_.erase(it);
    }
  }

  // The v1 beat stack, unchanged: the announcer pair and (via
  // set_on_peer in start) on_beat's pairing. Kept as its own block so
  // the serve path in start() reads as one line.
  void announce_beats() {
    if (!listener_) {
      return;
    }
    // The beat says "netchat", not "chatter": the prefix is the
    // protocol's app tag, and sharing it is what lets chatter and the
    // terminal netchat find each other.
    const std::string beat = "netchat " + std::to_string(port_) + " " + name_;
    auto lan_r = ws::net::DiscoveryAnnouncer::announce(
        loop_, to_bytes(beat),
        ws::net::Address::broadcast(ws::net::kDefaultDiscoveryPort), 500);
    if (lan_r) {
      lan_announcer_ = *lan_r;
    } else {
      ws::core::log_error(lan_r.error());
    }
    // The same-machine rendezvous: Windows does not loop 255.255.255.255
    // back to the sender's own listeners, but 127.255.255.255 is
    // loopback's directed broadcast - it stays on this machine and
    // reaches every browser bound here. The library already sets
    // SO_BROADCAST on every UDP socket, so no special arrangement.
    auto loopback_r = ws::net::DiscoveryAnnouncer::announce(
        loop_, to_bytes(beat),
        ws::net::Address{"127.255.255.255", ws::net::kDefaultDiscoveryPort},
        500);
    if (loopback_r) {
      loopback_announcer_ = *loopback_r;
    } else {
      ws::core::log_error(loopback_r.error());
    }
  }

  // The beat parser: v1's, verbatim, reading this copy's port from
  // port_ instead of a captured local.
  void on_beat(std::span<const std::byte> payload, ws::net::Address from) {
    if (!listener_) {
      return;  // nothing to be dialled into; announcing was skipped too
    }
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
    if (port == port_) {
      return;  // my own beat, looped back to me
    }
    // First sight of a peer's port - the one log a quiet run ever prints
    // (joins and errors say the rest): that discovery itself is alive,
    // and where the beat came from. Beats repeat every 500 ms; this
    // fires once per port per process.
    if (noticed_.insert(std::string{port_s}).second) {
      ws::core::info("chatter: beat - {} (port {}) via {}",
                     text.substr(space + 1), port_s, from.host);
    }
    if (dialed_.count(std::string{port_s}) != 0) {
      return;  // already dialing, or talking, to this announced port
    }
    if (port_ >= port) {
      answer_beat(from);  // teach a one-way-deaf peer this copy exists
      return;  // the smaller listen port dials: one connection per pair
    }
    auto conn_r = ws::net::TcpConnection::connect(
        loop_,
        ws::net::Address{from.host, static_cast<std::uint16_t>(port)});
    if (!conn_r) {
      ws::core::log_error(conn_r.error());  // the next beat retries
      return;
    }
    ws::net::TcpConnection::Ptr conn = *conn_r;
    dialed_[std::string{port_s}] = conn.get();
    adopt(conn, Kind::lan, {});
  }

  // The answer a heard beat earns when this copy is not the dialer: one
  // unicast copy of our own beat, straight back at the beat's source -
  // the announcer's own socket, off-cadence. A LAN hop can carry
  // broadcasts one way only (the wired PC here has never once received
  // the phone's Wi-Fi beats), so the peer on the far side may never
  // learn this copy exists; the unicast answer crosses that hop, and
  // the port rule on its side ends up holding both candidates.
  // Throttled per host - beats keep arriving every 500ms and each would
  // earn a fresh answer otherwise.
  void answer_beat(const ws::net::Address& from) {
    if (!lan_announcer_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    auto [it, fresh] = answered_.try_emplace(from.host, now);
    if (!fresh && now - it->second < std::chrono::seconds{2}) {
      return;
    }
    it->second = now;
    if (fresh) {
      ws::core::info("chatter: answered {} with a unicast beat", from.host);
    }
    const ws::core::Status sent = lan_announcer_->send_once(
        ws::net::Address{from.host, ws::net::kDefaultDiscoveryPort});
    if (!sent) {
      // The cadence beats stay best-effort-silent by the lib's design,
      // but the answer is the one datagram pairing can hinge on.
      ws::core::log_error(sent.error());
    }
  }

  // Queue one line for the gui (called on the net thread).
  void push_line(ChatLine line) {
    const std::lock_guard<std::mutex> lock{mtx_};
    pending_.push_back(std::move(line));
    if (pending_.size() > kQueuedLines) {
      pending_.erase(pending_.begin(),
                     pending_.end() -
                         static_cast<std::ptrdiff_t>(kQueuedLines));
    }
  }

  static constexpr std::size_t kQueuedLines = 256;

  // Declaration order matters: loop_ is declared first so it destructs
  // LAST - after the thread (which stop() has already joined) and after
  // every handle Ptr, the same order netchat's main-scope declarations
  // give. The thread is declared last, so it destructs first, joined.
  ws::net::EventLoop loop_;
  ws::net::TcpListener::Ptr listener_;
  ws::net::DiscoveryBrowser::Ptr browser_;
  ws::net::DiscoveryAnnouncer::Ptr lan_announcer_;
  ws::net::DiscoveryAnnouncer::Ptr loopback_announcer_;

  std::vector<Peer> peers_;  // every live connection (loop thread only)
  // Rooms we guest in (loop thread only; written once in start()).
  std::vector<ws::net::Address> relays_;
  // Endpoints with a redial timer in flight (loop thread only) - the
  // "already booked" guard dial_room/schedule_room_retry share.
  std::set<std::string> room_waiting_;
  // Announced ports this copy has dialed, and the connection each got
  // (loop thread only). Keyed by the announced port alone: a peer is its
  // listen port in this protocol. on_closed clears the entry, so a beat
  // that keeps arriving can re-dial a peer that came back.
  std::map<std::string, ws::net::TcpConnection*> dialed_;
  // Last unicast answer time per source host (loop thread only).
  std::map<std::string, std::chrono::steady_clock::time_point> answered_;
  // Peer ports this copy has already logged a beat line for (loop
  // thread only) - on_beat's once-per-port discovery log.
  std::set<std::string> noticed_;
  // Names we hold as direct Lan peers, refcounted (loop thread only) -
  // the relay drop rule reads this. Two peers sharing a name keep the
  // entry alive until the last one leaves (forget_direct).
  std::map<std::string, int> direct_names_;
  // Set at the top of the posted shutdown, before any close fires
  // on_closed (loop thread only): no redials, no room farewells.
  bool stopping_ = false;

  // Written once in start(), before the net thread exists - read-only to
  // both threads after, no lock needed.
  std::string name_;
  int port_ = 0;
  bool serving_ = false;

  std::atomic<int> live_{0};          // connections up, for the status line
  std::atomic<bool> running_{false};  // run() has not returned yet

  mutable std::mutex mtx_;         // guards pending_ only
  std::vector<ChatLine> pending_;  // loop thread writes, gui drains
  std::thread thread_;             // the net thread; joined in stop()
};

}  // namespace chatter
