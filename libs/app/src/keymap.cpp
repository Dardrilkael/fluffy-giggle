#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include <ws/app/keymap.hpp>
#include <ws/settings/json.hpp>

namespace ws::app {
namespace {

// The document shape this code reads and writes (the layer-file
// rule): a file with no "version" reads as shape 1, and any other
// shape is refused - never downgraded.
constexpr std::int64_t kShape = 1;

// Json that parsed but is not a keymap. The split from parse_failed:
// a grammar error is app/4, a well-formed document of the wrong
// shape is app/5.
[[nodiscard]] ws::core::Status corrupt(std::string what) {
    return fail(AppError::corrupt_file, "keymap: " + std::move(what));
}

// One chord object: { "key": 83, "mods": 2 }. "mods" is optional
// (absent = none); both fields are bounded to what a Shortcut holds.
[[nodiscard]] ws::core::Status parse_chord(const ws::settings::json::Value& value,
                                           const std::string& where, Shortcut& out) {
    const ws::settings::json::Object* object = value.as_object();
    if (object == nullptr) {
        return corrupt(where + ": the chord is not an object");
    }
    const ws::settings::json::Value* key = object->find("key");
    if (key == nullptr) {
        return corrupt(where + ": the chord has no key");
    }
    const ws::core::Result<std::int64_t> key_value = key->get_integer();
    if (!key_value.has_value()) {
        return corrupt(where + ": the chord key is not an integer");
    }
    if (*key_value <= 0 || *key_value > std::numeric_limits<int>::max()) {
        return corrupt(where + ": the chord key " + std::to_string(*key_value) +
                       " is out of range");
    }
    std::int64_t mods = 0;
    if (const ws::settings::json::Value* mods_value = object->find("mods");
        mods_value != nullptr) {
        const ws::core::Result<std::int64_t> mods_int = mods_value->get_integer();
        if (!mods_int.has_value()) {
            return corrupt(where + ": the chord mods is not an integer");
        }
        mods = *mods_int;
    }
    if (mods < 0 || mods > 15) {
        return corrupt(where + ": the chord mods " + std::to_string(mods) +
                       " is not a modifier mask");
    }
    out.key = static_cast<int>(*key_value);
    out.mods = static_cast<Mod>(static_cast<std::uint32_t>(mods));
    return {};
}

// One context's table: { "bindings": [...], "sequences": [...] }.
// Both keys are optional (absent = an empty context); entries
// validate in file order and the first refusal wins.
[[nodiscard]] ws::core::Status parse_table(const ws::settings::json::Value& value,
                                           const std::string& name,
                                           ShortcutTable& out) {
    const ws::settings::json::Object* object = value.as_object();
    if (object == nullptr) {
        return corrupt("the '" + name + "' context is not an object");
    }
    if (const ws::settings::json::Value* bindings = object->find("bindings");
        bindings != nullptr && !bindings->is_null()) {
        const ws::settings::json::Array* array = bindings->as_array();
        if (array == nullptr) {
            return corrupt("the '" + name + "' bindings is not an array");
        }
        for (std::size_t i = 0; i < array->size(); ++i) {
            const std::string where =
                "binding " + std::to_string(i) + " in '" + name + "'";
            const ws::settings::json::Object* binding = (*array)[i].as_object();
            if (binding == nullptr) {
                return corrupt(where + " is not an object");
            }
            const ws::settings::json::Value* command = binding->find("command");
            if (command == nullptr) {
                return corrupt(where + " has no command id");
            }
            const ws::core::Result<std::string> command_id = command->get_text();
            if (!command_id.has_value() || command_id->empty()) {
                return corrupt(where + " has no usable command id");
            }
            const ws::settings::json::Value* chord = binding->find("chord");
            if (chord == nullptr) {
                return corrupt(where + " has no chord");
            }
            Shortcut parsed;
            if (const ws::core::Status chord_ok = parse_chord(*chord, where, parsed);
                !chord_ok.has_value()) {
                return chord_ok;
            }
            out.bindings.push_back(ShortcutBinding{parsed, *command_id});
        }
    }
    if (const ws::settings::json::Value* sequences = object->find("sequences");
        sequences != nullptr && !sequences->is_null()) {
        const ws::settings::json::Array* array = sequences->as_array();
        if (array == nullptr) {
            return corrupt("the '" + name + "' sequences is not an array");
        }
        for (std::size_t i = 0; i < array->size(); ++i) {
            const std::string where =
                "sequence " + std::to_string(i) + " in '" + name + "'";
            const ws::settings::json::Object* sequence = (*array)[i].as_object();
            if (sequence == nullptr) {
                return corrupt(where + " is not an object");
            }
            const ws::settings::json::Value* command = sequence->find("command");
            if (command == nullptr) {
                return corrupt(where + " has no command id");
            }
            const ws::core::Result<std::string> command_id = command->get_text();
            if (!command_id.has_value() || command_id->empty()) {
                return corrupt(where + " has no usable command id");
            }
            const ws::settings::json::Value* first = sequence->find("first");
            if (first == nullptr) {
                return corrupt(where + " has no first chord");
            }
            Shortcut first_chord;
            if (const ws::core::Status first_ok =
                    parse_chord(*first, where + " first", first_chord);
                !first_ok.has_value()) {
                return first_ok;
            }
            const ws::settings::json::Value* second = sequence->find("second");
            if (second == nullptr) {
                return corrupt(where + " has no second chord");
            }
            Shortcut second_chord;
            if (const ws::core::Status second_ok =
                    parse_chord(*second, where + " second", second_chord);
                !second_ok.has_value()) {
                return second_ok;
            }
            const ws::settings::json::Value* timeout = sequence->find("timeout_ms");
            if (timeout == nullptr) {
                return corrupt(where + " has no timeout_ms");
            }
            const ws::core::Result<std::int64_t> timeout_ms = timeout->get_integer();
            if (!timeout_ms.has_value()) {
                return corrupt(where + " timeout_ms is not an integer");
            }
            if (*timeout_ms <= 0) {
                return corrupt(where + " timeout_ms " + std::to_string(*timeout_ms) +
                               " is not positive");
            }
            out.sequences.push_back(
                ShortcutSequence{first_chord, second_chord, *command_id,
                                 *timeout_ms});
        }
    }
    return {};
}

// The writer's chord object - both fields always present, so a save
// and any hand-copy of it re-read identically.
[[nodiscard]] ws::settings::json::Value chord_value(Shortcut chord) {
    ws::settings::json::Object object;
    object.set("key", ws::settings::json::Value::integer(chord.key));
    object.set(
        "mods",
        ws::settings::json::Value::integer(static_cast<std::int64_t>(chord.mods)));
    return ws::settings::json::Value::object(std::move(object));
}

}  // namespace

Keymap::Keymap(ws::settings::FileSystem& fs, std::string path)
    : fs_(fs), path_(std::move(path)) {}

ws::core::Status Keymap::load(ShortcutResolver& resolver) {
    if (!fs_.exists(path_)) {
        return {};  // the first run: the defaults seed stands
    }
    const ws::core::Result<std::string> bytes = fs_.read(path_);
    if (!bytes.has_value()) {
        return fail(AppError::io_failed,
                    "keymap: read '" + path_ + "': " + bytes.error().message);
    }
    return import_text(*bytes, resolver);
}

ws::core::Status Keymap::save(const ShortcutResolver& resolver) {
    return fs_.write(path_, export_text(resolver));
}

std::string Keymap::export_text(const ShortcutResolver& resolver) const {
    struct Rung {
        const char* name;
        KeyContext context;
    };
    static constexpr Rung rungs[] = {
        {"global", KeyContext::global},
        {"panel", KeyContext::panel},
        {"text_editing", KeyContext::text_editing},
    };

    // Every context and both lists always present, in rung order:
    // save -> load -> save re-emits byte-identical text (install
    // keeps table order, the json object keeps insertion order).
    ws::settings::json::Object root;
    root.set("version", ws::settings::json::Value::integer(kShape));
    ws::settings::json::Object contexts;
    for (const Rung& rung : rungs) {
        const ShortcutTable table = resolver.table_of(rung.context);
        ws::settings::json::Object context;
        ws::settings::json::Array bindings;
        for (const ShortcutBinding& binding : table.bindings) {
            ws::settings::json::Object entry;
            entry.set("command", ws::settings::json::Value::text(binding.command));
            entry.set("chord", chord_value(binding.chord));
            bindings.push_back(ws::settings::json::Value::object(std::move(entry)));
        }
        context.set("bindings",
                    ws::settings::json::Value::array(std::move(bindings)));
        ws::settings::json::Array sequences;
        for (const ShortcutSequence& sequence : table.sequences) {
            ws::settings::json::Object entry;
            entry.set("command",
                      ws::settings::json::Value::text(sequence.command));
            entry.set("first", chord_value(sequence.first));
            entry.set("second", chord_value(sequence.second));
            entry.set("timeout_ms",
                      ws::settings::json::Value::integer(sequence.timeout_ms));
            sequences.push_back(ws::settings::json::Value::object(std::move(entry)));
        }
        context.set("sequences",
                    ws::settings::json::Value::array(std::move(sequences)));
        contexts.set(rung.name,
                      ws::settings::json::Value::object(std::move(context)));
    }
    root.set("contexts", ws::settings::json::Value::object(std::move(contexts)));
    return ws::settings::json::write(ws::settings::json::Value::object(std::move(root)));
}

ws::core::Status Keymap::import_text(std::string_view text,
                                     ShortcutResolver& resolver) {
    const ws::core::Result<ws::settings::json::Value> parsed =
        ws::settings::json::parse(text);
    if (!parsed.has_value()) {
        return fail(AppError::parse_failed, "keymap: " + parsed.error().message);
    }
    const ws::settings::json::Object* root = parsed->as_object();
    if (root == nullptr) {
        return corrupt("the document is not an object");
    }
    std::int64_t shape = kShape;  // no "version" reads as shape 1 (the layer-file rule)
    if (const ws::settings::json::Value* version = root->find("version");
        version != nullptr) {
        const ws::core::Result<std::int64_t> version_int = version->get_integer();
        if (!version_int.has_value()) {
            return corrupt("the version is not an integer");
        }
        shape = *version_int;
    }
    if (shape != kShape) {
        return corrupt("keymap shape " + std::to_string(shape) +
                       " is not supported (this build reads shape 1)");
    }
    // Parsed whole into scratch tables, then handed to install()'s
    // all-or-nothing rules: a refusal anywhere leaves the resolver
    // exactly as it was.
    ShortcutTable tables[3];  // indexed by KeyContext (global, panel, text_editing)
    if (const ws::settings::json::Value* contexts = root->find("contexts");
        contexts != nullptr && !contexts->is_null()) {
        const ws::settings::json::Object* contexts_object = contexts->as_object();
        if (contexts_object == nullptr) {
            return corrupt("contexts is not an object");
        }
        for (const ws::settings::json::Object::Entry& entry : *contexts_object) {
            int rung = -1;
            if (entry.first == "global") {
                rung = 0;
            } else if (entry.first == "panel") {
                rung = 1;
            } else if (entry.first == "text_editing") {
                rung = 2;
            } else {
                continue;  // a context this version does not know: forward data
            }
            const ws::core::Status parsed_ok =
                parse_table(entry.second, entry.first, tables[static_cast<std::size_t>(rung)]);
            if (!parsed_ok.has_value()) {
                return parsed_ok;
            }
        }
    }
    return resolver.install(tables[0], tables[1], tables[2]);
}

}  // namespace ws::app
