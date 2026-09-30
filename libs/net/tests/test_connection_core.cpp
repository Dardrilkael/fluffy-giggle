// The TCP connection core without a backend: ConnectionLogic driven
// through FakeStreamPeer covers framing, partial reads, graceful-close
// flushing, abrupt disconnects, oversize aborts and the callback-order
// guarantees before any real socket gets involved (N5 adds loopback
// tests over the real thing). Callbacks only set flags - assertions
// live at test level.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/frame.hpp"
#include "core/connection_logic.hpp"
#include "fake_transport.hpp"
#include <ws/net/errors.hpp>

namespace {

using ws::net::NetError;
using ws::net::detail::ConnectionEvents;
using ws::net::detail::ConnectionLogic;
using ws::net::detail::FakeStreamPeer;
using ws::net::detail::append_frame;
using ws::net::detail::kDefaultMaxPayload;

std::vector<std::byte> to_bytes(std::string_view text) {
  std::vector<std::byte> out;
  out.reserve(text.size());
  for (const char c : text) {
    out.push_back(std::byte{static_cast<std::uint8_t>(c)});
  }
  return out;
}

std::string to_text(std::span<const std::byte> data) {
  if (data.empty()) {
    return {};
  }
  return {reinterpret_cast<const char*>(data.data()), data.size()};
}

std::vector<std::byte> framed(std::string_view payload) {
  std::vector<std::byte> out;
  const std::vector<std::byte> body = to_bytes(payload);
  append_frame(out, body);
  return out;
}

std::vector<std::byte> concat(
    std::initializer_list<std::vector<std::byte>> parts) {
  std::vector<std::byte> out;
  for (const auto& part : parts) {
    out.insert(out.end(), part.begin(), part.end());
  }
  return out;
}

std::int32_t code_of(const ws::core::Status& status) {
  return status.error().code;
}

// Wires a ConnectionLogic onto a FakeStreamPeer and records everything
// the callbacks do, in order, for post-hoc assertions.
struct Harness {
  std::vector<std::string> log;
  std::vector<std::string> messages;
  std::int32_t closed_code = -1;
  int closed_count = 0;
  std::span<const std::byte> stashed;  // the span-lifetime probe

  std::unique_ptr<FakeStreamPeer> peer = std::make_unique<FakeStreamPeer>();
  FakeStreamPeer* fake = peer.get();
  std::unique_ptr<ConnectionLogic> logic;

  explicit Harness(std::uint32_t cap = kDefaultMaxPayload) {
    ConnectionEvents events;
    events.on_connected = [this] { log.push_back("connected"); };
    events.on_message = [this](std::span<const std::byte> msg) {
      messages.push_back(to_text(msg));
      log.push_back("msg:" + messages.back());
      stashed = msg;  // last one wins: what the poison probe reads after
    };
    events.on_closed = [this](NetError why) {
      ++closed_count;
      closed_code = static_cast<std::int32_t>(why);
      log.push_back("closed:" + std::to_string(closed_code));
    };
    logic = std::make_unique<ConnectionLogic>(std::move(peer),
                                              std::move(events), cap);
  }
};

}  // namespace

TEST_CASE("connect flushes sends queued before the socket was up") {
  Harness h;
  REQUIRE(h.logic->send(to_bytes("early")).has_value());
  REQUIRE(h.logic->send(to_bytes("second")).has_value());
  CHECK(h.fake->wire().empty());
  CHECK(h.logic->pending_bytes() == std::size_t{(4 + 5) + (4 + 6)});
  h.fake->complete_connect();
  REQUIRE(h.log.at(0) == "connected");
  REQUIRE(h.logic->send(to_bytes("late")).has_value());
  h.fake->pump();
  CHECK(h.fake->wire() ==
        concat({framed("early"), framed("second"), framed("late")}));
  CHECK(h.logic->pending_bytes() == std::size_t{0});
}

