#include <string>
#include <utility>

#include <ws/settings/migration.hpp>

namespace ws::settings {

ws::core::Status Migrations::add(std::int64_t from, MigrationStep step) {
    if (from < 1) {
        return fail(SettingsError::invalid_argument,
                    "a migration step walks from shape 1 or later, got " +
                        std::to_string(from));
    }
    if (!step) {
        return fail(SettingsError::invalid_argument,
                    "a migration step must be callable");
    }
    if (steps_.contains(from)) {
        return fail(SettingsError::invalid_argument,
                    "a step from shape " + std::to_string(from) +
                        " is already registered");
    }
    steps_.emplace(from, std::move(step));
    return {};
}

ws::core::Status Migrations::run(json::Value& document,
                                  std::int64_t current) const {
    json::Object* root = document.as_object();
    if (root == nullptr) {
        return fail(SettingsError::corrupt_file,
                    "the layer file root is not an object");
    }
    std::int64_t version = 1;  // a pre-versioning file reads as shape 1
    if (const json::Value* declared = root->find("version")) {
        const auto parsed = declared->get_integer();
        if (!parsed) {
            return fail(SettingsError::corrupt_file,
                        "\"version\" must be an integer");
        }
        version = *parsed;
    }
    if (version > current) {
        return fail(SettingsError::migrate_failed,
                    "file version " + std::to_string(version) +
                        " is newer than the current shape " +
                        std::to_string(current));
    }
    while (version < current) {
        const auto step = steps_.find(version);
        if (step == steps_.end()) {
            return fail(SettingsError::migrate_failed,
                        "no migration step from version " +
                            std::to_string(version) + " to version " +
                            std::to_string(version + 1));
        }
        WS_TRY(step->second(document));
        // The step may have rebuilt the root entirely.
        root = document.as_object();
        if (root == nullptr) {
            return fail(SettingsError::corrupt_file,
                        "a migration step left the layer file root a "
                        "non-object");
        }
        ++version;
    }
    // Stamp the shape the document now carries, so a save writes the
    // version it was migrated to.
    root->set("version", json::Value::integer(current));
    return {};
}

}  // namespace ws::settings
