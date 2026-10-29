#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <ws/core/result.hpp>
#include <ws/jobs/errors.hpp>
#include <ws/jobs/marshal.hpp>

// The record is one job's shared state: the handle the caller holds, the
// worker, and the drain thread all view the same instance through a
// shared_ptr. It lives under include/ws/jobs/detail/ because the typed
// JobHandle<T> must see Record<T>'s shape - but everything here is an
// implementation name; job_system.hpp is the public surface, and no user
// names ws::jobs::detail.

namespace ws::jobs {

// One job's lifecycle. `invalid` is never stored in a record - it is the
// null JobHandle's answer, so "no job" reads distinctly from every real
// terminal state.
enum class JobState : std::int32_t {
    queued,    // submitted, not yet picked up
    running,   // a worker is inside the callable
    done,      // returned a success Result
    failed,    // returned a failure Result (other than the cancelled code)
    cancelled, // returned the cancelled error, or was skipped/stopped
    invalid,   // the null handle: no job at all
};

// A cheap copyable view of one job's cancel flag - hand it into nested
// calls so deep code can poll or honour the cancel without threading the
// whole context down. Default-constructed is the null token: never
// cancelled, request_cancel() a no-op.
class CancelToken {
public:
    CancelToken() = default;
    explicit CancelToken(std::shared_ptr<std::atomic<bool>> flag)
        : flag_(std::move(flag)) {}

    [[nodiscard]] bool cancelled() const noexcept {
        return flag_ != nullptr &&
               flag_->load(std::memory_order_acquire);
    }
    void request_cancel() const noexcept {
        if (flag_ != nullptr) {
            flag_->store(true, std::memory_order_release);
        }
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

namespace detail {

// The untyped half every participant can touch: identity, the cancel flag,
// the state atom (read without the mutex), and the mutex-guarded progress
// mailbox + terminal handoff. Lock order note: when a closure holds both
// this mutex and the marshal queue's, it takes THIS one first, always
// (record -> queue; never the reverse).
struct RecordBase {
    RecordBase(std::uint64_t id_, std::uint64_t key_, std::uint64_t group_)
        : id(id_),
          key(key_),
          group(group_),
          cancel_flag(std::make_shared<std::atomic<bool>>(false)) {}

    virtual ~RecordBase() = default;
    RecordBase(const RecordBase&) = delete;
    RecordBase& operator=(const RecordBase&) = delete;

    // Identity (immutable; submit order for ids, caller-chosen for the rest).
    const std::uint64_t id;
    const std::uint64_t key;
    const std::uint64_t group;

    // Cross-thread state. `state` is atomic so state() never takes the
    // mutex; everything below it in this block is mutex-guarded, and the
    // mutex (not the atom) is what orders the Result's visibility.
    std::atomic<JobState> state{JobState::queued};
    std::shared_ptr<std::atomic<bool>> cancel_flag;  // shared with the token

    std::mutex mutex;
    std::condition_variable cv;  // signalled on every terminal transition

    // The progress mailbox: the worker writes, the drain thread reads. A
    // report stores the max fraction (a bar never runs backwards) plus the
    // latest message, and bumps progress_seq; the armed flag coalesces
    // delivery to at most one marshal post in flight per job.
    float progress = 0.f;
    std::string progress_message;
    std::uint64_t progress_seq = 0;
    bool progress_armed = false;

    // Set (under mutex) when take() has moved the Result out.
    bool result_taken = false;

    [[nodiscard]] bool cancel_requested() const noexcept {
        return cancel_flag->load(std::memory_order_acquire);
    }
    void request_cancel() const noexcept {
        cancel_flag->store(true, std::memory_order_release);
    }
    [[nodiscard]] JobState load_state() const noexcept {
        return state.load(std::memory_order_acquire);
    }
    void store_state(JobState s) noexcept {
        state.store(s, std::memory_order_release);
    }
    [[nodiscard]] bool terminal() const noexcept {
        const JobState s = load_state();
        return s == JobState::done || s == JobState::failed ||
               s == JobState::cancelled;
    }
};

// The typed half: the Result the job produced, moved in once by the worker
// (under the record mutex) and moved out once by take().
//
// The Result's default is the CANCELLED error, not a success: a job can
// reach a terminal state without its callable ever running (cancelled
// while queued, submit refused during shutdown), and take() on every such
// path must answer the cancelled error - never a default-constructed
// value pretending to be success. "Presumed cancelled until the worker
// proves otherwise."
template <class T>
struct Record : RecordBase {
    using RecordBase::RecordBase;

    ws::core::Result<T> result = fail(JobError::cancelled);
};

// Map a finished Result to its terminal state. A jobs-category cancelled
// error is the honoured-cancel contract (JobState::cancelled) - any other
// failure, whatever its category, is JobState::failed. Only OUR cancelled
// code counts: another lib's code 2 means something else.
template <class T>
[[nodiscard]] inline JobState map_state(
    const ws::core::Result<T>& result) noexcept {
    if (result.has_value()) {
        return JobState::done;
    }
    const ws::core::Error& error = result.error();
    if (std::string_view{error.category} == "jobs" &&
        error.code == static_cast<std::int32_t>(JobError::cancelled)) {
        return JobState::cancelled;
    }
    return JobState::failed;
}

// The worker-thread half of progress reporting, type-erased (one body for
// every T). Called from JobContext::report_progress on the job's thread.
// Clamps the fraction, keeps the max, keeps the latest message, and arms
// at most ONE marshal post per job: the post's deliver loop re-reads the
// mailbox under the record mutex and keeps looping while reports land
// during its delivery, clearing the armed flag only when it sees the
// mailbox quiet (no report can slip between the seq check and the clear -
// both happen inside the same locked section).
inline void store_progress(const std::shared_ptr<RecordBase>& rec,
                           MarshalQueue& marshal,
                           const std::function<void(float, const char*)>& sink,
                           float fraction, const char* message) {
    // rec_lock, not lock: the progress lambda below re-locks in its own
    // blocks, and MSVC /W4 (C4456) rejects the shadowing spelling.
    std::scoped_lock rec_lock{rec->mutex};
    fraction = std::clamp(fraction, 0.f, 1.f);
    if (fraction > rec->progress) {
        rec->progress = fraction;
    }
    if (message != nullptr) {
        rec->progress_message = message;
    }
    ++rec->progress_seq;
    if (!sink || rec->progress_armed) {
        return;  // no sink to tell, or a post already in flight
    }
    rec->progress_armed = true;
    marshal.post([rec, sink]() {
        for (;;) {
            // latest_*, not fraction/message: the enclosing parameters
            // carry the incoming report; these are the mailbox's latest.
            float latest_fraction = 0.f;
            std::string latest_message;
            std::uint64_t seen = 0;
            {
                std::scoped_lock lock{rec->mutex};
                seen = rec->progress_seq;
                latest_fraction = rec->progress;
                latest_message = rec->progress_message;
            }
            sink(latest_fraction, latest_message.c_str());
            {
                std::scoped_lock lock{rec->mutex};
                if (rec->progress_seq == seen) {
                    rec->progress_armed = false;
                    return;
                }
            }
            // Reports landed while we were delivering: run the mailbox
            // once more (the armed flag stayed true, so no second post).
        }
    });
}

}  // namespace detail
}  // namespace ws::jobs
