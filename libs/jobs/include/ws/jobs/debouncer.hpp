#pragma once

#include <cstdint>
#include <functional>
#include <utility>

#include <ws/core/result.hpp>
#include <ws/jobs/job_system.hpp>

namespace ws::jobs {

// The debounced auto-run pattern (nextplan stage 8): every run() slides
// the fire moment further out; the task fires only once the noise has
// been quiet for the whole window - "run when the user stops typing".
//
// The clock is INJECTED (milliseconds since any fixed epoch) so tests are
// deterministic under a fake clock: nothing sleeps, poll() only compares
// numbers. The owner ticks poll() once per frame (same cadence as drain);
// a due task is submitted to the JobSystem as a void job - it runs on a
// worker and its Result/cancel/progress story is a normal job's.
//
// Owner-thread only, like every non-job side of the system: run/cancel/
// poll are not called from jobs or workers.
class Debouncer {
public:
    using Clock = std::function<std::int64_t()>;  // ms, injectable
    using Task = std::function<void()>;

    Debouncer(JobSystem& system, Clock clock, std::int64_t quiet_ms)
        : system_(system),
          clock_(std::move(clock)),
          quiet_ms_(quiet_ms) {}

    // (Re)schedule: the task fires quiet_ms after the LAST run call - a
    // reschedule replaces the pending task and slides the window, it does
    // not stack work.
    void run(Task task) {
        task_ = std::move(task);
        deadline_ = clock_() + quiet_ms_;
    }

    // Drop the pending task (the window closes without firing).
    void cancel() { task_ = nullptr; }

    // Once per frame: submit the task to the system when its window has
    // been quiet. Fires at most one task per poll.
    void poll() {
        if (task_ == nullptr || clock_() < deadline_) {
            return;
        }
        Task due = std::move(task_);
        task_ = nullptr;
        // Refused submits (teardown racing the fire) answer a cancelled
        // handle: the task is dropped, which is the documented outcome -
        // the system is going away either way.
        auto handle = system_.submit<void>(
            [due = std::move(due)](JobContext&) -> ws::core::Status {
                due();
                return {};
            });
        (void)handle;  // the fire-and-forget shape: progress/completions
                       // arrive by the caller's own callbacks, if any
    }

    // True while a task is waiting out its window.
    [[nodiscard]] bool pending() const { return task_ != nullptr; }

private:
    JobSystem& system_;
    Clock clock_;
    std::int64_t quiet_ms_;
    std::int64_t deadline_ = 0;
    Task task_;
};

}  // namespace ws::jobs
