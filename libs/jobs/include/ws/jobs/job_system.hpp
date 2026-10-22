#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <ws/core/log.hpp>
#include <ws/core/result.hpp>
#include <ws/jobs/detail/record.hpp>
#include <ws/jobs/errors.hpp>
#include <ws/jobs/marshal.hpp>

// Stage 8's headless core (nextplan.md): a Result-shaped job system. Jobs
// are callables running on worker threads; progress and completions cross
// back to the owner thread through the marshal queue, drained once per
// frame. Cancellation is cooperative - the callable polls the context (or
// hands the token to nested code) and RETURNS the cancelled error to say
// "I stopped where you asked". Exceptions never cross: the worker wrapper
// catches anything thrown and converts it to a failed Result.
//
// Jobs never touch the gui or any other non-thread-safe surface: by design
// the ONLY things a job may call back through are report_progress and its
// own Result. Everything else waits for the drain.

namespace ws::jobs {

// Queue admission order. Higher priorities are picked first; within one
// priority jobs run in submit order (the record's id is the tiebreak).
enum class Priority : std::int32_t {
    low = 0,
    normal = 1,
    high = 2,
};

// Optional submit-time registration. Phase 1 stores both fields on the
// record; the latest-wins key registry and the group registry are the
// next arc's work - zero means "not registered".
struct JobOptions {
    std::uint64_t key = 0;   // nonzero: a latest-wins key (same key resubmit
                             // cancels the previous run)
    std::uint64_t group = 0; // nonzero: a group tag for cancel/wait by group
};

// Delivery callbacks, both invoked on the DRAIN thread (never a worker).
// Each is optional - leave it unset and the marshal post for it is never
// made. Progress arrives coalesced: at most one delivery per drain per job,
// carrying the highest fraction seen so far (the bar never runs backwards)
// and the latest message. The message pointer is valid only inside the
// callback. Declared (not defined) here because it names JobHandle<T>;
// the definition follows it.
template <class T>
struct JobCallbacks;
class JobSystem;

// The caller's shared view of one submitted job: a copyable window onto the
// record. The default handle is null - every call on it is safe and answers
// as "no job": state() reads JobState::invalid, busy()/wait() are false/void
// no-ops, take() fails with invalid_handle, request_cancel() does nothing.
//
// wait() is for the OWNER thread only (app thread, outside any job): a job
// waiting on another job's handle inside the pool is the one deadlock the
// system leaves to the caller, because the pool may be full of waiters with
// nothing left to run what they wait on.
template <class T>
class [[nodiscard]] JobHandle {
public:
    using element_type = T;

    JobHandle() = default;

    [[nodiscard]] bool null() const noexcept { return record_ == nullptr; }
    explicit operator bool() const noexcept { return record_ != nullptr; }

    // The record's state, read without any lock. JobState::invalid for the
    // null handle.
    [[nodiscard]] JobState state() const noexcept {
        return record_ != nullptr ? record_->load_state() : JobState::invalid;
    }
    // True while a worker still owes this job work (queued or running).
    [[nodiscard]] bool busy() const noexcept {
        if (record_ == nullptr) {
            return false;
        }
        const JobState s = record_->load_state();
        return s == JobState::queued || s == JobState::running;
    }

    // Block until the job reaches a terminal state (done/failed/cancelled).
    // Null handle: returns immediately. Owner thread only - see the class
    // comment. Returns the terminal state it observed.
    JobState wait() const {
        if (record_ == nullptr) {
            return JobState::invalid;
        }
        std::unique_lock lock{record_->mutex};
        record_->cv.wait(lock, [this] { return record_->terminal(); });
        return record_->load_state();
    }

    // Ask the job to stop. Asynchronous and cooperative: a running job sees
    // it at its next check_cancel/cancelled poll; a still-queued job is
    // skipped at pickup (it never runs and reads cancelled).
    void request_cancel() const noexcept {
        if (record_ != nullptr) {
            record_->request_cancel();
        }
    }

    // The job's cancel flag as a copyable token - hand it to nested calls.
    // Null handle: the null token (never cancelled).
    [[nodiscard]] CancelToken cancel_token() const noexcept {
        return record_ != nullptr ? CancelToken{record_->cancel_flag}
                                  : CancelToken{};
    }

    [[nodiscard]] std::uint64_t id() const noexcept {
        return record_ != nullptr ? record_->id : 0;
    }

