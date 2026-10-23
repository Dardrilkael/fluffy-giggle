#include <algorithm>
#include <cmath>
#include <regex>
#include <utility>

#include <ws/settings/schema.hpp>

namespace ws::settings {

// ---- coerce -----------------------------------------------------------

ws::core::Result<SettingValue> coerce(const SettingDesc& desc,
                                      SettingValue value) {
    if (value.type() != desc.type) {
        return fail(SettingsError::type_mismatch,
                    "'" + desc.key + "' is " +
                        std::string{type_name(desc.type)} + ", given " +
                        std::string{type_name(value.type())});
    }
    for (const Constraint& c : desc.constraints) {
        if (const auto* range = std::get_if<RangeConstraint>(&c)) {
            double x = 0.0;
            if (desc.type == SettingType::integer) {
                WS_TRY_ASSIGN(auto i, value.as_integer());
                x = static_cast<double>(i);
            } else {
                WS_TRY_ASSIGN(auto d, value.as_real());
                x = d;
            }
            if (x < range->min || x > range->max) {
                if (range->action == ConstraintAction::reject) {
                    return fail(
                        SettingsError::constraint_rejected,
                        "'" + desc.key + "': " + value.to_text() +
                            " outside [" + std::to_string(range->min) + ", " +
                            std::to_string(range->max) + "]");
                }
                // Clamp: the pulled value lands, and later constraints
                // (and the observer payload) see the clamped one.
                x = std::clamp(x, range->min, range->max);
                value = desc.type == SettingType::integer
                            ? SettingValue::integer(std::llround(x))
                            : SettingValue::real(x);
            }
        } else if (const auto* allowed = std::get_if<EnumConstraint>(&c)) {
            const bool in_set = std::any_of(
                allowed->allowed.begin(), allowed->allowed.end(),
                [&value](const SettingValue& v) { return v == value; });
            if (!in_set) {
                return fail(SettingsError::constraint_rejected,
                            "'" + desc.key + "': " + value.to_text() +
                                " is not one of the allowed values");
            }
        } else {
            const auto& constraint = std::get<RegexConstraint>(c);
            WS_TRY_ASSIGN(auto text, value.as_text());
            if (!std::regex_search(text, std::regex{constraint.pattern})) {
                return fail(SettingsError::constraint_rejected,
                            "'" + desc.key + "': " + value.to_text() +
                                " does not match the required pattern");
            }
        }
    }
    return value;
}

// ---- Schema -----------------------------------------------------------

ws::core::Status Schema::declare(SettingDesc desc) {
    if (desc.key.empty()) {
        return fail(SettingsError::bad_schema, "declare: empty key");
    }
    if (find(desc.key) != nullptr) {
        return fail(SettingsError::bad_schema,
                    "declare: duplicate key '" + desc.key + "'");
    }
    if (desc.default_value.type() != desc.type) {
        return fail(SettingsError::bad_schema,
                    "declare: '" + desc.key + "' is " +
                        std::string{type_name(desc.type)} +
                        " but its default is " +
                        std::string{type_name(desc.default_value.type())});
    }
    for (const Constraint& c : desc.constraints) {
        if (const auto* range = std::get_if<RangeConstraint>(&c)) {
            if (desc.type == SettingType::boolean ||
                desc.type == SettingType::text) {
                return fail(SettingsError::bad_schema,
                            "declare: '" + desc.key +
                                "': a range cannot apply to a " +
                                std::string{type_name(desc.type)} +
                                " setting");
            }
            if (range->min > range->max) {
                return fail(SettingsError::bad_schema,
                            "declare: '" + desc.key +
                                "': range min > max");
            }
        } else if (const auto* allowed = std::get_if<EnumConstraint>(&c)) {
            if (allowed->allowed.empty()) {
                return fail(SettingsError::bad_schema,
                            "declare: '" + desc.key + "': empty enum");
            }
            for (const SettingValue& v : allowed->allowed) {
                if (v.type() != desc.type) {
                    return fail(SettingsError::bad_schema,
                                "declare: '" + desc.key +
                                    "': enum entries must be " +
                                    std::string{type_name(desc.type)} +
                                    ", got " + std::string{type_name(v.type())});
                }
            }
        } else {
            const auto& constraint = std::get<RegexConstraint>(c);
            if (desc.type != SettingType::text) {
                return fail(SettingsError::bad_schema,
                            "declare: '" + desc.key +
                                "': a regex constraint needs a text setting");
            }
            try {
                (void)std::regex{constraint.pattern};
            } catch (const std::regex_error&) {
                return fail(SettingsError::bad_schema,
                            "declare: '" + desc.key +
                                "': unparsable regex: " + constraint.pattern);
            }
        }
    }
    descs_.push_back(std::move(desc));
    return {};
}

const SettingDesc* Schema::find(std::string_view key) const {
    // Linear scan on purpose: schemas run to hundreds of keys, not
    // thousands, and every get() rides this - a map would trade a
    // steady allocation-free read for a second copy of every key. Index
    // it if a profile ever says to.
    for (const SettingDesc& d : descs_) {
        if (d.key == key) {
            return &d;
        }
    }
    return nullptr;
}

bool Schema::contains(std::string_view key) const {
    return find(key) != nullptr;
}

std::vector<std::string> Schema::categories() const {
    std::vector<std::string> out;
    for (const SettingDesc& d : descs_) {
        if (std::find(out.begin(), out.end(), d.category) == out.end()) {
            out.push_back(d.category);
        }
    }
    return out;
}

std::vector<const SettingDesc*> Schema::in_category(
    std::string_view category) const {
    std::vector<const SettingDesc*> out;
    for (const SettingDesc& d : descs_) {
        if (d.category == category) {
            out.push_back(&d);
        }
    }
    return out;
}

}  // namespace ws::settings
