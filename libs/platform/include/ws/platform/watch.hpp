#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include <ws/core/result.hpp>

namespace ws::platform {

// What one poll saw on disk (stage 12, the file watch).
enum class FileEvent : std::uint8_t {
    none,      // nothing due
    modified,  // the file changed and stayed quiet for the window
    removed,   // the file is gone and stayed gone
};

// The time half of watching that the settings FileSystem seam leaves
// out (five content verbs, no stamps). Tests inject a fake; production
// rides StdWatchSource.
class WatchSource {
  public:
    virtual ~WatchSource() = default;

    // Milliseconds on the source's epoch for the file's last write.
    // The stamp only needs to move when the file moves - two writes
    // inside one stamp tick go unseen (documented, accepted).
    [[nodiscard]] virtual ws::core::Result<std::int64_t> modified_ms(
        std::string_view path) const = 0;

    // False when the answer cannot be determined - the FileSystem's
    // exists contract, read by the watch as "gone".
    [[nodiscard]] virtual bool exists(std::string_view path) const = 0;
};

// The std::filesystem-backed default, the StdFileSystem routing (utf-8
// through char8_t). Every failure answers io_failed naming the path.
class StdWatchSource final : public WatchSource {
  public:
    [[nodiscard]] ws::core::Result<std::int64_t> modified_ms(
        std::string_view path) const override;
    [[nodiscard]] bool exists(std::string_view path) const override;
};

// One watched file's "changed on disk" detector, the LayerFile
// debouncer's shape: an injected clock, owner-thread state, poll() on
// the frame cadence, at most one event per call.
//
//   ws::platform::StdWatchSource source;
//   ws::platform::FileWatch watch{source, "config/theme.json"};
//   // once per frame:
//   if (watch.poll() == ws::platform::FileEvent::modified) reload();
//
// The event fires quiet_ms after the LAST observed change - a writer
// still writing slides the deadline and never stacks events. A removal
// and a fast reappear merge into one modified. Owner-thread only, no
// locks; arm() re-baselines (a load followed by arm() swallows the
// load's own write).
class FileWatch {
  public:
    // Milliseconds on some steady epoch; injectable so tests can
    // time-travel. Defaults to std::chrono::steady_clock.
    using Clock = std::function<std::int64_t()>;

    // Arms on the current disk state; a file that does not exist yet
    // baselines as missing, so its first appearance fires modified.
    FileWatch(const WatchSource& source, std::string path,
              Clock clock = {}, std::int64_t quiet_ms = 500);

    FileWatch(const FileWatch&) = delete;
    FileWatch& operator=(const FileWatch&) = delete;

    // Re-baseline on the current disk state and drop any pending
    // event.
    void arm();

    // Sample the source; answer the event whose observation has been
    // quiet for the window. A sample that cannot be read (exists but
    // unstatable) is skipped - no new observation, nothing lost.
    [[nodiscard]] FileEvent poll();

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

  private:
    // One sampled disk state; exists=false is the missing stamp.
    struct Stamp {
        bool exists = false;
        std::int64_t ms = 0;

        [[nodiscard]] bool operator==(const Stamp&) const = default;
    };

    [[nodiscard]] Stamp sample() const;
    [[nodiscard]] std::int64_t now_ms() const;

    const WatchSource& source_;
    std::string path_;
    Clock clock_;
    std::int64_t quiet_ms_ = 0;
    Stamp baseline_{};   // what the last fired (or armed) state was
    Stamp observed_{};  // the newest moving sample
    FileEvent pending_ = FileEvent::none;
    std::int64_t changed_at_ms_ = 0;  // when observed_ last moved
};

}  // namespace ws::platform