    // Move the Result out, once. Before terminal: not_finished; after a
    // first successful take: already_taken; null handle: invalid_handle.
    // A cancelled job answers the cancelled error here - the SAME error the
    // callable returned (or would have), so WS_TRY in the owner's on_complete
    // or drain code propagates it like any failure. const because the
    // handle is a view: the one-time flag lives on the record.
    [[nodiscard]] ws::core::Result<T> take() const {
        if (record_ == nullptr) {
            return fail(JobError::invalid_handle);
        }
        std::scoped_lock lock{record_->mutex};
        if (!record_->terminal()) {
            return fail(JobError::not_finished);
        }
        if (record_->result_taken) {
            return fail(JobError::already_taken);
        }
        record_->result_taken = true;
        return std::move(static_cast<detail::Record<T>*>(record_.get())->result);
    }

private:
    friend class JobSystem;  // submit() constructs the live handles
    explicit JobHandle(std::shared_ptr<detail::Record<T>> record)
        : record_(std::move(record)) {}

    std::shared_ptr<detail::Record<T>> record_;
};

// (Defined after JobHandle, which on_complete carries by value.)
template <class T>
struct JobCallbacks {
    std::function<void(float fraction, const char* message)> on_progress;
    std::function<void(JobHandle<T> handle)> on_complete;
};

// What the callable sees. Built by the worker before the call, one per job
// run; carries the cancel flag and the identity, nothing else - progress
// reporting funnels back through the system's marshal queue.
class JobContext {
public:
    JobContext() = default;

    // The poll form: true once request_cancel() has been seen anywhere.
    [[nodiscard]] bool cancelled() const noexcept { return token_.cancelled(); }

    // The WS_TRY form: an ok Status while running, the cancelled error once
    // a cancel has been requested. Returning it is exactly how a callable
    // says "I honoured the cancel" - the system maps it to
    // JobState::cancelled:
    //     WS_TRY(ctx.check_cancel());
    [[nodiscard]] ws::core::Status check_cancel() const {
        if (token_.cancelled()) {
            return fail(JobError::cancelled);
        }
        return ws::core::Status{};
    }

    // Report progress, from the job's thread, at any cadence. The fraction
    // is clamped to [0, 1] and the sink keeps the max - the delivered bar
    // never runs backwards even if reports do. A null message keeps the
    // previous one. Delivery is coalesced: at most one on_progress per
    // drain() per job.
    void report_progress(float fraction, const char* message = nullptr) const {
        if (report_ != nullptr) {
            report_(fraction, message);
        }
    }

    // The submit-order id (0 for the default context).
    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }

    // A copyable token to hand into nested calls so deep code can poll or
    // honour the cancel without the whole context.
    [[nodiscard]] CancelToken token() const noexcept { return token_; }

private:
    friend class JobSystem;  // the worker builds the live context
    JobContext(CancelToken token, std::uint64_t id,
               std::function<void(float, const char*)> report)
        : token_(std::move(token)), id_(id), report_(std::move(report)) {}

    CancelToken token_{};
    std::uint64_t id_ = 0;
    std::function<void(float, const char*)> report_;
};

