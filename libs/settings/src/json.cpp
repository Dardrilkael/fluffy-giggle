#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include <ws/settings/json.hpp>

namespace ws::settings::json {
namespace {

[[nodiscard]] bool is_ws(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

// UTF-8-encode one code point (the parser's \u escapes decode here; the
// writer never needs it - raw UTF-8 passes through).
void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

}  // namespace

// ---- Object -----------------------------------------------------------

std::size_t Object::size() const noexcept { return entries_.size(); }

bool Object::empty() const noexcept { return entries_.empty(); }

const Value* Object::find(std::string_view key) const noexcept {
    for (const Entry& entry : entries_) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    return nullptr;
}

Value* Object::find(std::string_view key) noexcept {
    for (Entry& entry : entries_) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    return nullptr;
}

void Object::set(std::string key, Value value) {
    if (Value* existing = find(key)) {
        *existing = std::move(value);
        return;
    }
    entries_.emplace_back(std::move(key), std::move(value));
}

bool Object::operator==(const Object& other) const {
    return entries_ == other.entries_;
}

std::vector<Object::Entry>::const_iterator Object::begin() const noexcept {
    return entries_.begin();
}

std::vector<Object::Entry>::const_iterator Object::end() const noexcept {
    return entries_.end();
}

// ---- Value ------------------------------------------------------------

namespace {

[[nodiscard]] ws::core::unexpected<ws::core::Error> wrong_kind(
    const Value& value, std::string_view wanted) {
    return fail(SettingsError::type_mismatch,
                "json value is " + std::string{kind_name(value.kind())} +
                    ", read as " + std::string{wanted});
}

}  // namespace

ws::core::Result<bool> Value::get_bool() const {
    if (const bool* v = std::get_if<bool>(&storage_)) {
        return *v;
    }
    return wrong_kind(*this, "bool");
}

ws::core::Result<std::int64_t> Value::get_integer() const {
    if (const std::int64_t* v = std::get_if<std::int64_t>(&storage_)) {
        return *v;
    }
    return wrong_kind(*this, "int");
}

ws::core::Result<double> Value::get_real() const {
    if (const double* v = std::get_if<double>(&storage_)) {
        return *v;
    }
    return wrong_kind(*this, "float");
}

ws::core::Result<std::string> Value::get_text() const {
    if (const std::string* v = std::get_if<std::string>(&storage_)) {
        return *v;
    }
    return wrong_kind(*this, "string");
}

bool Value::operator==(const Value& other) const {
    return storage_ == other.storage_;
}

// ---- the parser -------------------------------------------------------

namespace {

// One pass over a string_view, Result-shaped failures, the strict json
// grammar with two tolerances: comments (line and block) and a
// leading UTF-8 BOM. Every failure names itself plus its line/column.
class Parser {
public:
    explicit Parser(std::string_view text) : text_{text} {}

    ws::core::Result<Value> run() {
        if (text_.substr(0, 3) == "\xEF\xBB\xBF") {
            pos_ = 3;  // a UTF-8 BOM, from Windows editors
        }
        WS_TRY_ASSIGN(auto root, parse_value(0));
        WS_TRY(skip_ws_and_comments());
        if (pos_ != text_.size()) {
            return err("trailing characters after the document");
        }
        return root;
    }

private:
    [[nodiscard]] char peek() const noexcept {
        return pos_ < text_.size() ? text_[pos_] : '\0';
    }

    // parse_failed at the current position, with line/column.
    [[nodiscard]] ws::core::unexpected<ws::core::Error> err(
        std::string message) const {
        std::size_t line = 1;
        std::size_t column = 1;
        for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        return fail(SettingsError::parse_failed,
                    std::move(message) + " at line " + std::to_string(line) +
                        ", column " + std::to_string(column));
    }

    ws::core::Status skip_ws_and_comments() {
        for (;;) {
            while (pos_ < text_.size() && is_ws(text_[pos_])) {
                ++pos_;
            }
            if (pos_ + 1 < text_.size() && text_[pos_] == '/' &&
                text_[pos_ + 1] == '/') {
                pos_ += 2;
                while (pos_ < text_.size() && text_[pos_] != '\n') {
                    ++pos_;
                }
                continue;
            }
            if (pos_ + 1 < text_.size() && text_[pos_] == '/' &&
                text_[pos_ + 1] == '*') {
                const std::size_t open = pos_;
                pos_ += 2;
                bool closed = false;
                while (pos_ + 1 < text_.size()) {
                    if (text_[pos_] == '*' && text_[pos_ + 1] == '/') {
                        pos_ += 2;
                        closed = true;
                        break;
                    }
                    ++pos_;
                }
                if (!closed) {
                    pos_ = open;  // point the error at the '/*'
                    return err("unterminated block comment");
                }
                continue;
            }
            return {};
        }
    }

