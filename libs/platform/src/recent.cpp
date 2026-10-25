#include <ws/platform/recent.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <utility>

#include <ws/platform/errors.hpp>
#include <ws/platform/paths.hpp>
#include <ws/settings/json.hpp>

namespace ws::platform {

RecentFiles::RecentFiles(ws::settings::FileSystem& fs, std::string path,
                         std::size_t cap)
    : fs_{fs}, path_{std::move(path)}, cap_{cap} {}

std::vector<RecentFiles::Entry>* RecentFiles::list_of(
    std::string_view doc_type) {
    const auto it = lists_.find(std::string{doc_type});
    return it == lists_.end() ? nullptr : &it->second;
}

const std::vector<RecentFiles::Entry>* RecentFiles::list_of(
    std::string_view doc_type) const {
    const auto it = lists_.find(std::string{doc_type});
    return it == lists_.end() ? nullptr : &it->second;
}

bool RecentFiles::touch(std::string_view doc_type, std::string_view path) {
    const std::string norm = normalize_path(path);
    if (norm.empty()) {
        return false;
    }
    std::vector<Entry>& entries = lists_[std::string{doc_type}];

    // Promote: an existing entry keeps its pin, wherever it sat.
    bool kept_pin = false;
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [&](const Entry& e) { return e.path == norm; });
    if (it != entries.end()) {
        kept_pin = it->pinned;
        entries.erase(it);
    }
    entries.insert(entries.begin(), Entry{norm, kept_pin});

    // Over the cap: the oldest unpinned tail entry falls off. The new
    // head is never its own eviction - when every prior entry is
    // pinned there is nowhere to make room, the new touch is refused,
    // the list exactly as it was.
    while (entries.size() > cap_) {
        // Scan the tail down to - but never past - the new head.
        const auto oldest_unpinned = std::find_if(
            entries.rbegin(), std::prev(entries.rend()),
            [](const Entry& e) { return !e.pinned; });
        if (oldest_unpinned == std::prev(entries.rend())) {
            entries.erase(entries.begin());
            return false;
        }
        entries.erase(std::next(oldest_unpinned).base());
    }
    return true;
}

bool RecentFiles::touch(std::string_view path) {
    return touch(std::string_view{}, path);
}

bool RecentFiles::pin(std::string_view doc_type, std::string_view path) {
    std::vector<Entry>* entries = list_of(doc_type);
    if (entries == nullptr) {
        return false;
    }
    const std::string norm = normalize_path(path);
    const auto it = std::find_if(entries->begin(), entries->end(),
                                 [&](const Entry& e) { return e.path == norm; });
    if (it == entries->end()) {
        return false;
    }
    it->pinned = true;
    return true;
}

bool RecentFiles::unpin(std::string_view doc_type, std::string_view path) {
    std::vector<Entry>* entries = list_of(doc_type);
    if (entries == nullptr) {
        return false;
    }
    const std::string norm = normalize_path(path);
    const auto it = std::find_if(entries->begin(), entries->end(),
                                 [&](const Entry& e) { return e.path == norm; });
    if (it == entries->end()) {
        return false;
    }
    it->pinned = false;
    return true;
}

std::vector<RecentItem> RecentFiles::menu_items(std::string_view doc_type) {
    std::vector<Entry>* entries = list_of(doc_type);
    if (entries == nullptr) {
        return {};
    }
    // The lazy prune: unpinned entries whose file is gone. The
    // FileSystem's exists answers false for an undeterminable path,
    // which reads as gone - the seam's contract, inherited.
    std::erase_if(*entries, [&](const Entry& e) {
        return !e.pinned && !fs_.exists(e.path);
    });

    std::vector<RecentItem> out;
    out.reserve(entries->size());
    for (const Entry& entry : *entries) {
        RecentItem item;
        item.path = entry.path;
        item.pinned = entry.pinned;
        const std::string name = name_of(entry.path);
        // The first same-named file wears the bare name; every later
        // sibling disambiguates with its parent directory.
        const bool dup = std::any_of(out.begin(), out.end(),
                                     [&](const RecentItem& made) {
                                         return made.label == name;
                                     });
        item.label = dup ? name + " (" +
                               name_of(parent_of(entry.path)) + ")"
                         : name;
        out.push_back(std::move(item));
    }
    return out;
}

