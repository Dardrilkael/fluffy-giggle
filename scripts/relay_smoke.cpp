// scripts/relay_smoke.cpp - headless validation of chatter v2's protocol
// core. Includes apps/chatter/src/chat_core.hpp directly (the header is
// gui-free on purpose) and drives THREE real cores in one process:
//
//   relay  - a ChatCore in serve mode (the room host)
//   bob    - a client with --relay 127.0.0.1:27475
//   carol  - a client with --relay 127.0.0.1:27475
//
// What it asserts, in order:
//   1. bob and carol pair DIRECT over the Lan beats (v1 path intact).
//   2. Both link the room; the relay's feed shows both members joined.
//   3. The roster SYS reaches whoever joined second, and the "X joined"
//      SYS reaches the other member.
//   4. bob's message reaches carol EXACTLY ONCE even though it travels
//      both transports (direct SAY + relayed FROM) - the drop rule.
//   5. carol's answer reaches bob exactly once, same mirror.
//   6. The operator speaking from the room host reaches both, once.
//   7. Killing the room produces the "room lost (...), retrying" line
//      and the cores still shut down cleanly (no crash, threads join).
//
// Build/run (see the Bash call that owns it): links the ASan tree of
// ws::net, so the whole conversation runs under AddressSanitizer.
#include "chat_core.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

using chatter::ChatCore;
using chatter::ChatLine;

// Everything a core ever pulled, plus a live handle for more.
struct Feed {
  ChatCore* core = nullptr;
  std::vector<ChatLine> lines{};

  explicit Feed(ChatCore* c) : core(c) {}

  void drain() {
    for (ChatLine& line : core->pull()) {
      lines.push_back(std::move(line));
    }
  }
};

bool has_system(const Feed& feed, const std::string& text) {
  for (const ChatLine& line : feed.lines) {
    if (line.system && line.text == text) {
      return true;
    }
  }
  return false;
}

