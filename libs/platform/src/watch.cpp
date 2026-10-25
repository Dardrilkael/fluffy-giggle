#include <ws/platform/watch.hpp>

#include <chrono>
#include <filesystem>
#include <system_error>
#include <utility>

#include <ws/platform/errors.hpp>

namespace ws::platform {
namespace {

// utf-8 narrow -> fs::path, the workspace's one routing rule (the
// StdFileSystem spelling).
[[nodiscard]] std::filesystem::path to_fs_path(std::string_view path) {
    return std::filesystem::path{std::u8string{path.begin(), path.end()}};
}

}  // namespace

ws::core::Result<std::int64_t> StdWatchSource::modified_ms(
    std::string_view path) const {
    std::error_code ec;
    const std::filesystem::file_time_type stamp =
        std::filesystem::last_write_time(to_fs_path(path), ec);
    if (ec) {
        return fail(PlatformError::io_failed,
                    "stat '" + std::string{path} + "': " + ec.message());
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               stamp.time_since_epoch())
        .count();
}

bool StdWatchSource::exists(std::string_view path) const {
    std::error_code ec;
    const bool there = std::filesystem::exists(to_fs_path(path), ec);
    return !ec && there;
}

FileWatch::FileWatch(const WatchSource& source, std::string path,
                     Clock clock, std::int64_t quiet_ms)
    : source_{source}, path_{std::move(path)}, clock_{std::move(clock)},
      quiet_ms_{quiet_ms} {
    arm();
}

void FileWatch::arm() {
    baseline_ = sample();
    observed_ = baseline_;
    pending_ = FileEvent::none;
}

FileEvent FileWatch::poll() {
    const std::int64_t now = now_ms();
    const Stamp fresh = sample();
    if (fresh != baseline_) {
        if (fresh != observed_) {
            // The change is still moving (or brand new): slide the
            // deadline to the last movement, never stacking events.
            observed_ = fresh;
            changed_at_ms_ = now;
        }
        pending_ = fresh.exists ? FileEvent::modified : FileEvent::removed;
    }
    if (pending_ == FileEvent::none ||
        now - changed_at_ms_ < quiet_ms_) {
        return FileEvent::none;
    }
    const FileEvent out = pending_;
    pending_ = FileEvent::none;
    baseline_ = observed_;  // settled: this state is the new truth
    return out;
}

FileWatch::Stamp FileWatch::sample() const {
    Stamp out;
    out.exists = source_.exists(path_);
    if (!out.exists) {
        return out;  // missing - the removal stamp
    }
    // A failing stamp on an existing file reads as ms=0: no sample is
    // better than a dropped file, and the next poll re-reads.
    const ws::core::Result<std::int64_t> ms = source_.modified_ms(path_);
    out.ms = ms ? *ms : 0;
    return out;
}

std::int64_t FileWatch::now_ms() const {
    if (clock_) {
        return clock_();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace ws::platform
