#include <ws/platform/autosave.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <ws/platform/errors.hpp>
#include <ws/platform/locations.hpp>
#include <ws/platform/paths.hpp>
#include <ws/settings/json.hpp>

namespace ws::platform {
namespace {

// FNV-1a 64 over the normalized document path - the gui id hasher's
// arithmetic, file-local here (gui_id lives inside ws::ui, the wrong
// direction to depend on).
[[nodiscard]] std::uint64_t fnv1a64(std::string_view text) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char c : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

// The store-dir file stem: the document's name for the human, a hash
// of its full normalized path for uniqueness - same-named documents
// in different folders never collide.
[[nodiscard]] std::string stem_of(std::string_view document) {
    std::string out = name_of(document);
    out += '-';
    static constexpr char kHex[] = "0123456789abcdef";
    const std::uint64_t hash = fnv1a64(document);
    for (int shift = 28; shift >= 0; shift -= 4) {
        out += kHex[(hash >> shift) & 0xF];
    }
    return out;
}

}  // namespace

Autosave::Autosave(ws::settings::FileSystem& fs, std::string document_path,
                   std::string store_dir, Clock clock, DirEnsurer ensure)
    : fs_{fs},
      document_{normalize_path(document_path)},
      clock_{std::move(clock)},
      ensure_{std::move(ensure)} {
    if (!ensure_) {
        // The seam has no mkdir verb; this lib's own ensure_directory
        // is the whole default (the WatchSource precedent for the
        // half the FileSystem leaves out).
        ensure_ = [](std::string_view dir) {
            return ensure_directory(dir);
        };
    }
    const std::string stem = stem_of(document_);
    autosave_path_ = join_path(store_dir, stem + ".autosave");
    marker_path_ = join_path(store_dir, stem + ".marker");
}

ws::core::Result<std::optional<Recovery>> Autosave::probe() const {
    if (!fs_.exists(marker_path_)) {
        return std::optional<Recovery>{};
    }
    const ws::core::Result<std::string> bytes = fs_.read(marker_path_);
    if (!bytes) {
        return fail(PlatformError::io_failed,
                    "autosave: read marker '" + marker_path_ + "': " +
                        bytes.error().message);
    }
    const ws::core::Result<ws::settings::json::Value> parsed =
        ws::settings::json::parse(*bytes);
    if (!parsed) {
        return fail(PlatformError::corrupt_file,
                    "autosave: marker '" + marker_path_ + "': " +
                        parsed.error().message);
    }
    const ws::settings::json::Value& root = *parsed;
    if (!root.is_object()) {
        return fail(PlatformError::corrupt_file,
                    "autosave: marker '" + marker_path_ +
                        "' is not an object");
    }
    if (const auto* v = root.as_object()->find("version")) {
        // Shape 1 only, like the recent-files store; an absent version
        // reads as 1 (hand-written files).
        bool shape_ok = false;
        if (const ws::core::Result<std::int64_t> shape = v->get_integer()) {
            shape_ok = *shape == std::int64_t{1};
        }
        if (!shape_ok) {
            return fail(PlatformError::corrupt_file,
                        "autosave: marker '" + marker_path_ +
                            "' has an unsupported shape");
        }
    }
    const auto* document = root.as_object()->find("document");
    const auto* autosave = root.as_object()->find("autosave");
    const auto* saved = root.as_object()->find("saved_ms");
    if (document == nullptr || !document->is_text() ||
        autosave == nullptr || !autosave->is_text() ||
        saved == nullptr || !saved->is_integer()) {
        return fail(PlatformError::corrupt_file,
                    "autosave: marker '" + marker_path_ +
                        "' is missing its fields");
    }
    Recovery out;
    out.document = *document->as_text();
    out.autosave = *autosave->as_text();
    out.saved_ms = *saved->get_integer();
    return out;
}

ws::core::Result<std::string> Autosave::recover() {
    if (!fs_.exists(autosave_path_)) {
        return fail(PlatformError::invalid_argument,
                    "autosave: nothing to recover");
    }
    const ws::core::Result<std::string> bytes = fs_.read(autosave_path_);
    if (!bytes) {
        return fail(PlatformError::io_failed,
                    "autosave: read '" + autosave_path_ + "': " +
                        bytes.error().message);
    }
    // The content is in hand before anything is cleared: a failure
    // below leaves the pair as it was, still recoverable.
    if (!fs_.remove(marker_path_)) {
        return fail(PlatformError::io_failed,
                    "autosave: remove marker '" + marker_path_ + "'");
    }
    if (!fs_.remove(autosave_path_)) {
        return fail(PlatformError::io_failed,
                    "autosave: remove '" + autosave_path_ + "'");
    }
    return *bytes;
}

ws::core::Status Autosave::discard() {
    if (!fs_.remove(marker_path_)) {
        return fail(PlatformError::io_failed,
                    "autosave: remove marker '" + marker_path_ + "'");
    }
    if (!fs_.remove(autosave_path_)) {
        return fail(PlatformError::io_failed,
                    "autosave: remove '" + autosave_path_ + "'");
    }
    return {};
}

ws::core::Status Autosave::snapshot(std::string_view bytes) {
    // The store dir may not exist on the first snapshot - the caller
    // hands a config path, not a made one.
    WS_TRY(ensure_(parent_of(autosave_path_)));
    // Content first, marker last: the marker is the commit point, so
    // a crash between the two leaves an orphan content file nothing
    // claims, and a failed write never promises a recovery.
    WS_TRY(fs_.write(autosave_path_, bytes));
    return write_marker();
}

ws::core::Status Autosave::finish() {
    // Marker first: the crash window leaves an orphan content file
    // (probe: none), never a marker promising content that is gone.
    if (!fs_.remove(marker_path_)) {
        return fail(PlatformError::io_failed,
                    "autosave: remove marker '" + marker_path_ + "'");
    }
    if (!fs_.remove(autosave_path_)) {
        return fail(PlatformError::io_failed,
                    "autosave: remove '" + autosave_path_ + "'");
    }
    return {};
}

ws::core::Status Autosave::write_marker() {
    namespace json = ws::settings::json;
    json::Object doc;
    doc.set("version", json::Value::integer(1));
    doc.set("document", json::Value::text(document_));
    doc.set("autosave", json::Value::text(autosave_path_));
    doc.set("saved_ms", json::Value::integer(now_ms()));
    return fs_.write(marker_path_,
                     json::write(json::Value::object(std::move(doc))));
}

std::int64_t Autosave::now_ms() const {
    if (clock_) {
        return clock_();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace ws::platform