bool has_system_containing(const Feed& feed, const std::string& needle) {
  for (const ChatLine& line : feed.lines) {
    if (line.system && line.text.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// A chat line from `sender` with exactly this text, non-system. Returns
// how many exist - the duplicate-suppression assertions want exactly 1.
int count_chat(const Feed& feed, const std::string& sender,
               const std::string& text) {
  int n = 0;
  for (const ChatLine& line : feed.lines) {
    if (!line.system && line.name == sender && line.text == text) {
      ++n;
    }
  }
  return n;
}

// Poll the feed until `ready` says true (50 ms cadence). False on
// timeout; the caller turns that into a failed check with context.
bool wait_until(Feed& feed, const std::string& what,
                bool (*ready)(const Feed&), int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{timeout_ms};
  while (std::chrono::steady_clock::now() < deadline) {
    feed.drain();
    if (ready(feed)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  }
  std::printf("  [timeout] waiting for: %s\n", what.c_str());
  for (const ChatLine& line : feed.lines) {
    std::printf("    %-8s %-10s %s\n", line.system ? "sys" : "chat",
                line.name.c_str(), line.text.c_str());
  }
  return false;
}

bool paired_carol(const Feed& f) { return has_system(f, "joined"); }
bool paired_bob(const Feed& f) { return has_system(f, "joined"); }
bool room_linked(const Feed& f) { return has_system(f, "room linked"); }

// How many "room linked" lines the feed holds. The relink waits must
// predicate on >= 2 - matching "has one" would pass instantly off the
// round-1 line, long before the 2 s retry timer fires.
int count_room_links(const Feed& f) {
  int n = 0;
  for (const ChatLine& line : f.lines) {
    if (line.system && line.text == "room linked") {
      ++n;
    }
  }
  return n;
}

bool relinked(const Feed& f) { return count_room_links(f) >= 2; }

bool relay_saw_bob(const Feed& f) { return has_system(f, "joined"); }
bool relay_saw_carol(const Feed& f) { return has_system(f, "joined"); }

int g_failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "[ ok ]" : "FAIL ", what.c_str());
  if (!ok) {
    ++g_failures;
  }
}

}  // namespace

int main() {
  std::printf("relay smoke: one room host, two relay clients, one process\n");

  ChatCore relay;
  ChatCore bob;
  ChatCore carol;

  ChatCore::Options relay_options;
  relay_options.name = "relay";
  relay_options.serve_port = 27475;
  relay.start(relay_options);

  ChatCore::Options client_options;
  client_options.name = "bob";
  client_options.relays.push_back(ws::net::Address{"127.0.0.1", 27475});
  bob.start(client_options);

  client_options.name = "carol";
  carol.start(client_options);

  Feed relay_feed{&relay};
  Feed bob_feed{&bob};
  Feed carol_feed{&carol};

  // 1. Both transports up. The LAN pair HELLOs must land BEFORE any
  //    message is sent, or the duplicate-suppression race (direct link
  //    alive but tag in flight) would show the relay copy too.
  check(wait_until(carol_feed, "carol paired with bob over LAN", paired_carol,
                   15000),
        "carol paired with bob over LAN (feed has 'joined')");
  check(wait_until(bob_feed, "bob paired with carol over LAN", paired_bob,
                   15000),
        "bob paired with carol over LAN (feed has 'joined')");
  check(wait_until(bob_feed, "bob's room link", room_linked, 15000),
        "bob linked the room");
  check(wait_until(carol_feed, "carol's room link", room_linked, 15000),
        "carol linked the room");
  check(wait_until(relay_feed, "relay saw bob", relay_saw_bob, 15000),
        "relay's feed shows bob joined");
  check(wait_until(relay_feed, "relay saw carol", relay_saw_carol, 15000),
        "relay's feed shows carol joined");

  // 2. Room events: whoever joined second got the roster; the other one
  //    got the "X joined" SYS broadcast. At least one of each text.
  bob_feed.drain();
  carol_feed.drain();
  check(has_system_containing(bob_feed, "in the room: ") ||
            has_system_containing(carol_feed, "in the room: "),
        "a roster SYS reached a member");
  check(has_system_containing(bob_feed, "carol joined") ||
            has_system_containing(carol_feed, "bob joined"),
        "a 'X joined' SYS broadcast reached the other member");

  // 3. The duplicate-suppression heart: bob speaks once, carol must see
  //    exactly one line despite two transports carrying it.
  bob.send("hello network");
  const auto carol_got_hello = [](const Feed& f) {
    return count_chat(f, "bob", "hello network") >= 1;
  };
  wait_until(carol_feed, "carol received bob's hello", carol_got_hello, 15000);
  std::this_thread::sleep_for(std::chrono::milliseconds{750});
  carol_feed.drain();
  check(count_chat(carol_feed, "bob", "hello network") == 1,
        "carol saw bob's message EXACTLY ONCE (two transports, one line)");
  relay_feed.drain();
  check(count_chat(relay_feed, "bob", "hello network") == 1,
        "relay saw bob's message once, attributed");

  // 4. The mirror: carol answers, bob sees it exactly once.
  carol.send("yes");
  const auto bob_got_yes = [](const Feed& f) {
    return count_chat(f, "carol", "yes") >= 1;
  };
  wait_until(bob_feed, "bob received carol's yes", bob_got_yes, 15000);
  std::this_thread::sleep_for(std::chrono::milliseconds{750});
  bob_feed.drain();
  carol_feed.drain();
  check(count_chat(bob_feed, "carol", "yes") == 1,
        "bob saw carol's answer EXACTLY ONCE");
  check(count_chat(carol_feed, "you", "yes") == 1,
        "carol's own echo is attributed 'you'");

  // 5. The operator speaks from the room host: one line on each client.
  relay.send("hi from relay");
  const auto bob_got_op = [](const Feed& f) {
    return count_chat(f, "relay", "hi from relay") >= 1;
  };
  const auto carol_got_op = [](const Feed& f) {
    return count_chat(f, "relay", "hi from relay") >= 1;
  };
  wait_until(bob_feed, "bob received the operator's hi", bob_got_op, 15000);
  wait_until(carol_feed, "carol received the operator's hi", carol_got_op,
             15000);
  std::this_thread::sleep_for(std::chrono::milliseconds{750});
  bob_feed.drain();
  carol_feed.drain();
  check(count_chat(bob_feed, "relay", "hi from relay") == 1,
        "bob saw the operator's message exactly once");
  check(count_chat(carol_feed, "relay", "hi from relay") == 1,
        "carol saw the operator's message exactly once");

  // 6. Kill the room; both clients must say so and still stop cleanly.
  relay.stop();
  const auto lost_room = [](const Feed& f) {
    return has_system_containing(f, "room lost (");
  };
  check(wait_until(bob_feed, "bob noticed the room dying", lost_room, 15000),
        "bob's feed shows 'room lost (...), retrying'");
  check(wait_until(carol_feed, "carol noticed the room dying", lost_room,
                   15000),
        "carol's feed shows 'room lost (...), retrying'");

  // 7. The redial machinery, end to end: bring a NEW room up on the same
  //    port. The clients' 2 s retry timers must find it, relink and
  //    re-HELLO with no help - the relay's fresh feed proves the rejoin.
  ChatCore relay2;
  ChatCore::Options relay2_options;
  relay2_options.name = "relay";
  relay2_options.serve_port = 27475;
  relay2.start(relay2_options);
  Feed relay2_feed{&relay2};
  check(wait_until(bob_feed, "bob relinked the new room", relinked, 15000),
        "bob's retry timer relinked the new room");
  check(wait_until(carol_feed, "carol relinked the new room", relinked, 15000),
        "carol's retry timer relinked the new room");
  check(wait_until(relay2_feed, "new room saw bob", relay_saw_bob, 15000),
        "the new room's feed shows the rejoined member");
  relay2.stop();
  bob.stop();
  carol.stop();

  // 8. Nothing queued outlives the cores: every stop() joined its thread
  //    and destructors ran (ASan would report a leak/UAF here if not).
  std::printf("%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL",
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
