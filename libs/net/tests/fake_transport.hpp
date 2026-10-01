#pragma once

// Test-only fake of the stream seam (core/transport.hpp): a synchronous
// byte-pipe with test controls for latency (pump when you choose),
// failures (simulate_write_failure / simulate_reset / simulate_remote_eof)
// and observation (wire()). No sockets anywhere - this is what makes the
// connection core testable without a backend.

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include "core/transport.hpp"

namespace ws::net::detail {

class FakeStreamPeer final : public IStreamPeer {
 public:
  void attach_sink(IStreamSink* sink) override { sink_ = sink; }

  void start() override {}  // tests drive the fake by hand
  void set_keepalive(std::shared_ptr<void>) override {}

  void write(std::vector<std::byte> bytes) override {
    if (dead_) {
      return;  // inert after death
    }
    outbox_bytes_ += bytes.size();
    outbox_.push_back(std::move(bytes));
  }

  void finish_and_close() override {
    if (dead_) {
      return;
    }
    finishing_ = true;
    if (outbox_.empty()) {
      die(NetError::connection_closed);  // nothing to flush: done now
    }
  }

  void abort() override {
    if (!dead_) {
      die(NetError::connection_reset);
    }
  }

  [[nodiscard]] std::size_t pending() const override {
    return outbox_bytes_;
  }

  // --- test controls ---

  // Move queued writes onto the wire. A finishing peer whose queue just
  // emptied completes its graceful close.
  void pump() {
    if (dead_) {
      return;
    }
    for (auto& chunk : outbox_) {
      wire_.insert(wire_.end(), chunk.begin(), chunk.end());
    }
    outbox_bytes_ = 0;
    outbox_.clear();
    if (finishing_) {
      die(NetError::connection_closed);
    }
  }

  // Remote -> us: deliver bytes straight to the sink.
  void deliver(std::span<const std::byte> data) {
    if (dead_ || data.empty()) {
      return;
    }
    if (sink_) {
      sink_->on_bytes(data);
    }
  }

  void complete_connect() {
    if (!dead_ && sink_) {
      sink_->on_connected();
    }
  }

  // The remote closed its write side cleanly: our pending writes still
  // flush before the stream ends (the symmetric graceful close).
  void simulate_remote_eof() {
    if (dead_) {
      return;
    }
    finishing_ = true;
    if (outbox_.empty()) {
      die(NetError::connection_closed);
    }
  }

  // A queued write blew up (mid-close flush failures included).
  void simulate_write_failure() {
    if (!dead_) {
      die(NetError::socket_error);
    }
  }

  // The remote reset the stream (abrupt disconnect).
  void simulate_reset() {
    if (!dead_) {
      die(NetError::connection_reset);
    }
  }

  [[nodiscard]] bool dead() const noexcept { return dead_; }
  [[nodiscard]] const std::vector<std::byte>& wire() const noexcept {
    return wire_;
  }

 private:
  void die(NetError reason) {
    dead_ = true;
    outbox_.clear();
    outbox_bytes_ = 0;
    if (sink_) {
      sink_->on_stream_dead(reason);  // exactly once, always last
    }
  }

  IStreamSink* sink_ = nullptr;
  std::vector<std::vector<std::byte>> outbox_;
  std::size_t outbox_bytes_ = 0;
  std::vector<std::byte> wire_;  // every byte that made it out
  bool finishing_ = false;
  bool dead_ = false;
};

}  // namespace ws::net::detail
