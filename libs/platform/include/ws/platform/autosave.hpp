#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <ws/core/result.hpp>
#include <ws/settings/filesystem.hpp>

namespace ws::platform {

// What a leftover marker says about a crashed session, for the
// "we found an autosave - recover it?" dialog. The fields are the
// marker's own record; ownership of the files is by the derived paths,
// so a stale record is information, never authority.
struct Recovery {
    std::string document;         // the document the session was editing
    std::string autosave;         // where the content waits
    std::int64_t saved_ms = 0;    // the last snapshot's clock reading
};

// The autosave/crash-recovery helper. snapshot() keeps a content copy
// plus a marker file in a caller-chosen store dir; a clean finish()
// clears both, so a marker found at startup means the last session
// went down without one and the content copy is waiting to be offered.
// Owner-thread, one instance per document: the seam's fixed ".tmp"
// sibling already forbids a second writer on one path.
//
//   ws::settings::StdFileSystem fs;
//   Autosave autosave{fs, document_path, config_dir + "/autosave"};
//   if (const auto crashed = autosave.probe()) {
//       if (*crashed) offer_recovery(crashed->autosave);
//   }
//   while (editing) autosave.snapshot(bytes);   // debounced by the caller
//   autosave.finish();                          // the real save landed
class Autosave {
  public:
    // Milliseconds, injected for tests like FileWatch's clock. The
    // default is WALL-clock ms: saved_ms is display information ("the
    // autosave is from 14:02"), not a duration to debounce.
    using Clock = std::function<std::int64_t()>;

    // The store-dir maker, injected for tests like WatchSource: the
    // FileSystem seam has five content verbs and no mkdir, and this
    // lib's own ensure_directory is the default (mkdir -p over utf-8
    // narrow paths). Fake-disk tests inject a no-op.
    using DirEnsurer = std::function<ws::core::Status(std::string_view)>;

    // The store dir may not exist yet - snapshot() creates it (the
    // injected ensurer, mkdir -p by default). The content and marker
    // file names derive from the document path (its name plus a hash
    // of the normalized path, so same-named documents in different
    // folders never collide).
    Autosave(ws::settings::FileSystem& fs, std::string document_path,
             std::string store_dir, Clock clock = {},
             DirEnsurer ensure = {});

    Autosave(const Autosave&) = delete;
    Autosave& operator=(const Autosave&) = delete;

    // A leftover marker from a previous session: that session crashed
    // (a clean exit always clears). nullopt = nothing to recover. The
    // marker alone is the crash evidence - the content file may be
    // gone (recover() refines that case). A marker that will not
    // parse answers corrupt_file; discard() still cleans the pair.
    // An undeterminable exists read answers none (the seam's
    // contract, inherited).
    [[nodiscard]] ws::core::Result<std::optional<Recovery>> probe() const;

    // Hand the crashed content back and clear both files: the content
    // is the caller's document from here. Requires the content file
    // (invalid_argument when there is nothing to recover); a marker
    // is not required - an orphaned content file (a crash between the
    // two writes of snapshot/finish) is recoverable by construction.
    [[nodiscard]] ws::core::Result<std::string> recover();

    // Drop a crashed autosave without recovering it. Afterwards
    // probe() answers none.
    [[nodiscard]] ws::core::Status discard();

    // The session work: write the content first, then the marker (the
    // commit point). A crash between the two leaves an orphan content
    // file probe() does not claim, overwritten by the next session's
    // snapshot. A failed write propagates with no marker touched, so
    // a recovery is never promised that is not on disk.
    ws::core::Status snapshot(std::string_view bytes);

    // The clean exit: the real save landed. Marker first, then the
    // content - a crash between the two leaves an orphan probe() does
    // not claim. Idempotent.
    ws::core::Status finish();

    [[nodiscard]] const std::string& document() const noexcept {
        return document_;
    }
    [[nodiscard]] const std::string& autosave_path() const noexcept {
        return autosave_path_;
    }
    [[nodiscard]] const std::string& marker_path() const noexcept {
        return marker_path_;
    }

  private:
    [[nodiscard]] std::int64_t now_ms() const;
    [[nodiscard]] ws::core::Status write_marker();

    ws::settings::FileSystem& fs_;
    std::string document_;
    std::string autosave_path_;
    std::string marker_path_;
    Clock clock_;
    DirEnsurer ensure_;
};

}  // namespace ws::platform
