#include "core/connection_logic.hpp"

#include <utility>

#include <ws/core/assert.hpp>

namespace ws::net::detail {

ConnectionLogic::ConnectionLogic(std::unique_ptr<IStreamPeer> peer,
                                 ConnectionEvents events,
                                 std::uint32_t max_message)
    : peer_(std::move(peer)),
      events_(std::move(events)),
      parser_(max_message) {
  WS_CORE_ASSERT(peer_ != nullptr);
  peer_->attach_sink(this);
}

void ConnectionLogic::on_connected() {
  WS_CORE_ASSERT(state_ == State::connecting);
  state_ = State::open;
  if (!pre_connect_.empty()) {
    peer_->write(std::move(pre_connect_));
    pre_connect_.clear();
  }
  if (events_.on_connected) {
    events_.on_connected();
  }
}

void ConnectionLogic::on_bytes(std::span<const std::byte> data) {
  if (state_ == State::closed) {
    return;  // trailing bytes after the stream died: ignore
  }
  parser_.append(data);
  for (;;) {
    if (state_ == State::closed) {
      break;  // a callback closed us mid-drain: on_closed is always last
    }
    const FrameResult frame = parser_.next();
    if (frame.status == FrameStatus::need_more) {
      break;
    }
    if (frame.status == FrameStatus::too_large) {
      // Sticky by design (frame.hpp): a stream claiming an oversized
      // message has no recovery - abort, and say why.
      close_reason_ = NetError::message_too_large;
      peer_->abort();
      return;
    }
    if (events_.on_message) {
      events_.on_message(frame.message);
    }
    // The span is dead from here (the documented rule): poison now
    // rather than at the next parser call.
    parser_.invalidate_last_message();
  }
  parser_.compact();  // drop consumed bytes so the buffer stays bounded
}

void ConnectionLogic::on_stream_dead(NetError reason) {
  if (state_ == State::closed) {
    return;  // already terminal (the peer reports exactly once anyway)
  }
  state_ = State::closed;
  const NetError reported = close_reason_.value_or(reason);
  close_reason_.reset();
  pre_connect_.clear();  // dead weight now; pending_bytes() reports 0
  parser_.reset();
  if (events_.on_closed) {
    events_.on_closed(reported);
  }
}

void ConnectionLogic::start() {
  peer_->start();
}

ws::core::Status ConnectionLogic::send(std::span<const std::byte> payload) {
  if (state_ != State::open && state_ != State::connecting) {
    return fail(NetError::already_closed, "send: connection is not open");
  }
  if (payload.size() > parser_.max_payload()) {
    return fail(NetError::message_too_large,
                "send: payload exceeds the message cap");
  }
  std::vector<std::byte> framed;
  append_frame(framed, payload);
  if (state_ == State::connecting) {
    pre_connect_.insert(pre_connect_.end(), framed.begin(), framed.end());
  } else {
    peer_->write(std::move(framed));
  }
  return {};
}

ws::core::Status ConnectionLogic::close() {
  if (state_ != State::open && state_ != State::connecting) {
    return fail(NetError::already_closed, "close: connection is not open");
  }
  if (state_ == State::connecting) {
    // Nothing established yet: cancel immediately. The peer reports the
    // death; connection_closed is the outcome the user sees (no failure
    // to surface - there are no pending writes that could fail).
    close_reason_ = NetError::connection_closed;
    state_ = State::closing;
    peer_->abort();
    return {};
  }
  state_ = State::closing;
  peer_->finish_and_close();
  return {};
}

void ConnectionLogic::abort() {
  if (state_ == State::closed) {
    return;
  }
  close_reason_ = NetError::connection_reset;  // the user killed it
  state_ = State::closing;
  peer_->abort();
}

bool ConnectionLogic::is_open() const noexcept {
  return state_ == State::open;
}

std::size_t ConnectionLogic::pending_bytes() const noexcept {
  return pre_connect_.size() + peer_->pending();
}

}  // namespace ws::net::detail
