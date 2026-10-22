#include <ws/jobs/job_system.hpp>

namespace ws::jobs {

JobSystem::JobSystem(unsigned threads) : queues_(kPriorityCount) {
    // 0 (the default) means "you pick": hardware_concurrency, clamped to
    // [1..8] - a job pool wider than that buys contention, not throughput.
    unsigned n = threads != 0 ? threads : std::thread::hardware_concurrency();
    n = std::clamp(n, 1u, 8u);
    workers_.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
    // The one startup log line (the gfx tier-line style).
    ws::core::info("jobs: {} worker thread(s)", n);
}

JobSystem::~JobSystem() {
    {
        std::scoped_lock lock{queue_mutex_};
        stopping_.store(true, std::memory_order_release);
    }
    // Cancel-all: running jobs see the request at their next poll; queued
    // jobs additionally get their terminal state set below.
    request_cancel_all();
    // Workers skip remaining queued entries - each is marked cancelled
    // without running, and anyone blocked on its handle is woken. (Lock
    // order here is queue -> record; nothing anywhere takes them the
    // other way round.)
    {
        std::scoped_lock lock{queue_mutex_};
        for (auto& level : queues_) {
            for (auto& work : level) {
                std::scoped_lock record_lock{work.record->mutex};
                if (work.record->load_state() == JobState::queued) {
                    work.record->store_state(JobState::cancelled);
                    work.record->cv.notify_all();
                }
            }
            level.clear();
        }
    }
    queue_cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    // Undrained posts are DROPPED, never run after teardown begins - say
    // so once. Drain() before destroying the system if completions matter.
    if (const std::size_t dropped = marshal_.pending_count(); dropped > 0) {
        ws::core::warn(
            "jobs: dropped {} marshal post(s) at shutdown - drain() before "
            "tearing down",
            dropped);
    }
}

void JobSystem::worker_loop() {
    for (;;) {
        Work work{};
        {
            std::unique_lock lock{queue_mutex_};
            queue_cv_.wait(lock,
                           [this] { return stopping_.load() ||
                                           has_work_locked(); });
            if (stopping_.load(std::memory_order_acquire)) {
                return;  // queued entries are the dtor's to mark, never ours
            }
            work = pop_locked();
        }
        if (work.record != nullptr) {
            // Claim the record: queued -> running. The compare-exchange
            // guards the one shutdown corner - a record the dtor's mark
            // pass already took to cancelled must not regress to running;
            // the worker defers to the dtor's terminal state entirely
            // (a failed exchange leaves the actual state in `was`).
            JobState was = JobState::queued;
            work.record->state.compare_exchange_strong(
                was, JobState::running, std::memory_order_acq_rel);
            if (was == JobState::queued) {
                work.run();
            }
        }
    }
}

bool JobSystem::has_work_locked() const noexcept {
    for (const auto& level : queues_) {
        if (!level.empty()) {
            return true;
        }
    }
    return false;
}

JobSystem::Work JobSystem::pop_locked() {
    // Highest priority first; within one level, front is the oldest submit
    // (push_back/pop_front keeps the record id order as the tiebreak).
    for (std::size_t level = queues_.size(); level-- > 0;) {
        if (!queues_[level].empty()) {
            Work work = std::move(queues_[level].front());
            queues_[level].pop_front();
            return work;
        }
    }
    return Work{};  // unreachable: the caller only calls with work present
}

void JobSystem::prune_live(detail::RecordBase* base) noexcept {
    std::scoped_lock lock{live_mutex_};
    const auto it = std::find_if(
        live_.begin(), live_.end(),
        [base](const std::shared_ptr<detail::RecordBase>& rec) {
            return rec.get() == base;
        });
    if (it != live_.end()) {
        live_.erase(it);
    }
    // Unregister from the key/group rosters. The key slot is dropped only
    // when it still points at THIS record - a newer same-key job has
    // already taken it (and cancelled us), and the slot belongs to them.
    if (base->key != 0) {
        const auto slot = keys_.find(base->key);
        if (slot != keys_.end()) {
            const auto held = slot->second.lock();
            if (held == nullptr || held.get() == base) {
                keys_.erase(slot);
            }
        }
    }
    if (base->group != 0) {
        const auto roster = groups_.find(base->group);
        if (roster != groups_.end()) {
            auto& members = roster->second;
            members.erase(
                std::remove_if(members.begin(), members.end(),
                               [base](const std::weak_ptr<detail::RecordBase>& w) {
                                   const auto held = w.lock();
                                   return held == nullptr || held.get() == base;
                               }),
                members.end());
            if (members.empty()) {
                groups_.erase(roster);
            }
        }
    }
}

void JobSystem::request_cancel_all() {
    std::scoped_lock lock{live_mutex_};
    for (const auto& rec : live_) {
        rec->request_cancel();
    }
}

void JobSystem::wait_all() {
    std::vector<std::shared_ptr<detail::RecordBase>> snapshot;
    {
        std::scoped_lock lock{live_mutex_};
        snapshot.reserve(live_.size());
        for (const auto& rec : live_) {
            snapshot.push_back(rec);
        }
    }
    wait_records(snapshot);
}

void JobSystem::cancel_group(std::uint64_t group) {
    std::scoped_lock lock{live_mutex_};
    const auto roster = groups_.find(group);
    if (roster == groups_.end()) {
        return;
    }
    for (const auto& weak : roster->second) {
        if (const auto rec = weak.lock()) {
            if (!rec->terminal()) {
                rec->request_cancel();
            }
        }
    }
}

void JobSystem::wait_group(std::uint64_t group) {
    std::vector<std::shared_ptr<detail::RecordBase>> snapshot;
    {
        std::scoped_lock lock{live_mutex_};
        const auto roster = groups_.find(group);
        if (roster == groups_.end()) {
            return;
        }
        for (const auto& weak : roster->second) {
            if (const auto rec = weak.lock()) {
                snapshot.push_back(rec);
            }
        }
    }
    wait_records(snapshot);
}

void JobSystem::wait_records(
    const std::vector<std::shared_ptr<detail::RecordBase>>& records) {
    // Each cv is signalled at its own terminal transition, so one pass
    // over the snapshot suffices: nothing leaves terminal afterwards.
    for (const auto& rec : records) {
        std::unique_lock lock{rec->mutex};
        rec->cv.wait(lock, [&rec] { return rec->terminal(); });
    }
}

}  // namespace ws::jobs
