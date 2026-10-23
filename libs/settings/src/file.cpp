#include <chrono>
#include <utility>

#include <ws/settings/errors.hpp>
#include <ws/settings/file.hpp>
#include <ws/settings/json.hpp>

namespace ws::settings {
namespace {

[[nodiscard]] std::int64_t steady_ms() {
    const auto since_epoch =
        std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch)
        .count();
}

}  // namespace

LayerFile::LayerFile(SettingsStore& store, Layer layer, FileSystem& fs,
                     std::string path, const Migrations& migrations,
                     Clock clock, std::int64_t quiet_ms)
    : store_(store),
      layer_(layer),
      fs_(fs),
      path_(std::move(path)),
      migrations_(migrations),
      clock_(std::move(clock)),
      quiet_ms_(quiet_ms) {}

std::int64_t LayerFile::now_ms() const {
    return clock_ ? clock_() : steady_ms();
}

ws::core::Result<FileLoad> LayerFile::load() {
    if (layer_ == Layer::defaults) {
        return fail(SettingsError::invalid_argument,
                    "load: the schema owns the defaults layer; target "
                    "another one");
    }
    FileLoad report;
    if (!fs_.exists(path_)) {
        report.missing = true;
        return report;
    }
    auto text = fs_.read(path_);
    if (!text) {
        // An existing file that cannot be read is an I/O problem
        // (locked, not corrupt) - propagate it, preserve nothing.
        return ws::core::unexpected<ws::core::Error>(std::move(text.error()));
    }
    auto document = json::parse(*text);
    if (!document) {
        return recover(std::move(document.error()));
    }
    ws::core::Status folded =
        store_.load_layer(layer_, *document, migrations_);
    if (!folded) {
        return recover(std::move(folded.error()));
    }
    return report;
}

ws::core::Result<FileLoad> LayerFile::recover(ws::core::Error reason) {
    FileLoad report;
    report.recovered = true;
    report.reason = std::move(reason);
    // Best-effort preserve: on failure the corrupt file stays put
    // (still reported) and the next successful save overwrites it.
    const std::string backup = path_ + ".bak";
    if (fs_.rename(path_, backup)) {
        report.backup = backup;
    }
    return report;
}

ws::core::Status LayerFile::save() {
    if (layer_ == Layer::defaults) {
        return fail(SettingsError::invalid_argument,
                    "save: the schema owns the defaults layer; target "
                    "another one");
    }
    auto document = store_.save_layer(layer_);
    if (!document) {
        return ws::core::unexpected<ws::core::Error>(
            std::move(document.error()));
    }
    return fs_.write(path_, json::write(*document));
}

void LayerFile::touch() {
    armed_ = true;
    deadline_ = now_ms() + quiet_ms_;
}

void LayerFile::cancel() { armed_ = false; }

bool LayerFile::pending() const noexcept { return armed_; }

ws::core::Status LayerFile::poll() {
    if (!armed_ || now_ms() < deadline_) {
        return {};
    }
    armed_ = false;
    return save();
}

ws::core::Status LayerFile::flush() {
    if (!armed_) {
        return {};
    }
    armed_ = false;
    return save();
}

}  // namespace ws::settings
