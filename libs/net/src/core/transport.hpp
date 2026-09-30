#pragma once

// Internal: the backend seam (the swappable-backend contract). Core logic
// and the test fakes talk ONLY to these interfaces; src/backend/uv is the
// sole implementation today and the only place uv.h exists. A future
// WSAPoll/epoll backend implements the same interface and nothing above
// src/ changes - no public header, no user code.

#include <cstdint>
#include <functional>
#include <memory>

namespace ws::net::detail {

class ILoopBackend {
 public:
  virtual ~ILoopBackend() = default;

  // Thread-safe by contract - the ONE call allowed from other threads.
  // Wakes the loop and runs fn on the loop thread, FIFO with earlier
  // posts. Posted work runs on the loop's next turn even when posted from
  // the loop thread itself; it never keeps the loop alive on its own.
  virtual void post(std::function<void()> fn) = 0;

  // One-shot timer, delay_ms from now. Returns the timer id, 0 on backend
  // failure (0 is never a valid id). Loop-thread only (or before run()).
  virtual std::uint32_t start_timer(std::uint64_t delay_ms,
                                    std::function<void()> on_fire) = 0;

  // false: unknown id, or the timer already fired / is closing.
  virtual bool cancel_timer(std::uint32_t id) = 0;

  // Runs until stop() or until no user work remains (timers, streams,
  // datagrams - not the post channel). Posts queued before run() execute
  // at entry: the unref'd wake channel cannot make an otherwise-empty
  // loop "alive", so the entry drain is what runs them.
  virtual void run() = 0;

  // One turn; may block until an event completes.
  virtual void run_once() = 0;

  // Thread-safe, idempotent - wakes a sleeping run() so it returns
  // promptly instead of at the next event.
  virtual void stop() = 0;

  // True on the thread currently inside run()/run_once().
  [[nodiscard]] virtual bool on_loop_thread() = 0;
};

// Implemented in src/backend/uv (backend_factory.cpp).
[[nodiscard]] std::unique_ptr<ILoopBackend> make_loop_backend();

}  // namespace ws::net::detail
