#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/jobs/debouncer.hpp>
#include <ws/jobs/job_system.hpp>
#include <ws/jobs/marshal.hpp>
#include <ws/jobs/published.hpp>

// Stage 8's headless core: the pool, the marshal queue, the handle/token
// contract and shutdown. The deterministic-order tests all run on a
// one-worker system gated by an in-test flag: while the worker is inside
// the gate job the test thread submits freely, so what the worker picks
// after release is exactly the queue order.

namespace {

using ws::core::Result;
using ws::core::Status;
using ws::jobs::JobCallbacks;
using ws::jobs::JobContext;
using ws::jobs::JobError;
using ws::jobs::JobHandle;
using ws::jobs::JobOptions;
using ws::jobs::JobState;
using ws::jobs::JobSystem;
using ws::jobs::Priority;

// A spin gate: the test thread releases it to let a gated worker proceed.
class Gate {
public:
    // Block the calling (worker) thread until release().
    void hold_on() {
        while (!released_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    void release() { released_.store(true, std::memory_order_release); }
    [[nodiscard]] bool released() const {
        return released_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> released_{false};
};

// Drain in a bounded spin until the condition holds. A completion post can
// still be in flight right after wait(): the worker stores the terminal
// state before it posts, so one drain is not guaranteed to catch it.
void drain_until(JobSystem& js, const std::function<bool()>& done,
                 int max_spins = 500) {
    for (int i = 0; i < max_spins && !done(); ++i) {
        js.drain();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

}  // namespace

TEST_CASE("errors: describe, fail shape, category") {
    using ws::jobs::describe;
    using ws::jobs::fail;

    CHECK(describe(JobError::cancelled) == std::string_view{"cancelled"});
    CHECK(describe(JobError::exception) ==
          std::string_view{"job threw an exception"});
    CHECK(!describe(static_cast<JobError>(999)).empty());

    const auto made = ws::jobs::make_job_error(
        JobError::not_finished, "still running");
    CHECK(made.category == std::string_view{"jobs"});
    CHECK(made.code == static_cast<std::int32_t>(JobError::not_finished));
    CHECK(made.message == "still running");

    const Result<int> defaulted = fail(JobError::invalid_handle);
    CHECK(defaulted.error().message == describe(JobError::invalid_handle));
    CHECK(defaulted.error().category == std::string_view{"jobs"});
}

TEST_CASE("handle: the null handle is safe on every call") {
    const JobHandle<int> h{};

    CHECK(h.null());
    CHECK(!static_cast<bool>(h));
    CHECK(h.state() == JobState::invalid);
    CHECK(!h.busy());
    CHECK(h.wait() == JobState::invalid);
    CHECK(h.cancel_token().cancelled() == false);
    CHECK(h.id() == 0);

    h.request_cancel();  // must be a quiet no-op
    const Result<int> taken = h.take();
    REQUIRE(!taken.has_value());
    CHECK(taken.error().code ==
          static_cast<std::int32_t>(JobError::invalid_handle));

    const JobHandle<void> v{};
    const Status taken_void = v.take();
    REQUIRE(!taken_void.has_value());
    CHECK(taken_void.error().code ==
          static_cast<std::int32_t>(JobError::invalid_handle));
}

TEST_CASE("marshal: post/drain is FIFO on the draining thread") {
    ws::jobs::MarshalQueue queue;
    CHECK(!queue.has_pending());
    CHECK(queue.pending_count() == 0);

    std::vector<int> order;
    queue.post([&order] { order.push_back(1); });
    queue.post([&order] { order.push_back(2); });
    queue.post([&order] { order.push_back(3); });
    CHECK(queue.has_pending());
    CHECK(queue.pending_count() == 3);

    const std::thread::id drainer = std::this_thread::get_id();
    std::thread::id seen{};
    queue.post([&seen] { seen = std::this_thread::get_id(); });

    CHECK(queue.drain() == 4);
    CHECK(order == std::vector<int>{1, 2, 3});
    CHECK(seen == drainer);  // closures run on the draining thread
    CHECK(!queue.has_pending());
}

TEST_CASE("marshal: a post made during a drain lands the next drain") {
    ws::jobs::MarshalQueue queue;
    std::vector<int> order;

    queue.post([&] {
        order.push_back(1);
        queue.post([&] { order.push_back(2); });  // re-entrant: not this pass
    });
    queue.post([&] { order.push_back(3); });

    CHECK(queue.drain() == 2);
    CHECK(order == std::vector<int>{1, 3});
    CHECK(queue.pending_count() == 1);
    CHECK(queue.drain() == 1);
    CHECK(order == std::vector<int>{1, 3, 2});
    CHECK(queue.drain() == 0);  // steady state: the reused buffer
    CHECK(order == std::vector<int>{1, 3, 2});
}

TEST_CASE("submit: happy paths, wait, take, ids") {
    JobSystem js(1);

    SUBCASE("int job returns its Result") {
        auto h = js.submit<int>([](JobContext&) -> Result<int> {
            return 42;
        });
        CHECK(h.busy());  // queued or running, never neither
        CHECK(h.wait() == JobState::done);
        CHECK(!h.busy());
        const Result<int> taken = h.take();
        REQUIRE(taken.has_value());
        CHECK(taken.value() == 42);
    }

    SUBCASE("void job returns an ok Status") {
        auto h = js.submit<void>(
            [](JobContext&) -> Status { return {}; });
        CHECK(h.wait() == JobState::done);
        CHECK(h.take().has_value());
    }

    SUBCASE("a second take answers already_taken") {
        auto h = js.submit<int>([](JobContext&) -> Result<int> { return 1; });
        CHECK(h.wait() == JobState::done);
        CHECK(h.take().has_value());
        const Result<int> again = h.take();
        REQUIRE(!again.has_value());
        CHECK(again.error().code ==
              static_cast<std::int32_t>(JobError::already_taken));
    }

    SUBCASE("take before terminal answers not_finished") {
        Gate gate;
        auto h = js.submit<int>([&gate](JobContext&) -> Result<int> {
            gate.hold_on();
            return 5;
        });
        while (h.state() != JobState::running) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        const Result<int> early = h.take();
        REQUIRE(!early.has_value());
        CHECK(early.error().code ==
              static_cast<std::int32_t>(JobError::not_finished));

        gate.release();
        CHECK(h.wait() == JobState::done);
        const Result<int> taken = h.take();
        REQUIRE(taken.has_value());
        CHECK(taken.value() == 5);
    }

    SUBCASE("ids are positive and distinct") {
        std::set<std::uint64_t> ids;
        for (int i = 0; i < 8; ++i) {
            auto h = js.submit<int>([](JobContext& ctx) -> Result<int> {
                return static_cast<int>(ctx.id());
            });
            h.wait();
            const Result<int> taken = h.take();
            REQUIRE(taken.has_value());
            CHECK(taken.value() > 0);
            ids.insert(static_cast<std::uint64_t>(taken.value()));
        }
        CHECK(ids.size() == 8);
        CHECK(js.thread_count() == 1);
    }
}

TEST_CASE("submit: priority order is high before normal before low") {
    JobSystem js(1);
    Gate gate;
    std::vector<std::string> order;  // one worker: pushes are serialised

    // Hold the worker inside a gate job, then stack one job per priority.
    auto blocker = js.submit<void>([&gate](JobContext&) -> Status {
        gate.hold_on();
        return {};
    });
    while (blocker.state() != JobState::running) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    auto low = js.submit<int>(
        [&](JobContext&) -> Result<int> {
            order.push_back("low");
            return 0;
        },
        Priority::low);
    auto normal = js.submit<int>(
        [&](JobContext&) -> Result<int> {
            order.push_back("normal");
            return 0;
        },
        Priority::normal);
    auto high = js.submit<int>(
        [&](JobContext&) -> Result<int> {
            order.push_back("high");
            return 0;
        },
        Priority::high);

    gate.release();
    blocker.wait();
    low.wait();
    normal.wait();
    high.wait();

    CHECK(order == std::vector<std::string>{"high", "normal", "low"});
}

TEST_CASE("cancel: a queued job cancelled before pickup never runs") {
    JobSystem js(1);
    Gate gate;
    std::atomic<bool> ran{false};

    auto blocker = js.submit<void>([&gate](JobContext&) -> Status {
        gate.hold_on();
        return {};
    });
    while (blocker.state() != JobState::running) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    auto h = js.submit<int>([&ran](JobContext&) -> Result<int> {
        ran.store(true);
        return 1;
    });
    h.request_cancel();  // still queued: the worker is inside the gate

    gate.release();
    CHECK(h.wait() == JobState::cancelled);
    CHECK(!ran.load());

    const Result<int> taken = h.take();
    REQUIRE(!taken.has_value());
    CHECK(taken.error().code ==
          static_cast<std::int32_t>(JobError::cancelled));
    CHECK(taken.error().category == std::string_view{"jobs"});
}

TEST_CASE("cancel: a running job honours it via check_cancel") {
    JobSystem js(1);

    auto h = js.submit<int>([](JobContext& ctx) -> Result<int> {
        while (!ctx.cancelled()) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        WS_TRY(ctx.check_cancel());  // the honoured-cancel return itself
        return 1;                    // unreachable
    });

    h.request_cancel();
    CHECK(h.wait() == JobState::cancelled);
    const Result<int> taken = h.take();
    REQUIRE(!taken.has_value());
    CHECK(taken.error().code ==
          static_cast<std::int32_t>(JobError::cancelled));
}

TEST_CASE("cancel: the lost race lands done, not cancelled") {
    JobSystem js(1);
    Gate gate;  // the job parks past its last cancel check

    auto h = js.submit<int>([&gate](JobContext& ctx) -> Result<int> {
        while (!gate.released() && !ctx.cancelled()) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        gate.hold_on();  // past every check: a cancel now loses the race
        return 7;
    });
    while (h.state() != JobState::running) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    h.request_cancel();
    gate.release();
    CHECK(h.wait() == JobState::done);  // success despite the request

    const Result<int> taken = h.take();
    REQUIRE(taken.has_value());
    CHECK(taken.value() == 7);
}

TEST_CASE("progress: clamped, monotonic, coalesced to one delivery") {
    JobSystem js(1);
    struct Seen {
        float fraction = -1.f;
        std::string message;
        int calls = 0;
    } seen;

    JobCallbacks<int> callbacks;
    callbacks.on_progress = [&seen](float fraction, const char* message) {
        ++seen.calls;
        seen.fraction = fraction;
        seen.message = message;  // valid during the callback: copy out
    };

    auto h = js.submit<int>(
        [](JobContext& ctx) -> Result<int> {
            ctx.report_progress(-5.0f, "negative clamps to zero");
            ctx.report_progress(0.5f, "half");
            ctx.report_progress(0.3f, "lower never moves the bar");
            ctx.report_progress(0.4f);  // null message keeps the last one
            ctx.report_progress(1.5f, "over clamps to one");
            return 1;
        },
        Priority::normal, JobOptions{}, std::move(callbacks));

    CHECK(h.wait() == JobState::done);

    // All reports landed before the drain: one armed post, one delivery,
    // carrying the max fraction and the latest message.
    CHECK(js.drain() == 1);
    CHECK(seen.calls == 1);
    CHECK(seen.fraction == 1.0f);
    CHECK(seen.message == "over clamps to one");

    // No sink, no post: reports with no on_progress arm nothing, and the
    // drain is quiet.
    auto quiet = js.submit<int>([](JobContext& ctx) -> Result<int> {
        ctx.report_progress(0.9f, "nobody is listening");
        return 2;
    });
    quiet.wait();
    CHECK(js.drain() == 0);

    // A report mid-run, drained while the job is still busy, delivers
    // straight away - and a later report arms exactly one more post.
    Gate gate;
    float live_fraction = -1.f;
    int live_calls = 0;
    JobCallbacks<int> live_callbacks;
    live_callbacks.on_progress = [&live_fraction,
                                   &live_calls](float fraction, const char*) {
        ++live_calls;
        live_fraction = fraction;
    };
    auto live = js.submit<int>(
        [&gate](JobContext& ctx) -> Result<int> {
            ctx.report_progress(0.25f, "quarter");
            gate.hold_on();
            ctx.report_progress(1.0f, "done reporting");
            return 3;
        },
        Priority::normal, JobOptions{}, std::move(live_callbacks));
    while (live.state() != JobState::running) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    drain_until(js, [&live_calls] { return live_calls == 1; });

    CHECK(live_calls == 1);
    CHECK(live_fraction == 0.25f);
    CHECK(live.busy());  // delivered, but the job is still running

    gate.release();
    live.wait();
    drain_until(js, [&live_calls] { return live_calls == 2; });
    CHECK(live_fraction == 1.0f);
    CHECK(js.drain() == 0);  // both posts consumed, nothing armed
}

TEST_CASE("exceptions: a thrown job lands a failed Result, not a crash") {
    JobSystem js(1);

    SUBCASE("std::exception carries what()") {
        auto h = js.submit<int>([](JobContext&) -> Result<int> {
            throw std::runtime_error{"boom"};
        });
        CHECK(h.wait() == JobState::failed);
        const Result<int> taken = h.take();
        REQUIRE(!taken.has_value());
        CHECK(taken.error().code ==
              static_cast<std::int32_t>(JobError::exception));
        CHECK(taken.error().message.find("boom") != std::string::npos);
    }

    SUBCASE("a non-std exception is still caught") {
        auto h = js.submit<int>([](JobContext&) -> Result<int> {
            throw 42;
        });
        CHECK(h.wait() == JobState::failed);
        CHECK(h.take().error().code ==
              static_cast<std::int32_t>(JobError::exception));
    }
}

TEST_CASE("completions: on_complete fires once, on the drain thread") {
    JobSystem js(1);
    std::thread::id drainer = std::this_thread::get_id();
    std::thread::id seen{};
    JobState seen_state = JobState::invalid;
    int seen_value = -1;
    int calls = 0;

    JobCallbacks<int> callbacks;
    callbacks.on_complete = [&](JobHandle<int> handle) {
        ++calls;
        seen = std::this_thread::get_id();
        seen_state = handle.state();
        const Result<int> taken = handle.take();
        if (taken.has_value()) {
            seen_value = taken.value();
        }
    };

    auto h = js.submit<int>([](JobContext&) -> Result<int> { return 9; },
                            Priority::normal, JobOptions{},
                            std::move(callbacks));
    h.wait();

    // Nothing fires until the drain - completions wait for the owner even
    // after the job is terminal.
    CHECK(calls == 0);
    drain_until(js, [&calls] { return calls == 1; });
    CHECK(calls == 1);
    CHECK(seen == drainer);  // the drain thread, never a worker
    CHECK(seen_state == JobState::done);
    CHECK(seen_value == 9);
    CHECK(js.drain() == 0);  // exactly one post, never two
}

TEST_CASE("shutdown: joins running and queued work") {
    // The destructor's contract on one worker: the running job is
    // cancel-requested (cooperative - it parks until the cancel arrives)
    // and finishes as cancelled; the queued job is skipped without ever
    // running. Reaching the CHECKs at all means the join completed.
    std::atomic<bool> queued_ran{false};
    std::atomic<bool> running_saw_cancel{false};

    {
        JobSystem js(1);

        auto runner = js.submit<int>([&](JobContext& ctx) -> Result<int> {
            while (!ctx.cancelled()) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            running_saw_cancel.store(true);
            return ws::jobs::fail(JobError::cancelled);
        });
        while (runner.state() != JobState::running) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        auto queued = js.submit<int>([&queued_ran](JobContext&) -> Result<int> {
            queued_ran.store(true);
            return 1;
        });
    }  // ~JobSystem: stopping, cancel-all, mark queued, join

    CHECK(running_saw_cancel.load());  // the running job honoured the cancel
    CHECK(!queued_ran.load());         // the queued job never ran
}

TEST_CASE("shutdown: a submit racing teardown is refused, not enqueued") {
    // The jobs-submitting-jobs deadlock killer. The parent parks on the
    // cancel flag itself (the dtor's cancel-all is its releaser); stopping
    // was set BEFORE cancel-all, so by the time the parent wakes the
    // system refuses its child submit: an already-terminal cancelled
    // handle, nothing enqueued. The parent then returns and the join
    // completes - if the child were enqueued, child_state would read
    // queued instead, and a queued child could never run (mark pass).
    std::atomic<JobState> child_state{JobState::invalid};
    std::atomic<bool> child_ran{false};

    {
        JobSystem js(1);

        auto parent = js.submit<void>([&](JobContext& ctx) -> Status {
            while (!ctx.cancelled()) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            auto child =
                js.submit<int>([&child_ran](JobContext&) -> Result<int> {
                    child_ran.store(true);
                    return 1;
                });
            child_state.store(child.state());
            return {};
        });
        while (parent.state() != JobState::running) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }  // ~JobSystem is the releaser

    CHECK(child_state.load() == JobState::cancelled);
    CHECK(!child_ran.load());
}

TEST_CASE("keys: resubmitting a key cancels the running previous") {
    JobSystem js(1);

    auto first = js.submit<int>(
        [](JobContext& ctx) -> Result<int> {
            // Parks until the same-key resubmit requests the cancel.
            while (!ctx.cancelled()) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            return ws::jobs::fail(JobError::cancelled);
        },
        Priority::normal, JobOptions{.key = 7});
    while (first.state() != JobState::running) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    auto second = js.submit<int>([](JobContext&) -> Result<int> { return 2; },
                                 Priority::normal, JobOptions{.key = 7});

    CHECK(first.wait() == JobState::cancelled);
    CHECK(second.wait() == JobState::done);
    const Result<int> taken = second.take();
    REQUIRE(taken.has_value());
    CHECK(taken.value() == 2);
}

TEST_CASE("keys: a queued previous with the same key never runs") {
    JobSystem js(1);
    Gate gate;
    std::atomic<bool> ran{false};

    auto blocker = js.submit<void>([&gate](JobContext&) -> Status {
        gate.hold_on();
        return {};
    });
    while (blocker.state() != JobState::running) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    auto first = js.submit<int>(
        [&ran](JobContext&) -> Result<int> {
            ran.store(true);
            return 1;
        },
        Priority::normal, JobOptions{.key = 9});
    auto second = js.submit<int>([](JobContext&) -> Result<int> { return 2; },
                                 Priority::normal, JobOptions{.key = 9});

    gate.release();
    CHECK(first.wait() == JobState::cancelled);
    CHECK(!ran.load());  // skipped at pickup, never ran
    CHECK(second.wait() == JobState::done);
}

TEST_CASE("groups: cancel_group requests only its members") {
    JobSystem js(3);
    Gate gate_c;

    auto parked = [](JobContext& ctx) -> Result<int> {
        while (!ctx.cancelled()) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return ws::jobs::fail(JobError::cancelled);
    };
    auto a = js.submit<int>(parked, Priority::normal, JobOptions{.group = 5});
    auto b = js.submit<int>(parked, Priority::normal, JobOptions{.group = 5});
    auto c = js.submit<int>(
        [&gate_c](JobContext&) -> Result<int> {
            gate_c.hold_on();
            return 3;
        },
        Priority::normal, JobOptions{.group = 6});
    for (auto* h : {&a, &b, &c}) {
        while (h->state() != JobState::running) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    js.cancel_group(5);
    CHECK(a.wait() == JobState::cancelled);
    CHECK(b.wait() == JobState::cancelled);

    gate_c.release();
    CHECK(c.wait() == JobState::done);
    js.cancel_group(12345);  // unknown group: a quiet no-op
}

TEST_CASE("groups: wait_group blocks until the group is terminal") {
    JobSystem js(3);  // three gated jobs must all be running at once
    Gate gate;

    auto one = js.submit<int>(
        [&gate](JobContext&) -> Result<int> {
            gate.hold_on();
            return 1;
        },
        Priority::normal, JobOptions{.group = 1});
    auto two = js.submit<int>(
        [&gate](JobContext&) -> Result<int> {
            gate.hold_on();
            return 2;
        },
        Priority::normal, JobOptions{.group = 1});
    auto outside = js.submit<int>(
        [&gate](JobContext&) -> Result<int> {
            gate.hold_on();
            return 3;
        });
    for (auto* h : {&one, &two, &outside}) {
        while (h->state() != JobState::running) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    std::atomic<bool> waiter_done{false};
    std::thread waiter([&] {
        js.wait_group(1);
        waiter_done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(!waiter_done.load());  // the group's work is gated: still blocked

    gate.release();
    one.wait();
    two.wait();
    waiter.join();
    CHECK(waiter_done.load());  // returned without waiting for `outside`

    outside.wait();
    js.wait_group(1);  // empty now: immediate
}

TEST_CASE("wait_all: blocks until every live job is terminal") {
    JobSystem js(2);
    Gate gate;

    auto one = js.submit<int>([&gate](JobContext&) -> Result<int> {
        gate.hold_on();
        return 1;
    });
    auto two = js.submit<int>([&gate](JobContext&) -> Result<int> {
        gate.hold_on();
        return 2;
    });
    for (auto* h : {&one, &two}) {
        while (h->state() != JobState::running) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    std::atomic<bool> waiter_done{false};
    std::thread waiter([&] {
        js.wait_all();
        waiter_done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(!waiter_done.load());

    gate.release();
    one.wait();
    two.wait();
    waiter.join();
    CHECK(waiter_done.load());
    js.wait_all();  // nothing live: returns immediately
}

TEST_CASE("cancel_all: requests every live job, queued included") {
    JobSystem js(2);
    std::atomic<int> finished_cancels{0};

    auto parked = [&finished_cancels](JobContext& ctx) -> Result<int> {
        while (!ctx.cancelled()) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        ++finished_cancels;
        return ws::jobs::fail(JobError::cancelled);
    };
    auto a = js.submit<int>(parked);
    auto b = js.submit<int>(parked);
    for (auto* h : {&a, &b}) {
        while (h->state() != JobState::running) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    auto queued = js.submit<int>(parked);  // waits behind the parked pair

    js.request_cancel_all();
    CHECK(a.wait() == JobState::cancelled);
    CHECK(b.wait() == JobState::cancelled);
    CHECK(queued.wait() == JobState::cancelled);  // skipped at pickup
    CHECK(finished_cancels.load() == 2);  // only the running pair ran
}

TEST_CASE("published: versioned snapshots, read_if_new, never torn") {
    using Pair = std::pair<int, int>;

    ws::jobs::Published<Pair> pub;
    CHECK(pub.version() == 0);
    std::uint64_t seen = 0;
    Pair latest{0, 0};
    CHECK(!pub.read_if_new(seen, latest));

    pub.publish({1, 1});
    CHECK(pub.version() == 1);
    CHECK(pub.read_if_new(seen, latest));
    CHECK(latest == Pair{1, 1});
    CHECK(seen == 1);
    CHECK(!pub.read_if_new(seen, latest));  // same version: no delivery

    pub.publish({2, 2});
    pub.publish({3, 3});
    CHECK(pub.read_if_new(seen, latest));  // coalesced: the latest only
    CHECK(latest == Pair{3, 3});
    CHECK(seen == 3);
    CHECK(pub.read() == Pair{3, 3});

    const ws::jobs::Published<int> pre{42};  // pre-published: version 1
    CHECK(pre.version() == 1);
    std::uint64_t pre_seen = 0;
    int pre_value = 0;
    CHECK(pre.read_if_new(pre_seen, pre_value));
    CHECK(pre_value == 42);

    // Stress: a producer pairs (n, n) flat out; a consumer reads whatever
    // is newest. Any torn pair (or a version going backwards) is a failure.
    ws::jobs::Published<Pair> stressed;
    constexpr int kPublishes = 20000;
    std::atomic<bool> stop{false};
    std::thread producer([&] {
        for (int i = 0; i < kPublishes; ++i) {
            stressed.publish({i, i});
        }
        stop.store(true);
    });
    std::atomic<int> torn{0};
    std::atomic<int> version_went_backwards{0};
    std::atomic<std::uint64_t> highest{0};
    std::thread consumer([&] {
        std::uint64_t c_seen = 0;
        std::uint64_t c_last = 0;
        Pair value;
        // Keep reading until the producer stops AND at least one read
        // has landed: the consumer must not exit on its very first stop
        // check (the producer can finish all 20k publishes before this
        // thread is ever scheduled), or highest==0 flakes the check
        // below. The read is a structural guarantee once the version is
        // final, never a scheduling bet.
        bool stopped = false;
        while (!stopped || c_last == 0) {
            if (stressed.read_if_new(c_seen, value)) {
                if (c_seen <= c_last) {
                    ++version_went_backwards;
                }
                c_last = c_seen;
                if (value.first != value.second) {
                    ++torn;
                }
            }
            stopped = stop.load();
        }
        highest.store(c_last);
    });
    producer.join();
    consumer.join();

    CHECK(stressed.version() == kPublishes);  // version 0 + kPublishes bumps
    CHECK(torn.load() == 0);
    CHECK(version_went_backwards.load() == 0);
    CHECK(highest.load() > 0);
}

TEST_CASE("debouncer: quiet window, reschedule, cancel - on a fake clock") {
    JobSystem js(1);
    std::int64_t now = 0;  // the injected clock: no sleep ever runs this
    ws::jobs::Debouncer deb(js, [&now] { return now; }, 100);

    SUBCASE("fires through the system once the window is quiet") {
        std::atomic<int> fired{0};
        deb.run([&fired] { ++fired; });
        CHECK(deb.pending());

        now = 99;
        deb.poll();
        CHECK(fired.load() == 0);  // one ms short: window still open

        now = 100;
        deb.poll();
        CHECK(!deb.pending());
        for (int i = 0; i < 500 && fired.load() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        CHECK(fired.load() == 1);  // ran on the worker, via the system
    }

    SUBCASE("a reschedule slides the window and replaces the task") {
        std::atomic<int> first_fired{0};
        std::atomic<int> second_fired{0};

        now = 0;
        deb.run([&first_fired] { ++first_fired; });
        now = 60;
        deb.run([&second_fired] { ++second_fired; });  // deadline now 160

        now = 100;
        deb.poll();  // past the FIRST deadline, not the second
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(first_fired.load() == 0);

        now = 160;
        deb.poll();
        for (int i = 0; i < 500 && second_fired.load() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        CHECK(second_fired.load() == 1);
        CHECK(first_fired.load() == 0);  // replaced, never fired
    }

    SUBCASE("cancel drops the pending task for good") {
        std::atomic<int> fired{0};
        deb.run([&fired] { ++fired; });
        deb.cancel();
        CHECK(!deb.pending());

        now = 100000;
        deb.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(fired.load() == 0);
    }
}