    ws::core::Result<Value> parse_value(int depth) {
        if (depth > kMaxDepth) {
            return err("nesting deeper than " + std::to_string(kMaxDepth));
        }
        WS_TRY(skip_ws_and_comments());
        if (pos_ >= text_.size()) {
            return err("unexpected end of input");
        }
        const char c = text_[pos_];
        switch (c) {
            case '{': return parse_object(depth);
            case '[': return parse_array(depth);
            case '"': {
                WS_TRY_ASSIGN(auto text, parse_string());
                return Value::text(std::move(text));
            }
            case 't': return parse_literal("true", Value::boolean(true));
            case 'f': return parse_literal("false", Value::boolean(false));
            case 'n': return parse_literal("null", Value::null());
            default:
                if (c == '-' || is_digit(c)) {
                    return parse_number();
                }
                return err(std::string{"unexpected character '"} + c + "'");
        }
    }

    ws::core::Result<Value> parse_object(int depth) {
        ++pos_;  // '{'
        Object obj;
        WS_TRY(skip_ws_and_comments());
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return Value::object(std::move(obj));
        }
        for (;;) {
            // A comma leaves the reader between tokens; every entry
            // re-skips whitespace and comments before demanding a key.
            WS_TRY(skip_ws_and_comments());
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return err("expected a key");
            }
            WS_TRY_ASSIGN(auto key, parse_string());
            WS_TRY(skip_ws_and_comments());
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return err("expected ':'");
            }
            ++pos_;
            WS_TRY_ASSIGN(auto value, parse_value(depth + 1));
            obj.set(std::move(key), std::move(value));
            WS_TRY(skip_ws_and_comments());
            if (pos_ >= text_.size()) {
                return err("unexpected end of input");
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return Value::object(std::move(obj));
            }
            return err("expected ',' or '}'");
        }
    }

    ws::core::Result<Value> parse_array(int depth) {
        ++pos_;  // '['
        Array arr;
        WS_TRY(skip_ws_and_comments());
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return Value::array(std::move(arr));
        }
        for (;;) {
            WS_TRY_ASSIGN(auto value, parse_value(depth + 1));
            arr.push_back(std::move(value));
            WS_TRY(skip_ws_and_comments());
            if (pos_ >= text_.size()) {
                return err("unexpected end of input");
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return Value::array(std::move(arr));
            }
            return err("expected ',' or ']'");
        }
    }

    ws::core::Result<Value> parse_literal(std::string_view word, Value out) {
        if (text_.substr(pos_, word.size()) != word) {
            return err("invalid literal");
        }
        pos_ += word.size();
        return out;
    }

    ws::core::Result<Value> parse_number() {
        const std::size_t start = pos_;
        bool real = false;
        if (peek() == '-') {
            ++pos_;
        }
        if (pos_ >= text_.size()) {
            return err("unexpected end of input");
        }
        if (text_[pos_] == '0') {
            ++pos_;
        } else if (is_digit(text_[pos_])) {
            while (pos_ < text_.size() && is_digit(text_[pos_])) {
                ++pos_;
            }
        } else {
            return err("invalid number");
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            real = true;
            ++pos_;
            if (pos_ >= text_.size() || !is_digit(text_[pos_])) {
                return err("invalid number: fraction needs a digit");
            }
            while (pos_ < text_.size() && is_digit(text_[pos_])) {
                ++pos_;
            }
        }
        if (pos_ < text_.size() &&
            (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            real = true;
            ++pos_;
            if (pos_ < text_.size() &&
                (text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= text_.size() || !is_digit(text_[pos_])) {
                return err("invalid number: exponent needs a digit");
            }
            while (pos_ < text_.size() && is_digit(text_[pos_])) {
                ++pos_;
            }
        }
        // A number never ends pointing at more number: "01", "1.2.3",
        // "1e3e3" die here instead of mis-failing somewhere vague.
        if (pos_ < text_.size() &&
            (is_digit(text_[pos_]) || text_[pos_] == '.' ||
             text_[pos_] == 'e' || text_[pos_] == 'E')) {
            return err("invalid number");
        }
        const std::string digits{text_.substr(start, pos_ - start)};
        if (!real) {
            errno = 0;
            char* end = nullptr;
            const long long parsed = std::strtoll(digits.c_str(), &end, 10);
            if (errno == ERANGE || *end != '\0') {
                return err("integer literal out of range");
            }
            return Value::integer(static_cast<std::int64_t>(parsed));
        }
        const double parsed = std::strtod(digits.c_str(), nullptr);
        if (!std::isfinite(parsed)) {
            return err("invalid number");  // 1e999 and friends
        }
        return Value::real(parsed);
    }

    ws::core::Result<std::string> parse_string() {
        ++pos_;  // the opening quote
        std::string out;
        for (;;) {
            if (pos_ >= text_.size()) {
                return err("unterminated string");
            }
            const unsigned char c =
                static_cast<unsigned char>(text_[pos_]);
            if (c == '"') {
                ++pos_;
                return out;
            }
            if (c == '\\') {
                ++pos_;
                if (pos_ >= text_.size()) {
                    return err("unterminated string");
                }
                const char esc = text_[pos_++];
                switch (esc) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        WS_TRY_ASSIGN(auto cp, parse_hex4());
                        if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            return err("lone low surrogate");
                        }
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (pos_ + 1 >= text_.size() ||
                                text_[pos_] != '\\' ||
                                text_[pos_ + 1] != 'u') {
                                return err("unpaired high surrogate");
                            }
                            pos_ += 2;
                            WS_TRY_ASSIGN(auto low, parse_hex4());
                            if (low < 0xDC00 || low > 0xDFFF) {
                                return err("invalid surrogate pair");
                            }
                            cp = 0x10000 + ((cp - 0xD800) << 10) +
                                 (low - 0xDC00);
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default:
                        return err(std::string{"invalid escape '\\"} +
                                   esc + "'");
                }
            } else if (c < 0x20) {
                return err("control character in string");
            } else {
                out += static_cast<char>(c);
                ++pos_;
            }
        }
    }

    ws::core::Result<std::uint32_t> parse_hex4() {
        if (pos_ + 4 > text_.size()) {
            return err("invalid \\u escape");
        }
        std::uint32_t cp = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = text_[pos_ + static_cast<std::size_t>(i)];
            std::uint32_t v = 0;
            if (h >= '0' && h <= '9') {
                v = static_cast<std::uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                v = static_cast<std::uint32_t>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                v = static_cast<std::uint32_t>(h - 'A' + 10);
            } else {
                return err("invalid \\u escape");
            }
            cp = (cp << 4) | v;
        }
        pos_ += 4;
        return cp;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

}  // namespace

