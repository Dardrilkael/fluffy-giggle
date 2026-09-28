#pragma once

#include <cstdint>
#include <source_location>
#include <string>
#include <utility>

// tl::expected is the std::expected backport backing Result/Status. It is the
// one third-party that appears in a ws header - always through the ws-owned
// names below; no other header may name tl:: directly.
#include <tl/expected.hpp>

namespace ws::core {

// Lightweight, copyable, lib-agnostic error. Each lib defines its own enum and
// converts to Error at its public boundary, so core never knows lib codes.
struct Error {
  std::int32_t code = 0;          // lib-defined value, 0 = no error
  const char* category = "core";  // static string, e.g. "audio", "ui", "math"
  std::string message;            // human-readable detail
  std::source_location where{};   // filled by make_error / fail

  [[nodiscard]] explicit operator bool() const noexcept { return code != 0; }
};

template <class T, class E = Error>
using Result = tl::expected<T, E>;

template <class E>
using unexpected = tl::unexpected<E>;

using Status = Result<void>;

[[nodiscard]] inline Error make_error(
    std::int32_t code, const char* category, std::string message,
    std::source_location where = std::source_location::current()) {
  return Error{code, category, std::move(message), where};
}

// Returns an unexpected<Error> that converts into any Result<T>:
//   return ws::core::fail(1, "audio", "device open failed");
[[nodiscard]] inline unexpected<Error> fail(
    std::int32_t code, const char* category, std::string message,
    std::source_location where = std::source_location::current()) {
  return unexpected<Error>(make_error(code, category, std::move(message), where));
}

}  // namespace ws::core

// Early-return error propagation for functions returning ws::core::Result.
// Written for MSVC: no statement expressions, temps get unique per-line names.
//   WS_TRY(engine.init());            // Status-style: propagate or continue
//   WS_TRY_ASSIGN(auto h, load());    // unwrap the value or propagate
#define WS_TRY_CAT_(a, b) a##b
#define WS_TRY_CAT(a, b) WS_TRY_CAT_(a, b)

#define WS_TRY_IMPL(tmp, expr)                                                   \
  do {                                                                           \
    auto&& tmp = (expr);                                                         \
    if (!tmp)                                                                    \
      return ::ws::core::unexpected<::ws::core::Error>(std::move(tmp.error()));  \
  } while (0)
#define WS_TRY(expr) WS_TRY_IMPL(WS_TRY_CAT(ws_try_r_, __LINE__), expr)

#define WS_TRY_ASSIGN_IMPL(tmp, lhs, expr)                                       \
  auto&& tmp = (expr);                                                           \
  if (!tmp)                                                                      \
    return ::ws::core::unexpected<::ws::core::Error>(std::move(tmp.error()));    \
  lhs = std::move(*tmp)
#define WS_TRY_ASSIGN(lhs, expr) \
  WS_TRY_ASSIGN_IMPL(WS_TRY_CAT(ws_try_tmp_, __LINE__), lhs, expr)