// The pool. Owns worker threads, the work queues and the marshal queue;
// neither copyable nor movable (threads hold `this`).
//
// Shutdown (the destructor) is safe by construction: every live job is
// cancel-requested first (running jobs see it at their next poll, queued
// jobs additionally read cancelled without ever running), workers exit and
// are joined, and marshal posts still queued at teardown are DROPPED with
// one warn - drain() before destroying the system if completions matter.
// A submit() that races the destructor answers an already-cancelled handle
// and enqueues nothing (this is what kills the submit-from-a-job-at-
// shutdown deadlock class).
class JobSystem {
public:
    explicit JobSystem(unsigned threads = 0);
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&) = delete;
    JobSystem& operator=(JobSystem&&) = delete;

    // Submit one job. T is the Result's value type, explicit even when
    // deducible (`submit<int>(...)`), because the callable's own return
    // type is the one thing the signature must NOT depend on - a job may
    // return anything convertible. `fn` runs on a worker as
    // Result<T>(JobContext&); exceptions it throws never cross the
    // boundary (they become failed Results). Progress/completions cross
    // back through the marshal queue; callbacks fire on the DRAIN thread.
    //
    // The returned handle is live even before the work is picked up.
    template <class T, class F>
    JobHandle<T> submit(F fn, Priority priority = Priority::normal,
                        JobOptions options = {},
                        JobCallbacks<T> callbacks = {}) {
        static_assert(
            std::is_invocable_r_v<ws::core::Result<T>, F&, JobContext&>,
            "a job is F(JobContext&) -> ws::core::Result<T>");

        // The id is a plain atomic bump: identity and submit order, not
        // queue order (that is serialised by the queue mutex below).
        auto record = std::make_shared<detail::Record<T>>(
            next_id_.fetch_add(1, std::memory_order_relaxed) + 1, options.key,
            options.group);
        auto on_progress = std::move(callbacks.on_progress);
        auto on_complete = std::move(callbacks.on_complete);
        std::shared_ptr<detail::RecordBase> base{record};
        MarshalQueue* const marshal = &marshal_;
        JobSystem* const self = this;

        // The job-thread half of progress reporting, fully type-erased.
        std::function<void(float, const char*)> report;
        if (on_progress) {
            report = [base, marshal,
                      sink = std::move(on_progress)](float fraction,
                                                     const char* message) {
                detail::store_progress(base, *marshal, sink, fraction,
                                       message);
            };
        }

        // The worker closure: everything typed lives here. Cancel-before-
        // pickup is honoured by never invoking the callable (skip straight
        // to the cancelled result).
        auto run = [record, base, marshal, self, fn = std::move(fn),
                    on_complete = std::move(on_complete),
                    report = std::move(report)]() mutable {
            detail::RecordBase& rec = *base;
            ws::core::Result<T> result = fail(JobError::cancelled);
            if (!rec.cancel_requested()) {
                JobContext ctx{CancelToken{rec.cancel_flag}, rec.id, report};
                try {
                    result = fn(ctx);
                } catch (const std::exception& e) {
                    result = fail(JobError::exception, e.what());
                } catch (...) {
                    result = fail(JobError::exception, "unknown exception");
                }
            }
            {
                std::scoped_lock lock{rec.mutex};
                record->result = std::move(result);
                rec.store_state(detail::map_state(record->result));
                rec.cv.notify_all();
            }
            self->prune_live(base.get());
            if (on_complete) {
                marshal->post(
                    [record, on_complete] { on_complete(JobHandle<T>{record}); });
            }
        };

        {
            std::scoped_lock live_lock{live_mutex_};
            std::scoped_lock queue_lock{queue_mutex_};
            if (stopping_.load(std::memory_order_acquire)) {
                // Refused: already-terminal cancelled, no enqueue, no
                // completion post (the marshal is dying with the system).
                std::scoped_lock lock{base->mutex};
                record->result = fail(JobError::cancelled);
                base->store_state(JobState::cancelled);
                base->cv.notify_all();
                return JobHandle<T>{record};
            }
            live_.push_back(base);
            queues_[static_cast<std::size_t>(priority)].push_back(
                Work{base, std::move(run)});
        }
        queue_cv_.notify_one();
        return JobHandle<T>{record};
    }

    // Run everything the marshal queue holds (progress deliveries,
    // completions), once, FIFO, on the calling thread. Returns how many
    // closures ran. The ui calls this once per frame; a post made during
    // the drain lands the next drain.
    std::size_t drain() { return marshal_.drain(); }

    // The number of worker threads actually running (never zero).
    [[nodiscard]] unsigned thread_count() const noexcept {
        return static_cast<unsigned>(workers_.size());
    }

private:
    struct Work {
        std::shared_ptr<detail::RecordBase> record;
        std::function<void()> run;
    };
    static constexpr std::size_t kPriorityCount = 3;

    void worker_loop();
    [[nodiscard]] bool has_work_locked() const noexcept;
    [[nodiscard]] Work pop_locked();
    void prune_live(detail::RecordBase* base) noexcept;

    // marshal_ is declared FIRST so it is destroyed LAST: its posts hold
    // `this`-owned pointers, and no member may die before the queue that
    // still runs them (teardown drops undrained posts with a warn instead).
    MarshalQueue marshal_;
    std::vector<std::deque<Work>> queues_;  // indexed by Priority
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::atomic<std::uint64_t> next_id_{0};
    std::atomic<bool> stopping_{false};
    std::mutex live_mutex_;
    std::vector<std::shared_ptr<detail::RecordBase>> live_;  // unfinished
    std::vector<std::thread> workers_;
};

}  // namespace ws::jobs
