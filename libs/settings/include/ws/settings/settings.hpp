#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/settings/schema.hpp>
#include <ws/settings/value.hpp>

namespace ws::settings {

// The layers, lowest to highest priority. defaults is the schema's own -
// nothing ever writes into it - and the rest hold explicit values (a
// set() here, a loaded file there). The rank IS the precedence.
enum class Layer : std::int32_t {
    defaults = 0,
    system = 1,
    user = 2,
    project = 3,
    command_line = 4,
};

[[nodiscard]] constexpr std::string_view layer_name(Layer layer) noexcept {
    switch (layer) {
        case Layer::defaults: return "defaults";
        case Layer::system: return "system";
        case Layer::user: return "user";
        case Layer::project: return "project";
        case Layer::command_line: return "command line";
    }
    return "unknown";
}

namespace detail {

// One slot per Layer rank. Slot 0 (defaults) stays empty - the schema
// owns that layer.
inline constexpr std::size_t kLayerCount = 5;

}  // namespace detail

// What a get() hands back: the value and the layer it came from.
template <class T>
struct SettingRead {
    T value{};
    Layer layer = Layer::defaults;
};

// What every observer sees.
struct SettingChange {
    std::string key;
    SettingValue value;
    Layer layer = Layer::defaults;
};

using Observer = std::function<void(const SettingChange&)>;
using ObserverId = std::uint64_t;

// The layered store. Owner-thread state with no locks (the jobs wait_*
// rule): every call runs on one thread, and observers run on the thread
// that called set(). To land a change on a gui thread, marshal the set
// there first - ws::ui's App drains a ws::jobs::JobSystem once per
// frame, and a set posted through it lands (and notifies) between
// frames, gui-safe.
class SettingsStore {
public:
    explicit SettingsStore(Schema schema);
    SettingsStore(const SettingsStore&) = delete;
    SettingsStore& operator=(const SettingsStore&) = delete;

    // Read through the layers (command line -> project -> user ->
    // system), falling to the schema default. The answer carries the
    // winning layer. T is exactly bool / std::int64_t / double /
    // std::string.
    template <class T>
    [[nodiscard]] ws::core::Result<SettingRead<T>> get(
        std::string_view key) const {
        static_assert(detail::kSettingReadType<T>,
                      "get<T>: T must be bool, std::int64_t, double or "
                      "std::string");
        const SettingDesc* desc = schema_.find(key);
        if (desc == nullptr) {
            return fail(SettingsError::unknown_key,
                        "no such setting: " + std::string{key});
        }
        if (desc->type != detail::setting_type_of<T>()) {
            return fail(
                SettingsError::type_mismatch,
                "'" + std::string{key} + "' is " +
                    std::string{type_name(desc->type)} + ", read as " +
                    std::string{type_name(detail::setting_type_of<T>())});
        }
        const std::optional<std::pair<SettingValue, Layer>> stored =
            find_stored(key);
        if (stored.has_value()) {
            WS_TRY_ASSIGN(T value, stored->first.as<T>());
            return SettingRead<T>{std::move(value), stored->second};
        }
        WS_TRY_ASSIGN(T value, desc->default_value.as<T>());
        return SettingRead<T>{std::move(value), Layer::defaults};
    }

    // Validate and store into one layer. Constraints run in declaration
    // order (a clamp lands the clamped value); the defaults layer
    // refuses sets (the schema owns it); observers fire after the value
    // landed.
    template <class T>
    ws::core::Status set(Layer layer, std::string_view key, const T& value) {
        static_assert(detail::kSettingWriteType<std::decay_t<T>>,
                      "set: the value must be integral, floating point or a "
                      "string");
        return set_value(layer, key, detail::make_value(value));
    }
    ws::core::Status set_value(Layer layer, std::string_view key,
                                SettingValue value);

    // Which layer currently provides the key: the highest layer holding
    // an explicit value, else defaults. Unknown key -> unknown_key.
    [[nodiscard]] ws::core::Result<Layer> layer_of(std::string_view key) const;

    // True when THIS layer holds an explicit value. The defaults layer
    // never does (the schema owns it); an out-of-range layer answers
    // false.
    [[nodiscard]] bool has_value(Layer layer, std::string_view key) const;

    // Drop one layer's explicit value; the key falls through to the next
    // layer down. True when there was one.
    bool clear(Layer layer, std::string_view key);

    // Register an observer. A pattern matches a changed key when equal
    // to it, or when it ends in '.' and the key starts with it
    // ("editor." sees every editor.* set). Delivery: after the value
    // landed, in registration order, on the set() thread. Removal during
    // a notification is honored - the loop snapshots, then skips entries
    // removed before their turn; observers registered mid-notification
    // run from the NEXT change. An observer that sets another key runs
    // that key's observers recursively: do not build cycles.
    [[nodiscard]] ObserverId observe(std::string pattern, Observer observer);
    void remove_observer(ObserverId id);

    [[nodiscard]] const Schema& schema() const noexcept { return schema_; }

private:
    struct ObserverEntry {
        ObserverId id = 0;
        std::string pattern;
        Observer fn;
    };

    [[nodiscard]] std::optional<std::pair<SettingValue, Layer>> find_stored(
        std::string_view key) const;
    [[nodiscard]] bool observer_live(ObserverId id) const;
    void notify(const SettingChange& change);

    Schema schema_;
    std::map<std::string, SettingValue, std::less<>>
        layers_[detail::kLayerCount];
    std::vector<ObserverEntry> observers_;
    ObserverId next_observer_id_ = 1;
};

}  // namespace ws::settings
