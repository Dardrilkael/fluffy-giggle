// The SAF port's pure half (see saf_store.hpp) - no Android type, no
// JNI, no OS call: the policy the bridge rides, headless-testable.

#include "saf_store.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace editor::saf {

namespace {

// The name cap: a display name's sane budget, before the "-<hash8>"
// splice and the extension - long enough for any human name, short
// enough for any filesystem (255's own limit minus room to spare).
constexpr std::size_t kNameCap = 80;

[[nodiscard]] bool is_path_byte(char c) {
    return c == '/' || c == '\\';
}

[[nodiscard]] bool is_control_byte(char c) {
    const auto raw = static_cast<unsigned char>(c);
    return raw < 0x20 || raw == 0x7F;
}

}  // namespace

// --- the map ----------------------------------------------------------------

void SafMap::load(std::string_view bytes) {
    entries_.clear();
    std::size_t at = 0;
    while (at <= bytes.size()) {
        const std::size_t end = bytes.find('\n', at);
        const std::string_view line =
            end == std::string_view::npos
                ? bytes.substr(at)
                : bytes.substr(at, end - at);
        const std::size_t tab = line.find('\t');
        if (tab != std::string_view::npos && tab > 0 &&
            tab + 1 < line.size()) {
            add(std::string{line.substr(0, tab)},
                std::string{line.substr(tab + 1)});
        }
        if (end == std::string_view::npos) {
            break;
        }
        at = end + 1;
    }
}

std::string SafMap::format() const {
    std::string out;
    for (const SafEntry& entry : entries_) {
        out += entry.path;
        out += '\t';
        out += entry.uri;
        out += '\n';
    }
    return out;
}

void SafMap::add(std::string path, std::string uri) {
    // The newest pick wins: a re-picked document re-points its sandbox
    // file (the same name-for-URI lands on the same path).
    remove(path);
    entries_.push_back(SafEntry{std::move(path), std::move(uri)});
}

void SafMap::remove(std::string_view path) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [path](const SafEntry& entry) {
                                      return entry.path == path;
                                  }),
                   entries_.end());
}

const std::string* SafMap::find(std::string_view path) const {
    for (const SafEntry& entry : entries_) {
        if (entry.path == path) {
            return &entry.uri;
        }
    }
    return nullptr;
}

// --- the naming -------------------------------------------------------------

std::string hash_tag(std::string_view bytes) {
    std::uint64_t hash = 1469598103934665603ull;  // FNV-1a 64's offset
    for (const char byte : bytes) {
        hash ^= static_cast<std::uint64_t>(
            static_cast<unsigned char>(byte));
        hash *= 1099511628211ull;  // the FNV-1a 64 prime
    }
    char out[9];
    for (int at = 7; at >= 0; --at) {
        out[at] = "0123456789abcdef"[static_cast<std::size_t>(hash & 0xF)];
        hash >>= 4;
    }
    out[8] = '\0';
    return std::string{out, 8};
}

std::string sandbox_name_for(std::string_view display, std::string_view uri,
                             std::string_view fallback) {
    // Sanitize: path bytes would escape the sandbox, control bytes
    // would corrupt the map's line format, and surrounding whitespace
    // is noise a picked name sometimes carries.
    std::string clean;
    for (const char c : display) {
        if (is_path_byte(c) || is_control_byte(c)) {
            continue;
        }
        clean += c;
    }
    const std::size_t first = clean.find_first_not_of(" \t");
    const std::size_t last = clean.find_last_not_of(" \t");
    if (first == std::string::npos) {
        clean.clear();
    } else {
        clean = clean.substr(first, last - first + 1);
    }
    if (clean.empty()) {
        clean.assign(fallback);
    }
    // The extension survives the splice; a bare name earns ".csim"
    // (the document factory's own gate reads the extension, so a
    // picked "notes.txt" must STAY notes.txt to be refused honestly).
    std::string_view stem = clean;
    std::string_view extension;
    const std::size_t dot = clean.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        stem = std::string_view{clean}.substr(0, dot);
        extension = std::string_view{clean}.substr(dot);
    } else {
        extension = ".csim";
    }
    if (stem.size() > kNameCap) {
        stem = stem.substr(0, kNameCap);
    }
    std::string out{stem};
    out += '-';
    out += hash_tag(uri);
    out += extension;
    return out;
}

// --- the write-through seam ---------------------------------------------------

WriteThroughFs::WriteThroughFs(ws::settings::FileSystem& base, SafMap& map,
                               SafPush push, std::string map_path)
    : base_(base), map_(map), push_(std::move(push)),
      map_path_(std::move(map_path)) {}

ws::core::Result<std::string> WriteThroughFs::read(
    std::string_view path) const {
    return base_.read(path);  // a mapped path always has the local copy
}

ws::core::Status WriteThroughFs::write(std::string_view path,
                                       std::string_view bytes) {
    // Local first, through the base seam's own atomic contract (the
    // .tmp + rename) - the durable copy the session restores from.
    const ws::core::Status st = base_.write(path, bytes);
    if (!st) {
        return st;
    }
    const std::lock_guard lock{mutex_};
    const std::string* uri = map_.find(path);
    if (uri == nullptr || push_ == nullptr) {
        return st;  // an ordinary file: nothing to mirror
    }
    if (const ws::core::Status pushed = push_(*uri, bytes); !pushed) {
        // The local save stands; the user learns the target refused -
        // this is "saved locally", never a lost save.
        push_error_ = pushed.error().message;
    }
    return st;
}

ws::core::Status WriteThroughFs::rename(std::string_view from,
                                        std::string_view to) {
    const ws::core::Status st = base_.rename(from, to);
    if (st) {
        const std::lock_guard lock{mutex_};
        if (map_.find(from) != nullptr) {
            // Nothing in the editor renames documents; the seam's atomic
            // writes rename .tmp files (never mapped). If something ever
            // does, the honest answer is to stop mirroring: the moved file
            // is a local fork until it is re-picked.
            map_.remove(from);
            save_map();
        }
    }
    return st;
}

bool WriteThroughFs::remove(std::string_view path) {
    const bool gone = base_.remove(path);
    {
        const std::lock_guard lock{mutex_};
        if (map_.find(path) != nullptr) {
            // The local copy is gone; the SAF document stays (the seam has
            // no delete verb, and an unpicked document is the user's to
            // manage in their provider).
            map_.remove(path);
            save_map();
        }
    }
    return gone;
}

bool WriteThroughFs::exists(std::string_view path) const {
    return base_.exists(path);
}

void WriteThroughFs::map(std::string path, std::string uri) {
    const std::lock_guard lock{mutex_};
    map_.add(std::move(path), std::move(uri));
    save_map();
}

std::optional<std::string> WriteThroughFs::take_push_error() {
    const std::lock_guard lock{mutex_};
    std::optional<std::string> out;
    out.swap(push_error_);
    return out;
}

void WriteThroughFs::save_map() {
    (void)base_.write(map_path_, map_.format());
}

}  // namespace editor::saf