TEST_CASE("received frames deliver as messages, split anywhere") {
  Harness h;
  h.fake->complete_connect();
  const std::vector<std::byte> stream =
      concat({framed("hello"), framed(""), framed("a longer one")});
  // feed one byte at a time: partial reads at every boundary
  for (const std::byte b : stream) {
    h.fake->deliver(std::span{&b, 1});
  }
  CHECK(h.messages ==
        std::vector<std::string>{"hello", "", "a longer one"});
}

TEST_CASE("the on_message span dies with the callback (DO NOT DO THIS)") {
  Harness h;
  h.fake->complete_connect();
  h.fake->deliver(framed("poison me"));
  // h.stashed still points into the parser buffer - reading it after the
  // callback returned is exactly what the API forbids. Debug builds
  // scribbled 0xCD over the region the moment the callback returned, so
  // the stale bytes read as poison (release builds only promise garbage;
  // the buffer is still the parser's own allocation either way).
#ifndef NDEBUG
  bool all_poison = !h.stashed.empty();
  for (const std::byte b : h.stashed) {
    all_poison = all_poison && b == std::byte{0xCD};
  }
  CHECK(all_poison);
#else
  (void)h.stashed;
#endif
  CHECK(h.messages == std::vector<std::string>{"poison me"});
}

TEST_CASE("an oversized frame kills the connection with message_too_large") {
  Harness h{8};
  h.fake->complete_connect();
  REQUIRE(h.logic->send(to_bytes("12345678")).has_value());  // at cap: fine
  // the remote claims a 9-byte message
  const std::vector<std::byte> evil{std::byte{0}, std::byte{0}, std::byte{0},
                                    std::byte{9}};
  h.fake->deliver(evil);
  CHECK(h.closed_count == 1);
  CHECK(h.closed_code ==
        static_cast<std::int32_t>(NetError::message_too_large));
  CHECK(!h.logic->is_open());
  // post-mortem: everything refuses, trailing bytes are ignored
  CHECK(code_of(h.logic->send(to_bytes("x"))) ==
        static_cast<std::int32_t>(NetError::already_closed));
  CHECK(code_of(h.logic->close()) ==
        static_cast<std::int32_t>(NetError::already_closed));
  h.fake->deliver(framed("trailing"));  // must neither deliver nor crash
  CHECK(h.messages.empty());
  h.logic->abort();  // no-op after death: no second on_closed
  CHECK(h.closed_count == 1);
}

TEST_CASE("an oversized send is refused, the connection survives") {
  Harness h{8};
  h.fake->complete_connect();
  const auto s = h.logic->send(to_bytes("123456789"));  // 9 > 8
  REQUIRE(!s.has_value());
  CHECK(s.error().code ==
        static_cast<std::int32_t>(NetError::message_too_large));
  CHECK(h.fake->pending() == std::size_t{0});
  CHECK(h.logic->is_open());
  REQUIRE(h.logic->send(to_bytes("ok")).has_value());
  h.fake->pump();
  CHECK(h.fake->wire() == framed("ok"));
}

TEST_CASE("close() flushes pending writes before the connection dies") {
  Harness h;
  h.fake->complete_connect();
  for (int i = 0; i < 3; ++i) {
    REQUIRE(h.logic->send(to_bytes("msg" + std::to_string(i))).has_value());
  }
  REQUIRE(h.logic->close().has_value());
  CHECK(code_of(h.logic->send(to_bytes("too late"))) ==
        static_cast<std::int32_t>(NetError::already_closed));
  CHECK(h.closed_count == 0);  // not yet: writes still pending
  h.fake->pump();              // the queue drains...
  CHECK(h.closed_count == 1);  // ...and only then the close lands
  CHECK(h.closed_code ==
        static_cast<std::int32_t>(NetError::connection_closed));
  CHECK(h.fake->wire().size() == std::size_t{3 * 8});
  CHECK(!h.logic->is_open());
}

