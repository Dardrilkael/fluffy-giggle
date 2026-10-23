#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/settings/errors.hpp>

namespace ws::settings::json {

// The value kinds. integer and real are DISTINCT: "3" parses to
// integer, "3.0" to real, and the writer keeps them apart the same way
// (a real always carries '.' or an exponent) so a parsed->written->
// parsed value is identical to the original, never silently re-typed.
enum class Kind : std::int8_t {
    null = 0,
    boolean = 1,
    integer = 2,
    real = 3,
    text = 4,
    array = 5,
    object = 6,
};

[[nodiscard]] constexpr std::string_view kind_name(Kind kind) noexcept {
    switch (kind) {
        case Kind::null: return "null";
        case Kind::boolean: return "bool";
        case Kind::integer: return "int";
        case Kind::real: return "float";
        case Kind::text: return "string";
        case Kind::array: return "array";
        case Kind::object: return "object";
    }
    return "unknown";
}

class Value;
using Array = std::vector<Value>;

// The insertion-order object: entries keep the order the parser saw and
// the writer emits (unknown-key round-trips never reorder behind the
// user's back), lookups are a linear scan (settings files run to
// hundreds of keys), and a duplicate key resolves to the FIRST
// position carrying the LAST value - hand-edited files override at the
// bottom. Everything is defined out of line: entries hold an incomplete
// Value here, complete in the .cpp.
class Object {
public:
    using Entry = std::pair<std::string, Value>;

    Object() = default;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] const Value* find(std::string_view key) const noexcept;
    [[nodiscard]] Value* find(std::string_view key) noexcept;
    // Insert or assign: a new key appends; an existing key keeps its
    // position and takes the value.
    void set(std::string key, Value value);
    // Drop a key if present; later entries shift up. True when there
    // was one. (Migration steps need this to retire a renamed key.)
    bool erase(std::string_view key);

    [[nodiscard]] std::vector<Entry>::const_iterator begin() const noexcept;
    [[nodiscard]] std::vector<Entry>::const_iterator end() const noexcept;

    bool operator==(const Object& other) const;

private:
    std::vector<Entry> entries_;
};

// One json value: exactly one of the seven kinds. A null value is
// Kind::null on purpose; settings-layer absence stays a missing key,
// never a stored null.
class Value {
public:
    Value() = default;

    [[nodiscard]] static Value null() { return Value{std::nullptr_t{}}; }
    [[nodiscard]] static Value boolean(bool v) { return Value{v}; }
    [[nodiscard]] static Value integer(std::int64_t v) { return Value{v}; }
    [[nodiscard]] static Value real(double v) { return Value{v}; }
    [[nodiscard]] static Value text(std::string_view v) {
        return Value{std::string{v}};
    }
    [[nodiscard]] static Value array(Array v = {}) { return Value{std::move(v)}; }
    [[nodiscard]] static Value object(Object v = {}) {
        return Value{std::move(v)};
    }

    [[nodiscard]] Kind kind() const noexcept {
        switch (storage_.index()) {
            case 0: return Kind::null;
            case 1: return Kind::boolean;
            case 2: return Kind::integer;
            case 3: return Kind::real;
            case 4: return Kind::text;
            case 5: return Kind::array;
            default: return Kind::object;
        }
    }

    [[nodiscard]] bool is_null() const noexcept { return kind() == Kind::null; }
    [[nodiscard]] bool is_bool() const noexcept { return kind() == Kind::boolean; }
    [[nodiscard]] bool is_integer() const noexcept {
        return kind() == Kind::integer;
    }
    [[nodiscard]] bool is_real() const noexcept { return kind() == Kind::real; }
    [[nodiscard]] bool is_text() const noexcept { return kind() == Kind::text; }
    [[nodiscard]] bool is_array() const noexcept { return kind() == Kind::array; }
    [[nodiscard]] bool is_object() const noexcept {
        return kind() == Kind::object;
    }

    // Container access: nullptr unless the kind matches.
    [[nodiscard]] const std::string* as_text() const noexcept {
        return std::get_if<std::string>(&storage_);
    }
    [[nodiscard]] const Array* as_array() const noexcept {
        return std::get_if<Array>(&storage_);
    }
    [[nodiscard]] Array* as_array() noexcept {
        return std::get_if<Array>(&storage_);
    }
    [[nodiscard]] const Object* as_object() const noexcept {
        return std::get_if<Object>(&storage_);
    }
    [[nodiscard]] Object* as_object() noexcept {
        return std::get_if<Object>(&storage_);
    }

    // The settings-facing scalar pulls. A wrong kind answers
    // type_mismatch - parse only produces matching kinds, so a failure
    // here means a hand-built value lied.
    [[nodiscard]] ws::core::Result<bool> get_bool() const;
    [[nodiscard]] ws::core::Result<std::int64_t> get_integer() const;
    [[nodiscard]] ws::core::Result<double> get_real() const;
    [[nodiscard]] ws::core::Result<std::string> get_text() const;

    [[nodiscard]] bool operator==(const Value& other) const;

private:
    explicit Value(
        std::variant<std::nullptr_t, bool, std::int64_t, double, std::string,
                     Array, Object>
            storage)
        : storage_(std::move(storage)) {}

    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string,
                 Array, Object>
        storage_;
};

// The nesting limit parse enforces - deep enough for any real settings
// file, shallow enough to never threaten the stack.
inline constexpr int kMaxDepth = 128;

// Parse one json document. Comments (// to end of line, /* */ blocks)
// are tolerated anywhere whitespace is, and a leading UTF-8 BOM is
// skipped. Every malformation answers parse_failed naming the error
// and its line/column.
[[nodiscard]] ws::core::Result<Value> parse(std::string_view text);

// The readable form: 4-space indentation, one entry per line, keys in
// insertion order, strings escaped (control chars as \uXXXX, UTF-8
// passes through raw), integers base-10, reals in the shortest form
// that re-parses to the same value - plain decimals where they fit,
// ".0" appended so integer and real never blend on re-parse.
// A non-finite real has no json form and writes as null (schema
// validation keeps those out of settings files).
[[nodiscard]] std::string write(const Value& value);

}  // namespace ws::settings::json
