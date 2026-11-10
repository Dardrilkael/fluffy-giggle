#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <locale>
#include <sstream>

#include "ws/circsim/text.hpp"

namespace ws::circsim {

namespace {

// ASCII-only case fold: 'A'-'Z' -> 'a'-'z', everything else untouched.
// Locale-independent by construction (std::tolower answers to LC_CTYPE).
[[nodiscard]] char fold_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// FP to_chars/from_chars landed in libstdc++ 11, MSVC 16.4(ish) and
// every toolchain this workspace targets; the feature-test macro is the
// honest gate. The fallback keeps the same contract through the classic
// locale, so a build without the macro cannot quietly regress.
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
inline constexpr bool kHasFpCharconv = true;
#else
inline constexpr bool kHasFpCharconv = false;
#endif

}  // namespace

bool CiLess::operator()(const std::string& a, const std::string& b) const {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const char ca = fold_ascii(a[i]);
        const char cb = fold_ascii(b[i]);
        if (ca != cb) {
            return ca < cb;
        }
    }
    return a.size() < b.size();
}

std::string fmt_num(double v, int digits) {
    if (digits < 1) {
        digits = 1;
    }
    if (digits > 17) {
        digits = 17;
    }
    if (kHasFpCharconv) {
        char buf[64];
        const auto [p, ec] =
            std::to_chars(buf, buf + sizeof buf, v,
                          std::chars_format::general, digits);
        if (ec == std::errc{}) {
            return std::string(buf, p);
        }
        // Unreachable for finite doubles in a 64-byte buffer (17 digits
        // + sign + dot + "e+308"); fall through to the stream path so
        // this function never has no answer.
    }
    // The classic-locale fallback: "%.12g" semantics, LC_NUMERIC-proof.
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(digits);
    out << v;
    return out.str();
}

std::optional<double> parse_plain_number(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    if (kHasFpCharconv) {
        double v = 0.0;
        const char* begin = text.data();
        const char* end = begin + text.size();
        // from_chars does not skip leading whitespace; the atof this
        // replaces did not either (callers hand it its own output).
        const auto [p, ec] = std::from_chars(begin, end, v);
        if (ec != std::errc{} || p != end || !std::isfinite(v)) {
            return std::nullopt;
        }
        return v;
    }
    // The classic-locale fallback (strtod in the "C" locale).
    std::string copy(text);
    char* end = nullptr;
    const double v = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || !std::isfinite(v)) {
        return std::nullopt;
    }
    return v;
}

}  // namespace ws::circsim