ws::core::Status RecentFiles::load() {
    if (!fs_.exists(path_)) {
        return {};  // the first run
    }
    const ws::core::Result<std::string> bytes = fs_.read(path_);
    if (!bytes) {
        return fail(PlatformError::io_failed,
                    "recent files: read '" + path_ + "': " +
                        bytes.error().message);
    }
    const ws::core::Result<ws::settings::json::Value> parsed =
        ws::settings::json::parse(*bytes);
    if (!parsed) {
        return fail(PlatformError::parse_failed,
                    "recent files: " + parsed.error().message);
    }
    const ws::settings::json::Value& root = *parsed;
    if (!root.is_object()) {
        return fail(PlatformError::parse_failed,
                    "recent files: the root is not an object");
    }
    if (const auto* v = root.as_object()->find("version")) {
        // Shape 1 only: a newer shape is refused, never downgraded;
        // an absent version reads as 1 (hand-written files).
        bool shape_ok = false;
        if (const ws::core::Result<std::int64_t> shape = v->get_integer()) {
            shape_ok = *shape == std::int64_t{1};
        }
        if (!shape_ok) {
            return fail(PlatformError::parse_failed,
                        "recent files: unsupported shape");
        }
    }

    std::map<std::string, std::vector<Entry>> loaded;
    if (const auto* lists = root.as_object()->find("lists")) {
        if (!lists->is_object()) {
            return fail(PlatformError::parse_failed,
                        "recent files: 'lists' is not an object");
        }
        for (const auto& [type, typed] : *lists->as_object()) {
            if (!typed.is_object()) {
                return fail(PlatformError::parse_failed,
                            "recent files: list '" + type +
                                "' is not an object");
            }
            std::vector<Entry> entries;
            if (const auto* paths = typed.as_object()->find("paths")) {
                if (!paths->is_array()) {
                    return fail(PlatformError::parse_failed,
                                "recent files: list '" + type +
                                    "' paths is not an array");
                }
                for (const ws::settings::json::Value& p :
                     *paths->as_array()) {
                    if (!p.is_text()) {
                        return fail(PlatformError::parse_failed,
                                    "recent files: list '" + type +
                                        "' holds a non-string path");
                    }
                    entries.push_back(
                        Entry{normalize_path(*p.as_text()), false});
                }
            }
            if (const auto* pinned = typed.as_object()->find("pinned")) {
                if (!pinned->is_array()) {
                    return fail(PlatformError::parse_failed,
                                "recent files: list '" + type +
                                    "' pinned is not an array");
                }
                for (const ws::settings::json::Value& p :
                     *pinned->as_array()) {
                    if (!p.is_text()) {
                        return fail(PlatformError::parse_failed,
                                    "recent files: list '" + type +
                                        "' holds a non-string pin");
                    }
                    const std::string pin = normalize_path(*p.as_text());
                    const auto it = std::find_if(
                        entries.begin(), entries.end(),
                        [&](const Entry& e) { return e.path == pin; });
                    if (it != entries.end()) {
                        it->pinned = true;
                    }
                }
            }
            loaded.emplace(std::string{type}, std::move(entries));
        }
    }
    // A load replaces what was in memory, all or nothing (the
    // layer-file precedent).
    lists_ = std::move(loaded);
    return {};
}

ws::core::Status RecentFiles::save() {
    namespace json = ws::settings::json;
    json::Object doc;
    doc.set("version", json::Value::integer(1));

    json::Object lists;
    for (const auto& [type, entries] : lists_) {
        json::Object typed;
        json::Array paths;
        json::Array pinned;
        for (const Entry& entry : entries) {
            paths.push_back(json::Value::text(entry.path));
            if (entry.pinned) {
                pinned.push_back(json::Value::text(entry.path));
            }
        }
        typed.set("paths", json::Value::array(std::move(paths)));
        typed.set("pinned", json::Value::array(std::move(pinned)));
        lists.set(type, json::Value::object(std::move(typed)));
    }
    doc.set("lists", json::Value::object(std::move(lists)));
    return fs_.write(path_, json::write(json::Value::object(std::move(doc))));
}

}  // namespace ws::platform
