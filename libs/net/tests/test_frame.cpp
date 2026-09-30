// The framing core, socket-free: wire format, partial feeds, caps, the
// compact accounting, the deliberate span-misuse demonstration, and a
// bounded fuzz pass. "core/frame.hpp" is internal on purpose (the ui
// pattern: tests reach src/ through the PRIVATE include path).

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "core/frame.hpp"
#include "frame_fuzz.hpp"

namespace {

using ws::net::detail::FrameParser;
using ws::net::detail::FrameStatus;

std::vector<std::byte> text(std::string_view s) {
  const auto* raw = reinterpret_cast<const std::byte*>(s.data());
  return std::vector<std::byte>(raw, raw + s.size());
}

bool same(std::span<const std::byte> got, std::string_view want) {
  return got.size() == want.size() &&
         (want.empty() ||
          std::memcmp(got.data(), want.data(), want.size()) == 0);
}

unsigned at(std::span<const std::byte> s, std::size_t i) {
  return std::to_integer<unsigned>(s[i]);
}

}  // namespace

TEST_CASE("wire format: 4-byte big-endian length + payload") {
  std::vector<std::byte> wire;
  ws::net::detail::append_frame(wire, text("AB"));
  REQUIRE(wire.size() == 6);
  CHECK(at(wire, 0) == 0);
  CHECK(at(wire, 1) == 0);
  CHECK(at(wire, 2) == 0);
  CHECK(at(wire, 3) == 2);
  CHECK(at(wire, 4) == 'A');
  CHECK(at(wire, 5) == 'B');

  // A 258-byte frame pins the multi-byte prefix order.
  const std::vector<std::byte> big(258, std::byte{7});
  ws::net::detail::append_frame(wire, big);
  CHECK(wire.size() == 6 + 4 + 258);
  CHECK(at(wire, 6) == 0);
  CHECK(at(wire, 7) == 0);
  CHECK(at(wire, 8) == 1);
  CHECK(at(wire, 9) == 2);
}

TEST_CASE("round trip: one message and the empty payload") {
  FrameParser parser;
  std::vector<std::byte> wire;
  ws::net::detail::append_frame(wire, text("hello"));
  parser.append(wire);
  const auto r = parser.next();
  REQUIRE(r.status == FrameStatus::message);
  CHECK(same(r.message, "hello"));
  CHECK(parser.buffered() == 0);
  CHECK(parser.next().status == FrameStatus::need_more);

  std::vector<std::byte> empty_wire;
  ws::net::detail::append_frame(empty_wire, {});
  REQUIRE(empty_wire.size() == 4);
  parser.append(empty_wire);
  const auto empty = parser.next();
  REQUIRE(empty.status == FrameStatus::message);
  CHECK(empty.message.empty());
}

TEST_CASE("byte-by-byte feed stays need_more until the last byte") {
  FrameParser parser;
  std::vector<std::byte> wire;
  ws::net::detail::append_frame(wire, text("abcdef"));
  for (std::size_t i = 0; i + 1 < wire.size(); ++i) {
    parser.append(std::span<const std::byte>{wire.data() + i, 1});
    CHECK(parser.next().status == FrameStatus::need_more);
  }
  parser.append(std::span<const std::byte>{wire.data() + wire.size() - 1, 1});
  const auto r = parser.next();
  REQUIRE(r.status == FrameStatus::message);
  CHECK(same(r.message, "abcdef"));
  CHECK(parser.buffered() == 0);
}

TEST_CASE("messages appended together drain in order") {
  FrameParser parser;
  std::vector<std::byte> wire;
  const std::vector<std::byte> hundred(100, std::byte{42});
  ws::net::detail::append_frame(wire, text("one"));
  ws::net::detail::append_frame(wire, {});  // empty mid-stream
  ws::net::detail::append_frame(wire, hundred);
  ws::net::detail::append_frame(wire, text("four"));
  parser.append(wire);

  const auto a = parser.next();
  REQUIRE(a.status == FrameStatus::message);
  CHECK(same(a.message, "one"));
  const auto b = parser.next();
  REQUIRE(b.status == FrameStatus::message);
  CHECK(b.message.empty());
  const auto c = parser.next();
  REQUIRE(c.status == FrameStatus::message);
  REQUIRE(c.message.size() == 100);
  CHECK(std::to_integer<unsigned>(c.message[37]) == 42);
  const auto d = parser.next();
  REQUIRE(d.status == FrameStatus::message);
  CHECK(same(d.message, "four"));
  CHECK(parser.buffered() == 0);
  CHECK(parser.next().status == FrameStatus::need_more);
}

