#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <ws/core/result.hpp>
#include <ws/settings/errors.hpp>

namespace ws::settings {

// The four scalar settings types (the JSON layer models more - arrays,
// objects - but a schema-declared setting is always one of these).
enum class SettingType : std::int32_t {
    boolean = 0,
    integer = 1,
    real = 2,
    text = 3,
};

// The stable name used in every type-mismatch message.
[[nodiscard]] constexpr std::string_view type_name(SettingType type) noexcept {
    switch (type) {
        case SettingType::boolean: return "bool";
        case SettingType::integer: return "int";
        case SettingType::real: return "float";
        case SettingType::text: return "string";
    }
    return "unknown";
}

// Forward-declared so detail::make_value (below) can name it in its
// declaration; defined after the traits.
class SettingValue;

namespace detail {

// The exact read types: get<T>/as<T> pin T to one of these.
template <class T>
inline constexpr bool kSettingReadType = std::is_same_v<T, bool> ||
                                        std::is_same_v<T, std::int64_t> ||
                                        std::is_same_v<T, double> ||
                                        std::is_same_v<T, std::string>;

// Everything set() accepts: any integral or floating value, plus
// anything string-view-convertible. The narrowed value still has to
// MATCH the schema's declared type - a 3 written into a float key
// answers type_mismatch, never a silent conversion.
template <class T>
inline constexpr bool kSettingWriteType =
    std::is_integral_v<T> || std::is_floating_point_v<T> ||
    std::is_convertible_v<const T&, std::string_view>;

// The read type -> SettingType mapping (static_asserts the rest out).
template <class T>
[[nodiscard]] constexpr SettingType setting_type_of() {
    static_assert(kSettingReadType<T>,
                  "T must be one of: bool, std::int64_t, double, std::string");
    if constexpr (std::is_same_v<T, bool>) {
        return SettingType::boolean;
    } else if constexpr (std::is_same_v<T, std::int64_t>) {
        return SettingType::integer;
    } else if constexpr (std::is_same_v<T, double>) {
        return SettingType::real;
    } else {
        return SettingType::text;
    }
}

// Narrow one write-side value into a SettingValue. Declared here
// (the traits above have no dependency on the class), defined below the
// class - its body names the makers.
template <class T>
[[nodiscard]] SettingValue make_value(const T& value);

}  // namespace detail

// One settings value: exactly one of the four scalar types. No null
// state - absence is a missing map entry (std::optional at the seams),
// never a value that claims to exist. The typed getters answer Result
// so a wrong-type read names its error instead of guessing a default.
class SettingValue {
public:
    SettingValue() = delete;

    [[nodiscard]] static SettingValue boolean(bool v) { return SettingValue{v}; }
    [[nodiscard]] static SettingValue integer(std::int64_t v) { return SettingValue{v}; }
    [[nodiscard]] static SettingValue real(double v) { return SettingValue{v}; }
    // The one text maker takes string_view so literals and strings both
    // resolve to it - separate string/string_view overloads would make
    // text("literal") ambiguous.
    [[nodiscard]] static SettingValue text(std::string_view v) {
        return SettingValue{std::string{v}};
    }

    [[nodiscard]] SettingType type() const noexcept {
        switch (value_.index()) {
            case 0: return SettingType::boolean;
            case 1: return SettingType::integer;
            case 2: return SettingType::real;
            default: return SettingType::text;
        }
    }

    [[nodiscard]] bool operator==(const SettingValue&) const = default;

    [[nodiscard]] ws::core::Result<bool> as_bool() const;
    [[nodiscard]] ws::core::Result<std::int64_t> as_integer() const;
    [[nodiscard]] ws::core::Result<double> as_real() const;
    [[nodiscard]] ws::core::Result<std::string> as_text() const;

    // The typed getter get<T> funnels through.
    template <class T>
    [[nodiscard]] ws::core::Result<T> as() const {
        static_assert(detail::kSettingReadType<T>,
                      "as<T>: T must be bool, std::int64_t, double or std::string");
        if constexpr (std::is_same_v<T, bool>) {
            return as_bool();
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            return as_integer();
        } else if constexpr (std::is_same_v<T, double>) {
            return as_real();
        } else {
            return as_text();
        }
    }

    // Compact human rendering for messages and logs: true/false,
    // base-10 integers, %g reals, quoted text.
    [[nodiscard]] std::string to_text() const;

private:
    explicit SettingValue(std::variant<bool, std::int64_t, double, std::string> v)
        : value_(std::move(v)) {}

    std::variant<bool, std::int64_t, double, std::string> value_;
};

namespace detail {

template <class T>
SettingValue make_value(const T& value) {
    if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
        return SettingValue::boolean(value);
    } else if constexpr (std::is_integral_v<std::decay_t<T>>) {
        return SettingValue::integer(static_cast<std::int64_t>(value));
    } else if constexpr (std::is_floating_point_v<std::decay_t<T>>) {
        return SettingValue::real(static_cast<double>(value));
    } else {
        return SettingValue::text(std::string_view{value});
    }
}

}  // namespace detail

}  // namespace ws::settings
