#pragma once

// The circsim-wide JSON reader/writer helpers (private to the library's
// TUs). One definition each: these used to live as identical per-file
// copies in format.cpp and library.cpp - five helpers times two files,
// each copy free to drift.

#include <optional>
#include <string>

#include <ws/circsim/model.hpp>
#include <ws/settings/json.hpp>

namespace ws::circsim {

// JSON numbers accept both integer and real spellings.
[[nodiscard]] std::optional<double> as_number(
    const ws::settings::json::Value* v);

[[nodiscard]] std::optional<std::int64_t> as_integer(
    const ws::settings::json::Value* v);

[[nodiscard]] std::optional<std::string> as_text(
    const ws::settings::json::Value* v);

[[nodiscard]] std::optional<bool> as_bool(
    const ws::settings::json::Value* v);

// A [x, y] pair of numbers, or nullopt.
[[nodiscard]] std::optional<Pos> as_pos(const ws::settings::json::Value* v);

[[nodiscard]] ws::settings::json::Value pos_value(Pos p);

}  // namespace ws::circsim
