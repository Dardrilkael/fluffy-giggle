#include <ws/net/event_loop.hpp>

#include <atomic>
#include <memory>
#include <utility>

#include "core/transport.hpp"
#include <ws/net/errors.hpp>

namespace ws::net {

struct EventLoop::Impl {
  std::unique_ptr<detail::ILoopBackend> backend = detail::make_loop_backend();
  std::atomic<bool> running{false};
};

EventLoop::EventLoop() : impl_(std::make_unique<Impl>()) {}

EventLoop::~EventLoop() = default;  // Impl dtor drains the backend

detail::ILoopBackend* EventLoop::backend() const {
  return impl_->backend.get();
}

ws::core::Status EventLoop::run() {
  if (impl_->backend->on_loop_thread()) {
    return fail(NetError::loop_stopped, "run() is not re-entrant");
  }
  bool expected = false;
  if (!impl_->running.compare_exchange_strong(expected, true)) {
    return fail(NetError::loop_stopped, "run() is already running");
  }
  impl_->backend->run();
  impl_->running.store(false);
  return {};
}

ws::core::Status EventLoop::run_once() {
  if (impl_->backend->on_loop_thread()) {
    return fail(NetError::loop_stopped, "run_once() is not re-entrant");
  }
  bool expected = false;
  if (!impl_->running.compare_exchange_strong(expected, true)) {
    return fail(NetError::loop_stopped, "run_once() is already running");
  }
  impl_->backend->run_once();
  impl_->running.store(false);
  return {};
}

void EventLoop::stop() { impl_->backend->stop(); }

ws::core::Status EventLoop::post(std::function<void()> fn) {
  if (!fn) {
    return fail(NetError::invalid_argument, "post: null callback");
  }
  impl_->backend->post(std::move(fn));
  return {};
}

ws::core::Result<TimerHandle> EventLoop::start_timer(
    std::uint64_t delay_ms, std::function<void()> on_fire) {
  if (!on_fire) {
    return fail(NetError::invalid_argument, "start_timer: null callback");
  }
  const std::uint32_t id =
      impl_->backend->start_timer(delay_ms, std::move(on_fire));
  if (id == 0) {
    return fail(NetError::socket_error, "timer init failed");
  }
  return TimerHandle{id};
}

ws::core::Status EventLoop::cancel_timer(TimerHandle handle) {
  if (!handle.valid()) {
    return fail(NetError::invalid_argument, "cancel_timer: null handle");
  }
  if (!impl_->backend->cancel_timer(handle.value)) {
    return fail(NetError::invalid_handle);
  }
  return {};
}

}  // namespace ws::net
