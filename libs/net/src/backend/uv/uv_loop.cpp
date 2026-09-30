#include "uv_loop.hpp"

#include <ws/core/log.hpp>

namespace ws::net::detail {

UvLoopBackend::UvLoopBackend() {
  wake_.data = this;
  if (uv_loop_init(&loop_) != 0) {
    ws::core::error("net: uv_loop_init failed - backend is inert");
    return;
  }
  if (uv_async_init(&loop_, &wake_, &UvLoopBackend::on_wake) != 0) {
    ws::core::error("net: post-channel init failed - backend is inert");
    uv_loop_close(&loop_);
    return;
  }
  // The post channel wakes a running loop but never holds it open: run()
  // returns as soon as only the wake async would remain.
  uv_unref(reinterpret_cast<uv_handle_t*>(&wake_));
  alive_ = true;
}

UvLoopBackend::~UvLoopBackend() {
  if (!alive_) {
    return;
  }
  uv_close(reinterpret_cast<uv_handle_t*>(&wake_), nullptr);
  for (const auto& [id, entry] : timers_) {
    if (!entry->closing) {  // closing entries already have their close in
      entry->closing = true;
      uv_timer_stop(&entry->handle);
      uv_close(reinterpret_cast<uv_handle_t*>(&entry->handle), nullptr);
    }
  }
  // Close callbacks only run while the loop turns: drive it until
  // uv_loop_close accepts the loop - at that point every handle has
  // completed its close, which is the "never free before the close
  // callback ran" rule made mechanical.
  for (int guard = 0; guard < 1000; ++guard) {
    if (uv_loop_close(&loop_) != UV_EBUSY) {
      return;
    }
    uv_run(&loop_, UV_RUN_NOWAIT);
  }
  ws::core::error("net: event loop failed to drain at teardown - leaking "
                  "the backend rather than freeing live handles");
}

void UvLoopBackend::post(std::function<void()> fn) {
  if (!alive_) {
    return;  // inert backend (init failed): work is dropped, already logged
  }
  {
    const std::lock_guard<std::mutex> lock(posts_mutex_);
    posts_.push_back(std::move(fn));
  }
  uv_async_send(&wake_);  // thread-safe wake
}

std::uint32_t UvLoopBackend::start_timer(
    std::uint64_t delay_ms, std::function<void()> on_fire) {
  if (!alive_) {
    return 0;
  }
  auto entry = std::make_unique<TimerEntry>();
  entry->owner = this;
  entry->id = next_timer_id_;
  if (++next_timer_id_ == 0) {
    next_timer_id_ = 1;  // wrapped: keep 0 reserved as "never issued"
  }
  entry->fn = std::move(on_fire);
  if (uv_timer_init(&loop_, &entry->handle) != 0) {
    return 0;
  }
  entry->handle.data = entry.get();
  const std::uint32_t id = entry->id;
  timers_[id] = std::move(entry);
  if (uv_timer_start(&timers_[id]->handle, &UvLoopBackend::on_timer,
                     delay_ms, 0) != 0) {
    cancel_timer(id);  // close the inited handle properly, then report 0
    return 0;
  }
  return id;
}

bool UvLoopBackend::cancel_timer(std::uint32_t id) {
  const auto it = timers_.find(id);
  if (it == timers_.end() || it->second->closing) {
    return false;
  }
  it->second->closing = true;
  uv_timer_stop(&it->second->handle);
  uv_close(reinterpret_cast<uv_handle_t*>(&it->second->handle),
           &UvLoopBackend::on_timer_closed);
  return true;
}

void UvLoopBackend::run() {
  if (!alive_) {
    return;
  }
  loop_thread_.store(std::this_thread::get_id(), std::memory_order_relaxed);
  drain_posts();  // posts queued before run() execute at entry
  uv_run(&loop_, UV_RUN_DEFAULT);
  loop_thread_.store(std::thread::id{}, std::memory_order_relaxed);
}

void UvLoopBackend::run_once() {
  if (!alive_) {
    return;
  }
  loop_thread_.store(std::this_thread::get_id(), std::memory_order_relaxed);
  drain_posts();
  uv_run(&loop_, UV_RUN_ONCE);
  loop_thread_.store(std::thread::id{}, std::memory_order_relaxed);
}

void UvLoopBackend::stop() {
  if (!alive_) {
    return;
  }
  uv_stop(&loop_);
  // A bare uv_stop is only noticed once the current poll returns - which
  // could be a timer timeout away. Wake the poll so stop lands now (any
  // posts that raced the stop still drain in this final turn).
  uv_async_send(&wake_);
}

bool UvLoopBackend::on_loop_thread() {
  return loop_thread_.load(std::memory_order_relaxed) ==
         std::this_thread::get_id();
}

void UvLoopBackend::on_wake(uv_async_t* async) {
  static_cast<UvLoopBackend*>(async->data)->drain_posts();
}

void UvLoopBackend::drain_posts() {
  std::deque<std::function<void()>> ready;
  {
    const std::lock_guard<std::mutex> lock(posts_mutex_);
    ready.swap(posts_);
  }
  for (auto& fn : ready) {
    fn();  // callbacks must not throw: a throw crosses the libuv C boundary
  }
}

void UvLoopBackend::on_timer(uv_timer_t* timer) {
  auto* entry = static_cast<TimerEntry*>(timer->data);
  entry->closing = true;  // it fired: cancel_timer must fail from now on
  std::function<void()> fn = std::move(entry->fn);
  uv_close(reinterpret_cast<uv_handle_t*>(timer),
           &UvLoopBackend::on_timer_closed);
  if (fn) {
    fn();  // re-arming (start_timer) is legal here; this entry outlives it
  }
}

void UvLoopBackend::on_timer_closed(uv_handle_t* handle) {
  auto* entry = static_cast<TimerEntry*>(handle->data);
  UvLoopBackend* const self = entry->owner;
  const std::uint32_t id = entry->id;
  // The handle finished closing - freeing the entry here (inside the
  // close callback) is exactly the sanctioned point.
  self->timers_.erase(id);
}

}  // namespace ws::net::detail
