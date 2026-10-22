#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace ws::jobs {

// The one bridge between worker threads and the thread that owns the results
// (in the ui that is the app thread, draining once per frame). post() is
// callable from ANY thread; drain() runs everything queued so far, in FIFO
// order, on the caller's thread. A post made WHILE a drain is running lands
// in the next drain - bounded and predictable, never re-entrant - so
// callbacks may post (a completion scheduling follow-up work) without
// recursion.
//
// Steady state allocates nothing: drain() swaps the incoming queue into a
// reused local buffer under the lock, then runs it with the lock released.
class MarshalQueue {
public:
    MarshalQueue() = default;
    MarshalQueue(const MarshalQueue&) = delete;
    MarshalQueue& operator=(const MarshalQueue&) = delete;
    MarshalQueue(MarshalQueue&&) = delete;
    MarshalQueue& operator=(MarshalQueue&&) = delete;

    // Queue one closure. Callable from any thread, including from inside a
    // drain callback. Never blocks beyond the queue's own mutex.
    void post(std::function<void()> task) {
        const std::scoped_lock lock{mutex_};
        incoming_.push_back(std::move(task));
    }

    // Run everything queued so far, FIFO, on the calling thread. Returns the
    // number of closures run. Tasks posted during the drain are NOT run by
    // it - they wait for the next drain by design.
    std::size_t drain() {
        {
            const std::scoped_lock lock{mutex_};
            running_.swap(incoming_);
            incoming_.clear();  // keep the deque's allocation for next time
        }
        for (auto& task : running_) {
            if (task) {
                task();
            }
        }
        const std::size_t ran = running_.size();
        running_.clear();  // drop the closures before the count leaves
        return ran;
    }

    // True when a drain() would run at least one closure.
    [[nodiscard]] bool has_pending() const {
        const std::scoped_lock lock{mutex_};
        return !incoming_.empty();
    }

    // How many closures a drain() would run right now (the shutdown warn's
    // "dropped N posts" number).
    [[nodiscard]] std::size_t pending_count() const {
        const std::scoped_lock lock{mutex_};
        return incoming_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::function<void()>> incoming_;  // written by post()
    std::vector<std::function<void()>> running_;   // drain's reused buffer
};

}  // namespace ws::jobs
