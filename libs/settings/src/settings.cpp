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

// ---- the layer-file bridge ---------------------------------------------

namespace {

// Prefix an inner validation failure with the load's key context,
// preserving its code, category and location (the inner failures are
// the store's own - only the message wants more context).
[[nodiscard]] ws::core::unexpected<ws::core::Error> wrap_key(
    std::string_view key, ws::core::Error inner) {
    inner.message = "load '" + std::string{key} + "': " + inner.message;
    return ws::core::unexpected<ws::core::Error>(std::move(inner));
}

// json -> the store's value model. Only the four scalar kinds map;
// arrays, objects and null have no scalar counterpart.
[[nodiscard]] ws::core::Result<SettingValue> json_to_setting(
    const json::Value& value) {
    switch (value.kind()) {
        case json::Kind::boolean:
            return SettingValue::boolean(value.get_bool().value_or(false));
        case json::Kind::integer:
            return SettingValue::integer(value.get_integer().value_or(0));
        case json::Kind::real:
            return SettingValue::real(value.get_real().value_or(0.0));
        case json::Kind::text:
            return SettingValue::text(*value.as_text());
        default:
            return fail(SettingsError::type_mismatch,
                        "json value is " +
                            std::string{json::kind_name(value.kind())} +
                            ", but a setting is one of the four scalar "
                            "types");
    }
}

// The store's value model -> json, kind for kind. Total: a SettingValue
// is always one of the four.
[[nodiscard]] json::Value setting_to_json(const SettingValue& value) {
    switch (value.type()) {
        case SettingType::boolean:
            return json::Value::boolean(value.as_bool().value_or(false));
        case SettingType::integer:
            return json::Value::integer(value.as_integer().value_or(0));
        case SettingType::real:
            return json::Value::real(value.as_real().value_or(0.0));
        default:
            return json::Value::text(value.as_text().value_or(""));
    }
}

}  // namespace

ws::core::Status SettingsStore::load_layer(Layer layer,
                                            const json::Value& document,
                                            const Migrations& migrations) {
    const auto slot = static_cast<std::size_t>(layer);
    if (slot == 0 || slot >= detail::kLayerCount) {
        return fail(SettingsError::invalid_argument,
                    "load: the schema owns the defaults layer; target "
                    "another one");
    }
    json::Value migrated = document;  // the caller's document stays intact
    WS_TRY(migrations.run(migrated, schema_.version()));
    const json::Object& root = *migrated.as_object();

    // Stage the whole file before touching the store, so a bad key
    // anywhere fails the load with the layer untouched.
    std::vector<std::pair<std::string, SettingValue>> staged;
    json::Object unknowns;
    for (const json::Object::Entry& entry : root) {
        if (entry.first == "version") {
            continue;  // the format's own field, never a setting
        }
        const SettingDesc* desc = schema_.find(entry.first);
        if (desc == nullptr) {
            unknowns.set(entry.first, entry.second);
            continue;
        }
        auto value = json_to_setting(entry.second);
        if (!value) {
            return wrap_key(entry.first, std::move(value.error()));
        }
        auto checked = coerce(*desc, std::move(*value));
        if (!checked) {
            return wrap_key(entry.first, std::move(checked.error()));
        }
        staged.emplace_back(entry.first, std::move(*checked));
    }

    // The file IS the layer: drop what the layer held, then apply.
    // Everything staged already passed validation, so these set_value
    // calls cannot fail - they run it again for the one-path rule (and
    // to land the clamped payload observers see).
    layers_[slot].clear();
    unknowns_[slot] = std::move(unknowns);
    for (auto& [key, value] : staged) {
        (void)set_value(layer, key, std::move(value));
    }
    return {};
}

ws::core::Result<json::Value> SettingsStore::save_layer(Layer layer) const {
    const auto slot = static_cast<std::size_t>(layer);
    if (slot == 0 || slot >= detail::kLayerCount) {
        return fail(SettingsError::invalid_argument,
                    "save: the schema owns the defaults layer; target "
                    "another one");
    }
    json::Object root;
    root.set("version", json::Value::integer(schema_.version()));
    // Known keys in map order (alphabetical): stable, if not the file's
    // original order.
    for (const auto& [key, value] : layers_[slot]) {
        root.set(key, setting_to_json(value));
    }
    // Unknown keys keep the order the file carried them in.
    for (const json::Object::Entry& entry : unknowns_[slot]) {
        root.set(entry.first, entry.second);
    }
    return json::Value::object(std::move(root));
}

}  // namespace ws::settings
