#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/settings/filesystem.hpp>

namespace ws::platform {

// One "Open Recent" item: the path plus the label a menu shows - the
// file's name, a same-named sibling growing " (parent)".
struct RecentItem {
    std::string path;
    std::string label;
    bool pinned = false;
};

// The capped MRU of recently used files, one list per document type
// ("" is the whole-app default list; a "scene" and a "project" never
// displace each other). Owner-thread state like everything in the lib.
// Persistence rides the settings FileSystem + json seams; save() is
// atomic through the seam's own temp+rename, so a failure anywhere
// leaves the previous file intact.
//
//   ws::settings::StdFileSystem fs;
//   ws::platform::RecentFiles recent{fs, config_dir + "/recent.json"};
//   recent.load();                       // a missing file is a first run
//   recent.touch(path);                  // a document opened
//   for (const auto& item : recent.menu_items()) open_row(item);
//   recent.save();                       // shutdown (or debounced)
class RecentFiles {
  public:
    RecentFiles(ws::settings::FileSystem& fs, std::string path,
                std::size_t cap = 20);

    RecentFiles(const RecentFiles&) = delete;
    RecentFiles& operator=(const RecentFiles&) = delete;

    // Promote to the head of the type's list, deduped by the
    // normalized path (two spellings of one file are one entry, its
    // pin kept). When the list is over the cap the oldest unpinned
    // tail entry falls off; an all-pinned full list refuses the new
    // entry and answers false, the list untouched.
    [[nodiscard]] bool touch(std::string_view doc_type,
                             std::string_view path);
    // The "" default list.
    [[nodiscard]] bool touch(std::string_view path);

    // Pinned entries survive the cap and missing-file pruning. Pinning
    // an absent entry is refused (touch it first); answers false when
    // the path is not in the type's list.
    [[nodiscard]] bool pin(std::string_view doc_type, std::string_view path);
    [[nodiscard]] bool unpin(std::string_view doc_type,
                             std::string_view path);

    // The type's list as menu rows, most recent first. Missing files
    // are pruned lazily here - unpinned entries whose file is gone
    // (the FileSystem's exists, so an undeterminable path reads as
    // gone); pinned entries stay even when missing. An unknown type
    // answers empty.
    [[nodiscard]] std::vector<RecentItem> menu_items(
        std::string_view doc_type = {});

    // ---- persistence (explicit, owner-thread) ----

    // Read the json document. A missing file is the first run (ok,
    // nothing loaded). A load REPLACES the in-memory lists (the
    // layer-file precedent). A corrupt file answers parse_failed with
    // nothing loaded and nothing preserved - unlike the settings .bak,
    // recent files rebuild from use. A file newer than shape 1 is
    // refused, never downgraded.
    [[nodiscard]] ws::core::Status load();

    // Write the json document atomically (the seam's temp + rename).
    [[nodiscard]] ws::core::Status save();

    [[nodiscard]] std::size_t cap() const noexcept { return cap_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

  private:
    struct Entry {
        std::string path;
        bool pinned = false;
    };

    [[nodiscard]] std::vector<Entry>* list_of(std::string_view doc_type);
    [[nodiscard]] const std::vector<Entry>* list_of(
        std::string_view doc_type) const;

    ws::settings::FileSystem& fs_;
    std::string path_;
    std::size_t cap_ = 0;
    // doc type -> entries, most recent first. A map: the saved file's
    // types come out alphabetical, the paths keep their MRU order.
    std::map<std::string, std::vector<Entry>> lists_;
};

}  // namespace ws::platform
