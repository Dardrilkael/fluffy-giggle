// The shared utility definitions (see util.hpp / json_helpers.hpp).

#include <algorithm>

#include "json_helpers.hpp"
#include "util.hpp"

namespace ws::circsim {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a')
                                      : c;
    });
    return out;
}

bool is_identifier(std::string_view s) {
    if (s.empty()) {
        return false;
    }
    for (const char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::optional<double> as_number(const ws::settings::json::Value* v) {
    if (v == nullptr) {
        return std::nullopt;
    }
    if (v->is_integer()) {
        const auto n = v->get_integer();
        if (n) {
            return static_cast<double>(*n);
        }
        return std::nullopt;
    }
    if (v->is_real()) {
        const auto n = v->get_real();
        if (n) {
            return *n;
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> as_integer(
    const ws::settings::json::Value* v) {
    if (v != nullptr && v->is_integer()) {
        const auto n = v->get_integer();
        if (n) {
            return *n;
        }
    }
    return std::nullopt;
}

std::optional<std::string> as_text(const ws::settings::json::Value* v) {
    if (v != nullptr && v->is_text()) {
        const auto t = v->get_text();
        if (t) {
            return *t;
        }
    }
    return std::nullopt;
}

std::optional<bool> as_bool(const ws::settings::json::Value* v) {
    if (v != nullptr && v->is_bool()) {
        const auto b = v->get_bool();
        if (b) {
            return *b;
        }
    }
    return std::nullopt;
}

std::optional<Pos> as_pos(const ws::settings::json::Value* v) {
    if (v == nullptr || !v->is_array()) {
        return std::nullopt;
    }
    const ws::settings::json::Array* a = v->as_array();
    if (a == nullptr || a->size() != 2) {
        return std::nullopt;
    }
    const auto x = as_number(&(*a)[0]);
    const auto y = as_number(&(*a)[1]);
    if (!x.has_value() || !y.has_value()) {
        return std::nullopt;
    }
    return Pos{*x, *y};
}

ws::settings::json::Value pos_value(Pos p) {
    ws::settings::json::Array a;
    a.push_back(ws::settings::json::Value::real(p.x));
    a.push_back(ws::settings::json::Value::real(p.y));
    return ws::settings::json::Value::array(std::move(a));
}

}  // namespace ws::circsim