ws::core::Result<Value> parse(std::string_view text) {
    return Parser{text}.run();
}

// ---- the writer -------------------------------------------------------

namespace {

void write_indent(std::string& out, int depth) {
    out.append(static_cast<std::size_t>(depth) * 4, ' ');
}

void write_escaped(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: {
                const auto byte = static_cast<unsigned char>(c);
                if (byte < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(byte));
                    out += buf;
                } else {
                    out += c;  // UTF-8 passes through raw
                }
                break;
            }
        }
    }
    out += '"';
}

// The shortest form that re-parses to the same double: plain decimals
// first (readable in a settings file), scientific notation for the
// magnitudes %f cannot spell in 64 chars. The ".0" suffix keeps integer
// and real from blending on re-parse.
void write_real(std::string& out, double v) {
    if (!std::isfinite(v)) {
        out += "null";  // no json form; schema validation keeps these out
        return;
    }
    char buf[64];
    bool ok = false;
    for (int frac = 0; frac <= 16; ++frac) {
        const int len = std::snprintf(buf, sizeof(buf), "%.*f", frac, v);
        if (len > 0 && static_cast<std::size_t>(len) < sizeof(buf) &&
            std::strtod(buf, nullptr) == v) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        for (int prec = 1; prec <= 17; ++prec) {
            const int len = std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
            if (len > 0 && static_cast<std::size_t>(len) < sizeof(buf) &&
                std::strtod(buf, nullptr) == v) {
                ok = true;
                break;
            }
        }
    }
    std::string s{buf};
    if (s.find_first_of(".eE") == std::string::npos) {
        s += ".0";
    }
    out += s;
}

void write_value(std::string& out, const Value& value, int depth) {
    switch (value.kind()) {
        case Kind::null:
            out += "null";
            break;
        case Kind::boolean:
            out += value.get_bool().value_or(false) ? "true" : "false";
            break;
        case Kind::integer:
            out += std::to_string(value.get_integer().value_or(0));
            break;
        case Kind::real:
            write_real(out, value.get_real().value_or(0.0));
            break;
        case Kind::text:
            write_escaped(out, *value.as_text());
            break;
        case Kind::array: {
            const Array& arr = *value.as_array();
            if (arr.empty()) {
                out += "[]";
                break;
            }
            out += "[\n";
            for (std::size_t i = 0; i < arr.size(); ++i) {
                write_indent(out, depth + 1);
                write_value(out, arr[i], depth + 1);
                out += (i + 1 < arr.size()) ? ",\n" : "\n";
            }
            write_indent(out, depth);
            out += ']';
            break;
        }
        case Kind::object: {
            const Object& obj = *value.as_object();
            if (obj.empty()) {
                out += "{}";
                break;
            }
            out += "{\n";
            std::size_t index = 0;
            for (const Object::Entry& entry : obj) {
                write_indent(out, depth + 1);
                write_escaped(out, entry.first);
                out += ": ";
                write_value(out, entry.second, depth + 1);
                out += (++index < obj.size()) ? ",\n" : "\n";
            }
            write_indent(out, depth);
            out += '}';
            break;
        }
    }
}

}  // namespace

std::string write(const Value& value) {
    std::string out;
    out.reserve(256);
    write_value(out, value, 0);
    out += '\n';
    return out;
}

}  // namespace ws::settings::json
