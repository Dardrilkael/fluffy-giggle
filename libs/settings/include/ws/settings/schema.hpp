#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/settings/value.hpp>

namespace ws::settings {

// What a range does with a value that sits outside it: reject answers
// the error; clamp pulls the value to the NEAREST boundary and the set
// lands. Enums and regexes never clamp - there is no nearest allowed
// value to pull to.
enum class ConstraintAction : std::int32_t {
    reject = 0,
    clamp = 1,
};

// Numeric bounds, for integer and real settings. The bounds are doubles;
// settings values live far inside double's exact-integer range (2^53),
// so an integer key clamped through a double bound stays exact (the
// clamped integer comes back through llround - a fractional bound on an
// integer key rounds half away from zero, the one documented wrinkle).
struct RangeConstraint {
    double min = 0.0;
    double max = 0.0;
    ConstraintAction action = ConstraintAction::reject;
};

// The value must equal one of the allowed entries (each of the key's own
// type - declare refuses anything else).
struct EnumConstraint {
    std::vector<SettingValue> allowed;
};

// Text settings must match the pattern (std::regex_search semantics: the
// pattern matches anywhere in the value unless it anchors itself). The
// pattern is compiled per check - set() is a rare interactive act, not a
// per-frame read.
struct RegexConstraint {
    std::string pattern;
};

using Constraint = std::variant<RangeConstraint, EnumConstraint, RegexConstraint>;

// One declared setting - the schema's whole entry.
struct SettingDesc {
    std::string key;             // dotted, e.g. "editor.font.size"
    SettingType type = SettingType::boolean;
    SettingValue default_value;  // must match type (checked at declare)
    std::vector<Constraint> constraints;
    std::string description;     // human-facing; introspection shows it
    std::string category;        // grouping; introspection lists these
};

// Type check + every constraint, in declaration order. The first reject
// answers; a clamp rewrites the value and later constraints see the
// clamped one. Returns the value to store - coerce never edits in place.
[[nodiscard]] ws::core::Result<SettingValue> coerce(const SettingDesc& desc,
                                                    SettingValue value);

class Schema {
public:
    Schema() = default;

    // Declare one setting. bad_schema answers: an empty key, a
    // duplicate key, a default whose type differs from the declared
    // type, a constraint that cannot apply to the type (a range on a
    // bool/text key, enum entries of another type, a regex on a
    // non-text key), an empty enum, an inside-out range, or an
    // unparsable regex.
    ws::core::Status declare(SettingDesc desc);

    [[nodiscard]] const SettingDesc* find(std::string_view key) const;
    [[nodiscard]] bool contains(std::string_view key) const;
    [[nodiscard]] std::size_t size() const noexcept { return descs_.size(); }

    // Introspection (the "generate a UI later" surface), in declaration
    // order everywhere.
    [[nodiscard]] const std::vector<SettingDesc>& descriptors() const noexcept {
        return descs_;
    }
    // The unique categories in first-declared order.
    [[nodiscard]] std::vector<std::string> categories() const;
    [[nodiscard]] std::vector<const SettingDesc*> in_category(
        std::string_view category) const;

private:
    std::vector<SettingDesc> descs_;
};

}  // namespace ws::settings
