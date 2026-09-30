#pragma once

// The single-threaded event loop every ws::net object lives on. All
// callbacks fire on the loop thread; post() is the one thread-safe call.
// The backend (libuv today) is behind a pimpl - no uv type ever appears
// here, and swapping the backend changes nothing in this header.
//
// Callback contract: ws::net invokes user callbacks on the loop thread
// and they must not throw - a throw would cross the backend's C boundary
// and terminate the process. Errors reach callbacks as NetError values,
// never exceptions.

#include <cstdint>
#include <functional>
#include <memory>

#include <ws/core/result.hpp>
#include <ws/net/visibility.hpp>

namespace ws::net {

struct TimerHandle {
  std::uint32_t value = 0;  // 0 is never issued
  [[nodiscard]] bool valid() const noexcept { return value != 0; }
};

class WS_NET_API EventLoop {
 public:
  EventLoop();
  ~EventLoop();  // drains: closes live timers/sockets before freeing

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;
  EventLoop(EventLoop&&) = delete;
  EventLoop& operator=(EventLoop&&) = delete;

  // Runs until stop() or until no user work remains. The calling thread
  // IS the loop thread while run() is in flight - every callback fires
  // on it. Work posted before run() executes at run() entry.
  [[nodiscard]] ws::core::Status run();

  // One loop turn (may block until an event completes). Same rules.
  [[nodiscard]] ws::core::Status run_once();

  // Thread-safe, idempotent: makes run()/run_once() return promptly -
  // it wakes a sleeping loop instead of waiting out its next event.
  void stop();

  // THE thread-safe call: wake the loop and run fn on the loop thread,
  // FIFO with earlier posts. Work posted before run() executes at run()
  // entry; posts never keep run() alive on their own.
  [[nodiscard]] ws::core::Status post(std::function<void()> fn);

  // One-shot timer on the loop thread. Repeating = re-arm from the
  // callback. Loop-thread only (or before run()): from any other thread
  // while the loop runs, this is misuse.
  [[nodiscard]] ws::core::Result<TimerHandle> start_timer(
      std::uint64_t delay_ms, std::function<void()> on_fire);

  // The timer will never fire after this. Unknown/already-fired handles
  // are invalid_handle: cancelling a fired timer is not an error, but it
  // does not resurrect it either.
  [[nodiscard]] ws::core::Status cancel_timer(TimerHandle handle);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ws::net