TEST_CASE("a write failure during the graceful drain surfaces its error") {
  Harness h;
  h.fake->complete_connect();
  REQUIRE(h.logic->send(to_bytes("doomed")).has_value());
  REQUIRE(h.logic->close().has_value());
  h.fake->simulate_write_failure();  // before the queue ever drains
  CHECK(h.closed_count == 1);
  CHECK(h.closed_code ==
        static_cast<std::int32_t>(NetError::socket_error));
}

TEST_CASE("close() before the socket is up cancels cleanly") {
  Harness h;
  REQUIRE(h.logic->send(to_bytes("never")).has_value());
  REQUIRE(h.logic->close().has_value());
  CHECK(h.closed_count == 1);
  CHECK(h.closed_code ==
        static_cast<std::int32_t>(NetError::connection_closed));
  CHECK(h.fake->wire().empty());
  CHECK(h.logic->pending_bytes() == std::size_t{0});
}

TEST_CASE("an abrupt reset reports connection_reset and ends everything") {
  Harness h;
  h.fake->complete_connect();
  REQUIRE(h.logic->send(to_bytes("lost")).has_value());
  h.fake->simulate_reset();
  CHECK(h.closed_count == 1);
  CHECK(h.closed_code ==
        static_cast<std::int32_t>(NetError::connection_reset));
  CHECK(!h.logic->is_open());
  CHECK(h.fake->wire().empty());  // queued writes died with the stream
  h.fake->deliver(framed("late"));  // trailing bytes: ignored, no crash
  CHECK(h.messages.empty());
}

TEST_CASE("a remote close lets pending writes flush, then ends cleanly") {
  Harness h;
  h.fake->complete_connect();
  REQUIRE(h.logic->send(to_bytes("last words")).has_value());
  h.fake->simulate_remote_eof();
  CHECK(h.closed_count == 0);  // our write is still in flight
  h.fake->pump();
  CHECK(h.closed_count == 1);
  CHECK(h.closed_code ==
        static_cast<std::int32_t>(NetError::connection_closed));
  CHECK(h.fake->wire() == framed("last words"));
}

TEST_CASE("callbacks fire in order and on_closed is exactly once, last") {
  Harness h;
  h.fake->complete_connect();
  h.fake->deliver(framed("one"));
  h.fake->deliver(framed("two"));
  REQUIRE(h.logic->close().has_value());
  h.fake->pump();
  CHECK(!h.logic->close().has_value());   // second close: already_closed
  h.logic->abort();                       // post-mortem abort: no-op
  CHECK(h.closed_count == 1);
  CHECK(h.log == std::vector<std::string>{"connected", "msg:one", "msg:two",
                                          "closed:10"});
}

TEST_CASE("pending_bytes tracks the queue and drains with it") {
  Harness h;
  h.fake->complete_connect();
  for (int i = 0; i < 100; ++i) {
    REQUIRE(h.logic->send(to_bytes("0123456789")).has_value());
  }
  CHECK(h.logic->pending_bytes() == std::size_t{1400});
  h.fake->pump();
  CHECK(h.logic->pending_bytes() == std::size_t{0});
  CHECK(h.fake->wire().size() == std::size_t{1400});
}

TEST_CASE("close() inside on_message ends delivery immediately") {
  auto peer = std::make_unique<FakeStreamPeer>();
  FakeStreamPeer* fake = peer.get();
  std::unique_ptr<ConnectionLogic> logic;
  std::vector<std::string> messages;
  int closed = 0;
  ConnectionEvents events;
  events.on_connected = [] {};
  events.on_message = [&](std::span<const std::byte> msg) {
    messages.push_back(to_text(msg));
    (void)logic->close();  // user code closing mid-delivery
  };
  events.on_closed = [&](NetError) { ++closed; };
  logic =
      std::make_unique<ConnectionLogic>(std::move(peer), std::move(events));
  fake->complete_connect();
  fake->deliver(concat({framed("first"), framed("second")}));
  CHECK(messages == std::vector<std::string>{"first"});
  fake->pump();
  CHECK(closed == 1);
}
