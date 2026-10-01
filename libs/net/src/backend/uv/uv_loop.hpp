#pragma once

// Internal: the libuv event-loop backend. This file and its siblings in
// src/backend/uv are the ONLY place uv.h appears in the workspace.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <uv.h>

#include "core/transport.hpp"

namespace ws::net::detail {

class UvLoopBackend final : public ILoopBackend {
 public:
  UvLoopBackend();
  ~UvLoopBackend() override;  // drains: every handle completes its close

  UvLoopBackend(const UvLoopBackend&) = delete;
  UvLoopBackend& operator=(const UvLoopBackend&) = delete;

  void post(std::function<void()> fn) override;
  std::uint32_t start_timer(std::uint64_t delay_ms,
                            std::function<void()> on_fire) override;
  bool cancel_timer(std::uint32_t id) override;
  void run() override;
  void run_once() override;
  void stop() override;
  [[nodiscard]] bool on_loop_thread() override;

  // For the sibling socket backends (src/backend/uv/uv_tcp.* etc).
  [[nodiscard]] uv_loop_t* raw() noexcept { return &loop_; }

  // Live-handle registry for the sibling socket backends: every socket
  // registers itself with its close callback; the destructor force-closes
  // whatever is still open (so ~EventLoop drains live connections instead
  // of leaking them), and each close callback untracks before freeing.
  void track(uv_handle_t* handle, uv_close_cb on_closed);
  void untrack(uv_handle_t* handle);

 private:
  struct TimerEntry {
    UvLoopBackend* owner = nullptr;
    std::uint32_t id = 0;
    uv_timer_t handle{};
    std::function<void()> fn;
    bool closing = false;  // uv_close issued: no second close, no cancel
  };

  static void on_wake(uv_async_t* async);
  static void on_timer(uv_timer_t* timer);
  static void on_timer_closed(uv_handle_t* handle);

  void drain_posts();
  [[nodiscard]] bool has_pending_posts() const;

  uv_loop_t loop_{};
  uv_async_t wake_{};  // unref'd: wakes, never keeps the loop alive
  bool alive_ = false;  // false when uv init failed: every API is a no-op
  mutable std::mutex posts_mutex_;  // has_pending_posts() reads under lock
  std::deque<std::function<void()>> posts_;
  std::map<std::uint32_t, std::unique_ptr<TimerEntry>> timers_;
  std::uint32_t next_timer_id_ = 1;  // monotonic; 0 is never issued
  std::atomic<std::thread::id> loop_thread_{};
  std::vector<std::pair<uv_handle_t*, uv_close_cb>> tracked_;
};

}  // namespace ws::net::detail
