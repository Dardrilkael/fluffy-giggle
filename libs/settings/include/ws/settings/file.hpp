#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <ws/core/result.hpp>
#include <ws/settings/filesystem.hpp>
#include <ws/settings/migration.hpp>
#include <ws/settings/settings.hpp>

namespace ws::settings {

// What a load reports back: the file's fate, plus the reason
// recovery kicked in when it did.
struct FileLoad {
    // No file at the path - the first run. Nothing was loaded.
    bool missing = false;
    // The file was corrupt (unparsable, unmigratable, or refusing to
    // fold) - it was preserved aside and defaults were served.
    bool recovered = false;
    // Where the corrupt file went: the path + ".bak". Empty when the
    // preserve itself failed: the corrupt file stays put, and the
    // next successful save overwrites it.
    std::string backup;
    // The content failure that triggered recovery, verbatim.
    ws::core::Error reason{};
};

// One layer's file: the plumbing between a SettingsStore layer and a
// path - load with corrupt recovery, atomic saves, and a debounced
// write for rapid-change bursts.
//
// Owner-thread state, like the store it serves: load/save/touch/
// cancel/poll/flush all run on one thread. poll() joins the App's
// once-per-frame drain cadence (the jobs Debouncer's shape, but the
// task is one atomic file write on the owner thread - a JobSystem is
// the wrong tool for it).
class LayerFile {
public:
    // Milliseconds on some steady epoch; injectable so tests can
    // time-travel. Defaults to std::chrono::steady_clock.
    using Clock = std::function<std::int64_t()>;

    LayerFile(SettingsStore& store, Layer layer, FileSystem& fs,
              std::string path, const Migrations& migrations,
              Clock clock = {}, std::int64_t quiet_ms = 500);

    LayerFile(const LayerFile&) = delete;
    LayerFile& operator=(const LayerFile&) = delete;

    // Read, parse, migrate, fold. A read failure on an EXISTING file
    // answers io_failed as-is - a locked file is trouble, not
    // corruption, and nothing is preserved. Any CONTENT failure (the
    // parse, the migration walk, the fold) is corrupt recovery: the
    // file is renamed to "<path>.bak" (an existing .bak is replaced -
    // the newest corrupt file wins the slot), the store keeps
    // serving what it already has, and the report carries the
    // reason. A missing file is the first run: missing=true, nothing
    // loaded, no error.
    [[nodiscard]] ws::core::Result<FileLoad> load();

    // The layer out as one json document, written atomically: the
    // previous file survives any failure intact.
    [[nodiscard]] ws::core::Status save();

    // ---- the debounced write ----

    // (Re)arm: the write fires quiet_ms after the LAST touch - a
    // re-touch slides the deadline, it does not stack writes.
    void touch();
    // Drop the pending write without firing it.
    void cancel();
    [[nodiscard]] bool pending() const noexcept;
    // Once per frame: fire the due write, answering its Status (ok
    // when nothing was due). A FAILED write is reported and dropped,
    // not retried - re-touch to retry.
    [[nodiscard]] ws::core::Status poll();
    // Shutdown: write now when one is pending (ok when not).
    [[nodiscard]] ws::core::Status flush();

private:
    [[nodiscard]] std::int64_t now_ms() const;
    [[nodiscard]] ws::core::Result<FileLoad> recover(ws::core::Error reason);

    SettingsStore& store_;
    Layer layer_;
    FileSystem& fs_;
    std::string path_;
    const Migrations& migrations_;
    Clock clock_;
    std::int64_t quiet_ms_ = 0;
    bool armed_ = false;
    std::int64_t deadline_ = 0;
};

}  // namespace ws::settings
