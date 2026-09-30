#include "core/frame.hpp"

#include <cstring>
#include <limits>

#include <ws/core/assert.hpp>

namespace ws::net::detail {

#ifndef NDEBUG
void debug_poison(std::span<std::byte> region) {
  if (!region.empty()) {
    std::memset(region.data(), 0xCD, region.size());
  }
}
#endif

std::uint32_t read_frame_prefix(const std::byte* at) {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < kFramePrefixBytes; ++i) {
    value = (value << 8) | std::to_integer<std::uint32_t>(at[i]);
  }
  return value;
}

void append_frame(std::vector<std::byte>& out,
                  std::span<const std::byte> payload) {
  WS_CORE_ASSERT(payload.size() <=
                 std::numeric_limits<std::uint32_t>::max());
  const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
  const std::byte prefix[kFramePrefixBytes] = {
      std::byte{static_cast<std::uint8_t>(len >> 24)},
      std::byte{static_cast<std::uint8_t>(len >> 16)},
      std::byte{static_cast<std::uint8_t>(len >> 8)},
      std::byte{static_cast<std::uint8_t>(len)}};
  out.insert(out.end(), std::begin(prefix), std::end(prefix));
  out.insert(out.end(), payload.begin(), payload.end());
}

FrameParser::FrameParser(std::uint32_t max_payload)
    : max_payload_(max_payload) {}

void FrameParser::set_max_payload(std::uint32_t max_payload) {
  max_payload_ = max_payload;
}

void FrameParser::append(std::span<const std::byte> data) {
  invalidate_last_message();
  if (data.empty()) {
    return;
  }
  buffer_.insert(buffer_.end(), data.begin(), data.end());
}

FrameResult FrameParser::next() {
  invalidate_last_message();
  const std::size_t avail = buffer_.size() - consumed_;
  if (avail < kFramePrefixBytes) {
    return {};
  }
  const std::uint32_t len = read_frame_prefix(buffer_.data() + consumed_);
  if (len > max_payload_) {
    return FrameResult{FrameStatus::too_large, {}};
  }
  if (avail < kFramePrefixBytes + len) {
    return {};
  }
  const std::size_t payload_at = consumed_ + kFramePrefixBytes;
  last_offset_ = payload_at;
  last_size_ = len;
  consumed_ = payload_at + len;
  return FrameResult{
      FrameStatus::message,
      std::span<const std::byte>{buffer_.data() + payload_at, len}};
}

void FrameParser::compact() {
  invalidate_last_message();
  if (consumed_ == 0) {
    return;
  }
  buffer_.erase(buffer_.begin(),
                buffer_.begin() + static_cast<std::ptrdiff_t>(consumed_));
  consumed_ = 0;
}

void FrameParser::reset() {
  invalidate_last_message();
  buffer_.clear();
  buffer_.shrink_to_fit();
  consumed_ = 0;
}

void FrameParser::invalidate_last_message() {
#ifndef NDEBUG
  if (last_size_ > 0) {
    WS_CORE_ASSERT(last_offset_ + last_size_ <= buffer_.size());
    debug_poison(
        std::span<std::byte>{buffer_.data() + last_offset_, last_size_});
  }
#endif
  last_offset_ = 0;
  last_size_ = 0;
}

}  // namespace ws::net::detail
