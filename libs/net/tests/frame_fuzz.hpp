#pragma once

// Test-local (never in include/): the bounded, deterministic fuzz harness
// for the frame parser. test_frame.cpp runs a few seeds inline; the
// standalone fuzz_net target runs it long with argv-chosen seeds.
// Hand-rolled LCG, not <random>: identical streams on every platform and
// preset, so a failure always reproduces from the seed alone.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "core/frame.hpp"

namespace ws::net::fuzz {

inline std::uint32_t lcg_next(std::uint32_t& state) {
  return state = state * 1664525u + 1013904223u;
}

// One deterministic run; 0 = the parser matched the reference model
// throughout. Distinct nonzero returns keep failures diagnosable from the
// exit code alone.
inline int run_frame_fuzz(std::uint32_t seed, std::uint32_t iterations) {
  std::uint32_t rng = seed;

  // ---- Phase A: real frames, random content, random partial feeds -------
  // The reference model is the list of messages built; the parser must
  // deliver exactly those, in order, byte-identical, regardless of how the
  // stream was chunked - and buffered() must always equal the model's
  // unconsumed byte count, mid-drain compacts included.
  detail::FrameParser parser;
  std::vector<std::byte> stream;  // the current iteration's wire bytes
  std::vector<std::vector<std::byte>> expected;  // every message ever built
  std::size_t delivered = 0;  // messages the parser handed out so far

  for (std::uint32_t it = 0; it < iterations; ++it) {
    const std::uint32_t len = lcg_next(rng) % 97;  // 0..96: empties too
    std::vector<std::byte> message(len);
    for (auto& b : message) {
      b = std::byte{static_cast<std::uint8_t>(lcg_next(rng) >> 24)};
    }
    detail::append_frame(stream, message);
    expected.push_back(std::move(message));

    std::size_t fed = 0;
    std::size_t consumed_model = 0;  // model bytes consumed this iteration
    while (fed < stream.size()) {
      const std::size_t take = std::min<std::size_t>(
          1 + lcg_next(rng) % 17, stream.size() - fed);
      parser.append(std::span<const std::byte>{stream.data() + fed, take});
      fed += take;
      for (;;) {
        const detail::FrameResult r = parser.next();
        if (r.status == detail::FrameStatus::need_more) {
          break;
        }
        if (r.status == detail::FrameStatus::too_large) {
          return 1;  // payloads stay far below the default cap
        }
        if (delivered >= expected.size()) {
          return 2;  // more messages than the model built
        }
        const std::vector<std::byte>& want = expected[delivered++];
        if (r.message.size() != want.size()) {
          return 3;
        }
        if (!want.empty() &&
            std::memcmp(r.message.data(), want.data(), want.size()) != 0) {
          return 4;
        }
        consumed_model += detail::kFramePrefixBytes + want.size();
      }
      if (parser.buffered() != fed - consumed_model) {
        return 5;  // buffered() drifted from the model accounting
      }
      if (lcg_next(rng) % 7 == 0) {
        parser.compact();  // mid-drain compacts: offsets must survive
      }
    }
    if (parser.buffered() != 0) {
      return 6;  // the iteration's stream is fully parsed
    }
    stream.clear();
  }

  // ---- Phase B: pure garbage -------------------------------------------
  // A u32 length prefix built from random bytes dwarfs any sane cap: the
  // parser must flag too_large on it, stay sticky on a repeat call, and
  // reset() must leave it usable again. The first byte is forced to 0xFF
  // so the prefix is guaranteed oversized for every seed, not merely
  // overwhelmingly likely.
  detail::FrameParser garbage_parser(64);
  std::vector<std::byte> garbage(64);
  for (auto& b : garbage) {
    b = std::byte{static_cast<std::uint8_t>(lcg_next(rng) >> 24)};
  }
  garbage.front() = std::byte{0xFF};
  garbage_parser.append(garbage);
  if (garbage_parser.next().status != detail::FrameStatus::too_large) {
    return 7;
  }
  if (garbage_parser.next().status != detail::FrameStatus::too_large) {
    return 8;  // too_large must be sticky (it never advances)
  }
  garbage_parser.reset();
  std::vector<std::byte> ok_wire;
  detail::append_frame(ok_wire, std::span<const std::byte>{garbage.data(), 5});
  garbage_parser.append(ok_wire);
  if (garbage_parser.next().status != detail::FrameStatus::message) {
    return 9;  // reset must recover the parser
  }
  return 0;
}

}  // namespace ws::net::fuzz