TEST_CASE("at-max passes; max+1 is a sticky too_large; cap/reset recover") {
  FrameParser parser(8);
  std::vector<std::byte> wire;
  ws::net::detail::append_frame(wire, std::vector<std::byte>(8, std::byte{1}));
  parser.append(wire);
  const auto ok = parser.next();
  REQUIRE(ok.status == FrameStatus::message);
  CHECK(ok.message.size() == 8);

  std::vector<std::byte> over;
  ws::net::detail::append_frame(over, std::vector<std::byte>(9, std::byte{2}));
  parser.append(over);
  CHECK(parser.next().status == FrameStatus::too_large);
  CHECK(parser.next().status == FrameStatus::too_large);  // sticky under the cap
  CHECK(parser.buffered() == 4 + 9);

  parser.set_max_payload(16);
  const auto recovered = parser.next();  // the same pending frame, now legal
  REQUIRE(recovered.status == FrameStatus::message);
  CHECK(recovered.message.size() == 9);

  parser.reset();
  CHECK(parser.buffered() == 0);
  std::vector<std::byte> fresh;
  ws::net::detail::append_frame(fresh, text("ok"));
  parser.append(fresh);
  const auto again = parser.next();
  REQUIRE(again.status == FrameStatus::message);
  CHECK(same(again.message, "ok"));
}

TEST_CASE("compact drops consumed bytes and never the pending tail") {
  FrameParser parser;
  std::vector<std::byte> wire;
  ws::net::detail::append_frame(wire, text("first"));
  ws::net::detail::append_frame(wire, text("second"));
  parser.append(wire);

  REQUIRE(parser.next().status == FrameStatus::message);  // "first"
  CHECK(parser.buffered() == 4 + 6);
  parser.compact();
  CHECK(parser.buffered() == 4 + 6);  // the tail is intact
  const auto r = parser.next();
  REQUIRE(r.status == FrameStatus::message);
  CHECK(same(r.message, "second"));
  CHECK(parser.buffered() == 0);
  parser.compact();  // fully consumed: empties
  CHECK(parser.buffered() == 0);

  // compact with nothing consumed must not drop unconsumed bytes.
  std::vector<std::byte> partial(3, std::byte{0});  // 3 prefix bytes only
  parser.append(partial);
  parser.compact();  // consumed_ == 0: a no-op
  CHECK(parser.buffered() == 3);
}

TEST_CASE("DO NOT DO THIS: stashing the delivered span outlives its validity") {
  // on_message-style spans point INTO the parser's buffer. They are valid
  // only until the parser moves on. This case demonstrates the contract the
  // only way it can be observed: stash pointer+size (NOT the bytes), let
  // the parser advance, and the payload is gone.
  //   - Debug: the misuse guard scribbles 0xCD over the region right
  //     before the parser reads the next frame, so the stale span reads
  //     garbage instead of the real payload - loud, caught here.
  //   - Release: the poison is compiled out; the stale span silently
  //     aliases the parser's LIVE buffer (or dangles entirely) and
  //     nothing checks it - exactly why user code must never do this.
  // If a callback needs the bytes later, it must copy them.
  FrameParser parser;
  std::vector<std::byte> wire;
  ws::net::detail::append_frame(wire, text("first"));
  ws::net::detail::append_frame(wire, text("second"));
  parser.append(wire);

  const auto first = parser.next();
  REQUIRE(first.status == FrameStatus::message);
  const std::span<const std::byte> stashed = first.message;  // the WRONG thing

  const auto second = parser.next();  // the parser moves on: `stashed` dies
  REQUIRE(second.status == FrameStatus::message);
  CHECK(same(second.message, "second"));

#ifndef NDEBUG
  CAPTURE(stashed.size());  // 5 - the size survived; the bytes did not
  for (const std::byte b : stashed) {
    CHECK(std::to_integer<unsigned>(b) == 0xCD);
  }
#else
  (void)stashed;  // release: nothing to observe - that IS the danger
#endif
}

TEST_CASE("bounded fuzz: the parser matches the reference model") {
  // A few fixed seeds run in-suite; fuzz_net (not a ctest target) runs the
  // same harness for minutes when wanted.
  CHECK(ws::net::fuzz::run_frame_fuzz(0xC0FFEEu, 300) == 0);
  CHECK(ws::net::fuzz::run_frame_fuzz(314159u, 250) == 0);
  CHECK(ws::net::fuzz::run_frame_fuzz(27474u, 250) == 0);
}
