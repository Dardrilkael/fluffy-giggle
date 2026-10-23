#include <cstdio>
#include <string>
#include <variant>

#include <ws/settings/value.hpp>

namespace ws::settings {
namespace {

// The one shared type_mismatch: names the value's own type and the type
// the caller asked for.
[[nodiscard]] ws::core::unexpected<ws::core::Error> wrong_type(
    SettingType actual, SettingType wanted) {
    return fail(SettingsError::type_mismatch,
                "value is " + std::string{type_name(actual)} + ", read as " +
                    std::string{type_name(wanted)});
}

}  // namespace

ws::core::Result<bool> SettingValue::as_bool() const {
    if (const bool* v = std::get_if<bool>(&value_)) {
        return *v;
    }
    return wrong_type(type(), SettingType::boolean);
}

ws::core::Result<std::int64_t> SettingValue::as_integer() const {
    if (const std::int64_t* v = std::get_if<std::int64_t>(&value_)) {
        return *v;
    }
    return wrong_type(type(), SettingType::integer);
}

ws::core::Result<double> SettingValue::as_real() const {
    if (const double* v = std::get_if<double>(&value_)) {
        return *v;
    }
    return wrong_type(type(), SettingType::real);
}

ws::core::Result<std::string> SettingValue::as_text() const {
    if (const std::string* v = std::get_if<std::string>(&value_)) {
        return *v;
    }
    return wrong_type(type(), SettingType::text);
}

std::string SettingValue::to_text() const {
    struct Render {
        std::string operator()(bool v) const { return v ? "true" : "false"; }
        std::string operator()(std::int64_t v) const {
            return std::to_string(v);
        }
        std::string operator()(double v) const {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", v);
            return std::string{buf};
        }
        std::string operator()(const std::string& v) const {
            return "\"" + v + "\"";
        }
    };
    return std::visit(Render{}, value_);
}

}  // namespace ws::settings
