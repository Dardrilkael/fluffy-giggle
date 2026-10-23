#include <algorithm>
#include <utility>

#include <ws/settings/settings.hpp>

namespace ws::settings {
namespace {

// A pattern matches a changed key when equal to it, or when it ends in
// '.' and the key starts with it (a dot-boundary prefix: "editor." sees
// editor.font.size but not editorx).
[[nodiscard]] bool pattern_matches(std::string_view pattern,
                                   std::string_view key) {
    return pattern == key ||
           (!pattern.empty() && pattern.back() == '.' && key.starts_with(pattern));
}

}  // namespace

SettingsStore::SettingsStore(Schema schema) : schema_(std::move(schema)) {}

ws::core::Status SettingsStore::set_value(Layer layer, std::string_view key,
                                           SettingValue value) {
    const auto slot = static_cast<std::size_t>(layer);
    if (slot == 0 || slot >= detail::kLayerCount) {
        return fail(SettingsError::invalid_argument,
                    "set: the schema owns the defaults layer; target "
                    "another one");
    }
    const SettingDesc* desc = schema_.find(key);
    if (desc == nullptr) {
        return fail(SettingsError::unknown_key,
                    "no such setting: " + std::string{key});
    }
    WS_TRY_ASSIGN(auto checked, coerce(*desc, std::move(value)));
    const std::string owned_key{key};
    const auto [stored, inserted] =
        layers_[slot].insert_or_assign(owned_key, std::move(checked));
    (void)inserted;
    // Build the payload from the STORED value so observers see exactly
    // what a get() would answer.
    const SettingChange change{owned_key, stored->second, layer};
    notify(change);
    return {};
}

std::optional<std::pair<SettingValue, Layer>> SettingsStore::find_stored(
    std::string_view key) const {
    // Highest rank first; slot 0 (defaults) is never in the maps.
    for (std::size_t rank = detail::kLayerCount - 1; rank >= 1; --rank) {
        const auto it = layers_[rank].find(key);
        if (it != layers_[rank].end()) {
            return std::pair{it->second, static_cast<Layer>(rank)};
        }
    }
    return std::nullopt;
}

ws::core::Result<Layer> SettingsStore::layer_of(std::string_view key) const {
    if (schema_.find(key) == nullptr) {
        return fail(SettingsError::unknown_key,
                    "no such setting: " + std::string{key});
    }
    if (const auto stored = find_stored(key)) {
        return stored->second;
    }
    return Layer::defaults;
}

bool SettingsStore::has_value(Layer layer, std::string_view key) const {
    const auto slot = static_cast<std::size_t>(layer);
    if (slot == 0 || slot >= detail::kLayerCount) {
        return false;
    }
    return layers_[slot].find(key) != layers_[slot].end();
}

bool SettingsStore::clear(Layer layer, std::string_view key) {
    const auto slot = static_cast<std::size_t>(layer);
    if (slot == 0 || slot >= detail::kLayerCount) {
        return false;
    }
    // erase() takes the owned key (no heterogeneous overload until
    // C++23); clear() is a rare interactive act, the one allocation is
    // fine.
    return layers_[slot].erase(std::string{key}) > 0;
}

ObserverId SettingsStore::observe(std::string pattern, Observer observer) {
    observers_.push_back(
        ObserverEntry{next_observer_id_, std::move(pattern), std::move(observer)});
    return next_observer_id_++;
}

void SettingsStore::remove_observer(ObserverId id) {
    std::erase_if(observers_,
                  [id](const ObserverEntry& entry) { return entry.id == id; });
}

bool SettingsStore::observer_live(ObserverId id) const {
    return std::any_of(observers_.begin(), observers_.end(),
                       [id](const ObserverEntry& entry) {
                           return entry.id == id;
                       });
}

void SettingsStore::notify(const SettingChange& change) {
    // Snapshot: an observer may remove another (or itself) mid-flight,
    // and the loop honors each removal by liveness-checking every entry
    // before its turn. Observers registered from inside the snapshot are
    // not in it - they run from the next change.
    const std::vector<ObserverEntry> snapshot = observers_;
    for (const ObserverEntry& entry : snapshot) {
        if (!observer_live(entry.id)) {
            continue;
        }
        if (!pattern_matches(entry.pattern, change.key)) {
            continue;
        }
        entry.fn(change);
    }
}

}  // namespace ws::settings
