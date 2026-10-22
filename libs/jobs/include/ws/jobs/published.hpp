#pragma once

#include <cstdint>
#include <mutex>
#include <utility>

namespace ws::jobs {

// The streaming-result shape (nextplan stage 8): one producer's latest
// value as a versioned snapshot any thread can copy. Where a Result hands
// back one answer at a terminal moment, Published hands back the CURRENT
// state whenever the consumer looks - the pattern for long-running jobs
// whose intermediate output is itself the product (a search's partial
// hits, a render's latest frame).
//
// A read is a short critical section that COPIES the value out, so no
// consumer ever sees a torn intermediate state - the mutex is held for
// the copy and nothing else. T must be copy-constructible; the default
// state is version 0, "nothing published yet".
//
//   ws::jobs::Published<Hits> hits;
//   ... worker: hits.publish(build_hits());            // bump + store
//   ... app:    if (Hits latest; hits.read_if_new(seen, latest)) { draw(latest); }
template <class T>
class Published {
public:
    Published() = default;

    // Start pre-published: version 1, `read_if_new(0, out)` fires once.
    explicit Published(T initial)
        : value_(std::move(initial)), version_(1) {}

    // Store the latest value and bump the version (the producer thread).
    void publish(T value) {
        const std::scoped_lock lock{mutex_};
        value_ = std::move(value);
        ++version_;
    }

    // The current version: 0 until the first publish, strictly rising
    // after. A consumer keeps the version it last saw and compares.
    [[nodiscard]] std::uint64_t version() const {
        const std::scoped_lock lock{mutex_};
        return version_;
    }

    // A copy of the latest value (T{} before the first publish).
    [[nodiscard]] T read() const {
        const std::scoped_lock lock{mutex_};
        return value_;
    }

    // The consumer's one-call loop: fill `out` with the latest value and
    // update `last_seen` when - and only when - the version moved since
    // `last_seen`. Returns whether a new value was delivered. The caller
    // keeps `last_seen` between calls; `out` is untouched when false.
    bool read_if_new(std::uint64_t& last_seen, T& out) const {
        const std::scoped_lock lock{mutex_};
        if (version_ == last_seen) {
            return false;
        }
        last_seen = version_;
        out = value_;
        return true;
    }

private:
    mutable std::mutex mutex_;
    T value_{};
    std::uint64_t version_ = 0;
};

}  // namespace ws::jobs
