#pragma once

// Internal (src/ never reaches apps): the length-prefixed TCP framing core.
// Wire format: 4-byte big-endian payload length, then that many payload
// bytes. Platform- and socket-free: the fuzz harness and the FakeStream
// peer drive it without any backend.
//
// This is src/, not include/ - the lib's own TUs reach it through the
// PRIVATE src include path, tests the same way (the ui pattern).

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ws::net::detail {

inline constexpr std::size_t kFramePrefixBytes = 4;

// Default receive cap (1 MiB) - matches TcpConnection's public default.
inline constexpr std::uint32_t kDefaultMaxPayload = 1u << 20;

enum class FrameStatus : std::uint8_t {
  need_more,  // no complete frame buffered yet; keep appending
  message,    // a complete frame is ready; .message points at its payload
  too_large,  // the buffered frame's length prefix exceeds the cap
};

struct FrameResult {
  FrameStatus status = FrameStatus::need_more;
  // Points into the parser's own buffer. Valid only until the next call
  // on the parser - debug builds scribble 0xCD over it as soon as the
  // parser moves on (see debug_poison). Copy the bytes if you need them
  // past your callback; never the span.
  std::span<const std::byte> message{};
};

// Misuse guard: debug builds scribble 0xCD over a region some stale span
// may still reference, so keeping a delivered span past its validity reads
// garbage instead of the real payload. Compiled out in release.
#ifndef NDEBUG
void debug_poison(std::span<std::byte> region);
#else
inline void debug_poison(std::span<std::byte>) noexcept {}
#endif

// Reads the 4-byte big-endian prefix at `at` (>= kFramePrefixBytes present).
[[nodiscard]] std::uint32_t read_frame_prefix(const std::byte* at);

// Appends one framed message (prefix + payload) to `out`. Precondition:
// payload.size() fits a uint32 - callers enforce the receive cap first.
void append_frame(std::vector<std::byte>& out,
                  std::span<const std::byte> payload);

class FrameParser {
 public:
  explicit FrameParser(std::uint32_t max_payload = kDefaultMaxPayload);

  void set_max_payload(std::uint32_t max_payload);
  [[nodiscard]] std::uint32_t max_payload() const noexcept {
    return max_payload_;
  }

  // Copies `data` into the parse buffer. No cap check happens here - one
  // is only meaningful once a full prefix is present (next()).
  void append(std::span<const std::byte> data);

  // Parses one frame. On `message`, the returned span is valid until the
  // NEXT call on the parser; debug builds poison the PREVIOUS message's
  // bytes before parsing this one, so a stashed span reads 0xCD.
  // `too_large` never advances: it stays until reset() or a cap raise
  // (the connection aborts on it).
  FrameResult next();

  // Bytes buffered but not yet consumed (partial prefix/payload included).
  [[nodiscard]] std::size_t buffered() const noexcept {
    return buffer_.size() - consumed_;
  }

  // Drops the consumed prefix of the buffer, keeping the pending tail.
  void compact();

  // Clears everything (too_large recovery / connection abort path).
  void reset();

 private:
  // Debug: poison the last delivered message's region, then forget it.
  // Every public entry point calls this first - "any parser call
  // invalidates the span it handed you" is the whole rule.
  void invalidate_last_message();

  std::vector<std::byte> buffer_;
  std::size_t consumed_ = 0;  // buffer_ offset of the first unconsumed byte
  std::uint32_t max_payload_ = kDefaultMaxPayload;
  std::size_t last_offset_ = 0;  // where the last message's payload lives
  std::size_t last_size_ = 0;
};

}  // namespace ws::net::detail
